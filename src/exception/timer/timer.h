#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

#define TIMER_PPI_INTID 30
#define TIMER_INTERVAL_MS 10 // 10ms tick for fine-grained sleep

// timer tick handler callback
typedef void (*timer_callback_t)(void);

void timer_init(void);
void timer_start(uint64_t interval_ms);
void timer_stop(void);
void timer_handle_irq(void);
void timer_set_callback(timer_callback_t cb);
uint64_t timer_get_frequency(void);
uint64_t timer_get_count(void);
uint64_t timer_get_ticks(void);
uint64_t timer_uptime_ms(void);
uint64_t timer_uptime_seconds(void);

/* Re-arm interval control, in raw timer ticks (CNTFRQ units). Used by the PMU
 * benchmark harness to temporarily speed the tick up so it can gather 1000
 * IRQ-latency samples in a fraction of a second instead of 10 s, then restore
 * the normal TIMER_INTERVAL_MS cadence. */
uint64_t timer_get_interval_ticks(void);
void timer_set_interval_ticks(uint64_t ticks);

#endif
