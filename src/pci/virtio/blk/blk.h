#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include "pci/pci.h"
#include "pci/virtio/virtio.h"
#include "pci/virtio/virtqueue.h"

/* Device config layout (virtio 1.2 §5.2.4). A field is only valid if its
 * corresponding feature bit was negotiated. */
#define VIRTIO_BLK_CFG_CAPACITY 0x00 /* u64: 512-byte sectors          */
#define VIRTIO_BLK_CFG_SIZE_MAX 0x08 /* u32: max bytes in one segment  */
#define VIRTIO_BLK_CFG_SEG_MAX 0x0C  /* u32: max segments in a request */
#define VIRTIO_BLK_CFG_BLK_SIZE 0x14 /* u32: optimal block size        */

#define VIRTIO_BLK_VENDOR_ID 0x1AF4
#define VIRTIO_BLK_DEVICE_ID 0x1042

#define VIRTIO_BLK_SECTOR_SIZE 512

/* Feature bits (virtio 1.2 §5.2.3). Bit positions, not masks. */
#define VIRTIO_BLK_F_SIZE_MAX 1 /* size_max in config is valid        */
#define VIRTIO_BLK_F_SEG_MAX 2  /* seg_max in config is valid         */
#define VIRTIO_BLK_F_GEOMETRY 4 /* geometry in config is valid        */
#define VIRTIO_BLK_F_RO 5       /* device is read-only                */
#define VIRTIO_BLK_F_BLK_SIZE 6 /* blk_size in config is valid        */
#define VIRTIO_BLK_F_FLUSH 9    /* device supports VIRTIO_BLK_T_FLUSH */
#define VIRTIO_F_VERSION_1 32   /* modern (non-legacy) virtio         */

/* Request Header Types */
#define VIRTIO_BLK_T_IN 0    /* read  */
#define VIRTIO_BLK_T_OUT 1   /* write */
#define VIRTIO_BLK_T_FLUSH 4 /* flush */

/* status byte values */
#define VIRTIO_BLK_S_OK 0
#define VIRTIO_BLK_S_IOERR 1
#define VIRTIO_BLK_S_UNSUPP 2

/* Fallbacks used when the device does not advertise the matching feature.
 * seg_max counts EVERY segment in a request, header and status included, so
 * a request carries at most (seg_max - 2) data segments. */
#define VIRTIO_BLK_DEFAULT_SEG_MAX 3
#define VIRTIO_BLK_DEFAULT_SIZE_MAX 0xFFFFFFFFu

/*
 * Return codes. This tree's convention is inverted from POSIX (ESUCCESS == 1,
 * EERROR == 0), and every existing caller tests `!= ESUCCESS`. Success keeps
 * returning ESUCCESS so those callers are unaffected; failures return distinct
 * NEGATIVE codes, which are also `!= ESUCCESS` and so remain compatible while
 * telling the caller what actually went wrong.
 */
#define BLK_ETIMEDOUT (-1) /* device never completed, retries exhausted    */
#define BLK_EIO (-2)       /* device returned a non-OK status byte         */
#define BLK_ERANGE (-3)    /* sector range outside device capacity         */
#define BLK_EROFS (-4)     /* write to a VIRTIO_BLK_F_RO device            */
#define BLK_ENOSPC (-5)    /* virtqueue full — no descriptors available    */
#define BLK_ENODEV (-6)    /* device absent or not initialized             */
#define BLK_EINVAL (-7)    /* bad arguments                                */
#define BLK_EUNSUPP (-8)   /* operation not negotiated (e.g. flush)        */

/* Completion mode. Polling is the default: it preserves the pre-existing
 * behaviour and is what the FAT32 mount path uses during early boot, before
 * interrupts are usable. */
#define BLK_MODE_POLL 0
#define BLK_MODE_IRQ 1

/* Defaults for the timeout / retry policy (Phase 4). */
#define BLK_DEFAULT_TIMEOUT_SPINS 10000000u
#define BLK_DEFAULT_RETRIES 3

struct virtio_blk {
  struct pci_device pci;
  struct virtio_pci_caps pci_caps;
  struct virtqueue vq;

  uint64_t capacity_sectors;
  uint32_t size_max; /* max bytes per segment                         */
  uint32_t seg_max;  /* max segments per request (header+data+status) */

  uint64_t features; /* negotiated feature bitmask */
  uint8_t read_only; /* VIRTIO_BLK_F_RO negotiated */
  uint8_t has_flush; /* VIRTIO_BLK_F_FLUSH negotiated */
  uint8_t initialized;

  /* Completion mode + interrupt state */
  uint8_t mode;      /* BLK_MODE_POLL | BLK_MODE_IRQ */
  uint32_t irq;      /* GIC INTID for this device's INTx line (0 = none) */
  uint8_t irq_ready; /* IRQ registered with the GIC                      */

  /* Bumped by the IRQ handler for every completion it reaps. The request path
   * waits for this to move. Volatile: written from the exception handler and
   * read from the requesting context. */
  volatile uint32_t completions;

  /* Timeout / retry policy */
  uint32_t timeout_spins;
  uint32_t retries;

  /* Diagnostics, surfaced by the fault-injection suite. */
  uint64_t stat_timeouts;
  uint64_t stat_retries;
  uint64_t stat_resets;
  uint64_t stat_irqs;
};

/* On-the-wire request header. The data payload and the status byte are
 * separate descriptors in the chain, not members of this struct. */
struct virtio_blk_req {
  uint32_t type;
  uint32_t reserved;
  uint64_t sector;
};

extern struct virtio_blk blk_dev;

void pci_virtio_blk_init(void);

/*
 * blk_read / blk_write — transfer `count` contiguous 512-byte sectors starting
 * at `sector`. buf must hold count * 512 bytes and be physically contiguous
 * (kernel VAs are, through the linear map).
 *
 * Transfers larger than the device's negotiated limits are split across
 * several requests: at most (seg_max - 2) data segments per request, each at
 * most size_max bytes. Returns ESUCCESS only if every chunk succeeded.
 *
 * blk_write fails with EERROR without touching the device when
 * VIRTIO_BLK_F_RO was negotiated.
 */
int blk_read(uint64_t sector, void *buf, uint32_t count);
int blk_write(uint64_t sector, const void *buf, uint32_t count);

/* blk_flush — commit volatile device writes to stable storage. ESUCCESS on
 * completion; a negative BLK_E* code on failure/timeout, or BLK_EUNSUPP if the
 * device did not negotiate VIRTIO_BLK_F_FLUSH. */
int blk_flush(void);

/* ---- completion mode ---------------------------------------------------- */

/* Select polling or interrupt completions. Returns the previous mode.
 * Requesting BLK_MODE_IRQ when no interrupt could be wired up leaves the
 * driver in polling mode and returns the unchanged mode. */
int blk_set_mode(int mode);
int blk_get_mode(void);

/* Convenience wrappers: run one operation in the given mode and restore the
 * previous mode afterwards. */
int blk_read_irq(uint64_t sector, void *buf, uint32_t count);
int blk_write_irq(uint64_t sector, const void *buf, uint32_t count);
int blk_flush_irq(void);

/* GIC INTID this device's INTx line is wired to (0 if none). */
uint32_t blk_get_irq(void);

/* Called from the IRQ dispatch path in exception.c when blk_get_irq() fires.
 * Acknowledges the device's ISR and reaps every pending used-ring entry. */
void blk_handle_irq(void);

/* ---- timeout / retry policy --------------------------------------------- */

/* Busy-wait budget for one attempt, in spin iterations. 0 restores the
 * default. Returns the previous value. */
uint32_t blk_set_timeout(uint32_t spins);

/* Attempts per request before it is failed with BLK_ETIMEDOUT. A value of 1
 * means no retry. Returns the previous value. */
uint32_t blk_set_retries(uint32_t retries);

/* Full device reset: RESET -> re-negotiate features -> re-create the
 * virtqueue -> DRIVER_OK. Used to recover a wedged device, and the only way to
 * resynchronise the used ring after a request times out. */
int blk_reset_device(void);

#endif
