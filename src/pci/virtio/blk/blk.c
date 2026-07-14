#include "blk.h"
#include "exception/gic/gic.h"
#include "mm/mmu/mmu.h"
#include "mmio/mmio.h"
#include "uart/uart.h"
#include "utils/utils.h"

struct virtio_blk blk_dev;

/* Page-aligned backing memory for the virtqueue */
static struct virtq_desc blk_desc[VIRTQ_MAX_SIZE]
    __attribute__((aligned(4096)));
static struct virtq_avail blk_avail __attribute__((aligned(4096)));
static struct virtq_used blk_used __attribute__((aligned(4096)));

/* Features this driver understands. Anything the device offers outside this
 * set is declined: accepting a feature we do not implement would change the
 * wire format under us (virtio 1.2 §2.2.1). */
#define BLK_FEATURE_MASK                                                       \
  ((1ULL << VIRTIO_BLK_F_SIZE_MAX) | (1ULL << VIRTIO_BLK_F_SEG_MAX) |          \
   (1ULL << VIRTIO_BLK_F_RO) | (1ULL << VIRTIO_BLK_F_FLUSH) |                  \
   (1ULL << VIRTIO_F_VERSION_1))

/*
 * blk_bringup — the virtio init handshake, from RESET to DRIVER_OK.
 *
 * Factored out of pci_virtio_blk_init() because blk_reset_device() has to run
 * exactly the same sequence to recover a wedged device. It assumes the PCI
 * side (BARs, bus-mastering, capability parsing) is already done, since that
 * survives a device reset.
 */
static int blk_bringup(void) {
  uintptr_t base = blk_dev.pci_caps.common_cfg;

  /* Step 1: Reset */
  mmio_write8(base + VIRTIO_COMMON_STATUS, VIRTIO_STATUS_RESET);
  dsb_sy();
  while (mmio_read8(base + VIRTIO_COMMON_STATUS) != VIRTIO_STATUS_RESET) {
  }

  /* Step 2: ACKNOWLEDGE */
  uint8_t status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_ACKNOWLEDGE);
  dsb_sy();

  /* Step 3: DRIVER */
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER);
  dsb_sy();

  /* Step 4: feature negotiation — intersect the device's 64-bit offer with
   * what this driver actually implements. */
  mmio_write32(base + VIRTIO_COMMON_DFSELECT, 0);
  dsb_sy();
  uint32_t feat_lo = mmio_read32(base + VIRTIO_COMMON_DF);
  mmio_write32(base + VIRTIO_COMMON_DFSELECT, 1);
  dsb_sy();
  uint32_t feat_hi = mmio_read32(base + VIRTIO_COMMON_DF);

  uint64_t offered = ((uint64_t)feat_hi << 32) | feat_lo;
  uint64_t accepted = offered & BLK_FEATURE_MASK;

  uart_printf("[BLK] Device offers: %x\n", offered);
  uart_printf("[BLK] Driver accepts: %x\n", accepted);

  if (!(accepted & (1ULL << VIRTIO_F_VERSION_1))) {
    uart_errorln("[BLK] Device does not offer VIRTIO_F_VERSION_1");
    mmio_write8(base + VIRTIO_COMMON_STATUS, VIRTIO_STATUS_FAILED);
    return BLK_ENODEV;
  }

  mmio_write32(base + VIRTIO_COMMON_GFSELECT, 0);
  dsb_sy();
  mmio_write32(base + VIRTIO_COMMON_GF, (uint32_t)(accepted & 0xFFFFFFFF));
  dsb_sy();
  mmio_write32(base + VIRTIO_COMMON_GFSELECT, 1);
  dsb_sy();
  mmio_write32(base + VIRTIO_COMMON_GF, (uint32_t)(accepted >> 32));
  dsb_sy();

  blk_dev.features = accepted;
  blk_dev.read_only = (accepted & (1ULL << VIRTIO_BLK_F_RO)) ? 1 : 0;
  blk_dev.has_flush = (accepted & (1ULL << VIRTIO_BLK_F_FLUSH)) ? 1 : 0;

  /* Step 5: FEATURES_OK, then confirm the device kept it set. */
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_FEATURES_OK);
  dsb_sy();

  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
    uart_errorln("[BLK] FEATURES_OK failed — device rejected feature subset");
    mmio_write8(base + VIRTIO_COMMON_STATUS, VIRTIO_STATUS_FAILED);
    return BLK_ENODEV;
  }

  /* Step 6: virtqueue 0. virtqueue_setup() rebuilds the descriptor table,
   * rings, and free list from scratch — which is exactly what makes a reset a
   * valid way to resynchronise a queue whose used ring we lost track of. */
  blk_dev.vq.desc = blk_desc;
  blk_dev.vq.avail = &blk_avail;
  blk_dev.vq.used = &blk_used;

  if (virtqueue_setup(base, 0, &blk_dev.vq, &blk_dev.pci_caps) != ESUCCESS) {
    uart_errorln("[BLK] Virtqueue setup failed");
    return BLK_ENODEV;
  }

  /* Step 7: DRIVER_OK */
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER_OK);
  dsb_sy();

  /* Step 8: device config. capacity is always valid; size_max and seg_max only
   * when their feature bit was negotiated — otherwise those config bytes are
   * undefined and must not be trusted. */
  uintptr_t dcfg = blk_dev.pci_caps.device_cfg;

  uint32_t cap_lo = mmio_read32(dcfg + VIRTIO_BLK_CFG_CAPACITY);
  uint32_t cap_hi = mmio_read32(dcfg + VIRTIO_BLK_CFG_CAPACITY + 4);
  blk_dev.capacity_sectors = ((uint64_t)cap_hi << 32) | cap_lo;

  blk_dev.size_max = (blk_dev.features & (1ULL << VIRTIO_BLK_F_SIZE_MAX))
                         ? mmio_read32(dcfg + VIRTIO_BLK_CFG_SIZE_MAX)
                         : VIRTIO_BLK_DEFAULT_SIZE_MAX;

  blk_dev.seg_max = (blk_dev.features & (1ULL << VIRTIO_BLK_F_SEG_MAX))
                        ? mmio_read32(dcfg + VIRTIO_BLK_CFG_SEG_MAX)
                        : VIRTIO_BLK_DEFAULT_SEG_MAX;

  /* A request always spends 2 descriptors on header and status, so a seg_max
   * below 3 leaves no room for data. Clamp defensively. */
  if (blk_dev.seg_max < 3) {
    blk_dev.seg_max = 3;
  }
  if (blk_dev.size_max < VIRTIO_BLK_SECTOR_SIZE) {
    blk_dev.size_max = VIRTIO_BLK_SECTOR_SIZE;
  }

  return ESUCCESS;
}

void pci_virtio_blk_init(void) {
  uart_println("[BLK] Initializing Device");

  if (!pci_find_device(VIRTIO_BLK_VENDOR_ID, VIRTIO_BLK_DEVICE_ID,
                       &blk_dev.pci)) {
    uart_errorln("[BLK] Device not found");
    return;
  }
  uart_println("[BLK] Device found");

  if ((pci_get_header_type(&blk_dev.pci) & 0x7F) != PCI_ENDPOINT_DEV_TYPE) {
    uart_errorln("[BLK]: Unexpected header type");
    return;
  }

  pci_assign_bars(&blk_dev.pci);
  pci_enable_device(&blk_dev.pci);
  virtio_parse_capabilities(&blk_dev.pci, &blk_dev.pci_caps);

  /* Policy defaults before any I/O can run. */
  blk_dev.mode = BLK_MODE_POLL;
  blk_dev.timeout_spins = BLK_DEFAULT_TIMEOUT_SPINS;
  blk_dev.retries = BLK_DEFAULT_RETRIES;

  if (blk_bringup() != ESUCCESS) {
    return;
  }
  uart_println("[BLK] DRIVER_OK set");

  uart_printf("[BLK] Capacity: %d sectors (%d MiB)\n",
              (uint64_t)blk_dev.capacity_sectors,
              (uint64_t)(blk_dev.capacity_sectors / 2048));
  uart_printf("[BLK] size_max=%d seg_max=%d read_only=%d flush=%d\n",
              (uint64_t)blk_dev.size_max, (uint64_t)blk_dev.seg_max,
              (uint64_t)blk_dev.read_only, (uint64_t)blk_dev.has_flush);

  /* ---- interrupt wiring (INTx) ----------------------------------------
   * We log the capability list first so it is on the record what this QEMU
   * actually offers. MSI-X is present, but on the virt machine MSIs are
   * routed through the GICv3 ITS (see the DT's msi-map -> its@8080000), which
   * would mean implementing LPIs, the ITS command queue, and the config/
   * pending tables. Legacy INTx reaches the same GIC as a plain SPI, so that
   * is what we wire up. MSI-X is left disabled (queue vector = NO_VECTOR in
   * virtqueue_setup), which is what keeps the device using its INTx pin. */
  uart_println("[BLK] PCI capabilities:");
  pci_dump_capabilities(&blk_dev.pci);

  uint8_t msix = pci_find_capability(&blk_dev.pci, PCI_CAP_ID_MSIX);
  uart_printf("[BLK] MSI-X capability: %s\n",
              msix ? "present (unused — needs GICv3 ITS/LPI)" : "absent");

  blk_dev.irq = pci_intx_intid(&blk_dev.pci);
  if (blk_dev.irq == 0) {
    uart_println("[BLK] no INTx pin — interrupt mode unavailable");
  } else {
    pci_enable_intx(&blk_dev.pci);
    blk_dev.irq_ready = 1;
    uart_printf("[BLK] INTx available on GIC INTID %d (unmasked only in IRQ "
                "mode; polling remains default)\n",
                (uint64_t)blk_dev.irq);
  }

  blk_dev.initialized = 1;
}

/* ---- interrupt handling -------------------------------------------------- */

uint32_t blk_get_irq(void) { return blk_dev.irq; }

void blk_handle_irq(void) {
  /* Reading the ISR both tells us why the device interrupted and de-asserts
   * the INTx line. It must be read exactly once per interrupt: a second read
   * returns 0 because the first one cleared it. */
  uint8_t isr = mmio_read8(blk_dev.pci_caps.isr_cfg);
  dsb_sy();

  blk_dev.stat_irqs++;

  if (!(isr & VIRTIO_ISR_QUEUE)) {
    return; /* config change or spurious — nothing to reap */
  }

  /* Drain every completion the device published. One interrupt can cover
   * several used entries, so loop until the ring is empty; otherwise a
   * completion could sit unreaped with no further interrupt coming. */
  uint16_t id;
  uint32_t len;
  while (virtqueue_get_used(&blk_dev.vq, &id, &len)) {
    blk_dev.completions++;
  }
}

int blk_get_mode(void) { return blk_dev.mode; }

/*
 * The GIC line is unmasked ONLY while IRQ mode is active. Two reasons, both
 * learned the hard way:
 *
 *  1. The device asserts INTx whenever it completes a request — it has no idea
 *     we are polling. If the line were unmasked during boot, the FAT32 mount's
 *     very first read would raise an IRQ before sched_init() has run, and the
 *     IRQ path's tail call into schedule() would dereference a NULL current
 *     task. (This is exactly what happened.)
 *
 *  2. In polling mode the requesting context reaps the used ring itself. If the
 *     handler were also live it would race that loop, consume the completion
 *     first, and leave the poller spinning until its timeout on a request that
 *     had in fact succeeded.
 *
 * So: unmask on entry to IRQ mode, mask on the way out, and drain the device's
 * ISR at each transition so a stale assertion cannot fire a spurious IRQ.
 */
int blk_set_mode(int mode) {
  int prev = blk_dev.mode;

  if (mode == BLK_MODE_IRQ) {
    if (!blk_dev.irq_ready) {
      uart_errorln("[BLK] IRQ mode unavailable (no INTx) — staying in polling");
      return prev;
    }
    if (prev != BLK_MODE_IRQ) {
      /* Clear any assertion left over from polling-mode traffic before
       * unmasking, or the GIC would immediately deliver a stale interrupt. */
      (void)mmio_read8(blk_dev.pci_caps.isr_cfg);
      dsb_sy();
      blk_dev.mode = BLK_MODE_IRQ;
      gic_enable_irq(blk_dev.irq);
    }
  } else {
    if (prev == BLK_MODE_IRQ) {
      gic_disable_irq(blk_dev.irq);
      blk_dev.mode = BLK_MODE_POLL;
      /* De-assert the line so it is quiet while masked. */
      (void)mmio_read8(blk_dev.pci_caps.isr_cfg);
      dsb_sy();
    }
    blk_dev.mode = BLK_MODE_POLL;
  }
  return prev;
}

/*
 * blk_wait_one — wait for exactly one completion, in whichever mode is active.
 *
 * Returns ESUCCESS, or BLK_ETIMEDOUT if the spin budget ran out. On timeout
 * the queue is left DESYNCHRONISED on purpose — the request may still be in
 * flight inside the device, and its descriptors are still owned by it. Only a
 * device reset can safely reclaim that state, which is what the caller
 * (blk_submit_retry) does once its retries are exhausted.
 */
static int blk_wait_one(void) {
  uint32_t budget = blk_dev.timeout_spins;

  if (blk_dev.mode == BLK_MODE_IRQ) {
    uint32_t target = blk_dev.completions + 1;
    for (uint32_t spins = 0; spins < budget; spins++) {
      /* The handler bumps `completions` and reaps the used ring for us. */
      if ((int32_t)(blk_dev.completions - target) >= 0) {
        return ESUCCESS;
      }
    }
    return BLK_ETIMEDOUT;
  }

  /* Polling mode: reap the used ring ourselves. */
  uint16_t id;
  uint32_t len;
  for (uint32_t spins = 0; spins < budget; spins++) {
    if (virtqueue_get_used(&blk_dev.vq, &id, &len)) {
      return ESUCCESS;
    }
  }
  return BLK_ETIMEDOUT;
}

/* ---- device reset -------------------------------------------------------- */

int blk_reset_device(void) {
  uart_println("[BLK] resetting device");

  blk_dev.stat_resets++;
  blk_dev.initialized = 0;

  /* Re-run the full handshake. virtqueue_setup() zeroes the descriptor table
   * and both rings and rebuilds the free list, so any descriptors the device
   * still held, and any used entries we never consumed, are discarded
   * together. That is what resynchronises last_used with the device's
   * used->idx — the desync a bare timeout would otherwise leave behind. */
  int rc = blk_bringup();
  if (rc != ESUCCESS) {
    uart_errorln("[BLK] device reset FAILED");
    return rc;
  }

  /* Any completion counted against the old queue is meaningless now. */
  blk_dev.completions = 0;

  blk_dev.initialized = 1;
  uart_println("[BLK] device reset complete");
  return ESUCCESS;
}

/* ---- timeout / retry policy ---------------------------------------------- */

uint32_t blk_set_timeout(uint32_t spins) {
  uint32_t prev = blk_dev.timeout_spins;
  blk_dev.timeout_spins = spins ? spins : BLK_DEFAULT_TIMEOUT_SPINS;
  return prev;
}

uint32_t blk_set_retries(uint32_t retries) {
  uint32_t prev = blk_dev.retries;
  blk_dev.retries = retries ? retries : 1;
  return prev;
}

/* Descriptors available for data in one request: seg_max counts the header and
 * status segments too, and the virtqueue itself is a hard ceiling. */
static uint32_t blk_max_data_segs(void) {
  uint32_t by_dev = blk_dev.seg_max - 2;
  uint32_t by_vq = (uint32_t)VIRTQ_MAX_SIZE - 2;
  return (by_dev < by_vq) ? by_dev : by_vq;
}

/*
 * blk_submit_once — build one chain, ring the doorbell, wait for it.
 * `status` is the caller's status byte, already reset to 0xFF.
 */
static int blk_submit_once(uint32_t type, uint64_t sector, uint8_t *buf,
                           uint32_t count, struct virtio_blk_req *hdr,
                           volatile uint8_t *status) {
  hdr->type = type;
  hdr->reserved = 0;
  hdr->sector = sector;
  *status = 0xFF;

  struct virtq_seg segs[VIRTQ_MAX_SIZE];
  uint16_t n = 0;

  segs[n].pa = VIRT_TO_PHYS((uint64_t)(uintptr_t)hdr);
  segs[n].len = sizeof(*hdr);
  segs[n].flags = VIRTQ_DESC_F_NONE; /* device reads the header */
  n++;

  if (type != VIRTIO_BLK_T_FLUSH) {
    /* Data segments: device-writable for a read, device-readable for a write.
     * Each is capped at size_max bytes. */
    uint32_t remaining = count * VIRTIO_BLK_SECTOR_SIZE;
    uint32_t offset = 0;
    uint16_t data_flags =
        (type == VIRTIO_BLK_T_IN) ? VIRTQ_DESC_F_WRITE : VIRTQ_DESC_F_NONE;

    while (remaining > 0) {
      uint32_t chunk =
          (remaining < blk_dev.size_max) ? remaining : blk_dev.size_max;

      segs[n].pa = VIRT_TO_PHYS((uint64_t)(uintptr_t)(buf + offset));
      segs[n].len = chunk;
      segs[n].flags = data_flags;
      n++;

      offset += chunk;
      remaining -= chunk;
    }
  }

  segs[n].pa = VIRT_TO_PHYS((uint64_t)(uintptr_t)status);
  segs[n].len = 1;
  segs[n].flags = VIRTQ_DESC_F_WRITE; /* device writes the status byte */
  n++;

  if (virtqueue_submit_chain(&blk_dev.vq, segs, n) == VIRTQ_NO_DESC) {
    return BLK_ENOSPC;
  }
  virtqueue_notify(&blk_dev.vq);

  int rc = blk_wait_one();
  if (rc != ESUCCESS) {
    return rc; /* BLK_ETIMEDOUT — queue state is now suspect */
  }

  if (*status != VIRTIO_BLK_S_OK) {
    return BLK_EIO;
  }
  return ESUCCESS;
}

/*
 * blk_submit_retry — one request, with the timeout/retry/reset policy applied.
 *
 * A timeout means the device never produced a used entry within the budget.
 * The request may still be in flight, so we cannot simply resubmit onto the
 * same queue: its descriptors are still owned by the device and a late
 * completion would be matched against a newer request. We therefore RESET the
 * device before each retry, which rebuilds the queue and discards the stale
 * in-flight state. That is the fix for the desync the old code left behind.
 *
 * A status error (BLK_EIO) is a definitive device answer, not a lost request:
 * it is returned immediately rather than retried, since retrying a read of an
 * invalid sector just produces the same error.
 */
static int blk_submit_retry(uint32_t type, uint64_t sector, uint8_t *buf,
                            uint32_t count) {
  static struct virtio_blk_req hdr __attribute__((aligned(16)));
  static volatile uint8_t status __attribute__((aligned(16)));

  int rc = BLK_ETIMEDOUT;

  for (uint32_t attempt = 0; attempt < blk_dev.retries; attempt++) {
    if (attempt > 0) {
      blk_dev.stat_retries++;
      uart_printf("[BLK] retry %d/%d for sector %d\n", (uint64_t)attempt,
                  (uint64_t)(blk_dev.retries - 1), sector);
    }

    rc = blk_submit_once(type, sector, buf, count, &hdr, &status);

    if (rc == ESUCCESS) {
      return ESUCCESS;
    }
    if (rc == BLK_EIO) {
      uart_printf("[BLK] I/O error: status=%x sector=%d\n", (uint32_t)status,
                  sector);
      return BLK_EIO; /* definitive — do not retry */
    }
    if (rc == BLK_ENOSPC) {
      uart_errorln("[BLK] virtqueue full");
      return BLK_ENOSPC; /* caller must drain; retrying cannot help */
    }

    /* BLK_ETIMEDOUT */
    blk_dev.stat_timeouts++;
    uart_printf("[BLK] TIMEOUT waiting for sector %d (attempt %d)\n", sector,
                (uint64_t)(attempt + 1));

    /* Resynchronise before trying again — see the comment above. */
    if (blk_reset_device() != ESUCCESS) {
      return BLK_ENODEV;
    }
  }

  uart_printf("[BLK] PERMANENT FAILURE: sector %d after %d attempts\n", sector,
              (uint64_t)blk_dev.retries);
  return rc;
}

static int blk_rw(uint32_t type, uint64_t sector, uint8_t *buf,
                  uint32_t count) {
  if (!blk_dev.initialized) {
    uart_errorln("[BLK] device not initialized");
    return BLK_ENODEV;
  }
  if (count == 0 || !buf) {
    return BLK_EINVAL;
  }

  /* Reject out-of-range access here rather than letting the device fail it: the
   * caller gets a clean error and we never put a bad sector on the wire. The
   * comparison is arranged so (sector + count) cannot overflow. */
  if (sector >= blk_dev.capacity_sectors ||
      count > blk_dev.capacity_sectors - sector) {
    uart_printf("[BLK] out of range: sector %d + %d > capacity %d\n", sector,
                (uint64_t)count, blk_dev.capacity_sectors);
    return BLK_ERANGE;
  }

  /* Largest transfer one request can carry, under both device limits. */
  uint64_t max_bytes_per_req =
      (uint64_t)blk_max_data_segs() * (uint64_t)blk_dev.size_max;
  uint32_t max_sectors_per_req =
      (uint32_t)(max_bytes_per_req / VIRTIO_BLK_SECTOR_SIZE);
  if (max_sectors_per_req == 0) {
    max_sectors_per_req = 1;
  }

  uint32_t done = 0;
  while (done < count) {
    uint32_t chunk = count - done;
    if (chunk > max_sectors_per_req) {
      chunk = max_sectors_per_req;
    }

    int rc = blk_submit_retry(type, sector + done,
                              buf + (uint64_t)done * VIRTIO_BLK_SECTOR_SIZE,
                              chunk);
    if (rc != ESUCCESS) {
      return rc;
    }
    done += chunk;
  }

  return ESUCCESS;
}

int blk_read(uint64_t sector, void *buf, uint32_t count) {
  return blk_rw(VIRTIO_BLK_T_IN, sector, (uint8_t *)buf, count);
}

int blk_write(uint64_t sector, const void *buf, uint32_t count) {
  if (blk_dev.read_only) {
    uart_errorln("[BLK] write rejected: device is read-only (VIRTIO_BLK_F_RO)");
    return BLK_EROFS;
  }
  /* Cast away const: for T_OUT the data segments carry no VIRTQ_DESC_F_WRITE,
   * so the device only reads them. */
  return blk_rw(VIRTIO_BLK_T_OUT, sector, (uint8_t *)(uintptr_t)buf, count);
}

int blk_flush(void) {
  if (!blk_dev.initialized) {
    return BLK_ENODEV;
  }
  if (!blk_dev.has_flush) {
    uart_errorln("[BLK] flush unsupported: VIRTIO_BLK_F_FLUSH not negotiated");
    return BLK_EUNSUPP;
  }
  if (blk_dev.read_only) {
    return ESUCCESS; /* nothing can be dirty on a read-only device */
  }

  /* A flush carries no data: blk_submit_once emits header + status only. */
  return blk_submit_retry(VIRTIO_BLK_T_FLUSH, 0, 0, 0);
}

/* ---- mode-scoped wrappers ------------------------------------------------ */

int blk_read_irq(uint64_t sector, void *buf, uint32_t count) {
  int prev = blk_set_mode(BLK_MODE_IRQ);
  int rc = blk_read(sector, buf, count);
  blk_set_mode(prev);
  return rc;
}

int blk_write_irq(uint64_t sector, const void *buf, uint32_t count) {
  int prev = blk_set_mode(BLK_MODE_IRQ);
  int rc = blk_write(sector, buf, count);
  blk_set_mode(prev);
  return rc;
}

int blk_flush_irq(void) {
  int prev = blk_set_mode(BLK_MODE_IRQ);
  int rc = blk_flush();
  blk_set_mode(prev);
  return rc;
}
