/* ---------------------------------------------------------------------------
 * blk_test.c — data-integrity self-test for the virtio-blk driver.
 *
 * Scratch region: the tests write real sectors, so they must not touch the
 * FAT32 filesystem that is mounted from the same image. Sector 0 holds the
 * BPB/boot sector — writing there (as a naive "write sectors 0-7" test would)
 * corrupts the mount underneath the running kernel. We instead use the last
 * BLK_TEST_SECTORS sectors of the device, which sit past every cluster FAT32
 * has allocated for the handful of files on the image.
 * ------------------------------------------------------------------------- */

#include "blk.h"
#include "strings/strings.h"
#include "uart/uart.h"
#include "utils/utils.h"

#define BLK_TEST_SECTORS 8
#define BLK_TEST_BYTES (BLK_TEST_SECTORS * VIRTIO_BLK_SECTOR_SIZE)

/* Page-aligned, physically contiguous DMA buffers. */
static uint8_t test_wbuf[BLK_TEST_BYTES] __attribute__((aligned(4096)));
static uint8_t test_rbuf[BLK_TEST_BYTES] __attribute__((aligned(4096)));

/* Fill with a position-dependent pattern: a wrong-sector or short transfer
 * shows up as a mismatch rather than as plausible-looking zeros. `seed`
 * distinguishes one run from the next so a stale buffer cannot pass. */
static void fill_pattern(uint8_t *buf, uint32_t len, uint8_t seed) {
  for (uint32_t i = 0; i < len; i++) {
    buf[i] = (uint8_t)((i * 31u + i / VIRTIO_BLK_SECTOR_SIZE + seed) & 0xFF);
  }
}

static int buffers_match(const uint8_t *a, const uint8_t *b, uint32_t len) {
  for (uint32_t i = 0; i < len; i++) {
    if (a[i] != b[i]) {
      uart_printf("[BLKTEST] mismatch at byte %d: wrote %x read %x\n",
                  (uint64_t)i, (uint32_t)a[i], (uint32_t)b[i]);
      return 0;
    }
  }
  return 1;
}

void blk_selftest(void) {
  uart_println("[BLKTEST] ---- virtio-blk data integrity self-test ----");

  if (!blk_dev.initialized) {
    uart_errorln("[BLKTEST] FAIL: device not initialized");
    return;
  }

  uart_printf("[BLKTEST] capacity=%d sectors size_max=%d seg_max=%d ro=%d "
              "flush=%d\n",
              blk_dev.capacity_sectors, (uint64_t)blk_dev.size_max,
              (uint64_t)blk_dev.seg_max, (uint64_t)blk_dev.read_only,
              (uint64_t)blk_dev.has_flush);

  /* Scratch sectors past the end of the FAT32 data area. */
  uint64_t base = blk_dev.capacity_sectors - BLK_TEST_SECTORS;
  uart_printf("[BLKTEST] scratch region: sectors %d..%d\n", base,
              base + BLK_TEST_SECTORS - 1);

  int failures = 0;

  /* ---- 0. read-only device ---------------------------------------------
   * On a device that negotiated VIRTIO_BLK_F_RO the correct behaviour is to
   * refuse writes, so the write/read-back tests below do not apply. Assert
   * the refusal and the still-working read path instead, then stop. */
  if (blk_dev.read_only) {
    uart_println("[BLKTEST] device is read-only (VIRTIO_BLK_F_RO)");

    if (blk_write(base, test_wbuf, BLK_TEST_SECTORS) == ESUCCESS) {
      uart_errorln("[BLKTEST] FAIL: write to a read-only device was accepted");
      failures++;
    } else {
      uart_println("[BLKTEST] PASS: write to read-only device rejected");
    }

    if (blk_read(base, test_rbuf, BLK_TEST_SECTORS) != ESUCCESS) {
      uart_errorln("[BLKTEST] FAIL: read failed on a read-only device");
      failures++;
    } else {
      uart_println("[BLKTEST] PASS: multi-sector read works on read-only "
                   "device");
    }

    if (failures == 0) {
      uart_println("[BLKTEST] ALL PASS (read-only device)");
    } else {
      uart_printf("[BLKTEST] FAILURES: %d\n", (uint64_t)failures);
    }
    return;
  }

  /* ---- 1. multi-sector write, then multi-sector read-back ---------------
   * Both directions issued as ONE request covering all 8 sectors, which is
   * what exercises the descriptor chain built by blk_rw_one(). */
  fill_pattern(test_wbuf, BLK_TEST_BYTES, 0xA5);
  memset(test_rbuf, 0, BLK_TEST_BYTES);

  if (blk_write(base, test_wbuf, BLK_TEST_SECTORS) != ESUCCESS) {
    uart_errorln("[BLKTEST] FAIL: multi-sector write returned error");
    failures++;
  } else if (blk_read(base, test_rbuf, BLK_TEST_SECTORS) != ESUCCESS) {
    uart_errorln("[BLKTEST] FAIL: multi-sector read returned error");
    failures++;
  } else if (!buffers_match(test_wbuf, test_rbuf, BLK_TEST_BYTES)) {
    uart_errorln("[BLKTEST] FAIL: multi-sector data mismatch");
    failures++;
  } else {
    uart_printf("[BLKTEST] PASS: multi-sector w/r %d sectors (%d bytes) "
                "byte-for-byte\n",
                (uint64_t)BLK_TEST_SECTORS, (uint64_t)BLK_TEST_BYTES);
  }

  /* ---- 2. flush --------------------------------------------------------- */
  if (blk_flush() != ESUCCESS) {
    uart_errorln("[BLKTEST] FAIL: flush returned error");
    failures++;
  } else {
    uart_println("[BLKTEST] PASS: flush completed");
  }

  /* ---- 3. single-sector reads must agree with the multi-sector read ------
   * Proves the multi-sector path placed each sector at the right offset: a
   * driver that read the same sector 8 times, or reversed them, passes test 1
   * only if the pattern is uniform — it isn't — but this pins it down
   * independently, one sector per request. */
  int sector_ok = 1;
  for (uint32_t s = 0; s < BLK_TEST_SECTORS; s++) {
    uint8_t one[VIRTIO_BLK_SECTOR_SIZE] __attribute__((aligned(4096)));
    memset(one, 0, sizeof(one));

    if (blk_read(base + s, one, 1) != ESUCCESS) {
      uart_printf("[BLKTEST] FAIL: single-sector read of sector %d\n",
                  base + s);
      sector_ok = 0;
      break;
    }
    if (!buffers_match(test_wbuf + s * VIRTIO_BLK_SECTOR_SIZE, one,
                       VIRTIO_BLK_SECTOR_SIZE)) {
      uart_printf("[BLKTEST] FAIL: sector %d content differs from what the "
                  "multi-sector write placed there\n",
                  base + s);
      sector_ok = 0;
      break;
    }
  }
  if (sector_ok) {
    uart_println("[BLKTEST] PASS: per-sector read-back agrees with "
                 "multi-sector write (ordering correct)");
  } else {
    failures++;
  }

  /* ---- 4. second pass with a different seed ----------------------------
   * A read that silently returned stale buffer contents would pass test 1
   * forever; changing the pattern and repeating catches that. */
  fill_pattern(test_wbuf, BLK_TEST_BYTES, 0x3C);
  memset(test_rbuf, 0, BLK_TEST_BYTES);

  if (blk_write(base, test_wbuf, BLK_TEST_SECTORS) != ESUCCESS ||
      blk_read(base, test_rbuf, BLK_TEST_SECTORS) != ESUCCESS ||
      !buffers_match(test_wbuf, test_rbuf, BLK_TEST_BYTES)) {
    uart_errorln("[BLKTEST] FAIL: second-pass (different pattern) mismatch");
    failures++;
  } else {
    uart_println("[BLKTEST] PASS: second pass with a different pattern");
  }

  /* ---- 5. out-of-range access must be rejected, not sent to the device --- */
  if (blk_read(blk_dev.capacity_sectors, test_rbuf, 1) == ESUCCESS) {
    uart_errorln("[BLKTEST] FAIL: read past capacity was accepted");
    failures++;
  } else if (blk_read(blk_dev.capacity_sectors - 1, test_rbuf, 2) ==
             ESUCCESS) {
    uart_errorln("[BLKTEST] FAIL: read straddling capacity was accepted");
    failures++;
  } else {
    uart_println("[BLKTEST] PASS: out-of-range reads rejected");
  }

  if (failures == 0) {
    uart_println("[BLKTEST] ALL PASS");
  } else {
    uart_printf("[BLKTEST] FAILURES: %d\n", (uint64_t)failures);
  }
}
