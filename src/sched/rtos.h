#ifndef SCHED_RTOS_H
#define SCHED_RTOS_H

#include "sched.h"
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * rtos.h — real-time scheduling on top of the fixed-priority scheduler.
 *
 * Phase 1: periodic tasks with deadlines, response-time / WCRT / deadline-miss
 *          tracking (timed with the CNTPCT counter).
 * Phase 2: rt_mutex / rt_sem blocking primitives with priority inheritance.
 * Phase 3: high-resolution (sub-tick) periodic release via a dynamic one-shot
 *          timer (see timer.c).
 * ------------------------------------------------------------------------- */

/* Create a periodic RT task at fixed `priority` (higher = more urgent). The
 * task body should be `while (1) { work(); rt_wait_next_period(); }`. `period_us`
 * and `deadline_us` are microseconds (deadline 0 => deadline == period). In
 * Phase 1 the release granularity is the scheduler tick; Phase 3 releases at
 * the exact CNTPCT deadline. Returns pid or -1. */
int rt_create_periodic(const char *name, task_entry_t entry, uint8_t priority,
                       uint64_t period_us, uint64_t deadline_us);

/* Called by a periodic task at the end of each job: records the job's response
 * time (release -> completion), updates WCRT, flags a deadline miss, then parks
 * the task until its next release. */
void rt_wait_next_period(void);

/* Release any periodic tasks whose deadline has arrived. Called from the timer
 * IRQ. Returns the CNTPCT of the earliest still-pending release (or 0 if none),
 * which the Phase-3 dynamic timer uses to program its next one-shot. */
uint64_t rt_tick_release(void);

/* Render a human-readable table of per-task RT stats into buf. Returns bytes
 * written. Used by the shell `rt` command. */
int rt_render_stats(char *buf, int len);

/* Spawn the built-in demo: three periodic tasks at different priorities/periods
 * (rt_hi 100ms/prio30, rt_mid 200ms/prio20, rt_lo 500ms/prio12). Idempotent. */
void rt_demo_start(void);

/* Run the priority-inversion demo: a low-priority task takes a mutex, a
 * high-priority task then blocks on it (triggering priority inheritance), while
 * a medium-priority task is runnable. Logs the boost. Idempotent. */
void rt_pi_demo_start(void);

/* ---- Phase 2: blocking primitives with priority inheritance ------------ */

typedef struct rt_mutex {
  task_t *owner;    /* current holder (NULL if free)                   */
  task_t *waiters;  /* wait-queue head, linked by task->wait_next      */
  int locked;
} rt_mutex_t;

typedef struct rt_sem {
  int count;
  task_t *waiters;
} rt_sem_t;

void rt_mutex_init(rt_mutex_t *m);
void rt_mutex_lock(rt_mutex_t *m);   /* blocks; boosts owner on contention */
void rt_mutex_unlock(rt_mutex_t *m); /* hands off to highest-priority waiter */

void rt_sem_init(rt_sem_t *s, int initial);
void rt_sem_wait(rt_sem_t *s);
void rt_sem_post(rt_sem_t *s);

#endif
