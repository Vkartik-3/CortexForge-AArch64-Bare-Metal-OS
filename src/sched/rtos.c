#include "rtos.h"
#include "timer/timer.h"
#include "uart/uart.h"
#include "strings/strings.h" // IWYU pragma: keep

/* ---------------------------------------------------------------------------
 * rtos.c — periodic tasks, deadline/WCRT tracking, and blocking primitives
 * with priority inheritance, layered on the fixed-priority scheduler.
 * Timing uses the ARM generic-timer physical counter (CNTPCT_EL0) via
 * timer_get_count(); CNTFRQ (timer_get_frequency()) converts to microseconds.
 * ------------------------------------------------------------------------- */

static inline uint64_t irq_save(void) {
  uint64_t d;
  __asm__ __volatile__("mrs %0, daif" : "=r"(d));
  __asm__ __volatile__("msr daifset, #2" ::: "memory");
  return d;
}
static inline void irq_restore(uint64_t d) {
  __asm__ __volatile__("msr daif, %0" ::"r"(d) : "memory");
}

static uint64_t us_to_cyc(uint64_t us) {
  return us * timer_get_frequency() / 1000000ULL;
}
static uint64_t cyc_to_us(uint64_t cyc) {
  uint64_t f = timer_get_frequency();
  return f ? (cyc * 1000000ULL / f) : 0;
}

/* ---- Phase 1: periodic tasks ------------------------------------------- */

int rt_create_periodic(const char *name, task_entry_t entry, uint8_t priority,
                       uint64_t period_us, uint64_t deadline_us) {
  int pid = sched_create_rt_task(name, entry, priority);
  if (pid < 0) {
    return -1;
  }
  task_t *t = sched_find_task((uint64_t)pid);
  if (!t) {
    return -1;
  }
  uint64_t now = timer_get_count();
  t->rt_period_cyc   = us_to_cyc(period_us);
  t->rt_deadline_cyc = deadline_us ? us_to_cyc(deadline_us) : t->rt_period_cyc;
  t->rt_release_cyc  = now;                       /* first job releases now   */
  t->rt_next_release = now + t->rt_period_cyc;    /* second release           */
  t->rt_activations  = 1;
  uart_printf("[RT] periodic '%s' pid=%d prio=%d period=%uus deadline=%uus\n",
              name, pid, (uint64_t)priority, period_us,
              deadline_us ? deadline_us : period_us);
  return pid;
}

void rt_wait_next_period(void) {
  task_t *t = sched_current();
  if (t->rt_period_cyc == 0) {
    return; /* not a periodic task */
  }
  uint64_t now  = timer_get_count();
  uint64_t resp = now - t->rt_release_cyc;
  t->rt_last_resp_cyc = resp;
  /* Skip the first (warm-up) job: its "release" is task-creation time, which
   * includes one-off spawn/first-schedule latency and is not representative of
   * steady-state response. */
  if (t->rt_activations > 1) {
    if (resp > t->rt_wcrt_cyc) {
      t->rt_wcrt_cyc = resp;
    }
    if (resp > t->rt_deadline_cyc) {
      t->rt_deadline_misses++;
    }
  }
  /* Park until the next release; rt_tick_release() wakes us at rt_next_release.
   * sched_wake_sleepers ignores periodic tasks, so sleep_until is unused. */
  t->state = TASK_SLEEPING;
  schedule();
}

uint64_t rt_tick_release(void) {
  uint64_t now      = timer_get_count();
  uint64_t earliest = 0;
  task_t  *head = sched_first_task();
  task_t  *t    = head;
  do {
    if (t->rt_period_cyc > 0) {
      if (now >= t->rt_next_release) {
        uint64_t sched_release = t->rt_next_release;
        if (t->state == TASK_SLEEPING) {
          t->rt_release_cyc = sched_release; /* deadline is relative to this */
          t->state          = TASK_READY;
          t->rt_activations++;
        } else {
          /* Still runnable from the previous release — the job overran its
           * period: a deadline miss. */
          t->rt_deadline_misses++;
        }
        t->rt_next_release = sched_release + t->rt_period_cyc;
      }
      /* Track the soonest pending release for the Phase-3 one-shot timer. */
      if (earliest == 0 || t->rt_next_release < earliest) {
        earliest = t->rt_next_release;
      }
    }
    t = t->next;
  } while (t != head);
  return earliest;
}

int rt_render_stats(char *buf, int len) {
  int pos = 0;
  const char hdr[] =
      "NAME       PRIO PERIOD(us) ACT   WCRT(us) LAST(us) MISS\n";
  for (int i = 0; i < (int)sizeof(hdr) - 1 && pos < len; i++) {
    buf[pos++] = hdr[i];
  }
  task_t *head = sched_first_task();
  task_t *t    = head;
  do {
    if (t->rt_period_cyc > 0 && pos < len) {
      char line[128];
      int n = ksnprintf(line, sizeof(line),
                        "%s  p%d  %u  %u  %u  %u  %u\n",
                        t->name, (uint64_t)t->base_priority,
                        cyc_to_us(t->rt_period_cyc), (uint64_t)t->rt_activations,
                        cyc_to_us(t->rt_wcrt_cyc), cyc_to_us(t->rt_last_resp_cyc),
                        (uint64_t)t->rt_deadline_misses);
      if (n > 0) {
        int c = (n > len - pos) ? len - pos : n;
        memcpy(buf + pos, line, c);
        pos += c;
      }
    }
    t = t->next;
  } while (t != head);
  return pos;
}

/* ---- Phase 2: blocking primitives with priority inheritance ------------ */

/* Insert `me` into a wait queue kept sorted by priority (highest first), so the
 * head is always the next task to grant. */
static void wq_insert(task_t **head, task_t *me) {
  me->wait_next = NULL;
  if (!*head || me->priority > (*head)->priority) {
    me->wait_next = *head;
    *head = me;
    return;
  }
  task_t *p = *head;
  while (p->wait_next && p->wait_next->priority >= me->priority) {
    p = p->wait_next;
  }
  me->wait_next = p->wait_next;
  p->wait_next = me;
}

/* Priority inheritance: raise the mutex owner (and, along a blocking chain, the
 * task it is itself blocked on) to at least `prio`. Bounded to avoid spinning
 * on a cyclic (deadlocked) chain. */
static void pi_propagate(task_t *owner, uint8_t prio) {
  int depth = 0;
  while (owner && depth++ < 8) {
    if (prio <= owner->priority) {
      break;
    }
    owner->priority = prio; /* boost effective priority */
    uart_printf("[RT] priority inheritance: '%s' boosted to %d\n", owner->name,
                (uint64_t)prio);
    rt_mutex_t *bm = (rt_mutex_t *)owner->blocked_on;
    if (!bm) {
      break;
    }
    owner = bm->owner;
  }
}

void rt_mutex_init(rt_mutex_t *m) {
  m->owner = (void *)0;
  m->waiters = (void *)0;
  m->locked = 0;
}

void rt_mutex_lock(rt_mutex_t *m) {
  uint64_t daif = irq_save();
  if (!m->locked) {
    m->locked = 1;
    m->owner = sched_current();
    irq_restore(daif);
    return;
  }
  /* Contended: enqueue and inherit-boost the owner, then block. */
  task_t *me = sched_current();
  me->blocked_on = m;
  wq_insert(&m->waiters, me);
  pi_propagate(m->owner, me->priority);
  me->state = TASK_BLOCKED;
  irq_restore(daif);
  schedule();          /* switch away; unlock() grants us ownership + READY */
  me->blocked_on = (void *)0;
}

void rt_mutex_unlock(rt_mutex_t *m) {
  uint64_t daif = irq_save();
  task_t *me = sched_current();
  /* Undo any priority-inheritance boost. Simplified single-mutex model: restore
   * to the nominal base priority (a task holding several contended mutexes would
   * need max-over-remaining-holders; out of scope for this demo). */
  me->priority = me->base_priority;

  if (!m->waiters) {
    m->locked = 0;
    m->owner = (void *)0;
    irq_restore(daif);
    return;
  }
  /* Hand ownership to the highest-priority waiter (queue head). */
  task_t *w = m->waiters;
  m->waiters = w->wait_next;
  w->wait_next = (void *)0;
  m->owner = w;                 /* locked stays 1 */
  w->state = TASK_READY;
  irq_restore(daif);
  schedule();                   /* preempt to w if it now outranks us */
}

void rt_sem_init(rt_sem_t *s, int initial) {
  s->count = initial;
  s->waiters = (void *)0;
}

void rt_sem_wait(rt_sem_t *s) {
  uint64_t daif = irq_save();
  if (s->count > 0) {
    s->count--;
    irq_restore(daif);
    return;
  }
  task_t *me = sched_current();
  wq_insert(&s->waiters, me);
  me->state = TASK_BLOCKED;
  irq_restore(daif);
  schedule();
}

void rt_sem_post(rt_sem_t *s) {
  uint64_t daif = irq_save();
  if (s->waiters) {
    task_t *w = s->waiters;
    s->waiters = w->wait_next;
    w->wait_next = (void *)0;
    w->state = TASK_READY;
    irq_restore(daif);
    schedule();
  } else {
    s->count++;
    irq_restore(daif);
  }
}

/* ---- built-in demos ---------------------------------------------------- */

static volatile uint64_t rt_demo_counters[3];

static void rt_busy(volatile int iters) {
  for (volatile int i = 0; i < iters; i++) {
    __asm__ __volatile__("nop");
  }
}

static void rt_demo_hi(void) {
  while (1) { rt_demo_counters[0]++; rt_busy(2000);  rt_wait_next_period(); }
}
static void rt_demo_mid(void) {
  while (1) { rt_demo_counters[1]++; rt_busy(4000);  rt_wait_next_period(); }
}
static void rt_demo_lo(void) {
  while (1) { rt_demo_counters[2]++; rt_busy(8000);  rt_wait_next_period(); }
}
/* Sub-tick period (2 ms < the 10 ms scheduler tick) — only achievable with the
 * Phase-3 high-resolution dynamic timer. */
static void rt_demo_fast(void) {
  while (1) { rt_demo_counters[0]++; rt_busy(500); rt_wait_next_period(); }
}

static int rt_demo_started = 0;
void rt_demo_start(void) {
  if (rt_demo_started) {
    return;
  }
  rt_demo_started = 1;
  /* Rate-monotonic priority assignment: shorter period => higher priority. */
  uart_println("[RT] starting periodic demo (rate-monotonic: rt_fast/hi/mid/lo)");
  rt_create_periodic("rt_fast", rt_demo_fast, 35,   5000,   5000); /*  5 ms (< 10 ms tick) */
  rt_create_periodic("rt_hi",  rt_demo_hi,  30, 100000, 100000);   /* 100 ms */
  rt_create_periodic("rt_mid", rt_demo_mid, 20, 200000, 200000);   /* 200 ms */
  rt_create_periodic("rt_lo",  rt_demo_lo,  12, 500000, 500000);   /* 500 ms */
}

/* ---- priority-inversion demo ------------------------------------------- */

static rt_mutex_t pi_mutex;
static int pi_demo_started = 0;

/* low runs first (high/med sleep at start), takes the mutex, and holds it across
 * a sleep so it is guaranteed still held when high wakes and blocks on it. */
static void pi_low(void) {
  rt_mutex_lock(&pi_mutex);
  uart_println("[PI] low(12): acquired mutex, holding ~80 ms");
  sleep_ms(80);
  uart_println("[PI] low: releasing mutex");
  rt_mutex_unlock(&pi_mutex);
  uart_println("[PI] low: done");
  task_exit();
}
static void pi_med(void) {
  sleep_ms(20);
  /* Medium-priority CPU hog: without priority inheritance it would outrank the
   * mutex-holding `low` and delay the release, starving `high` (classic
   * unbounded priority inversion). With PI, `low` is boosted above `med`. */
  uart_println("[PI] med(16): running CPU-bound work");
  rt_busy(6000000);
  uart_println("[PI] med: done");
  task_exit();
}
static void pi_high(void) {
  sleep_ms(40); /* let low acquire the mutex first */
  uart_println("[PI] high(20): blocking on mutex held by low -> triggers PI");
  rt_mutex_lock(&pi_mutex);
  uart_println("[PI] high: acquired mutex");
  rt_mutex_unlock(&pi_mutex);
  uart_println("[PI] high: done");
  task_exit();
}

void rt_pi_demo_start(void) {
  if (pi_demo_started) {
    return;
  }
  pi_demo_started = 1;
  rt_mutex_init(&pi_mutex);
  /* All three priorities are above the default task priority (PRIO_DEFAULT=8)
   * so none is starved by the shell; the RELATIVE order low<med<high is what
   * produces the inversion. */
  uart_println("[PI] priority-inversion demo: low(12) med(16) high(20)");
  sched_create_rt_task("pi_low",  pi_low,  12);
  sched_create_rt_task("pi_med",  pi_med,  16);
  sched_create_rt_task("pi_high", pi_high, 20);
}
