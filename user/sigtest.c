/*
 * user/sigtest.c — EL0 test for the POSIX signal subsystem.
 *
 * Registers a SIGALRM handler, arms a 2-second alarm, and loops printing
 * "waiting..." roughly every 500 ms. When the alarm fires the kernel delivers
 * SIGALRM on the return-to-EL0 path, the handler runs, prints a line, and sets
 * a global flag; the main loop then exits and reports PASS.
 *
 * Run it from the shell with:  ./sigtest   (or: exec /SIGTEST.ELF)
 */
#include "sys.h"

/* Written by the signal handler, polled by main. Volatile so the compiler
 * re-reads it each loop iteration rather than caching it in a register. */
static volatile int alarm_fired = 0;

static void on_sigalrm(int sig) {
  (void)sig;
  u_puts("SIGALRM received!\n");
  alarm_fired = 1;
}

int main(void) {
  u_puts("sigtest: registering SIGALRM handler\n");
  if (sys_sigaction(SIGALRM, on_sigalrm) < 0) {
    u_puts("sigtest: sigaction failed\n");
    return 1;
  }

  u_puts("sigtest: arming 2-second alarm\n");
  sys_alarm(2);

  /* Bounded loop (safety cap ~10 s) so a broken signal path can't hang CI. */
  int waited = 0;
  while (!alarm_fired && waited < 20) {
    u_puts("waiting...\n");
    sys_sleep(500);
    waited++;
  }

  if (alarm_fired) {
    u_puts("signal test PASS\n");
    return 0;
  }
  u_puts("signal test FAIL (no SIGALRM delivered)\n");
  return 1;
}
