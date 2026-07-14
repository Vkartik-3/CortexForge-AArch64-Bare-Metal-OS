#ifndef VIRTQUEUE_H
#define VIRTQUEUE_H

#include <stdint.h>

#include "pci/virtio/virtio.h"

/* Virtqueue descriptor flags */
#define VIRTQ_DESC_F_NONE 0  /* no flags; device reads this buffer */
#define VIRTQ_DESC_F_NEXT 1  /* buffer continues via 'next' field */
#define VIRTQ_DESC_F_WRITE 2 /* device writes (vs reads) */

/* MSI-X: no vector assigned (polling mode) */
#define VIRTIO_MSI_NO_VECTOR 0xFFFF

/* Max descriptors per queue.
 * A block request costs 3 descriptors (header + data + status), so a queue
 * depth of 32 needs 96 descriptors. 128 gives that headroom; the device's
 * own maximum still clamps this down in virtqueue_setup(). */
#define VIRTQ_MAX_SIZE 128

/* Sentinel: no descriptor (end of free list, or allocation failure). */
#define VIRTQ_NO_DESC 0xFFFF

struct virtq_desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
};

struct virtq_avail {
  uint16_t flags;
  uint16_t idx;
  uint16_t ring[VIRTQ_MAX_SIZE];
};

struct virtq_used_elem {
  uint32_t id;
  uint32_t len;
};

struct virtq_used {
  uint16_t flags;
  uint16_t idx;
  struct virtq_used_elem ring[VIRTQ_MAX_SIZE];
};

/* complete virtqueue */
struct virtqueue {
  /* negotiated queue size */
  uint16_t size;
  /* head of the free-descriptor list; VIRTQ_NO_DESC when exhausted.
   * Free descriptors are chained through their own `next` field. */
  uint16_t free_head;
  /* number of descriptors currently on the free list */
  uint16_t num_free;
  /* last used.idx we've consumed */
  uint16_t last_used;

  /* PA of notification doorbell */
  uintptr_t notify_addr;

  struct virtq_desc *desc;
  struct virtq_avail *avail;
  struct virtq_used *used;
};

/* single-segment descriptor for chain submission */
struct virtq_seg {
  uint64_t pa;
  uint32_t len;
  uint16_t flags; /* VIRTQ_DESC_F_WRITE if device writes into this segment */
};

/*
 * virtqueue_setup — configure a virtqueue via the common config MMIO registers.
 *   common_cfg_base: PA of VirtIO common config
 *   queue_idx:       which queue to configure (0 for RNG)
 *   vq:              output — filled with queue state
 *   caps:            VirtIO PCI caps (for notify_base + multiplier)
 */

int virtqueue_setup(uintptr_t common_cfg_base, uint16_t queue_idx,
                    struct virtqueue *vq, struct virtio_pci_caps *caps);

/*
 * virtqueue_submit — add a single buffer to the available ring.
 *   buf_pa: physical address of the buffer
 *   len:    buffer length in bytes
 *   flags:  descriptor flags (VIRTQ_DESC_F_WRITE for device→driver)
 * Returns the descriptor index allocated, or VIRTQ_NO_DESC if the queue is
 * full. Callers that track buffers per descriptor must use this return value
 * rather than peeking at vq->free_head — the free list is not sequential.
 */
uint16_t virtqueue_submit(struct virtqueue *vq, uint64_t buf_pa, uint32_t len,
                          uint16_t flags);
/* ring the doorbell */
void virtqueue_notify(struct virtqueue *vq);
/*
 * virtqueue_submit_chain — add a chain of N linked descriptors.
 *   segs: array of N segments
 *   n:    number of segments (must be >= 1)
 * The descriptors are linked via VIRTQ_DESC_F_NEXT through segs[n-1]; only
 * the head index is published to the available ring.
 * Returns the head descriptor index, or VIRTQ_NO_DESC if fewer than n
 * descriptors are free (queue full — nothing is submitted, no partial state).
 */
uint16_t virtqueue_submit_chain(struct virtqueue *vq,
                                const struct virtq_seg *segs, uint16_t n);

/*
 * virtqueue_get_used — non-blocking reap of one completion.
 * If a used entry is available, writes the completing chain's head descriptor
 * index to *id and the device-written byte count to *len, releases that chain
 * back to the free list, and returns 1. Returns 0 if nothing has completed.
 *
 * The id comes from used->ring[].id, so completions are matched by identity
 * and may arrive out of submission order (required for queue depth > 1).
 */
int virtqueue_get_used(struct virtqueue *vq, uint16_t *id, uint32_t *len);

/*
 * virtqueue_poll — spin until the device produces a used entry, then release
 * its descriptor chain. Returns the number of bytes written by the device;
 * on timeout logs and returns 0. Intended for single-outstanding-request
 * callers; use virtqueue_get_used() when several requests are in flight.
 */
uint32_t virtqueue_poll(struct virtqueue *vq);

/* Number of descriptors currently available for allocation. */
static inline uint16_t virtqueue_num_free(const struct virtqueue *vq) {
  return vq->num_free;
}

#endif
