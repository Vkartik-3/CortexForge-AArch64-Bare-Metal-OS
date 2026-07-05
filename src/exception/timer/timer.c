#include "timer.h"
#include "bench/bench.h"
#include "gic/gic.h"
#include "sched/sched.h"
#include "sched/rtos.h"
#include "signal.h"
#include "uart/uart.h"

// All four are read/written from IRQ context as well as task context, so
// mark them volatile to keep the compiler honest.
static volatile uint64_t timer_freq = 0;
static volatile uint64_t timer_interval = 0;
static volatile uint64_t tick_count = 0;
static volatile timer_callback_t tick_callback = 0;
/* Absolute CNTPCT of the next scheduler-tick boundary. With the Phase-3 dynamic
 * (tickless) timer the one-shot may fire EARLIER than this to release a periodic
 * RT task; g_next_tick keeps the 10 ms scheduler cadence (sleep/alarm/uptime)
 * independent of those early RT firings. */
static volatile uint64_t g_next_tick = 0;

void timer_init() {
  uart_println("[TIMER] Initializing hardware timer");
  // initial value set by fw
  __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(timer_freq));

  uart_printf("[TIMER] Frequency: %u Hz (%u MHz)\n", timer_freq,
              timer_freq / 1000000);

  gic_enable_irq(TIMER_PPI_INTID);

  uart_println("[TIMER] Initialized!");
}

void timer_start(uint64_t interval_ms) {
  if (timer_freq == 0) {
    uart_errorln("[TIMER] Not initialized! Call timer_init() first");
    return;
  }

  // milliseconds to timer ticks
  timer_interval = timer_freq * interval_ms / 1000;
  tick_count = 0;

  uart_printf("[TIMER] Starting with interval: %u ms (%u ticks)\n", interval_ms,
              timer_interval);

  // Use absolute deadlines (CVAL) instead of relative countdowns (TVAL)
  // so IRQ latency does not accumulate over time. CVAL is compared against
  // CNTPCT_EL0 by the timer; once they cross, the IRQ fires.
  uint64_t now;
  __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(now));
  g_next_tick = now + timer_interval;
  __asm__ __volatile__(
      "msr cntp_cval_el0, %0" ::"r"(g_next_tick));             // Deadline
  __asm__ __volatile__("msr cntp_ctl_el0, %0" ::"r"(1ULL));    // Enable

  uart_println("[TIMER] Started!");
}

void timer_stop() {
  __asm__ __volatile__("msr cntp_ctl_el0, %0" ::"r"(0ULL));
  uart_printf("[TIMER] Stopped after %u ticks\n", tick_count);
}

void timer_handle_irq() {
  // IRQ-latency instrumentation (PMU bench harness). CNTP_CVAL_EL0 still holds
  // the absolute deadline that just fired; CNTPCT_EL0 is "now". Their delta is
  // the deadline→handler-entry latency in timer ticks. Sampled first thing so
  // nothing in the handler (wake_sleepers, logging) inflates the measurement.
  {
    uint64_t fired_deadline, now_pct;
    __asm__ __volatile__("mrs %0, cntp_cval_el0" : "=r"(fired_deadline));
    __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(now_pct));
    bench_irq_sample(now_pct - fired_deadline);
  }

  uint64_t now;
  __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(now));

  // Scheduler tick: only when a 10 ms (timer_interval) boundary has been
  // reached. Early one-shot firings for RT releases skip this so sleep/alarm/
  // uptime stay on the fixed cadence.
  if (now >= g_next_tick) {
    tick_count++;
    g_next_tick += timer_interval;

    sched_wake_sleepers();   // wake tasks whose sleep deadline passed
    signal_tick_alarms();    // decrement per-task SIGALRM countdowns

    if (tick_callback) {
      tick_callback();
    } else if (tick_count % 100 == 0) {
      uart_printf("[TIMER] tick %u\n", tick_count);
    }
  }

  // Release any periodic RT tasks whose CNTPCT deadline has arrived; returns
  // the soonest still-pending release (0 if none).
  uint64_t rt_earliest = rt_tick_release();

  // Program the next one-shot deadline: the sooner of the next scheduler tick
  // and the next RT release (dynamic/tickless timer). Guard against a deadline
  // already in the past so we never spin re-firing immediately.
  uint64_t next_ev = g_next_tick;
  if (rt_earliest != 0 && rt_earliest < next_ev) {
    next_ev = rt_earliest;
  }
  uint64_t now2;
  __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(now2));
  uint64_t slack = timer_interval / 100;
  if (slack == 0) {
    slack = 1;
  }
  if (next_ev <= now2) {
    next_ev = now2 + slack;
  }
  __asm__ __volatile__("msr cntp_cval_el0, %0" ::"r"(next_ev));
}

void timer_set_callback(timer_callback_t cb) { tick_callback = cb; }

uint64_t timer_get_frequency() { return timer_freq; }

uint64_t timer_get_count() {
  uint64_t count;
  // Use the *physical* counter to match the physical timer (CNTPCT_EL0).
  // CNTVCT_EL0 (virtual) can diverge from CNTPCT_EL0 under virtualization.
  __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(count));
  return count;
}

uint64_t timer_get_ticks() { return tick_count; }

uint64_t timer_uptime_ms(void) { return tick_count * TIMER_INTERVAL_MS; }
uint64_t timer_uptime_seconds(void) {
  return (tick_count * TIMER_INTERVAL_MS) / 1000;
}

uint64_t timer_get_interval_ticks(void) { return timer_interval; }
void timer_set_interval_ticks(uint64_t ticks) { timer_interval = ticks; }
