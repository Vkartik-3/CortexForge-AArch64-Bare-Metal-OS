#ifndef GIC_H
#define GIC_H

#include <stdint.h>

#define GICD_BASE 0x08000000UL
#define GICR_BASE 0x080A0000UL

#define GICD_CTLR (GICD_BASE + 0x0000)
#define GICD_ISENABLER (GICD_BASE + 0x0100)
#define GICD_ICENABLER (GICD_BASE + 0x0180) /* write-1-to-clear enable */
#define GICD_IGROUPR (GICD_BASE + 0x0080)   /* 1 bit/INTID: 1 = Group 1     */
#define GICD_IGRPMODR (GICD_BASE + 0x0D00)  /* 1 bit/INTID: 0 = Non-secure  */
#define GICD_IROUTER (GICD_BASE + 0x6000)   /* 64 bits/INTID (SPIs, ARE on) */

#define GICD_CTLR_ENABLE_G1NS (1U << 1)
#define GICD_CTLR_ARE_NS (1U << 4)

#define GICR_WAKER (GICR_BASE + 0x0014)
#define GICR_SGI_BASE (GICR_BASE + 0x10000)
#define GICR_IGROUPR0 (GICR_SGI_BASE + 0x0080)
#define GICR_IGRPMODR0 (GICR_SGI_BASE + 0x0D00)
#define GICR_ISENABLER0 (GICR_SGI_BASE + 0x0100)
#define GICR_ICENABLER0 (GICR_SGI_BASE + 0x0180)
#define GICR_WAKER_PROCESSOR_SLEEP (1U << 1)
#define GICR_WAKER_CHILDREN_ASLEEP (1U << 2)

#define GIC_INTID_NO_PENDING 1023

void gic_init(void);
void gic_enable_irq(uint32_t intid);

/* Mask an INTID at the GIC. ICENABLER is write-1-to-clear: writing a 1 to a
 * bit disables that interrupt and writing 0 has no effect, so this is a plain
 * write of the single bit, NOT a read-modify-write. */
void gic_disable_irq(uint32_t intid);
uint64_t gic_ack_irq(void);
void gic_end_irq(uint64_t intid);

/* Per-INTID interrupt counters — surfaced by /proc/interrupts.
 * gic_count_irq() is called from the IRQ dispatch path after the
 * acknowledgement so spurious 1023 entries do not contribute. */
void gic_count_irq(uint32_t intid);

/* Render a /proc-style table of INTID -> count -> source string into buf.
 * Returns bytes written. */
int gic_render_interrupts(char *buf, uint32_t buflen);

#endif
