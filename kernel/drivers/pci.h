#ifndef PCI_H
#define PCI_H

#include <stdint.h>
#include <stdbool.h>

/* PCI Configuration Space Ports */
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

/* E1000 Intel Gigabit Ethernet IDs */
#define E1000_VENDOR_ID 0x8086
#define E1000_DEVICE_ID 0x100E

/* Structure to hold information about the detected E1000 card */
typedef struct {
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint32_t mmio_base;
} pci_device_t;

/* Global E1000 device structure */
extern pci_device_t e1000_device;
extern bool e1000_found;

/* Initialize the PCI bus and scan for the E1000 Network Card */
void init_pci(void);

/* Read a 32-bit register from the PCI Configuration Space */
uint32_t pci_read_config_32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

/* Write a 32-bit register to the PCI Configuration Space */
void pci_write_config_32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

#endif
