/*
 * user/uartecho.c — EL0 echo server for the UART framing protocol.
 *
 * Opens /dev/uart0 (the reliable framing device on UART1), then loops:
 *   read a DATA frame's payload  ->  echo it straight back.
 * The kernel's /dev/uart0 read auto-ACKs the received frame; the write does a
 * reliable send (waits for the peer's ACK, retransmitting on timeout/NACK).
 *
 * Run from the shell with:  ./uartecho   (or: exec /UARTECHO.ELF)
 * Runs until killed.
 */
#include "sys.h"

/* Minimal unsigned-int -> decimal, returns chars written. */
static int uitoa(char *out, unsigned v) {
  char tmp[12];
  int n = 0;
  if (v == 0) { out[0] = '0'; return 1; }
  while (v && n < 12) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
  for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
  return n;
}

/* Print:  [UART] <tag>: "<payload>" (seq=<seq>, <n> bytes)\n  */
static void report(const char *tag, const char *p, int n, int seq) {
  char line[320];
  int k = 0;
  const char *pre = "[UART] ";
  while (*pre) line[k++] = *pre++;
  while (*tag) line[k++] = *tag++;
  line[k++] = '"';
  for (int i = 0; i < n && k < 290; i++) line[k++] = p[i];
  line[k++] = '"';
  const char *s = " (seq=";
  while (*s) line[k++] = *s++;
  k += uitoa(line + k, (unsigned)seq);
  const char *c = ", ";
  while (*c) line[k++] = *c++;
  k += uitoa(line + k, (unsigned)n);
  const char *e = " bytes)\n";
  while (*e) line[k++] = *e++;
  sys_write(STDOUT_FILENO, line, (size_t)k);
}

int main(void) {
  int fd = sys_open("/dev/uart0");
  if (fd < 0) {
    u_puts("uartecho: cannot open /dev/uart0\n");
    return 1;
  }
  u_puts("uartecho: echoing frames on /dev/uart0 (UART1). Ctrl-kill to stop.\n");

  char buf[256];
  /* seq mirrors the protocol: the host and the kernel TX counter both start
   * at 0 and increment in lockstep (one echo per receive), so this loop index
   * equals both the received and the sent sequence number. */
  int seq = 0;
  for (;;) {
    int n = sys_read(fd, buf, sizeof(buf));
    if (n <= 0) {
      /* Timeout, CRC error (already NACKed), or UART1 absent on this platform.
       * Brief pause so an absent-device -1 (which returns immediately) doesn't
       * spin the CPU; a normal idle timeout already blocked ~1 s. */
      sys_sleep(50);
      continue;
    }
    report("recv: ", buf, n, seq);

    int w = sys_write(fd, buf, (size_t)n);
    if (w < 0) {
      u_puts("[UART] echo send FAILED (no ACK after retries)\n");
    } else {
      report("sent: ", buf, n, seq);
    }
    seq++;
  }
  return 0;
}
