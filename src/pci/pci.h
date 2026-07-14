#ifndef PCI_H
#define PCI_H

#include <stdint.h>

/*
 * QEMU virt machine PCI layout:
 *   ECAM base:       0x4010000000 (256 buses)
 *   PIO window:      0x3eff0000 (64K)
 *   32-bit MMIO:     0x10000000 - 0x3efeffff
 *   64-bit MMIO:     0x8000000000 - 0xffffffffff
 */
// Physical addresses (from QEMU device tree)
#define PCI_ECAM_PHYS 0x4010000000UL
#define PCI_MMIO32_PHYS 0x10000000UL
#define PCI_MMIO32_LIMIT 0x3EFEFFFFUL
#define PCI_MMIO64_PHYS 0x8000000000UL
#define PCI_MMIO64_LIMIT 0xFFFFFFFFFFUL

#define MAX_PCI_DEVICES 16

#define MAX_PCI_BUS 256
#define MAX_PCI_SLOT 32
#define MAX_PCI_FUNC 8

/* PCI Config Space Offsets */
#define PCI_VENDOR_ID 0x00
#define PCI_DEVICE_ID 0x02
#define PCI_COMMAND 0x04
#define PCI_STATUS 0x06
#define PCI_HEADER_TYPE 0x0E
#define PCI_BAR0 0x10
#define PCI_CAP_PTR 0x34
#define PCI_INTERRUPT_LINE 0x3C
#define PCI_INTERRUPT_PIN 0x3D /* 1=INTA, 2=INTB, 3=INTC, 4=INTD, 0=none */

/* PCI command register bits */
#define PCI_CMD_INTX_DISABLE (1 << 10)

/* Capability IDs (PCI spec) */
#define PCI_CAP_ID_MSI 0x05
#define PCI_CAP_ID_VENDOR 0x09 /* virtio uses vendor-specific caps */
#define PCI_CAP_ID_MSIX 0x11

#define PCI_ENDPOINT_DEV_TYPE 0x00

struct pci_device {
  uint8_t bus;
  uint8_t slot;
  uint8_t func;

  uint16_t vendor_id;
  uint16_t device_id;

  uintptr_t bar_addr[6];
};

void pci_enumerate_bus(void);

uint32_t pci_config_read32(uint16_t bus, uint8_t slot, uint8_t func,
                           uint16_t offset);
uint16_t pci_config_read16(uint16_t bus, uint8_t slot, uint8_t func,
                           uint16_t offset);
uint8_t pci_config_read8(uint16_t bus, uint8_t slot, uint8_t func,
                         uint16_t offset);

void pci_config_write32(uint16_t bus, uint8_t slot, uint8_t func,
                        uint16_t offset, uint32_t val);
void pci_config_write16(uint16_t bus, uint8_t slot, uint8_t func,
                        uint16_t offset, uint16_t val);
void pci_config_write8(uint16_t bus, uint8_t slot, uint8_t func,
                       uint16_t offset, uint8_t val);

int pci_find_device(uint16_t vendor_id, uint16_t device_id,
                    struct pci_device *pci_device);

uint8_t pci_get_header_type(struct pci_device *dev);
void pci_assign_bars(struct pci_device *dev);
void pci_enable_device(struct pci_device *dev);

/* Walk the capability list and return the config-space offset of the first
 * capability with the given ID, or 0 if the device does not advertise it. */
uint8_t pci_find_capability(struct pci_device *dev, uint8_t cap_id);

/* Log every capability the device advertises (ID + offset). */
void pci_dump_capabilities(struct pci_device *dev);

/* Enable legacy INTx delivery: clear the INTX_DISABLE bit in the command
 * register. (MSI-X, when unused, must stay disabled or the device would send
 * message interrupts instead.) */
void pci_enable_intx(struct pci_device *dev);

/*
 * pci_intx_intid — GIC INTID for this device's legacy interrupt pin on the
 * QEMU virt machine.
 *
 * The DT interrupt-map masks the device number to its low 2 bits and swizzles
 * it with the pin:  SPI = 3 + ((slot + pin - 1) % 4),  giving SPIs 3..6.
 * A GIC SPI n is INTID 32 + n, so virt's PCI INTx lines land on INTID 35..38.
 * Returns 0 if the device reports no interrupt pin.
 */
uint32_t pci_intx_intid(struct pci_device *dev);

#endif