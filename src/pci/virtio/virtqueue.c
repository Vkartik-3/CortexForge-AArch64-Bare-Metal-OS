#include "virtqueue.h"
#include "mm/mmu/mmu.h"
#include "mmio/mmio.h"
#include "pci/virtio/virtio.h"
#include "strings/strings.h" // IWYU pragma: keep
#include "uart/uart.h"
#include "utils/utils.h"

/* ---- Descriptor free list ----------------------------------------------
 *
 * Descriptors are not consumed in submission order: the device may complete
 * requests out of order, and a chain freed in the middle of the table leaves
 * a hole. A bare incrementing cursor therefore hands out descriptors that are
 * still in flight once it wraps. Instead we keep an explicit free list,
 * threaded through the `next` field of each free descriptor (that field is
 * only meaningful to the device while a descriptor is live, so it is free
 * real estate while the descriptor sits on the list).
 *
 *   free_head -> d3 -> d0 -> d7 -> VIRTQ_NO_DESC
 *
 * alloc pops the head; freeing a chain walks its VIRTQ_DESC_F_NEXT links and
 * pushes every descriptor back. num_free lets a caller check for space before
 * committing to a submission.
 */
static void virtqueue_init_free_list(struct virtqueue *vq) {
  for (uint16_t i = 0; i < vq->size; i++) {
    vq->desc[i].next = (i + 1 < vq->size) ? (uint16_t)(i + 1) : VIRTQ_NO_DESC;
  }
  vq->free_head = (vq->size > 0) ? 0 : VIRTQ_NO_DESC;
  vq->num_free = vq->size;
}

/* Pop one descriptor off the free list. VIRTQ_NO_DESC when exhausted. */
static uint16_t virtqueue_alloc_desc(struct virtqueue *vq) {
  uint16_t idx = vq->free_head;
  if (idx == VIRTQ_NO_DESC) {
    return VIRTQ_NO_DESC;
  }
  vq->free_head = vq->desc[idx].next;
  vq->num_free--;
  return idx;
}

/* Release a descriptor chain (head + every VIRTQ_DESC_F_NEXT link). */
static void virtqueue_free_chain(struct virtqueue *vq, uint16_t head) {
  uint16_t idx = head;

  for (;;) {
    if (idx >= vq->size) {
      /* Corrupt link — stop rather than walk off the table or build a cycle
       * in the free list. Leaks the remainder of the chain, which is strictly
       * better than handing the device a descriptor that is still live. */
      uart_errorln("[VQ] free_chain: descriptor index out of range");
      return;
    }

    uint16_t flags = vq->desc[idx].flags;
    uint16_t next = vq->desc[idx].next;

    vq->desc[idx].addr = 0;
    vq->desc[idx].len = 0;
    vq->desc[idx].flags = 0;

    /* push onto the free list */
    vq->desc[idx].next = vq->free_head;
    vq->free_head = idx;
    vq->num_free++;

    if (!(flags & VIRTQ_DESC_F_NEXT)) {
      return;
    }
    idx = next;
  }
}

int virtqueue_setup(uintptr_t base, uint16_t queue_idx, struct virtqueue *vq,
                    struct virtio_pci_caps *caps) {
  /* Disable MSI-X for config changes (polling mode) */
  mmio_write16(base + VIRTIO_COMMON_MSIX, VIRTIO_MSI_NO_VECTOR);
  dsb_sy();

  /* Select queue */
  mmio_write16(base + VIRTIO_COMMON_Q_SELECT, queue_idx);
  dsb_sy();

  uint16_t max_size = mmio_read16(base + VIRTIO_COMMON_Q_SIZE);
  uart_printf("[VQ] Queue %d max size: %d\n", (uint32_t)queue_idx,
              (uint32_t)max_size);

  if (max_size == 0) {
    uart_errorln("[VQ] Queue not available");
    return EERROR;
  }

  uint16_t qsize = VIRTQ_MAX_SIZE;
  if (qsize > max_size)
    qsize = max_size;

  mmio_write16(base + VIRTIO_COMMON_Q_SIZE, qsize);

  /* Disable MSI-X for this queue (polling) */
  mmio_write16(base + VIRTIO_COMMON_Q_MSIX, VIRTIO_MSI_NO_VECTOR);
  dsb_sy();

  memset(vq->desc, 0, sizeof(struct virtq_desc) * qsize);
  memset(vq->avail, 0, sizeof(struct virtq_avail));
  memset(vq->used, 0, sizeof(struct virtq_used));

  /* Give physical addresses to device (as two 32-bit halves).
   * The pointers in vq are kernel VAs — convert to PA for DMA. */
  uint64_t desc_pa = VIRT_TO_PHYS((uint64_t)(uintptr_t)vq->desc);
  mmio_write32(base + VIRTIO_COMMON_Q_DESCLO, (uint32_t)(desc_pa & 0xFFFFFFFF));
  mmio_write32(base + VIRTIO_COMMON_Q_DESCHI, (uint32_t)(desc_pa >> 32));

  uint64_t avail_pa = VIRT_TO_PHYS((uint64_t)(uintptr_t)vq->avail);
  mmio_write32(base + VIRTIO_COMMON_Q_DRIVERLO,
               (uint32_t)(avail_pa & 0xFFFFFFFF));
  mmio_write32(base + VIRTIO_COMMON_Q_DRIVERHI, (uint32_t)(avail_pa >> 32));

  uint64_t used_pa = VIRT_TO_PHYS((uint64_t)(uintptr_t)vq->used);
  mmio_write32(base + VIRTIO_COMMON_Q_DEVICELO,
               (uint32_t)(used_pa & 0xFFFFFFFF));
  mmio_write32(base + VIRTIO_COMMON_Q_DEVICEHI, (uint32_t)(used_pa >> 32));
  dsb_sy();

  /* Compute notification address for this queue */
  uint16_t notify_off = mmio_read16(base + VIRTIO_COMMON_Q_NOFF);
  vq->notify_addr =
      caps->notify_base + (notify_off * caps->notify_off_multiplier);

  uart_printf("[VQ] Notify offset=%d addr=%x\n", (uint32_t)notify_off,
              (uint64_t)vq->notify_addr);

  vq->size = qsize;
  vq->last_used = 0;
  virtqueue_init_free_list(vq);

  /* Enable queue */
  mmio_write16(base + VIRTIO_COMMON_Q_ENABLE, 1);
  dsb_sy();

  uart_printf("[VQ] Queue %d enabled (size=%d)\n", (uint32_t)queue_idx,
              (uint32_t)qsize);
  return ESUCCESS;
}

uint16_t virtqueue_submit(struct virtqueue *vq, uint64_t buf_pa, uint32_t len,
                          uint16_t flags) {
  uint16_t idx = virtqueue_alloc_desc(vq);
  if (idx == VIRTQ_NO_DESC) {
    uart_errorln("[VQ] submit: queue full");
    return VIRTQ_NO_DESC;
  }

  vq->desc[idx].addr = buf_pa;
  vq->desc[idx].len = len;
  vq->desc[idx].flags = flags & ~VIRTQ_DESC_F_NEXT;
  vq->desc[idx].next = 0;

  uint16_t avail_idx = vq->avail->idx;
  vq->avail->ring[avail_idx % vq->size] = idx;
  dsb_sy();

  vq->avail->idx = avail_idx + 1;
  dsb_sy();

  return idx;
}

void virtqueue_notify(struct virtqueue *vq) {
  // VirtIO 1.x §4.1.4.4 mandates a 16-bit write to the notify register
  // (without VIRTIO_F_NOTIFICATION_DATA). The device looks up the queue
  // from the notify_addr's offset, so the data value isn't actually used
  // here — the *width* is what the spec requires.
  mmio_write16(vq->notify_addr, 0);
}

// Upper bound on busy-wait iterations before assuming the device is wedged.
// Virtio operations on QEMU complete within microseconds; 10 M iterations
// is a multi-millisecond grace period before we give up.
#define VIRTQUEUE_POLL_MAX_SPINS 10000000u

int virtqueue_get_used(struct virtqueue *vq, uint16_t *id, uint32_t *len) {
  if (*(volatile uint16_t *)&vq->used->idx == vq->last_used) {
    return 0; /* nothing completed */
  }
  /* Order the used->idx load before reading the ring entry it published. */
  dsb_sy();

  uint16_t slot = vq->last_used % vq->size;
  uint32_t head = vq->used->ring[slot].id;
  uint32_t written = vq->used->ring[slot].len;
  vq->last_used++;

  if (head >= vq->size) {
    uart_errorln("[VQ] get_used: device returned out-of-range descriptor id");
    return 0;
  }

  /* Release the whole chain the device just finished with. */
  virtqueue_free_chain(vq, (uint16_t)head);

  if (id) {
    *id = (uint16_t)head;
  }
  if (len) {
    *len = written;
  }
  return 1;
}

uint32_t virtqueue_poll(struct virtqueue *vq) {
  /* Busy wait until the device produces a used entry, with a timeout so a
   * wedged device cannot hang the kernel forever. */
  uint32_t spins = 0;
  uint32_t written = 0;

  for (;;) {
    if (virtqueue_get_used(vq, 0, &written)) {
      return written;
    }
    if (++spins >= VIRTQUEUE_POLL_MAX_SPINS) {
      uart_errorln("[VQ] virtqueue_poll: timeout waiting for device");
      return 0;
    }
  }
}

uint16_t virtqueue_submit_chain(struct virtqueue *vq,
                                const struct virtq_seg *segs, uint16_t n) {
  if (n == 0 || n > vq->num_free) {
    /* Not enough descriptors: fail cleanly without allocating any, so the
     * queue is never left holding a half-built chain. */
    uart_errorln("[VQ] submit_chain: queue full");
    return VIRTQ_NO_DESC;
  }

  uint16_t idx[VIRTQ_MAX_SIZE];
  for (uint16_t i = 0; i < n; i++) {
    idx[i] = virtqueue_alloc_desc(vq);
  }

  for (uint16_t i = 0; i < n; i++) {
    uint16_t flags = segs[i].flags;
    if (i < n - 1) {
      flags |= VIRTQ_DESC_F_NEXT;
    }

    vq->desc[idx[i]].addr = segs[i].pa;
    vq->desc[idx[i]].len = segs[i].len;
    vq->desc[idx[i]].flags = flags;
    vq->desc[idx[i]].next = (i < n - 1) ? idx[i + 1] : 0;
  }

  uint16_t head = idx[0];

  uint16_t avail_idx = vq->avail->idx;
  vq->avail->ring[avail_idx % vq->size] = head;
  dsb_sy();

  vq->avail->idx = avail_idx + 1;
  dsb_sy();

  return head;
}
