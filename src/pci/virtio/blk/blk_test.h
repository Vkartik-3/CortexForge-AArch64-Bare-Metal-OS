#ifndef VIRTIO_BLK_TEST_H
#define VIRTIO_BLK_TEST_H

/* Data-integrity self-test for the virtio-blk driver. Runs at EL1: it drives
 * the driver directly and uses kernel DMA buffers. Writes only to a scratch
 * region at the end of the device, clear of the mounted FAT32 filesystem.
 * Prints [BLKTEST] lines; ends with "[BLKTEST] ALL PASS" on success. */
void blk_selftest(void);

#endif
