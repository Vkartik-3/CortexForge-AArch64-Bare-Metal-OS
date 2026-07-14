#ifndef VIRTIO_BLK_TEST_H
#define VIRTIO_BLK_TEST_H

/* Data-integrity self-test for the virtio-blk driver. Runs at EL1: it drives
 * the driver directly and uses kernel DMA buffers. Writes only to a scratch
 * region at the end of the device, clear of the mounted FAT32 filesystem.
 * Prints [BLKTEST] lines; ends with "[BLKTEST] ALL PASS" on success. */
void blk_selftest(void);

/* Interrupt-mode test: 10 reads completed via the device's INTx line, with
 * verification that the interrupt actually fired and the data is correct. */
void blk_irqtest(void);

/* Fault-injection suite: invalid sector, read-only violation, queue full,
 * timeout/retry/device-reset, and recovery after each. Prints [FAULT] lines. */
void blk_faulttest(void);

#endif
