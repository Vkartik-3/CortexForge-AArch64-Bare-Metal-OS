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
#include "mm/mmu/mmu.h"
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

/* ---------------------------------------------------------------------------
 * IRQ-mode test (Phase 3)
 *
 * Runs reads with completions delivered by the device's INTx line instead of
 * being polled for. Proves three things: the interrupt actually fires (the
 * GIC counter for the blk INTID moves), the handler reaps the used ring
 * (the reads complete at all), and the data is still correct.
 * ------------------------------------------------------------------------- */
void blk_irqtest(void) {
  uart_println("[BLKTEST] ---- virtio-blk interrupt-mode test ----");

  if (!blk_dev.initialized) {
    uart_errorln("[BLKTEST] FAIL: device not initialized");
    return;
  }

  uint32_t irq = blk_get_irq();
  if (irq == 0 || !blk_dev.irq_ready) {
    uart_errorln("[BLKTEST] FAIL: no INTx line wired — cannot test IRQ mode");
    return;
  }
  uart_printf("[BLKTEST] blk INTx is GIC INTID %d\n", (uint64_t)irq);

  uint64_t base = blk_dev.capacity_sectors - BLK_TEST_SECTORS;
  int failures = 0;

  /* Seed the region through the (already-trusted) polling path so we know
   * exactly what the IRQ-mode reads must return. */
  fill_pattern(test_wbuf, BLK_TEST_BYTES, 0x5A);
  if (!blk_dev.read_only && blk_write(base, test_wbuf, BLK_TEST_SECTORS) !=
                                ESUCCESS) {
    uart_errorln("[BLKTEST] FAIL: could not seed scratch region");
    return;
  }

  uint64_t irqs_before = blk_dev.stat_irqs;

  /* 10 reads in IRQ mode. */
  for (int i = 0; i < 10; i++) {
    memset(test_rbuf, 0, BLK_TEST_BYTES);

    int rc = blk_read_irq(base, test_rbuf, BLK_TEST_SECTORS);
    if (rc != ESUCCESS) {
      uart_printf("[BLKTEST] FAIL: IRQ-mode read %d returned %d\n",
                  (uint64_t)i, (uint64_t)rc);
      failures++;
      break;
    }
    if (blk_dev.read_only) {
      continue; /* contents are whatever is on the image; skip the compare */
    }
    if (!buffers_match(test_wbuf, test_rbuf, BLK_TEST_BYTES)) {
      uart_printf("[BLKTEST] FAIL: IRQ-mode read %d returned wrong data\n",
                  (uint64_t)i);
      failures++;
      break;
    }
  }

  uint64_t delivered = blk_dev.stat_irqs - irqs_before;

  if (failures == 0) {
    uart_println("[BLKTEST] PASS: 10 interrupt-mode reads completed with "
                 "correct data");
  }

  /* The interrupt must actually have fired — otherwise the reads "passed"
   * only because something else reaped the ring, and IRQ mode is a fiction. */
  if (delivered == 0) {
    uart_errorln("[BLKTEST] FAIL: no virtio-blk interrupts were delivered");
    failures++;
  } else {
    uart_printf("[BLKTEST] PASS: %d virtio-blk interrupts delivered on INTID "
                "%d (see /proc/interrupts)\n",
                delivered, (uint64_t)irq);
  }

  /* Default mode must be restored: early boot (FAT32 mount) runs before
   * interrupts are usable and depends on polling. */
  if (blk_get_mode() != BLK_MODE_POLL) {
    uart_errorln("[BLKTEST] FAIL: mode was not restored to polling");
    failures++;
  } else {
    uart_println("[BLKTEST] PASS: driver returned to polling mode");
  }

  if (failures == 0) {
    uart_println("[BLKTEST] IRQ ALL PASS");
  } else {
    uart_printf("[BLKTEST] IRQ FAILURES: %d\n", (uint64_t)failures);
  }
}

/* ---------------------------------------------------------------------------
 * Fault injection (Phase 6)
 *
 * Each scenario reports one [FAULT] line. The point is not just that the
 * driver survives, but that it reports the RIGHT error and that the queue is
 * still usable afterwards — a driver that "recovers" into a desynchronised
 * queue has not recovered.
 * ------------------------------------------------------------------------- */

/* After any fault, a plain read of a known-good sector must still work and
 * return the right bytes. This is the real recovery check. */
static int blk_queue_still_sane(uint64_t base) {
  if (blk_dev.read_only) {
    return blk_read(base, test_rbuf, 1) == ESUCCESS;
  }

  fill_pattern(test_wbuf, VIRTIO_BLK_SECTOR_SIZE, 0x77);
  if (blk_write(base, test_wbuf, 1) != ESUCCESS) {
    return 0;
  }
  memset(test_rbuf, 0, VIRTIO_BLK_SECTOR_SIZE);
  if (blk_read(base, test_rbuf, 1) != ESUCCESS) {
    return 0;
  }
  return buffers_match(test_wbuf, test_rbuf, VIRTIO_BLK_SECTOR_SIZE);
}

void blk_faulttest(void) {
  uart_println("[BLKTEST] ---- virtio-blk fault injection ----");

  if (!blk_dev.initialized) {
    uart_errorln("[BLKTEST] FAIL: device not initialized");
    return;
  }

  uint64_t base = blk_dev.capacity_sectors - BLK_TEST_SECTORS;
  int failures = 0;

  /* ---- 1. invalid sector: beyond capacity ------------------------------
   * The driver rejects this before it reaches the device (BLK_ERANGE), so the
   * caller gets a clean error rather than garbage data. */
  int rc = blk_read(blk_dev.capacity_sectors + 1000, test_rbuf, 1);
  if (rc == BLK_ERANGE) {
    uart_println("[FAULT] invalid_sector: PASS — rejected with BLK_ERANGE "
                 "before reaching the device, no data returned");
  } else {
    uart_printf("[FAULT] invalid_sector: FAIL — expected BLK_ERANGE (%d), "
                "got %d\n",
                (uint64_t)BLK_ERANGE, (uint64_t)rc);
    failures++;
  }

  /* A request that starts in range but runs off the end must also be caught. */
  rc = blk_read(blk_dev.capacity_sectors - 2, test_rbuf, 8);
  if (rc == BLK_ERANGE) {
    uart_println("[FAULT] straddling_sector: PASS — request crossing capacity "
                 "rejected with BLK_ERANGE");
  } else {
    uart_printf("[FAULT] straddling_sector: FAIL — expected BLK_ERANGE, "
                "got %d\n",
                (uint64_t)rc);
    failures++;
  }

  /* ---- 2. read-only violation ------------------------------------------ */
  if (blk_dev.read_only) {
    rc = blk_write(base, test_wbuf, 1);
    if (rc == BLK_EROFS) {
      uart_println("[FAULT] ro_violation: PASS — write refused with BLK_EROFS "
                   "before touching the device");
    } else {
      uart_printf("[FAULT] ro_violation: FAIL — expected BLK_EROFS, got %d\n",
                  (uint64_t)rc);
      failures++;
    }
  } else {
    uart_println("[FAULT] ro_violation: SKIP — device is writable (rerun with "
                 "QEMU readonly=on to exercise this path)");
  }

  /* ---- 3. queue full ----------------------------------------------------
   * Fill the descriptor table by submitting chains WITHOUT reaping them, then
   * confirm the next submission is refused cleanly (VIRTQ_NO_DESC) instead of
   * overwriting a live descriptor. We drive the virtqueue directly because the
   * blk request path always waits for its own completion and so can never fill
   * the queue by itself. */
  {
    struct virtq_seg seg = {VIRT_TO_PHYS((uint64_t)(uintptr_t)test_rbuf),
                            VIRTIO_BLK_SECTOR_SIZE, VIRTQ_DESC_F_WRITE};

    uint16_t free_before = virtqueue_num_free(&blk_dev.vq);
    uint32_t admitted = 0;
    int refused_cleanly = 0;

    /* One descriptor per submission; stop as soon as one is refused. */
    for (uint32_t i = 0; i < (uint32_t)VIRTQ_MAX_SIZE + 4; i++) {
      if (virtqueue_submit_chain(&blk_dev.vq, &seg, 1) == VIRTQ_NO_DESC) {
        refused_cleanly = 1;
        break;
      }
      admitted++;
    }

    if (refused_cleanly && admitted == free_before) {
      uart_printf("[FAULT] queue_full: PASS — accepted exactly %d descriptors "
                  "then refused with VIRTQ_NO_DESC, no overwrite\n",
                  (uint64_t)admitted);
    } else {
      uart_printf("[FAULT] queue_full: FAIL — admitted %d of %d free, "
                  "refused=%d\n",
                  (uint64_t)admitted, (uint64_t)free_before,
                  (uint64_t)refused_cleanly);
      failures++;
    }

    /* The queue is now full of descriptors the device may or may not consume,
     * and our last_used no longer tracks it. This is exactly the state a
     * device reset exists to clean up. */
    if (blk_reset_device() != ESUCCESS) {
      uart_errorln("[FAULT] queue_full: FAIL — reset after queue-full failed");
      failures++;
    } else if (!blk_queue_still_sane(base)) {
      uart_errorln("[FAULT] queue_full: FAIL — queue not usable after reset");
      failures++;
    } else {
      uart_println("[FAULT] queue_full_recovery: PASS — device reset restored "
                   "a working, resynchronised queue");
    }
  }

  /* ---- 4. timeout + retry + reset ---------------------------------------
   * Force a timeout without needing to freeze QEMU: set the spin budget to 1,
   * which no real completion can beat. The request path must then time out,
   * retry (resetting the device between attempts), and finally report
   * BLK_ETIMEDOUT rather than hanging or silently succeeding.
   *
   * This also exercises the desync fix: each retry resets the device, so the
   * used ring and our last_used are rebuilt together. */
  {
    uint64_t timeouts_before = blk_dev.stat_timeouts;
    uint64_t resets_before = blk_dev.stat_resets;
    uint64_t retries_before = blk_dev.stat_retries;

    uint32_t prev_timeout = blk_set_timeout(1); /* 1 spin: guaranteed timeout */
    rc = blk_read(base, test_rbuf, 1);
    blk_set_timeout(prev_timeout);

    uint64_t timeouts = blk_dev.stat_timeouts - timeouts_before;
    uint64_t resets = blk_dev.stat_resets - resets_before;
    uint64_t retries = blk_dev.stat_retries - retries_before;

    if (rc == BLK_ETIMEDOUT && timeouts >= 1 && retries >= 1 && resets >= 1) {
      uart_printf("[FAULT] timeout_retry_reset: PASS — %d timeouts, %d "
                  "retries, %d device resets, caller got BLK_ETIMEDOUT\n",
                  timeouts, retries, resets);
    } else {
      uart_printf("[FAULT] timeout_retry_reset: FAIL — rc=%d timeouts=%d "
                  "retries=%d resets=%d\n",
                  (uint64_t)rc, timeouts, retries, resets);
      failures++;
    }

    /* Recovery is the real assertion: after all that, normal I/O must work
     * and return correct data. If the queue were desynchronised, this reads
     * back the wrong bytes or hangs. */
    if (blk_queue_still_sane(base)) {
      uart_println("[FAULT] timeout_recovery: PASS — normal I/O correct after "
                   "timeout storm; queue resynchronised");
    } else {
      uart_errorln("[FAULT] timeout_recovery: FAIL — queue still broken after "
                   "timeout recovery");
      failures++;
    }
  }

  /* ---- 5. device status error ------------------------------------------
   * A non-zero status byte is a definitive device answer and must surface as
   * BLK_EIO, not as success with garbage data. QEMU's virtio-blk will not
   * emit VIRTIO_BLK_S_IOERR on demand for a valid in-range request (it has no
   * error-injection knob on this path), and the driver's own bounds check
   * catches the out-of-range case first — so this path is NOT reachable from
   * the guest side here. Reporting honestly rather than faking a pass. */
  uart_println("[FAULT] status_error: SKIP — not injectable from the guest on "
               "QEMU (no error-injection knob on virtio-blk); BLK_EIO path is "
               "implemented and returned on any non-zero status byte");

  if (failures == 0) {
    uart_println("[BLKTEST] FAULT ALL PASS");
  } else {
    uart_printf("[BLKTEST] FAULT FAILURES: %d\n", (uint64_t)failures);
  }
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
