#ifndef LIB_BENCH_H
#define LIB_BENCH_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * bench.h — cycle-level microbenchmark harness built on the ARM PMU cycle
 * counter (PMCCNTR_EL0, exposed via cpu_read_cycles()).
 *
 * All three benchmarks MUST run at EL1 (kernel context): PMCCNTR_EL0 is not
 * readable from EL0 because PMUSERENR_EL0.EN is left at 0 (see cpu.c). The
 * `bench` shell built-in therefore traps into the kernel via SYS_BENCH and
 * the syscall dispatcher calls bench_run() with full EL1 privileges.
 *
 * Each benchmark collects BENCH_ITERS samples and reports min / max / mean /
 * p50 (median) / p99 in CPU cycles, plus a nanosecond figure derived from the
 * assumed CPU frequency (see bench.c for the frequency assumption).
 * ------------------------------------------------------------------------- */

#define BENCH_ITERS 1000

/* Run all benchmarks and print the [BENCH] result lines to the UART. */
void bench_run(void);

/* Timer-IRQ instrumentation hook. Called at the very top of timer_handle_irq()
 * with the measured "deadline → handler-entry" latency in CNTPCT ticks. It is
 * a cheap no-op (single flag test) unless IRQ-latency sampling is armed by
 * bench_run(). Declared here so timer.c can feed samples into the bench module
 * without pulling in the whole harness. */
void bench_irq_sample(uint64_t latency_ticks);

/* Non-zero while bench_run() is collecting IRQ-latency samples. The IRQ path
 * (exception.c) skips its post-interrupt schedule() while this is set so the
 * core stays idle (WFI) between ticks — otherwise a ready task would run
 * between ticks and every timer IRQ would land mid-execution, measuring
 * scheduler load rather than interrupt-response latency. */
int bench_irq_sampling(void);

#endif
