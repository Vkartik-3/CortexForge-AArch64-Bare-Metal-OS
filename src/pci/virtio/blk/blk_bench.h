#ifndef VIRTIO_BLK_BENCH_H
#define VIRTIO_BLK_BENCH_H

/* virtio-blk IOPS / throughput / latency benchmark. Runs at EL1 (the driver
 * and its DMA buffers are kernel-only) and prints [BLK] result lines.
 * Requires a writable device: it writes to a 64 MiB scratch region at the end
 * of the disk, clear of the mounted FAT32 filesystem. */
void blk_bench_run(void);

#endif
