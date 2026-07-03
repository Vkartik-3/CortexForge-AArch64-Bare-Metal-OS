#include "framing.h"
#include "uart.h"
#include "exception.h"     /* trap_frame_t */
#include "gic/gic.h"
#include "mm/mmu/mmu.h"    /* PHYS_TO_VIRT */
#include "timer/timer.h"
#include "sched/sched.h"
#include "strings/strings.h" // IWYU pragma: keep

/* ---------------------------------------------------------------------------
 * framing.c — reliable UART framing (CRC-16/CCITT, ACK/NACK, retransmission).
 * Runs at EL1 (invoked from the /dev/uart0 char-device ops). TX uses
 * uart1_putc(); RX drains the interrupt-fed UART1 ring via uart1_getc_nonblock.
 * ------------------------------------------------------------------------- */

static framing_stats_t g_stats;
static uint64_t        g_timeout = FRAMING_DEFAULT_TIMEOUT_TICKS;

void framing_init(void) {
  memset(&g_stats, 0, sizeof(g_stats));
  g_timeout = FRAMING_DEFAULT_TIMEOUT_TICKS;
}

/* UART1 MMIO region base as seen through the kernel's upper-half device map. */
#define UART1_PHYS_BASE 0x09040000ULL
#define UART1_VA_BASE   (PHYS_TO_VIRT(UART1_PHYS_BASE))
#define UART1_FR_VA     (PHYS_TO_VIRT(UART1_PHYS_BASE + 0x18))

static int g_hw_ready       = 0; /* UART1 present + initialized */
static int g_hw_absent      = 0; /* probe faulted => no device  */
static volatile int g_probing        = 0; /* set while the probe load is live */
static volatile int g_probe_faulted  = 0;

int framing_available(void) { return g_hw_ready; }

int framing_probe_abort(struct trap_frame *frame) {
  if (!g_probing) {
    return 0;
  }
  /* Only swallow aborts whose FAR is inside the UART1 page. */
  if (((uint64_t)frame->far & ~0xFFFULL) != UART1_VA_BASE) {
    return 0;
  }
  g_probe_faulted = 1;
  frame->elr += 4; /* skip the faulting probe load and continue */
  return 1;
}

void framing_ensure_hw(void) {
  if (g_hw_ready || g_hw_absent) {
    return;
  }
  /* Fault-guarded presence probe: a single load from UART1_FR. If the region
   * is unbacked, it external-aborts and framing_probe_abort() skips the load
   * and sets g_probe_faulted (instead of the handler panicking). */
  g_probe_faulted = 0;
  g_probing = 1;
  __asm__ __volatile__("dsb sy" ::: "memory");
  volatile uint32_t probe;
  __asm__ __volatile__("ldr %w0, [%1]" : "=r"(probe) : "r"(UART1_FR_VA) : "memory");
  __asm__ __volatile__("dsb sy\n\tisb" ::: "memory");
  g_probing = 0;
  (void)probe;

  if (g_probe_faulted) {
    g_hw_absent = 1;
    uart_println("[UART1] not present at 0x09040000 (no device) — framing disabled");
    return;
  }

  uart1_init();                /* configure PL011 + unmask RX interrupt */
  gic_enable_irq(UART1_INTID); /* route INTID 40 to this CPU            */
  g_hw_ready = 1;
}

uint16_t crc16_ccitt(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (int b = 0; b < 8; b++) {
      if (crc & 0x8000) {
        crc = (uint16_t)((crc << 1) ^ 0x1021);
      } else {
        crc = (uint16_t)(crc << 1);
      }
    }
  }
  return crc;
}

/* ---- transmit ------------------------------------------------------------ */

int framing_encode(uint8_t *out, uint8_t type, uint8_t seq,
                   const uint8_t *payload, uint16_t len) {
  if (len > FRAMING_MAX_PAYLOAD) {
    return -1;
  }
  /* Body = [type, seq, len_hi, len_lo, payload...] — the CRC input. */
  uint8_t body[4 + FRAMING_MAX_PAYLOAD];
  body[0] = type;
  body[1] = seq;
  body[2] = (uint8_t)(len >> 8);
  body[3] = (uint8_t)(len & 0xFF);
  for (uint16_t i = 0; i < len; i++) {
    body[4 + i] = payload[i];
  }
  uint16_t crc = crc16_ccitt(body, (size_t)(4 + len));

  int k = 0;
  out[k++] = FRAME_START; /* delimiter — never stuffed */
  for (uint16_t i = 0; i < 4 + len; i++) {
    out[k++] = body[i];
    if (body[i] == FRAME_START) out[k++] = 0x00; /* byte stuff */
  }
  uint8_t ch = (uint8_t)(crc >> 8), cl = (uint8_t)(crc & 0xFF);
  out[k++] = ch; if (ch == FRAME_START) out[k++] = 0x00;
  out[k++] = cl; if (cl == FRAME_START) out[k++] = 0x00;
  return k;
}

int framing_send(uint8_t type, uint8_t seq, const uint8_t *payload,
                 uint16_t len) {
  uint8_t buf[FRAMING_MAX_ENCODED];
  int k = framing_encode(buf, type, seq, payload, len);
  if (k < 0) {
    return FRAMING_ERR_BADLEN;
  }
  for (int i = 0; i < k; i++) {
    uart1_putc(buf[i]);
  }
  g_stats.frames_sent++;
  return FRAMING_OK;
}

int framing_send_ack(uint8_t seq) {
  return framing_send(FRAME_TYPE_ACK, seq, NULL, 0);
}
int framing_send_nack(uint8_t seq) {
  return framing_send(FRAME_TYPE_NACK, seq, NULL, 0);
}

/* ---- receive ------------------------------------------------------------- */

/* Block for one byte from the UART1 ring until `deadline` (absolute tick).
 * Yields the CPU while waiting so other tasks (and the RX IRQ) make progress.
 * Returns 0..255 or -1 on timeout. */
static int recv_byte(uint64_t deadline) {
  for (;;) {
    int b = uart1_getc_nonblock();
    if (b >= 0) {
      return b;
    }
    if (timer_get_ticks() >= deadline) {
      return -1;
    }
    yield();
  }
}

/* Read one logical (un-stuffed) body byte.
 * Returns 0..255 = data byte; -1 = timeout; -2 = a new frame START was seen
 * mid-stream (its type byte is stored in *restart_type). */
static int read_logical(uint64_t deadline, uint8_t *restart_type) {
  int b = recv_byte(deadline);
  if (b < 0) {
    return -1;
  }
  if (b == FRAME_START) {
    int n = recv_byte(deadline);
    if (n < 0) {
      return -1;
    }
    if (n == 0x00) {
      return FRAME_START; /* stuffed literal 0xAA */
    }
    *restart_type = (uint8_t)n; /* 0xAA + non-zero => new frame START */
    return -2;
  }
  return b;
}

/* Scan the stream until a real frame START (0xAA followed by a valid type). */
static int sync_start(uint64_t deadline, uint8_t *type_out) {
  for (;;) {
    int b = recv_byte(deadline);
    if (b < 0) {
      return -1;
    }
    if (b != FRAME_START) {
      continue;
    }
    int t = recv_byte(deadline);
    if (t < 0) {
      return -1;
    }
    if (t == FRAME_TYPE_DATA || t == FRAME_TYPE_ACK || t == FRAME_TYPE_NACK) {
      *type_out = (uint8_t)t;
      return 0;
    }
    /* t == 0x00 (stuffed literal) or garbage: keep scanning. */
  }
}

int framing_recv(frame_t *out, uint64_t timeout_ticks) {
  uint64_t deadline = timer_get_ticks() + timeout_ticks;
  uint8_t  type;
  if (sync_start(deadline, &type) < 0) {
    return FRAMING_ERR_TIMEOUT;
  }

have_type:;
  uint8_t hdr[3]; /* seq, len_hi, len_lo */
  for (int i = 0; i < 3; i++) {
    uint8_t rt;
    int b = read_logical(deadline, &rt);
    if (b == -1) return FRAMING_ERR_TIMEOUT;
    if (b == -2) { type = rt; goto have_type; }
    hdr[i] = (uint8_t)b;
  }
  uint8_t  seq = hdr[0];
  uint16_t len = ((uint16_t)hdr[1] << 8) | hdr[2];
  if (len > FRAMING_MAX_PAYLOAD) {
    return FRAMING_ERR_BADLEN;
  }

  uint8_t payload[FRAMING_MAX_PAYLOAD];
  for (uint16_t i = 0; i < len; i++) {
    uint8_t rt;
    int b = read_logical(deadline, &rt);
    if (b == -1) return FRAMING_ERR_TIMEOUT;
    if (b == -2) { type = rt; goto have_type; }
    payload[i] = (uint8_t)b;
  }

  uint8_t crcb[2];
  for (int i = 0; i < 2; i++) {
    uint8_t rt;
    int b = read_logical(deadline, &rt);
    if (b == -1) return FRAMING_ERR_TIMEOUT;
    if (b == -2) { type = rt; goto have_type; }
    crcb[i] = (uint8_t)b;
  }
  uint16_t rx_crc = ((uint16_t)crcb[0] << 8) | crcb[1];

  /* Recompute CRC over [type, seq, len_hi, len_lo, payload]. */
  uint8_t crcbuf[4 + FRAMING_MAX_PAYLOAD];
  crcbuf[0] = type;
  crcbuf[1] = seq;
  crcbuf[2] = hdr[1];
  crcbuf[3] = hdr[2];
  for (uint16_t i = 0; i < len; i++) {
    crcbuf[4 + i] = payload[i];
  }
  uint16_t calc = crc16_ccitt(crcbuf, (size_t)(4 + len));

  out->type = type;
  out->seq  = seq;
  out->len  = len;
  memcpy(out->payload, payload, len);

  if (calc != rx_crc) {
    g_stats.crc_errors++;
    return FRAMING_ERR_CRC;
  }
  g_stats.frames_recv++;
  return FRAMING_OK;
}

/* ---- reliable send (ACK wait + retransmission) --------------------------- */

int framing_send_reliable(uint8_t seq, const uint8_t *payload, uint16_t len) {
  for (int attempt = 0; attempt <= FRAMING_MAX_RETRIES; attempt++) {
    if (attempt > 0) {
      g_stats.retransmits++;
    }
    framing_send(FRAME_TYPE_DATA, seq, payload, len);

    frame_t f;
    int r = framing_recv(&f, g_timeout);
    if (r == FRAMING_OK && f.type == FRAME_TYPE_ACK && f.seq == seq) {
      return FRAMING_OK; /* acknowledged */
    }
    if (r == FRAMING_OK && f.type == FRAME_TYPE_NACK && f.seq == seq) {
      continue; /* explicit NACK — retransmit immediately (no wait) */
    }
    /* timeout / CRC error / unexpected frame — retransmit on next iteration */
  }
  return FRAMING_ERR_TIMEOUT;
}

/* ---- config / stats ------------------------------------------------------ */

void framing_set_timeout(uint64_t ticks) {
  g_timeout = ticks ? ticks : FRAMING_DEFAULT_TIMEOUT_TICKS;
}
uint64_t framing_get_timeout(void) { return g_timeout; }

void framing_get_stats(framing_stats_t *out) { *out = g_stats; }

/* ---- in-memory decode + self-test (no hardware) -------------------------- */

int framing_decode(const uint8_t *buf, int len, frame_t *out) {
  if (len < 1 || buf[0] != FRAME_START) {
    return FRAMING_ERR_BADLEN;
  }
  /* Un-stuff the body+CRC into a linear buffer. */
  uint8_t lin[4 + FRAMING_MAX_PAYLOAD + 2];
  int lpos = 0;
  int pos = 1;
  while (pos < len && lpos < (int)sizeof(lin)) {
    uint8_t b = buf[pos++];
    if (b == FRAME_START) {
      if (pos >= len) {
        return FRAMING_ERR_BADLEN;
      }
      uint8_t n = buf[pos++];
      if (n == 0x00) {
        lin[lpos++] = FRAME_START; /* stuffed literal */
      } else {
        return FRAMING_ERR_BADLEN; /* unexpected delimiter mid-frame */
      }
    } else {
      lin[lpos++] = b;
    }
  }
  if (lpos < 6) {
    return FRAMING_ERR_BADLEN;
  }
  uint16_t plen = ((uint16_t)lin[2] << 8) | lin[3];
  if (plen > FRAMING_MAX_PAYLOAD || lpos != 4 + (int)plen + 2) {
    return FRAMING_ERR_BADLEN;
  }
  uint16_t rx_crc = ((uint16_t)lin[4 + plen] << 8) | lin[4 + plen + 1];
  uint16_t calc   = crc16_ccitt(lin, (size_t)(4 + plen));

  out->type = lin[0];
  out->seq  = lin[1];
  out->len  = plen;
  memcpy(out->payload, lin + 4, plen);
  return (calc == rx_crc) ? FRAMING_OK : FRAMING_ERR_CRC;
}

int framing_selftest(void) {
  /* Payload includes a 0xAA so the encode/decode exercises byte stuffing. */
  static const uint8_t pl[] = {'C', 'F', '-', 0xAA, 'f', 'r', 'a', 'm', 'e'};
  const int plen = (int)sizeof(pl);

  uint8_t enc[FRAMING_MAX_ENCODED];
  int k = framing_encode(enc, FRAME_TYPE_DATA, 0x2A, pl, (uint16_t)plen);
  if (k < 0) {
    return -1;
  }

  frame_t f;
  if (framing_decode(enc, k, &f) != FRAMING_OK) {
    return -1;
  }
  if (f.type != FRAME_TYPE_DATA || f.seq != 0x2A || f.len != plen) {
    return -1;
  }
  for (int i = 0; i < plen; i++) {
    if (f.payload[i] != pl[i]) {
      return -1;
    }
  }

  /* Corrupt the seq byte (enc[2]; type=0x01, seq=0x2A — neither is 0xAA, so no
   * stuffing shifts it). CRC covers seq, so decode must now report a CRC error. */
  enc[2] ^= 0xFF;
  if (framing_decode(enc, k, &f) != FRAMING_ERR_CRC) {
    return -1;
  }
  return 0;
}
