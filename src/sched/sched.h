#ifndef SCHED_H
#define SCHED_H

#include "syscall/elf.h"  /* elf_image_t for per-task region tracking */
#include <stdint.h>

/* forward decl */
struct fd_table;

#define TASK_STACK_PAGES 4 // 16 KiB per task (kernel stack)
#define USER_STACK_PAGES 4 // 16 KiB user stack
/* Hard cap for demand-paged user stack growth. The initial USER_STACK_PAGES
 * are eagerly mapped at task creation; faults in [USER_STACK_TOP - MAX*PAGE,
 * USER_STACK_TOP - INITIAL*PAGE) trigger sched_try_grow_stack() which maps
 * a fresh page on demand up to this cap. 64 pages = 256 KiB total stack,
 * matching a typical Linux RLIMIT_STACK of ~8 MiB scaled to a hobby-kernel
 * memory budget. */
#define USER_STACK_PAGES_MAX 64
#define USER_STACK_GROWN_MAX (USER_STACK_PAGES_MAX - USER_STACK_PAGES)

typedef enum {
  TASK_READY,
  TASK_RUNNING,
  TASK_SLEEPING,
  TASK_DEAD,
  TASK_BLOCKED   /* blocked on an rt_mutex / rt_sem (Phase 2) */
} task_state_t;

typedef void (*task_entry_t)(void);

/* Fixed-priority scheduling: HIGHER number = HIGHER priority (FreeRTOS-style).
 * The idle task is PRIO_IDLE (0); ordinary tasks default to PRIO_DEFAULT;
 * real-time periodic tasks pass an explicit priority up to PRIO_MAX. */
#define PRIO_IDLE    0
#define PRIO_DEFAULT 8
#define PRIO_MAX     255

typedef struct task {
  uint64_t sp; // offset 0: kernel SP (context_switch saves/restores here)
  uint64_t pid;
  task_state_t state;
  uint64_t sleep_until;
  uintptr_t stack_phys;
  // EL0 user-space fields
  uint64_t ttbr0;        // user page table base (physical address)
  uint64_t user_sp;      // SP_EL0 — user stack pointer
  uintptr_t kstack_top;  // top of per-task kernel stack (for exception entry)
  uintptr_t ustack_phys; // physical address of user stack (for freeing)
  /* If the task was loaded by exec(), exec_image holds the per-PT_LOAD
   * PMM allocations so sched_reap can free them all. For tasks running
   * the kernel-shared .text image (every task created via
   * sched_create_task), region_count is 0. */
  elf_image_t exec_image;
  char name[16];
  struct fd_table *fds;
  struct task *next;
  /* Demand-paged stack growth tracking. Each entry is the physical base
   * of a 4 KiB page that was lazily mapped into the user stack region by
   * sched_try_grow_stack(); sched_reap() walks the array to pmm_free them.
   * Placed at the end of task_t so the existing TASK_TTBR0=40 offset used
   * by switch.S stays put. */
  uintptr_t stack_grown_phys[USER_STACK_GROWN_MAX];
  uint16_t  stack_grown_count;

  /* ---- POSIX signal state (Extension 3) --------------------------------
   * Appended at the very END of task_t so the switch.S-hard-coded
   * TASK_TTBR0 = 40 offset (and every other assembly offset) is unaffected.
   * sig_handlers[N] holds the EL0 handler address for signal N, or the
   * sentinels SIG_DFL (0 = terminate) / SIG_IGN (1 = ignore). sig_pending
   * and sig_blocked are bitmasks indexed by signal number (bit N = signal
   * N; bit 0 unused). sig_alarm_ticks is a per-task SIGALRM countdown in
   * timer ticks (0 = disarmed), decremented once per timer IRQ. */
  uint64_t sig_handlers[32];
  uint32_t sig_pending;
  uint32_t sig_blocked;
  uint64_t sig_alarm_ticks;

  /* ---- RTOS scheduling (real-time extension) — appended at the END so the
   * switch.S TASK_SP=0 / TASK_TTBR0=40 offsets are unaffected.
   * priority is the EFFECTIVE priority the scheduler uses (may be temporarily
   * raised by priority inheritance); base_priority is the nominal value it is
   * restored to. Periodic-task timing uses the CNTPCT counter (rt_*_cyc). */
  uint8_t  priority;
  uint8_t  base_priority;
  uint64_t rt_period_cyc;     /* period in CNTPCT ticks (0 = not periodic)    */
  uint64_t rt_deadline_cyc;   /* relative deadline in CNTPCT ticks            */
  uint64_t rt_next_release;   /* absolute CNTPCT of the next release          */
  uint64_t rt_release_cyc;    /* CNTPCT at the current release (for response) */
  uint64_t rt_wcrt_cyc;       /* worst-case response time observed (CNTPCT)   */
  uint64_t rt_last_resp_cyc;  /* last job's response time (CNTPCT)            */
  uint32_t rt_activations;    /* number of releases                          */
  uint32_t rt_deadline_misses;
  struct task *wait_next;     /* link in an rt_mutex / rt_sem wait queue      */
  void *blocked_on;           /* rt_mutex_t* the task is blocked on (PI chains)*/
} task_t;

// switch.S
extern void context_switch(task_t *prev, task_t *next);

void sched_init(void);
int sched_create_task(const char *name, task_entry_t entry);

/* fork() implementation — deep-copies the calling task into a new task and
 * returns the child pid (called from SYS_FORK; the trap_frame_t pointer is
 * the parent's frame on its kstack). The child's frame is prepared so that
 * on its first schedule, fork_return ereturns to the same instruction the
 * parent will return to, with x0 = 0. */
struct trap_frame;
int sched_fork(task_t *parent, struct trap_frame *frame);
extern void fork_return(void);


/* Create an EL1 kernel-mode task. Unlike sched_create_task, no user_l0
 * is allocated and TTBR0 is left at 0 (so context_switch skips the
 * per-task user mapping swap). The entry function runs at EL1 with the
 * kernel's full privileges and is expected to call task_exit() or fall
 * off the end (kernel_task_trampoline calls task_exit if it returns). */
int sched_create_kernel_task(const char *name, task_entry_t entry);
void schedule(void);
void yield(void);
void task_exit(void);

/* Kill the task with the given pid. Marks it DEAD, unlinks it from the
 * run queue, and pushes it onto the reaper list. If pid refers to the
 * caller, falls through to task_exit(). Returns 0 on success, -1 if the
 * pid is not found, already dead, or refers to the idle task (pid 0). */
int sched_kill_task(uint64_t pid);

/* Look up a task by pid without altering the run queue. Returns NULL if no
 * such task exists. Used by the signal path (SYS_KILL) to set a pending
 * signal bit on the target. */
task_t *sched_find_task(uint64_t pid);
void sleep_ms(uint64_t ms);
void sched_wake_sleepers(void);
void sched_reap(void);
task_t *sched_current(void);

/* Iteration support for /proc/tasks. The run queue is circular and
 * always non-empty (idle is always present); walk t->next until you
 * loop back to the returned head. */
task_t *sched_first_task(void);
const char *task_state_name(task_state_t s);

/* Allocate a fresh ASID for a new user address space.
 *
 * Returns a 16-bit value in [1, 65535]. ASID 0 is reserved for the kernel
 * (idle / sched_create_kernel_task whose ttbr0 stays 0).
 *
 * On wraparound (every 65535 task creations) does a global TLB flush and
 * resets the counter so the recycled ASIDs are guaranteed not to alias
 * stale TLB entries. With the current hobby-kernel workloads we never
 * observe a wrap. */
uint16_t sched_asid_alloc(void);


/* Demand-page handler for the user stack. Called from the EL0 data-abort
 * path BEFORE the fatal kill. If `far` lies in the growth zone
 * [USER_STACK_TOP - USER_STACK_PAGES_MAX * PAGE, USER_STACK_TOP -
 * USER_STACK_PAGES * PAGE) and the task hasn't exhausted USER_STACK_GROWN_MAX,
 * allocates a fresh PMM page, zeros it, and maps it into t->ttbr0 at the
 * page containing far. Returns 1 on success (caller should resume the user
 * task), 0 on failure (caller should fall through to the fault dump). */
int sched_try_grow_stack(task_t *t, uint64_t far);

/* ---- RTOS scheduling helpers (real-time extension) --------------------- */

/* Create an EL1 kernel task with an explicit fixed priority (higher = more
 * urgent). Returns pid, or -1. Used by the periodic-task factory. */
int sched_create_rt_task(const char *name, task_entry_t entry, uint8_t priority);

/* Set a task's EFFECTIVE priority (base_priority is left untouched). Used by
 * priority inheritance to boost a mutex owner. */
void sched_set_priority(task_t *t, uint8_t prio);

/* Block the current task (TASK_BLOCKED) and switch away; returns once another
 * path makes it READY again. */
void sched_block_current(void);

/* Transition a blocked/sleeping task back to READY (does not switch). */
void sched_make_ready(task_t *t);

#endif
