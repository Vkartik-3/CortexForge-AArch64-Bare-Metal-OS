#include "devices.h"
#include "blk/blk.h"
#include "rng/rng.h"
#include "console/console.h"
#include "uart/uart.h"
#include "uart/framing.h"
#include "utils/utils.h"
#include "vfs/vfs.h"
#include "strings/strings.h" // IWYU pragma: keep
#include "mm/mmu/mmu.h"       // USER_STACK_TOP (ioctl pointer range check)

#define SECTOR 512

/* ---- /dev/uart ---- */
static int console_read(vnode_t *n, file_t *f, void *buf, size_t count) {
  (void)n;
  (void)f;
  unsigned char *p = buf;

  for (size_t i = 0; i < count; i++) {
    p[i] = uart_getc();
  }

  return (int)count;
}

static int console_write(vnode_t *n, file_t *f, const void *buf, size_t count) {
  (void)n;
  (void)f;
  const char *p = buf;

  for (size_t i = 0; i < count; i++) {
    uart_putc(p[i]);
  }

  return (int)count;
}

static file_operations_t console_ops = {
    .read = console_read,
    .write = console_write,
};

/* ---- /dev/null ---- */
static int null_read(vnode_t *n, file_t *f, void *buf, size_t count) {
  (void)n;
  (void)f;
  (void)buf;
  (void)count;

  return 0; /* always EOF */
}

static int null_write(vnode_t *n, file_t *f, const void *buf, size_t count) {
  (void)n;
  (void)f;
  (void)buf;

  return (int)count; /* always accept, discard */
}

static file_operations_t null_ops = {
    .read = null_read,
    .write = null_write,
};

/* ---- /dev/zero ---- */
static int zero_read(vnode_t *n, file_t *f, void *buf, size_t count) {
  (void)n;
  (void)f;
  unsigned char *p = buf;

  for (size_t i = 0; i < count; i++) {
    p[i] = 0;
  }

  return (int)count;
}

static int zero_write(vnode_t *n, file_t *f, const void *buf, size_t count) {
  (void)n;
  (void)f;
  (void)buf;
  return (int)count; /* accept and discard */
}

static file_operations_t zero_ops = {
    .read = zero_read,
    .write = zero_write,
};

/* ---- /dev/rng ---- */
static int rng_dev_read(vnode_t *n, file_t *f, void *buf, size_t count) {
  (void)n;
  (void)f;

  return rng_read(buf, (uint32_t)count);
}

static file_operations_t rng_ops = {
    .read = rng_dev_read,
    .write = NULL,
};

/* ---- /dev/blk (raw block device) ----
 * Byte-granular offsets but reads/writes must be sector-aligned:
 *   - f->offset % 512 == 0
 *   - count      % 512 == 0
 * Returns number of bytes transferred, -1 on error.
 */

static int blk_dev_read(vnode_t *n, file_t *f, void *buf, size_t count) {
  (void)n;

  if ((f->offset % SECTOR) != 0 || (count % SECTOR) != 0) {
    return -1;
  }

  size_t sectors = count / SECTOR;
  uint64_t sector = (uint64_t)f->offset / SECTOR;
  uint8_t *p = buf;

  for (size_t i = 0; i < sectors; i++) {
    if (blk_read(sector + i, p + i * SECTOR) != ESUCCESS) {
      return -1;
    }
  }

  f->offset += (int64_t)count;
  return (int)count;
}

static int blk_dev_write(vnode_t *n, file_t *f, const void *buf, size_t count) {
  (void)n;
  if ((f->offset % SECTOR) != 0 || (count % SECTOR) != 0) {
    return -1;
  }

  size_t sectors = count / SECTOR;
  uint64_t sector = (uint64_t)f->offset / SECTOR;
  const uint8_t *p = buf;

  for (size_t i = 0; i < sectors; i++) {
    if (blk_write(sector + i, p + i * SECTOR) != ESUCCESS) {
      return -1;
    }
  }

  f->offset += (int64_t)count;
  return (int)count;
}

static file_operations_t blk_ops = {
    .read = blk_dev_read,
    .write = blk_dev_write,
};

/* ---- /dev/vcons ---- virtio-console TX side-channel.
 * Anything written here lands on the host file wired up by
 * `-chardev file,id=vc,path=$(BUILD_DIR)/virtio-console.txt` in QEMU.
 * Reads always return 0 (EOF) — we don't post RX buffers yet. */
static int vcons_read(vnode_t *n, file_t *f, void *buf, size_t count) {
  (void)n;
  (void)f;
  (void)buf;
  (void)count;
  return 0;
}

static int vcons_write(vnode_t *n, file_t *f, const void *buf, size_t count) {
  (void)n;
  (void)f;
  int n2 = vcons_send(buf, (uint32_t)count);
  return (n2 < 0) ? -1 : n2;
}

static file_operations_t vcons_ops = {
    .read = vcons_read,
    .write = vcons_write,
};


/* ---- /dev/uart0 ---- reliable framing protocol over the secondary UART.
 * read()  blocks for one DATA frame, ACKs it, and returns the payload.
 * write() sends the buffer as a DATA frame with an auto-incrementing seq and
 *         waits for the peer's ACK (retransmitting on timeout/NACK).
 * On a CRC error the reader sends a NACK so the peer retransmits.
 * NOTE: /dev/uart0 lives on UART1 (0x09040000), physically separate from
 * /dev/console on UART0, so framed bytes never mix with console output. */
static uint8_t g_uart0_tx_seq = 0;

static int uart0_read(vnode_t *n, file_t *f, void *buf, size_t count) {
  (void)n;
  (void)f;
  framing_ensure_hw();
  if (!framing_available()) {
    return -1; /* UART1 not provided by this platform/QEMU */
  }
  frame_t fr;
  int r = framing_recv(&fr, framing_get_timeout());
  if (r == FRAMING_OK && fr.type == FRAME_TYPE_DATA) {
    framing_send_ack(fr.seq);
    uint16_t n2 = fr.len;
    if (n2 > count) n2 = (uint16_t)count;
    memcpy(buf, fr.payload, n2);
    return (int)n2;
  }
  if (r == FRAMING_ERR_CRC) {
    framing_send_nack(fr.seq); /* ask the peer to retransmit */
    return -1;
  }
  return -1; /* timeout or unexpected control frame */
}

static int uart0_write(vnode_t *n, file_t *f, const void *buf, size_t count) {
  (void)n;
  (void)f;
  framing_ensure_hw();
  if (!framing_available()) {
    return -1; /* UART1 not provided by this platform/QEMU */
  }
  uint16_t len = (count > FRAMING_MAX_PAYLOAD) ? FRAMING_MAX_PAYLOAD
                                               : (uint16_t)count;
  int r = framing_send_reliable(g_uart0_tx_seq, buf, len);
  g_uart0_tx_seq++; /* auto-increment per frame (wraps at 256) */
  return (r == FRAMING_OK) ? (int)len : -1;
}

static int uart0_ioctl(vnode_t *n, file_t *f, uint64_t cmd, uint64_t arg) {
  (void)n;
  (void)f;
  switch (cmd) {
  case UART_IOCTL_SET_TIMEOUT:
    framing_set_timeout(arg);
    return 0;
  case UART_IOCTL_GET_STATS: {
    /* arg is a user pointer to a framing_stats_t; range-check it. */
    if (arg == 0 || arg + sizeof(framing_stats_t) > USER_STACK_TOP ||
        arg + sizeof(framing_stats_t) < arg) {
      return -1;
    }
    framing_stats_t st;
    framing_get_stats(&st);
    memcpy((void *)arg, &st, sizeof(st));
    return 0;
  }
  default:
    return -1;
  }
}

static file_operations_t uart0_ops = {
    .read = uart0_read,
    .write = uart0_write,
    .ioctl = uart0_ioctl,
};

/* ---- Entry point ---- */
void devices_register(void) {
  vfs_register_chardev("console", &console_ops);
  vfs_register_chardev("null", &null_ops);
  vfs_register_chardev("zero", &zero_ops);
  vfs_register_chardev("rng", &rng_ops);
  vfs_register_chardev("vcons", &vcons_ops);
  vfs_register_chardev("uart0", &uart0_ops);
  vfs_register_blockdev("blk", &blk_ops);
}
