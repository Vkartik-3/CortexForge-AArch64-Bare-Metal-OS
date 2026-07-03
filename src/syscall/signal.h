#ifndef SIGNAL_H
#define SIGNAL_H

#include "exception.h" /* trap_frame_t */
#include <stdint.h>

/* forward decl to avoid pulling all of sched.h into every includer */
struct task;

/* ---- Signal numbers (subset of POSIX) ---------------------------------- */
#define SIGHUP   1
#define SIGINT   2
#define SIGKILL  9  /* cannot be caught or ignored — always terminates */
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17

#define SIG_MIN  1
#define SIG_MAX 31  /* bits 1..31 of the 32-bit pending/blocked masks */

/* ---- Handler sentinels (stored in task->sig_handlers[N]) ---------------- */
#define SIG_DFL 0UL /* default action: terminate the task */
#define SIG_IGN 1UL /* ignore the signal */

/* ---- sigprocmask `how` values ------------------------------------------- */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* ---- Signal frame pushed onto the USER stack on delivery ----------------
 * Layout (grows downward from the interrupted SP_EL0). Saved so SYS_SIGRETURN
 * can restore the exact interrupted context. 35 * 8 = 280 bytes; delivery
 * reserves 288 (16-byte aligned) on the user stack. Must NOT use kernel heap. */
typedef struct sigframe {
  uint64_t regs[31];  /* x0 - x30 at interruption            */
  uint64_t sp_el0;    /* interrupted user stack pointer       */
  uint64_t elr;       /* interrupted PC (ELR_EL1)             */
  uint64_t spsr;      /* interrupted CPSR (SPSR_EL1)          */
  uint64_t trampoline;/* USER_SIGTRAMP_VA (informational)     */
} sigframe_t;

/* ---- Kernel-side entry points ------------------------------------------- */

/* Deliver at most one pending, unblocked signal to `t` before its trap frame
 * eret's back to EL0. Must only be called when `frame` returns to EL0 (SPSR
 * M[3:0] == 0). Builds a signal frame on the user stack, retargets `frame` at
 * the handler, and sets x30 = the sigreturn trampoline. SIG_DFL/SIGKILL
 * terminate the task (never returns); SIG_IGN is skipped. Returns 1 if a
 * handler was set up, 0 otherwise. */
int signal_check_and_deliver(struct task *t, trap_frame_t *frame);

/* SYS_SIGRETURN: restore the interrupted context from the signal frame at the
 * current SP_EL0 and resume. Rewrites `frame` in place; caller must not clobber
 * frame->regs[0] afterwards. */
void signal_sigreturn(trap_frame_t *frame);

/* Syscall backends (validated user pointers handled by the caller/here). */
int64_t signal_sigaction(struct task *t, int signum, uint64_t handler);
int64_t signal_sigprocmask(struct task *t, int how, uint64_t set_ptr,
                           uint64_t oldset_ptr);
int64_t signal_alarm(struct task *t, uint64_t seconds);

/* Called once per timer IRQ: decrement every task's SIGALRM countdown and set
 * SIGALRM pending on expiry. */
void signal_tick_alarms(void);

#endif
