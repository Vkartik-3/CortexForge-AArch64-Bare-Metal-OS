#ifndef LIB_UART_FRAMING_H
#define LIB_UART_FRAMING_H

#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * framing.h — reliable framing protocol over the raw UART1 byte stream.
 *
 * Wire format (big-endian LENGTH and CRC):
 *   START(1)=0xAA | MSG_TYPE(1) | SEQ(1) | LENGTH(2) | PAYLOAD(N) | CRC16(2)
 * CRC-16/CCITT (poly 0x1021, init 0xFFFF) is computed over
 *   MSG_TYPE, SEQ, LENGTH, PAYLOAD  (i.e. everything except START and the CRC).
 *
 * Byte stuffing: the START byte 0xAA is the unique frame delimiter, so any
 * 0xAA in the *body* (type/seq/length/payload/crc — the whole frame after the
 * START) is escaped on the wire as 0xAA 0x00. The receiver un-stuffs before
 * parsing / CRC checking. (The spec only required stuffing the payload; we
 * stuff the entire body so a 0xAA landing in seq/length/crc can't be mistaken
 * for a delimiter — a strict superset, and the Python client matches it.)
 * ------------------------------------------------------------------------- */

#define FRAME_START       0xAA
#define FRAME_TYPE_DATA   0x01
#define FRAME_TYPE_ACK    0x02
#define FRAME_TYPE_NACK   0x03

#define FRAMING_MAX_PAYLOAD 256
/* Worst-case encoded size: START + every body/CRC byte stuffed (doubled).
 * 1 + (4 header + 256 payload + 2 CRC) * 2 = 525. */
#define FRAMING_MAX_ENCODED 525
#define FRAMING_MAX_RETRIES 3    /* retransmit attempts after the first send */
#define FRAMING_DEFAULT_TIMEOUT_TICKS 100 /* ~1 s at TIMER_INTERVAL_MS = 10 ms */

/* /dev/uart0 ioctl commands (mirror in user/include/sys.h). */
#define UART_IOCTL_SET_TIMEOUT 0x01 /* arg = frame timeout in timer ticks     */
#define UART_IOCTL_GET_STATS   0x02 /* arg = user ptr to framing_stats_t       */

/* Return / error codes. */
#define FRAMING_OK           0
#define FRAMING_ERR_TIMEOUT (-1)
#define FRAMING_ERR_CRC     (-2)
#define FRAMING_ERR_BADLEN  (-3)

typedef struct frame {
  uint8_t  type;
  uint8_t  seq;
  uint16_t len;
  uint8_t  payload[FRAMING_MAX_PAYLOAD];
} frame_t;

/* Layout MUST match the user-side mirror in user/include/sys.h. */
typedef struct framing_stats {
  uint32_t frames_sent;
  uint32_t frames_recv;
  uint32_t retransmits;
  uint32_t crc_errors;
} framing_stats_t;

/* CRC-16/CCITT (poly 0x1021, init 0xFFFF, no final XOR, MSB-first). */
uint16_t crc16_ccitt(const uint8_t *data, size_t len);

void framing_init(void);

/* Encode a complete stuffed frame (START..CRC) into `out` (>= FRAMING_MAX_ENCODED
 * bytes) WITHOUT transmitting. Returns the encoded length, or -1 on bad len.
 * Pure computation (build + CRC-16 + byte-stuffing) — used by framing_send and
 * by the benchmark harness (which must not touch the UART FIFO). */
int framing_encode(uint8_t *out, uint8_t type, uint8_t seq,
                   const uint8_t *payload, uint16_t len);

/* Build one frame and transmit it (with byte stuffing) over UART1. */
int framing_send(uint8_t type, uint8_t seq, const uint8_t *payload,
                 uint16_t len);

/* Assemble one frame from UART1, un-stuffing and validating the CRC. Blocks
 * (cooperatively yielding) until a complete frame arrives or `timeout_ticks`
 * timer ticks elapse. Returns FRAMING_OK, FRAMING_ERR_TIMEOUT, FRAMING_ERR_CRC
 * (with out->seq/type filled best-effort), or FRAMING_ERR_BADLEN. */
int framing_recv(frame_t *out, uint64_t timeout_ticks);

int framing_send_ack(uint8_t seq);
int framing_send_nack(uint8_t seq);

/* Reliable DATA send: transmit, wait for a matching ACK, retransmit up to
 * FRAMING_MAX_RETRIES on timeout, retransmit immediately on NACK. Returns
 * FRAMING_OK once ACKed, or FRAMING_ERR_TIMEOUT after the retries are spent. */
int framing_send_reliable(uint8_t seq, const uint8_t *payload, uint16_t len);

void framing_set_timeout(uint64_t ticks);
uint64_t framing_get_timeout(void);
void framing_get_stats(framing_stats_t *out);

#endif
