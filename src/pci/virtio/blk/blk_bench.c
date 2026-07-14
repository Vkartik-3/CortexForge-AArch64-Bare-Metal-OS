/* ---------------------------------------------------------------------------
 * blk_bench.c — IOPS / throughput / latency harness for the virtio-blk driver.
 *
 * CLOCK
 * -----
 * Timing uses CNTPCT_EL0, the architectural physical counter, whose exact
 * frequency the hardware reports in CNTFRQ_EL0 (62.5 MHz on QEMU virt =>
 * 16 ns per tick). It is NOT the PMU cycle counter: under QEMU's TCG JIT,
 * PMCCNTR_EL0 does not count real hardware cycles, so a "cycles -> us"
 * conversion from it would be a fiction. CNTPCT with the frequency read from
 * the CPU is the only defensible wall-clock here, and it is what every latency
 * number below is derived from. The frequency actually read at runtime is
 * printed in the [BLK] bench header, so the conversion is never assumed.
 *
 * WHAT IS MEASURED
 * ----------------
 * Every sample covers the complete driver path for one request: descriptor
 * chain construction, the doorbell write, the device's turnaround, and the
 * completion reap. It therefore INCLUDES per-I/O setup overhead — this is the
 * latency a caller of blk_read() actually experiences, not an isolated
 * device-service time.
 *
 * These numbers describe QEMU's virtio-blk backend against a host page-cached
 * file, not physical storage. They are useful for comparing driver paths
 * (polling vs interrupt, queue depth 1 vs 32) against each other; they are not
 * disk numbers.
 *
 * SCRATCH REGION
 * --------------
 * All I/O targets a region at the very end of the device, clear of the FAT32
 * filesystem mounted from the same image (whose BPB is at sector 0).
 * ------------------------------------------------------------------------- */

#include "blk.h"
#include "blk_bench.h"
#include "exception/timer/timer.h"
#include "mm/mmu/mmu.h"
#include "strings/strings.h"
#include "uart/uart.h"
#include "utils/utils.h"

/* Sample counts. Kept modest: every sample is a real device round-trip, and
 * the whole suite has to finish inside a CI boot. */
#define BENCH_ITERS_SMALL 500 /* 512B / 4KB operations */
#define BENCH_ITERS_LARGE 100 /* 64KB operations       */
#define BENCH_MAX_SAMPLES 512

/* Largest single transfer: 64 KiB = 128 sectors. */
#define BENCH_MAX_SECTORS 128
#define BENCH_BUF_BYTES (BENCH_MAX_SECTORS * VIRTIO_BLK_SECTOR_SIZE)

/* Queue-depth sweep: 32 outstanding 4 KiB requests. */
#define BENCH_MAX_QD 32
#define BENCH_QD_SECTORS 8 /* 4 KiB */

/* Scratch window at the end of the device (64 MiB), well past anything FAT32
 * has allocated on this image. */
#define BENCH_REGION_SECTORS 131072

static uint8_t bench_buf[BENCH_BUF_BYTES] __attribute__((aligned(4096)));

/* Per-request state for the queue-depth sweep. Each in-flight request needs
 * its own header, status byte, and data buffer — they are all live in the
 * device's descriptors simultaneously, so they cannot be shared. */
static struct virtio_blk_req qd_hdr[BENCH_MAX_QD] __attribute__((aligned(16)));
static volatile uint8_t qd_status[BENCH_MAX_QD] __attribute__((aligned(64)));
static uint8_t qd_buf[BENCH_MAX_QD][BENCH_QD_SECTORS * VIRTIO_BLK_SECTOR_SIZE]
    __attribute__((aligned(4096)));

static uint64_t samples[BENCH_MAX_SAMPLES];

/* ---- clock --------------------------------------------------------------- */

static inline uint64_t now_ticks(void) {
  uint64_t t;
  __asm__ __volatile__("isb; mrs %0, cntpct_el0" : "=r"(t));
  return t;
}

static uint64_t tick_hz;

/* Ticks -> microseconds, rounded to nearest. Done in 64-bit integer math:
 * ticks * 1e6 / freq. At 62.5 MHz a 64 KiB request is a few thousand ticks,
 * so this cannot overflow. */
static uint64_t ticks_to_us(uint64_t ticks) {
  if (tick_hz == 0) {
    return 0;
  }
  return (ticks * 1000000ULL + tick_hz / 2) / tick_hz;
}

/* ---- statistics ---------------------------------------------------------- */

static void sort_samples(uint64_t *a, uint32_t n) {
  /* Shell sort: no recursion, no libc, fine for <= 512 samples. */
  for (uint32_t gap = n / 2; gap > 0; gap /= 2) {
    for (uint32_t i = gap; i < n; i++) {
      uint64_t tmp = a[i];
      uint32_t j = i;
      while (j >= gap && a[j - gap] > tmp) {
        a[j] = a[j - gap];
        j -= gap;
      }
      a[j] = tmp;
    }
  }
}

struct bench_stats {
  uint64_t p50_us, p99_us, p999_us;
  uint64_t total_ticks;
  uint64_t iops;
};

/* Percentiles come from the sorted per-request latencies; IOPS comes from the
 * total elapsed time across the run (not from 1/mean, which would ignore the
 * gaps between requests). */
static struct bench_stats compute(uint64_t *a, uint32_t n, uint64_t total) {
  struct bench_stats s;
  sort_samples(a, n);

  s.p50_us = ticks_to_us(a[(n * 50) / 100]);
  s.p99_us = ticks_to_us(a[(n * 99) / 100]);
  s.p999_us = ticks_to_us(a[(n * 999) / 1000]);
  s.total_ticks = total;

  uint64_t total_us = ticks_to_us(total);
  s.iops = total_us ? ((uint64_t)n * 1000000ULL) / total_us : 0;
  return s;
}

/* MB/s from bytes moved and total elapsed ticks. MB = 1e6 bytes. */
static uint64_t throughput_mbps(uint64_t bytes, uint64_t total_ticks) {
  uint64_t us = ticks_to_us(total_ticks);
  return us ? bytes / us : 0; /* bytes/us == MB/s */
}

/* Cheap deterministic PRNG for the random-access tests (no Math.random in a
 * kernel; and a fixed seed keeps runs comparable). */
static uint64_t rng_state = 0x243F6A8885A308D3ULL;
static uint64_t next_rand(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return rng_state;
}

/* ---- sequential / random workloads --------------------------------------- */

/*
 * run_io — `iters` requests of `sectors` each, sequential or random, read or
 * write, timing each one individually.
 */
static int run_io(const char *label, uint64_t base, uint32_t sectors,
                  uint32_t iters, int is_write, int is_random,
                  int report_throughput) {
  if (iters > BENCH_MAX_SAMPLES) {
    iters = BENCH_MAX_SAMPLES;
  }

  /* Keep every request inside the scratch window. */
  uint32_t span = BENCH_REGION_SECTORS / sectors;

  memset(bench_buf, 0xC7, (size_t)sectors * VIRTIO_BLK_SECTOR_SIZE);

  uint64_t t_start = now_ticks();

  for (uint32_t i = 0; i < iters; i++) {
    uint64_t slot = is_random ? (next_rand() % span) : (i % span);
    uint64_t sector = base + slot * sectors;

    uint64_t t0 = now_ticks();
    int rc = is_write ? blk_write(sector, bench_buf, sectors)
                      : blk_read(sector, bench_buf, sectors);
    uint64_t t1 = now_ticks();

    if (rc != ESUCCESS) {
      uart_printf("[BLK] %s: ABORTED — request failed with %d\n", label,
                  (uint64_t)rc);
      return EERROR;
    }
    samples[i] = t1 - t0;
  }

  uint64_t total = now_ticks() - t_start;
  struct bench_stats s = compute(samples, iters, total);

  if (report_throughput) {
    uint64_t bytes = (uint64_t)iters * sectors * VIRTIO_BLK_SECTOR_SIZE;
    uart_printf("[BLK] %s IOPS=%d  throughput=%d MB/s  p50=%dus  p99=%dus\n",
                label, s.iops, throughput_mbps(bytes, total), s.p50_us,
                s.p99_us);
  } else {
    uart_printf("[BLK] %s IOPS=%d  p50=%dus  p99=%dus  p999=%dus\n", label,
                s.iops, s.p50_us, s.p99_us, s.p999_us);
  }
  return ESUCCESS;
}

/* ---- queue-depth sweep ---------------------------------------------------
 *
 * The ordinary blk_read() waits for its own completion, so it can never have
 * more than one request outstanding. To reach QD > 1 the sweep drives the
 * virtqueue directly: submit `qd` chains back-to-back, ring the doorbell once,
 * then reap `qd` completions.
 *
 * This is the code path that the Phase 1 free-list and id-based completion
 * lookup exist for. With the old bare cursor it would hand out descriptors
 * still owned by the device; with position-indexed completion it would
 * mis-attribute any out-of-order used entry. Latency here is per BATCH, since
 * individual requests overlap and a per-request latency is not meaningful.
 */
static void run_qd(uint64_t base, uint32_t qd, uint32_t batches) {
  if (qd > BENCH_MAX_QD) {
    qd = BENCH_MAX_QD;
  }
  if (batches > BENCH_MAX_SAMPLES) {
    batches = BENCH_MAX_SAMPLES;
  }

  uint32_t span = BENCH_REGION_SECTORS / BENCH_QD_SECTORS;
  uint32_t completed = 0;
  uint64_t t_start = now_ticks();

  for (uint32_t b = 0; b < batches; b++) {
    uint64_t t0 = now_ticks();

    /* --- submit `qd` requests without waiting for any of them --- */
    for (uint32_t i = 0; i < qd; i++) {
      uint64_t sector = base + ((uint64_t)((b * qd + i) % span)) *
                                   BENCH_QD_SECTORS;

      qd_hdr[i].type = VIRTIO_BLK_T_IN;
      qd_hdr[i].reserved = 0;
      qd_hdr[i].sector = sector;
      qd_status[i] = 0xFF;

      struct virtq_seg segs[3] = {
          {VIRT_TO_PHYS((uint64_t)(uintptr_t)&qd_hdr[i]),
           sizeof(struct virtio_blk_req), VIRTQ_DESC_F_NONE},
          {VIRT_TO_PHYS((uint64_t)(uintptr_t)qd_buf[i]),
           BENCH_QD_SECTORS * VIRTIO_BLK_SECTOR_SIZE, VIRTQ_DESC_F_WRITE},
          {VIRT_TO_PHYS((uint64_t)(uintptr_t)&qd_status[i]), 1,
           VIRTQ_DESC_F_WRITE},
      };

      if (virtqueue_submit_chain(&blk_dev.vq, segs, 3) == VIRTQ_NO_DESC) {
        /* Ran out of descriptors: reap what is outstanding and stop growing
         * the batch. Reported rather than silently truncated. */
        uart_printf("[BLK] queue_depth=%d: descriptor exhaustion at %d "
                    "outstanding\n",
                    (uint64_t)qd, (uint64_t)i);
        break;
      }
    }

    /* One doorbell for the whole batch — this is where deep queues win. */
    virtqueue_notify(&blk_dev.vq);

    /* --- reap `qd` completions (possibly out of order) --- */
    uint32_t reaped = 0;
    uint64_t spins = 0;
    while (reaped < qd) {
      uint16_t id;
      uint32_t len;
      if (virtqueue_get_used(&blk_dev.vq, &id, &len)) {
        reaped++;
        spins = 0;
        continue;
      }
      if (++spins > 100000000ULL) {
        uart_printf("[BLK] queue_depth=%d: TIMEOUT with %d/%d reaped\n",
                    (uint64_t)qd, (uint64_t)reaped, (uint64_t)qd);
        return;
      }
    }

    /* Every request in the batch must have reported success. */
    for (uint32_t i = 0; i < qd; i++) {
      if (qd_status[i] != VIRTIO_BLK_S_OK) {
        uart_printf("[BLK] queue_depth=%d: request %d bad status %x\n",
                    (uint64_t)qd, (uint64_t)i, (uint32_t)qd_status[i]);
        return;
      }
    }

    samples[b] = now_ticks() - t0; /* per-batch latency */
    completed += qd;
  }

  uint64_t total = now_ticks() - t_start;
  sort_samples(samples, batches);

  uint64_t total_us = ticks_to_us(total);
  uint64_t iops = total_us ? ((uint64_t)completed * 1000000ULL) / total_us : 0;
  uint64_t bytes = (uint64_t)completed * BENCH_QD_SECTORS *
                   VIRTIO_BLK_SECTOR_SIZE;

  /* p99 of the per-batch latency; with qd requests per batch this is the
   * tail a caller submitting a full batch would see. */
  uint64_t p99_us = ticks_to_us(samples[(batches * 99) / 100]);
  uint64_t p50_us = ticks_to_us(samples[(batches * 50) / 100]);

  uart_printf("[BLK] queue_depth=%d   seq_read_4kb IOPS=%d  throughput=%d MB/s"
              "  batch_p50=%dus  batch_p99=%dus\n",
              (uint64_t)qd, iops, throughput_mbps(bytes, total), p50_us,
              p99_us);
}

/* ---- flush --------------------------------------------------------------- */

static void run_flush(void) {
  if (!blk_dev.has_flush) {
    uart_println("[BLK] flush            SKIP — VIRTIO_BLK_F_FLUSH not "
                 "negotiated");
    return;
  }

  uint32_t iters = 100;
  for (uint32_t i = 0; i < iters; i++) {
    uint64_t t0 = now_ticks();
    if (blk_flush() != ESUCCESS) {
      uart_println("[BLK] flush            ABORTED — flush failed");
      return;
    }
    samples[i] = now_ticks() - t0;
  }
  sort_samples(samples, iters);
  uart_printf("[BLK] flush            lat=%dus  p99=%dus\n",
              ticks_to_us(samples[iters / 2]),
              ticks_to_us(samples[(iters * 99) / 100]));
}

/* ---- entry point --------------------------------------------------------- */

void blk_bench_run(void) {
  if (!blk_dev.initialized) {
    uart_errorln("[BLK] bench: device not initialized");
    return;
  }
  if (blk_dev.read_only) {
    uart_errorln("[BLK] bench: device is read-only — write benchmarks would "
                 "fail; rerun without readonly=on");
    return;
  }
  if (blk_dev.capacity_sectors < BENCH_REGION_SECTORS + BENCH_MAX_SECTORS) {
    uart_errorln("[BLK] bench: device too small for the scratch region");
    return;
  }

  tick_hz = timer_get_frequency();

  uint64_t base = blk_dev.capacity_sectors - BENCH_REGION_SECTORS;

  uart_println("[BLK] ===== virtio-blk benchmark =====");
  uart_printf("[BLK] clock: CNTPCT_EL0 @ %d Hz (from CNTFRQ_EL0) — NOT the PMU "
              "cycle counter (TCG does not model real cycles)\n",
              tick_hz);
  uart_printf("[BLK] iters: %d per small op, %d per 64KB op, %d flushes\n",
              (uint64_t)BENCH_ITERS_SMALL, (uint64_t)BENCH_ITERS_LARGE,
              (uint64_t)100);
  uart_printf("[BLK] scratch: sectors %d..%d (%d MiB, clear of FAT32)\n", base,
              base + BENCH_REGION_SECTORS - 1,
              (uint64_t)(BENCH_REGION_SECTORS / 2048));
  uart_println("[BLK] latencies INCLUDE per-request driver setup (chain build "
               "+ doorbell + completion reap)");
  uart_printf("[BLK] mode: polling; seg_max=%d size_max=%d\n",
              (uint64_t)blk_dev.seg_max, (uint64_t)blk_dev.size_max);

  /* ---- sequential ---- */
  run_io("seq_read_512b   ", base, 1, BENCH_ITERS_SMALL, 0, 0, 0);
  run_io("seq_write_512b  ", base, 1, BENCH_ITERS_SMALL, 1, 0, 0);
  run_io("seq_read_4kb    ", base, 8, BENCH_ITERS_SMALL, 0, 0, 1);
  run_io("seq_write_4kb   ", base, 8, BENCH_ITERS_SMALL, 1, 0, 1);
  run_io("seq_read_64kb   ", base, 128, BENCH_ITERS_LARGE, 0, 0, 1);
  run_io("seq_write_64kb  ", base, 128, BENCH_ITERS_LARGE, 1, 0, 1);

  /* ---- random ---- */
  run_io("rand_read_4kb   ", base, 8, BENCH_ITERS_SMALL, 0, 1, 0);
  run_io("rand_write_4kb  ", base, 8, BENCH_ITERS_SMALL, 1, 1, 0);

  /* ---- flush ---- */
  run_flush();

  /* ---- polling vs interrupt completions, same workload ---- */
  int prev = blk_set_mode(BLK_MODE_POLL);
  run_io("polling_mode QD=1 seq_read_4kb", base, 8, BENCH_ITERS_SMALL, 0, 0, 0);

  if (blk_set_mode(BLK_MODE_IRQ) == BLK_MODE_POLL &&
      blk_get_mode() == BLK_MODE_IRQ) {
    run_io("irq_mode     QD=1 seq_read_4kb", base, 8, BENCH_ITERS_SMALL, 0, 0,
           0);
  } else {
    uart_println("[BLK] irq_mode     QD=1 seq_read_4kb  SKIP — no INTx line");
  }
  blk_set_mode(prev);

  /* ---- queue-depth sweep (bypasses the synchronous request path) ---- */
  run_qd(base, 1, 200);
  run_qd(base, 4, 200);
  run_qd(base, 16, 100);
  run_qd(base, 32, 100);

  uart_println("[BLK] ===== benchmark done =====");
}
