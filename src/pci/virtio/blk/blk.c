#include "blk.h"
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

void pci_virtio_blk_init(void) {
  uart_println("[BLK] Initializing Device");

  /* Step 0: Find device */
  if (!pci_find_device(VIRTIO_BLK_VENDOR_ID, VIRTIO_BLK_DEVICE_ID,
                       &blk_dev.pci)) {
    uart_errorln("[BLK] Device not found");
    return;
  }

  uart_println("[BLK] Device found");

  // Header Type register[6:0] = header layout type
  // https://wiki.osdev.org/PCI#Header_Type_Register
  if ((pci_get_header_type(&blk_dev.pci) & 0x7F) != PCI_ENDPOINT_DEV_TYPE) {
    uart_errorln("[BLK]: Unexpected header type");
    return;
  }

  pci_assign_bars(&blk_dev.pci);
  pci_enable_device(&blk_dev.pci);
  virtio_parse_capabilities(&blk_dev.pci, &blk_dev.pci_caps);

  /* VirtIO Device Init Sequence
   * All register accesses go through the MMIO layer. */
  uintptr_t base = blk_dev.pci_caps.common_cfg;

  /* Step 1: Reset Device */
  uart_println("[BLK][VIRTIO-INIT][1] Reset Device");
  mmio_write8(base + VIRTIO_COMMON_STATUS, VIRTIO_STATUS_RESET);
  dsb_sy();

  /* Wait for RESET to complete */
  while (mmio_read8(base + VIRTIO_COMMON_STATUS) != VIRTIO_STATUS_RESET) {
  }
  uart_println("[BLK][VIRTIO-INIT][1] Reset Device Complete");

  /* Step 2: ACK */
  uart_println("[BLK][VIRTIO-INIT][2] Ack");
  uint8_t status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_ACKNOWLEDGE);
  dsb_sy();

  /* Step 3: Set Driver status */
  uart_println("[BLK][VIRTIO-INIT][3] Driver Status");
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER);
  dsb_sy();

  /* Step 4: Feature Negotiation.
   * Read the device's 64-bit offer as two 32-bit halves, intersect it with
   * what this driver actually implements, and write the result back. */
  uart_println("[BLK][VIRTIO-INIT][4] Negotiate Features");

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

  /* VERSION_1 is mandatory for a modern device — without it the wire format
   * is the legacy one, which this driver does not implement. */
  if (!(accepted & (1ULL << VIRTIO_F_VERSION_1))) {
    uart_errorln("[BLK] Device does not offer VIRTIO_F_VERSION_1");
    mmio_write8(base + VIRTIO_COMMON_STATUS, VIRTIO_STATUS_FAILED);
    return;
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

  /* Step 5: FEATURES_OK */
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_FEATURES_OK);
  dsb_sy();

  /* Step 6: Re-read and verify FEATURES_OK stuck. If the device cleared it,
   * it rejected our subset and we must not drive it. */
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
    uart_errorln("[BLK] FEATURES_OK failed — device rejected feature subset");
    mmio_write8(base + VIRTIO_COMMON_STATUS, VIRTIO_STATUS_FAILED);
    return;
  }
  uart_printf("[BLK] Status: %x\n", (uint32_t)status);
  uart_println("[BLK] FEATURES_OK !");

  /* Step 7: Setup virtqueue 0 */
  blk_dev.vq.desc = blk_desc;
  blk_dev.vq.avail = &blk_avail;
  blk_dev.vq.used = &blk_used;

  if (virtqueue_setup(base, 0, &blk_dev.vq, &blk_dev.pci_caps) != ESUCCESS) {
    uart_errorln("[BLK] Virtqueue setup failed");
    return;
  }

  /* Step 8: DRIVER_OK */
  status = mmio_read8(base + VIRTIO_COMMON_STATUS);
  mmio_write8(base + VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER_OK);
  dsb_sy();
  uart_println("[BLK] DRIVER_OK set");

  /* Step 9: Read device config. capacity is always valid; size_max and
   * seg_max only if their feature bits were negotiated — otherwise the bytes
   * in the config space are undefined and must not be trusted. */
  uintptr_t dcfg = blk_dev.pci_caps.device_cfg;

  uint32_t cap_lo = mmio_read32(dcfg + VIRTIO_BLK_CFG_CAPACITY);
  uint32_t cap_hi = mmio_read32(dcfg + VIRTIO_BLK_CFG_CAPACITY + 4);
  blk_dev.capacity_sectors = ((uint64_t)cap_hi << 32) | cap_lo;

  if (blk_dev.features & (1ULL << VIRTIO_BLK_F_SIZE_MAX)) {
    blk_dev.size_max = mmio_read32(dcfg + VIRTIO_BLK_CFG_SIZE_MAX);
  } else {
    blk_dev.size_max = VIRTIO_BLK_DEFAULT_SIZE_MAX;
  }

  if (blk_dev.features & (1ULL << VIRTIO_BLK_F_SEG_MAX)) {
    blk_dev.seg_max = mmio_read32(dcfg + VIRTIO_BLK_CFG_SEG_MAX);
  } else {
    blk_dev.seg_max = VIRTIO_BLK_DEFAULT_SEG_MAX;
  }

  /* A request always spends 2 descriptors on the header and status bytes, so
   * a seg_max below 3 leaves no room for data. Clamp defensively. */
  if (blk_dev.seg_max < 3) {
    uart_printf("[BLK] seg_max=%d too small, clamping to 3\n",
                (uint64_t)blk_dev.seg_max);
    blk_dev.seg_max = 3;
  }
  if (blk_dev.size_max < VIRTIO_BLK_SECTOR_SIZE) {
    uart_printf("[BLK] size_max=%d below a sector, clamping to %d\n",
                (uint64_t)blk_dev.size_max, (uint64_t)VIRTIO_BLK_SECTOR_SIZE);
    blk_dev.size_max = VIRTIO_BLK_SECTOR_SIZE;
  }

  uart_printf("[BLK] Capacity: %d sectors (%d MiB)\n",
              (uint64_t)blk_dev.capacity_sectors,
              (uint64_t)(blk_dev.capacity_sectors / 2048));
  uart_printf("[BLK] size_max=%d seg_max=%d read_only=%d flush=%d\n",
              (uint64_t)blk_dev.size_max, (uint64_t)blk_dev.seg_max,
              (uint64_t)blk_dev.read_only, (uint64_t)blk_dev.has_flush);

  blk_dev.initialized = 1;
}

/* Descriptors available for data in one request: seg_max counts the header
 * and status segments too, and the virtqueue itself is a hard ceiling. */
static uint32_t blk_max_data_segs(void) {
  uint32_t by_dev = blk_dev.seg_max - 2;
  uint32_t by_vq = (uint32_t)VIRTQ_MAX_SIZE - 2;
  return (by_dev < by_vq) ? by_dev : by_vq;
}

/*
 * blk_rw_one — submit exactly one request covering `count` sectors, splitting
 * the payload across as many data descriptors as size_max requires.
 *
 * The caller guarantees count fits within one request's limits.
 */
static int blk_rw_one(uint32_t type, uint64_t sector, uint8_t *buf,
                      uint32_t count) {
  static struct virtio_blk_req hdr __attribute__((aligned(16)));
  static volatile uint8_t status __attribute__((aligned(16)));

  hdr.type = type;
  hdr.reserved = 0;
  hdr.sector = sector;
  status = 0xFF;

  /* header + up to (VIRTQ_MAX_SIZE - 2) data segments + status */
  struct virtq_seg segs[VIRTQ_MAX_SIZE];
  uint16_t n = 0;

  segs[n].pa = VIRT_TO_PHYS((uint64_t)(uintptr_t)&hdr);
  segs[n].len = sizeof(hdr);
  segs[n].flags = VIRTQ_DESC_F_NONE; /* device reads the header */
  n++;

  /* Data segments. The device writes into them for a read (T_IN) and reads
   * from them for a write (T_OUT). Each is capped at size_max bytes. */
  uint32_t remaining = count * VIRTIO_BLK_SECTOR_SIZE;
  uint32_t offset = 0;
  uint16_t data_flags =
      (type == VIRTIO_BLK_T_IN) ? VIRTQ_DESC_F_WRITE : VIRTQ_DESC_F_NONE;

  while (remaining > 0) {
    uint32_t chunk = (remaining < blk_dev.size_max) ? remaining : blk_dev.size_max;

    segs[n].pa = VIRT_TO_PHYS((uint64_t)(uintptr_t)(buf + offset));
    segs[n].len = chunk;
    segs[n].flags = data_flags;
    n++;

    offset += chunk;
    remaining -= chunk;
  }

  segs[n].pa = VIRT_TO_PHYS((uint64_t)(uintptr_t)&status);
  segs[n].len = 1;
  segs[n].flags = VIRTQ_DESC_F_WRITE; /* device writes the status byte */
  n++;

  if (virtqueue_submit_chain(&blk_dev.vq, segs, n) == VIRTQ_NO_DESC) {
    uart_errorln("[BLK] submit failed: virtqueue full");
    return EERROR;
  }
  virtqueue_notify(&blk_dev.vq);
  virtqueue_poll(&blk_dev.vq);

  if (status != VIRTIO_BLK_S_OK) {
    uart_printf("[BLK] %s sector %d (%d sectors) failed: status=%x\n",
                (type == VIRTIO_BLK_T_IN) ? "read" : "write", sector,
                (uint64_t)count, (uint32_t)status);
    return EERROR;
  }
  return ESUCCESS;
}

/*
 * blk_rw — split a transfer into device-legal requests and issue them in
 * order. Two independent limits apply per request: at most
 * (seg_max - 2) data segments, each at most size_max bytes.
 */
static int blk_rw(uint32_t type, uint64_t sector, uint8_t *buf,
                  uint32_t count) {
  if (!blk_dev.initialized) {
    uart_errorln("[BLK] device not initialized");
    return EERROR;
  }
  if (count == 0 || !buf) {
    return EERROR;
  }

  /* Reject out-of-range access here rather than letting the device fail it:
   * the caller gets a clean error and we never put a bad sector on the wire.
   * Checked as (sector + count) against capacity without overflowing. */
  if (sector >= blk_dev.capacity_sectors ||
      count > blk_dev.capacity_sectors - sector) {
    uart_printf("[BLK] out of range: sector %d + %d > capacity %d\n", sector,
                (uint64_t)count, blk_dev.capacity_sectors);
    return EERROR;
  }

  /* Largest transfer one request can carry. */
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

    int rc = blk_rw_one(type, sector + done,
                        buf + (uint64_t)done * VIRTIO_BLK_SECTOR_SIZE, chunk);
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
    return EERROR;
  }
  /* Cast away const: the device only reads these descriptors (T_OUT sets no
   * VIRTQ_DESC_F_WRITE on the data segments), so the buffer is not modified. */
  return blk_rw(VIRTIO_BLK_T_OUT, sector, (uint8_t *)(uintptr_t)buf, count);
}

int blk_flush(void) {
  static struct virtio_blk_req hdr __attribute__((aligned(16)));
  static volatile uint8_t status __attribute__((aligned(16)));

  if (!blk_dev.initialized) {
    return EERROR;
  }
  if (!blk_dev.has_flush) {
    uart_errorln("[BLK] flush unsupported: VIRTIO_BLK_F_FLUSH not negotiated");
    return EERROR;
  }
  if (blk_dev.read_only) {
    /* Nothing can be dirty on a read-only device. */
    return ESUCCESS;
  }

  hdr.type = VIRTIO_BLK_T_FLUSH;
  hdr.reserved = 0;
  hdr.sector = 0; /* ignored for flush */
  status = 0xFF;

  /* A flush carries no data: header + status only. */
  struct virtq_seg segs[2] = {
      {VIRT_TO_PHYS((uint64_t)(uintptr_t)&hdr), sizeof(hdr), VIRTQ_DESC_F_NONE},
      {VIRT_TO_PHYS((uint64_t)(uintptr_t)&status), 1, VIRTQ_DESC_F_WRITE},
  };

  if (virtqueue_submit_chain(&blk_dev.vq, segs, 2) == VIRTQ_NO_DESC) {
    uart_errorln("[BLK] flush submit failed: virtqueue full");
    return EERROR;
  }
  virtqueue_notify(&blk_dev.vq);
  virtqueue_poll(&blk_dev.vq);

  if (status != VIRTIO_BLK_S_OK) {
    uart_printf("[BLK] flush failed: status=%x\n", (uint32_t)status);
    return EERROR;
  }
  return ESUCCESS;
}
