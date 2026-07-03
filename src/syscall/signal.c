#include "signal.h"
#include "sched/sched.h"
#include "mm/mmu/mmu.h"   /* USER_STACK_TOP, USER_SIGTRAMP_VA, USER_STACK_PAGES */
#include "mm/pmm/pmm.h"   /* PAGE_SIZE */
#include "timer/timer.h"  /* TIMER_INTERVAL_MS */
#include "uart/uart.h"
#include "strings/strings.h" // IWYU pragma: keep

/* ---------------------------------------------------------------------------
 * signal.c — POSIX-style signal delivery for Fermi OS / CortexForge.
 *
 * Signals are delivered lazily on the return path to EL0 (never to an EL1
 * task). check_and_deliver builds a signal frame on the USER stack, points the
 * trap frame at the handler with x30 = the sigreturn trampoline, and lets the
 * normal eret enter the handler at EL0. When the handler returns it `ret`s into
 * the trampoline page (mov x8,#SYS_SIGRETURN; svc #0), and SYS_SIGRETURN
 * restores the interrupted context byte-for-byte from the frame.
 * ------------------------------------------------------------------------- */

/* SP_EL0 lives at byte offset 280 (index 35) in the 688-byte on-stack trap
 * frame; the C trap_frame_t only maps the first 280 bytes, so poke it raw.
 * Valid because `frame` always points into the real on-stack frame (or, in the
 * benchmark, a full 688-byte backing buffer). */
static inline uint64_t tf_get_sp(trap_frame_t *f) { return ((uint64_t *)f)[35]; }
static inline void tf_set_sp(trap_frame_t *f, uint64_t v) {
  ((uint64_t *)f)[35] = v;
}

#define SIGFRAME_RESERVE 288 /* 16-byte-aligned reservation for the 280B frame */

/* Terminate the current task on an uncatchable/default-action signal. task_exit
 * marks current DEAD and schedules away — it never returns. */
static void signal_terminate(struct task *t, int sig, const char *why) {
  uart_printf("[SIG] task %u '%s' terminated by signal %d (%s)\n",
              (uint64_t)t->pid, t->name, (uint64_t)sig, why);
  task_exit();
}

/* Build the signal frame on the user stack and retarget `frame` at `handler`.
 * Returns 1 on success. On user-stack overflow the task is terminated (no
 * return). Writes go through the current task's active TTBR0 mapping. */
static int deliver_one(struct task *t, trap_frame_t *frame, int sig,
                       uint64_t handler) {
  uint64_t old_sp = tf_get_sp(frame);
  uint64_t new_sp = (old_sp - SIGFRAME_RESERVE) & ~(uint64_t)15;

  /* Lowest currently-mapped stack address = initial eager pages plus any
   * demand-grown pages (which fill downward, contiguous with the initial
   * region). Placing the frame below that would fault the kernel when we write
   * it, so treat it as a stack overflow and kill the task instead. */
  uint64_t stack_lo =
      USER_STACK_TOP -
      (uint64_t)(USER_STACK_PAGES + t->stack_grown_count) * PAGE_SIZE;
  if (new_sp < stack_lo || old_sp > USER_STACK_TOP) {
    signal_terminate(t, sig, "signal-stack overflow");
    return 0; /* unreachable */
  }

  sigframe_t *sf = (sigframe_t *)new_sp;
  for (int i = 0; i < 31; i++) {
    sf->regs[i] = frame->regs[i];
  }
  sf->sp_el0     = old_sp;
  sf->elr        = frame->elr;
  sf->spsr       = frame->spsr;
  sf->trampoline = USER_SIGTRAMP_VA;

  /* Retarget the trap frame so the eret lands in the handler at EL0. */
  frame->elr      = handler;
  frame->regs[0]  = (uint64_t)sig;       /* handler(int signum) */
  frame->regs[30] = USER_SIGTRAMP_VA;    /* x30: handler `ret` -> trampoline */
  tf_set_sp(frame, new_sp);              /* SP_EL0 = frame base */
  /* frame->spsr unchanged: handler runs in the same EL0 mode. */
  return 1;
}

int signal_check_and_deliver(struct task *t, trap_frame_t *frame) {
  if (!t) {
    return 0;
  }
  /* Only deliver when returning to EL0 (SPSR_EL1.M[3:0] == 0). Never to EL1. */
  if ((frame->spsr & 0xF) != 0) {
    return 0;
  }

  for (;;) {
    uint32_t pending = t->sig_pending & ~t->sig_blocked;
    if (pending == 0) {
      return 0;
    }
    int sig = 0;
    for (int n = SIG_MIN; n <= SIG_MAX; n++) {
      if (pending & (1u << n)) {
        sig = n;
        break;
      }
    }
    if (sig == 0) {
      return 0;
    }
    t->sig_pending &= ~(1u << sig);

    /* SIGKILL is uncatchable and unignorable. */
    if (sig == SIGKILL) {
      signal_terminate(t, sig, "SIGKILL");
      return 0; /* unreachable */
    }

    uint64_t handler = t->sig_handlers[sig];
    if (handler == SIG_IGN) {
      continue; /* ignore, look for the next pending signal */
    }
    if (handler == SIG_DFL) {
      signal_terminate(t, sig, "default action");
      return 0; /* unreachable */
    }
    return deliver_one(t, frame, sig, handler); /* one handler per return */
  }
}

void signal_sigreturn(trap_frame_t *frame) {
  uint64_t user_sp = tf_get_sp(frame);
  /* Validate the frame lies wholly inside the user address range. */
  if (user_sp >= USER_STACK_TOP ||
      user_sp + sizeof(sigframe_t) > USER_STACK_TOP) {
    uart_errorln("[SIG] sigreturn: bad frame pointer — killing task");
    task_exit();
    return; /* unreachable */
  }
  sigframe_t *sf = (sigframe_t *)user_sp;
  for (int i = 0; i < 31; i++) {
    frame->regs[i] = sf->regs[i];
  }
  frame->elr  = sf->elr;
  frame->spsr = sf->spsr;
  tf_set_sp(frame, sf->sp_el0);
}

/* ---- syscall backends ---------------------------------------------------- */

int64_t signal_sigaction(struct task *t, int signum, uint64_t handler) {
  if (signum < SIG_MIN || signum > SIG_MAX || signum == SIGKILL) {
    return -1;
  }
  t->sig_handlers[signum] = handler;
  return 0;
}

/* Minimal user-pointer range check (matches the syscall layer's policy: range
 * only, not page-presence). */
static int u32_ptr_ok(uint64_t ptr) {
  if (ptr == 0) {
    return 0;
  }
  if (ptr + sizeof(uint32_t) > USER_STACK_TOP || ptr + sizeof(uint32_t) < ptr) {
    return 0;
  }
  return 1;
}

int64_t signal_sigprocmask(struct task *t, int how, uint64_t set_ptr,
                           uint64_t oldset_ptr) {
  uint32_t oldmask = t->sig_blocked;

  if (set_ptr != 0) {
    if (!u32_ptr_ok(set_ptr)) {
      return -1;
    }
    uint32_t set = *(volatile uint32_t *)set_ptr;
    uint32_t blocked = oldmask;
    switch (how) {
    case SIG_BLOCK:   blocked |= set;  break;
    case SIG_UNBLOCK: blocked &= ~set; break;
    case SIG_SETMASK: blocked = set;   break;
    default:          return -1;
    }
    /* SIGKILL can never be blocked. */
    t->sig_blocked = blocked & ~(1u << SIGKILL);
  }

  if (oldset_ptr != 0) {
    if (!u32_ptr_ok(oldset_ptr)) {
      return -1;
    }
    *(volatile uint32_t *)oldset_ptr = oldmask;
  }
  return 0;
}

int64_t signal_alarm(struct task *t, uint64_t seconds) {
  /* sig_alarm_ticks is decremented once per timer IRQ, i.e. every
   * TIMER_INTERVAL_MS. Convert seconds -> ticks accordingly. (The spec
   * mentioned CNTFRQ_EL0, but that is the hardware counter rate, not the
   * scheduler-tick rate that actually decrements the alarm; using the tick
   * period is what makes the alarm fire at the requested wall-clock time.) */
  uint64_t ticks_per_sec = 1000ULL / TIMER_INTERVAL_MS; /* 100 at 10 ms */
  uint64_t prev_ticks    = t->sig_alarm_ticks;
  uint64_t prev_seconds  = (prev_ticks + ticks_per_sec - 1) / ticks_per_sec;

  t->sig_alarm_ticks = seconds * ticks_per_sec;
  return (int64_t)prev_seconds;
}

void signal_tick_alarms(void) {
  task_t *head = sched_first_task();
  if (!head) {
    return;
  }
  task_t *t = head;
  do {
    if (t->sig_alarm_ticks > 0) {
      if (--t->sig_alarm_ticks == 0) {
        t->sig_pending |= (1u << SIGALRM);
      }
    }
    t = t->next;
  } while (t != head);
}
