#include "bench.h"
#include "cpu/cpu.h"
#include "sched/sched.h"
#include "timer/timer.h"
#include "uart/uart.h"
#include "syscall/syscall.h"
#include "syscall/signal.h"
#include "uart/framing.h"
#include "mm/mmu/mmu.h"       /* USER_STACK_TOP, USER_TEXT_BASE, USER_SIGTRAMP_VA */
#include "strings/strings.h"  // IWYU pragma: keep

/* ---------------------------------------------------------------------------
 * bench.c — cycle-level latency microbenchmarks on the ARM PMU.
 *
 * FREQUENCY ASSUMPTION
 * --------------------
 * The codebase does not define a CPU clock frequency anywhere; the only clock
 * it knows is the ARM generic timer's CNTFRQ_EL0, which QEMU's `virt` machine
 * reports as 62_500_000 Hz (62.5 MHz). Per the task spec we convert PMU cycles
 * to wall-clock time assuming CPU frequency == 62.5 MHz (i.e. 16 ns / cycle).
 *
 * IMPORTANT HONESTY NOTE: under QEMU/TCG, PMCCNTR_EL0 is an *emulated* cycle
 * counter — it does not correspond to real silicon cycles, and empirically it
 * advances at ~1 GHz relative to wall time on this host rather than 62.5 MHz.
 * The cycle *counts* below are real measured values from the running emulator;
 * the nanosecond figures are a nominal conversion under the 62.5 MHz assumption
 * and should be read as relative, not as hardware-accurate latencies.
 *
 * All measurements run at EL1 (invoked through SYS_BENCH) because PMCCNTR_EL0
 * is not enabled for EL0.
 * ------------------------------------------------------------------------- */

#define BENCH_CPU_HZ 62500000ULL /* assumed CPU freq (== CNTFRQ on QEMU virt) */

/* Shared sample buffer (BENCH_ITERS * 8 = 8 KiB). File-scope, not on the
 * 16 KiB kernel stack, and reused across the three benchmarks (each runs to
 * completion before the next starts). */
static uint64_t g_samples[BENCH_ITERS];

/* ---- IRQ-latency sampling state (written from timer_handle_irq) ---------- */
static volatile int      g_irq_armed = 0;
static volatile uint32_t g_irq_idx   = 0;
static uint64_t          g_irq_samples[BENCH_ITERS];

int bench_irq_sampling(void) { return g_irq_armed; }

void bench_irq_sample(uint64_t latency_ticks) {
  if (!g_irq_armed) {
    return;
  }
  uint32_t i = g_irq_idx;
  if (i < BENCH_ITERS) {
    g_irq_samples[i] = latency_ticks;
    g_irq_idx = i + 1;
  }
}

/* ---- statistics ---------------------------------------------------------- */

typedef struct {
  uint64_t min, max, mean, p50, p99;
} bench_stats_t;

/* In-place ascending sort. Shell sort — O(n^1.3)-ish, no recursion, no libc.
 * n = 1000 runs once per benchmark, comfortably fast even in TCG. */
static void bench_sort(uint64_t *a, uint32_t n) {
  for (uint32_t gap = n / 2; gap > 0; gap /= 2) {
    for (uint32_t i = gap; i < n; i++) {
      uint64_t tmp = a[i];
      uint32_t j = i;
      while (j >= gap && a[j - gap] > tmp) {
        a[j] = a[j - gap];
        j -= gap;
      }
      a[j] = tmp;
    }
  }
}

/* Computes stats and SORTS `a` in place (caller must not need original order). */
static bench_stats_t bench_compute(uint64_t *a, uint32_t n) {
  bench_stats_t s = {0, 0, 0, 0, 0};
  if (n == 0) {
    return s;
  }
  uint64_t sum = 0;
  for (uint32_t i = 0; i < n; i++) {
    sum += a[i];
  }
  bench_sort(a, n);
  s.min  = a[0];
  s.max  = a[n - 1];
  s.mean = sum / n;
  s.p50  = a[(n * 50) / 100];
  s.p99  = a[(n * 99) / 100];
  return s;
}

/* cycles -> nanoseconds under the 62.5 MHz assumption. */
static uint64_t cyc_to_ns(uint64_t cycles) {
  return (cycles * 1000000000ULL) / BENCH_CPU_HZ;
}

static void bench_report(const char *name, bench_stats_t s) {
  /* Exact format required by the spec (cycles line). */
  uart_printf("[BENCH] %s min=%u max=%u mean=%u p50=%u p99=%u cycles\n",
              name, s.min, s.max, s.mean, s.p50, s.p99);
  /* Derived nanoseconds (nominal, 62.5 MHz assumption). */
  uart_printf("[BENCH] %s min=%u max=%u mean=%u p50=%u p99=%u ns @62.5MHz\n",
              name, cyc_to_ns(s.min), cyc_to_ns(s.max), cyc_to_ns(s.mean),
              cyc_to_ns(s.p50), cyc_to_ns(s.p99));
}

/* ---- 1. syscall latency -------------------------------------------------- */
/* Round-trip cost of a null syscall: EL1 `svc #0` -> vector -> full trap-frame
 * save -> exception_dispatch -> syscall_dispatch(SYS_GETPID) -> trap-frame
 * restore -> eret. SYS_GETPID is chosen because it touches no fd table and has
 * no side effects; it just returns current->pid. */
static inline void bench_null_syscall(void) {
  register uint64_t x8 __asm__("x8") = SYS_GETPID;
  register uint64_t x0 __asm__("x0");
  __asm__ __volatile__("svc #0" : "=r"(x0) : "r"(x8) : "memory");
}

static void bench_syscall(void) {
  for (uint32_t i = 0; i < BENCH_ITERS; i++) {
    uint64_t t0 = cpu_read_cycles();
    bench_null_syscall();
    uint64_t t1 = cpu_read_cycles();
    g_samples[i] = t1 - t0;
  }
  bench_report("syscall_latency ", bench_compute(g_samples, BENCH_ITERS));
}

/* ---- 2. context switch --------------------------------------------------- */
/* A partner kernel task ping-pongs yields with the benchmark task. One yield()
 * from our side performs a full round trip: (us -> partner) then (partner ->
 * us) = TWO context switches plus two schedule() run-queue traversals. We
 * report the round-trip figure and, in bench_run's summary, the per-switch
 * cost (round-trip / 2). IRQs are masked around each measured yield so the
 * timer cannot preempt mid-switch; cooperative yielding still works fine. */
static volatile int g_pp_run = 0;

static void bench_pp_task(void) {
  while (g_pp_run) {
    yield();
  }
  /* Returning drops into kernel_task_trampoline -> task_exit(). */
}

static void bench_ctxsw(void) {
  g_pp_run = 1;
  sched_create_kernel_task("bench_pp", bench_pp_task);

  /* Warm-up: hand control to the partner once so it runs its trampoline and
   * parks inside its own yield() before we start timing. */
  yield();

  for (uint32_t i = 0; i < BENCH_ITERS; i++) {
    __asm__ __volatile__("msr daifset, #2" ::: "memory"); /* mask IRQ */
    uint64_t t0 = cpu_read_cycles();
    yield();
    uint64_t t1 = cpu_read_cycles();
    __asm__ __volatile__("msr daifclr, #2" ::: "memory"); /* unmask IRQ */
    g_samples[i] = t1 - t0;
  }

  /* Tear the partner down. */
  g_pp_run = 0;
  yield(); /* let it observe the flag and exit */

  bench_report("context_switch  ", bench_compute(g_samples, BENCH_ITERS));
}

/* ---- 3. IRQ latency ------------------------------------------------------ */
/* Measured inside timer_handle_irq as (CNTPCT_now - fired CNTP_CVAL), i.e. the
 * ticks between the timer deadline crossing and the handler reading the clock
 * — the hardware/emulator interrupt-response latency. Reported in timer ticks
 * AND, since we assume CPU freq == CNTFRQ (62.5 MHz), the tick count equals the
 * equivalent cycle count 1:1. We speed the tick up temporarily so 1000 samples
 * take ~0.25 s rather than 10 s. */
static void bench_irq_noop_tick(void) { /* silences the default periodic log */ }

static void bench_irq(void) {
  uint64_t freq    = timer_get_frequency();          /* 62.5 MHz on QEMU */
  uint64_t old_iv  = timer_get_interval_ticks();
  uint64_t fast_iv = freq / 1000;                    /* 1 ms tick */
  if (fast_iv < 1000) {
    fast_iv = 1000;
  }

  /* Install a no-op tick callback: the default timer path calls uart_printf
   * every 100 ticks, and that (polled, ~hundreds of us) print runs INSIDE the
   * handler, pushing the next absolute deadline into the past and manufacturing
   * a fake latency spike. Silencing it isolates true interrupt-response time.
   * A 1 ms interval is comfortably longer than the handler so deadlines never
   * pile up. */
  timer_set_callback(bench_irq_noop_tick);
  g_irq_idx   = 0;
  g_irq_armed = 1;
  timer_set_interval_ticks(fast_iv);

  /* IRQs are already unmasked in the syscall path; make sure, then busy-spin
   * until the sampler fills up. We deliberately do NOT use WFI here: under
   * -icount, WFI warps virtual time and smears the deadline→entry delta into a
   * beat pattern. Busy-spinning keeps the core genuinely executing so each
   * timer IRQ is taken mid-instruction-stream and the sample reflects the pure
   * vector-entry cost. schedule() is suppressed during sampling (see
   * exception.c) so no other task steals the core. */
  __asm__ __volatile__("msr daifclr, #2" ::: "memory");
  while (g_irq_idx < BENCH_ITERS) {
    __asm__ __volatile__("" ::: "memory");
  }
  g_irq_armed = 0;
  timer_set_interval_ticks(old_iv);
  timer_set_callback(0); /* restore default (no callback) tick behaviour */

  /* Timer ticks == cycles under the 62.5 MHz == CNTFRQ assumption. */
  bench_report("irq_latency     ", bench_compute(g_irq_samples, BENCH_ITERS));
}

/* ---- 4 & 5. signal delivery / sigreturn ---------------------------------
 * Kernel-internal microbenchmarks (no full EL0 round trip). We drive the real
 * signal machinery on a synthetic 688-byte trap frame with SP_EL0 pointed at
 * unused scratch space inside the current (shell) task's mapped user stack, so
 * the frame writes land in genuinely-mapped memory without disturbing the live
 * stack. The current task's signal state is saved and restored around the run.
 *
 * signal_delivery: cycles for signal_check_and_deliver() to build the signal
 * frame and retarget the trap frame at the handler (sig_pending set -> ready to
 * eret into the handler's first instruction).
 * sigreturn_latency: cycles for signal_sigreturn() to restore the interrupted
 * context from the signal frame. */

/* Full-size backing store for a synthetic trap frame (688 bytes -> 86 u64;
 * padded). SP_EL0 lives at index 35, past the 280-byte C struct, so a real
 * 688-byte buffer is required. */
static uint64_t g_fakeframe[88];

#define BENCH_SIG_NO      10                        /* arbitrary catchable sig */
#define BENCH_SCRATCH_SP  (USER_STACK_TOP - 0x1000) /* 4 KiB below stack top */

static void bench_signal(void) {
  task_t *cur = sched_current();
  /* Save + neutralize the caller's real signal state. */
  uint32_t save_pending = cur->sig_pending;
  uint32_t save_blocked = cur->sig_blocked;
  uint64_t save_hnd     = cur->sig_handlers[BENCH_SIG_NO];
  cur->sig_blocked                 = 0;
  cur->sig_handlers[BENCH_SIG_NO]  = USER_TEXT_BASE; /* a real (mapped) handler */

  trap_frame_t *tf = (trap_frame_t *)g_fakeframe;

  /* --- signal_delivery --- */
  for (uint32_t i = 0; i < BENCH_ITERS; i++) {
    memset(g_fakeframe, 0, sizeof(g_fakeframe));
    tf->spsr = 0;                              /* EL0 return */
    tf->elr  = USER_TEXT_BASE;
    ((uint64_t *)tf)[35] = BENCH_SCRATCH_SP;   /* SP_EL0 */
    cur->sig_pending = (1u << BENCH_SIG_NO);

    uint64_t t0 = cpu_read_cycles();
    signal_check_and_deliver(cur, tf);
    uint64_t t1 = cpu_read_cycles();
    g_samples[i] = t1 - t0;
  }
  bench_report("signal_delivery ", bench_compute(g_samples, BENCH_ITERS));

  /* --- sigreturn_latency --- Build one valid signal frame in scratch memory,
   * then time signal_sigreturn() restoring from it repeatedly (it only reads
   * the frame, so one setup suffices). */
  sigframe_t *sf = (sigframe_t *)BENCH_SCRATCH_SP;
  for (int j = 0; j < 31; j++) {
    sf->regs[j] = (uint64_t)j;
  }
  sf->sp_el0     = USER_STACK_TOP - 0x800;
  sf->elr        = USER_TEXT_BASE + 0x40;
  sf->spsr       = 0;
  sf->trampoline = USER_SIGTRAMP_VA;

  for (uint32_t i = 0; i < BENCH_ITERS; i++) {
    memset(g_fakeframe, 0, sizeof(g_fakeframe));
    ((uint64_t *)tf)[35] = BENCH_SCRATCH_SP;   /* SP_EL0 -> signal frame */

    uint64_t t0 = cpu_read_cycles();
    signal_sigreturn(tf);
    uint64_t t1 = cpu_read_cycles();
    g_samples[i] = t1 - t0;
  }
  bench_report("sigreturn_latency", bench_compute(g_samples, BENCH_ITERS));

  /* Restore the caller's real signal state. */
  cur->sig_pending                = save_pending;
  cur->sig_blocked                = save_blocked;
  cur->sig_handlers[BENCH_SIG_NO] = save_hnd;
}

/* ---- 6. UART framing / CRC ---------------------------------------------
 * Pure-computation benchmarks (no UART TX): frame encode = build + CRC-16 +
 * byte-stuffing into a buffer; crc16 over 64 and 256 bytes. */
static uint8_t g_framebuf[FRAMING_MAX_ENCODED];
static uint8_t g_crcbuf[256];

static void bench_uart(void) {
  for (uint32_t i = 0; i < 256; i++) {
    g_crcbuf[i] = (uint8_t)i;
  }

  /* uart_frame_encode: DATA frame with a 64-byte payload. */
  for (uint32_t i = 0; i < BENCH_ITERS; i++) {
    uint64_t t0 = cpu_read_cycles();
    framing_encode(g_framebuf, FRAME_TYPE_DATA, (uint8_t)i, g_crcbuf, 64);
    uint64_t t1 = cpu_read_cycles();
    g_samples[i] = t1 - t0;
  }
  bench_report("uart_frame_encode", bench_compute(g_samples, BENCH_ITERS));

  /* crc16 over 64 bytes. */
  for (uint32_t i = 0; i < BENCH_ITERS; i++) {
    uint64_t t0 = cpu_read_cycles();
    volatile uint16_t c = crc16_ccitt(g_crcbuf, 64);
    uint64_t t1 = cpu_read_cycles();
    (void)c;
    g_samples[i] = t1 - t0;
  }
  bench_report("crc16_64b        ", bench_compute(g_samples, BENCH_ITERS));

  /* crc16 over 256 bytes. */
  for (uint32_t i = 0; i < BENCH_ITERS; i++) {
    uint64_t t0 = cpu_read_cycles();
    volatile uint16_t c = crc16_ccitt(g_crcbuf, 256);
    uint64_t t1 = cpu_read_cycles();
    (void)c;
    g_samples[i] = t1 - t0;
  }
  bench_report("crc16_256b       ", bench_compute(g_samples, BENCH_ITERS));
}

/* ---- entry --------------------------------------------------------------- */
void bench_run(void) {
  uart_printf("[BENCH] PMU harness: %u iterations/benchmark, "
              "CPU freq assumed %u Hz (62.5 MHz == CNTFRQ)\n",
              (uint64_t)BENCH_ITERS, BENCH_CPU_HZ);
  uart_printf("[BENCH] note: context_switch is a round trip = 2 switches; "
              "per-switch = value/2\n");

  bench_syscall();
  bench_ctxsw();
  bench_irq();
  bench_signal();
  bench_uart();

  uart_printf("[BENCH] done\n");
}
