#include "pci.h"
#include "io.h"

pci_device_t e1000_device;
bool e1000_found = false;

uint32_t pci_read_config_32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address;
    uint32_t lbus  = (uint32_t)bus;
    uint32_t lslot = (uint32_t)slot;
    uint32_t lfunc = (uint32_t)func;
    
    // Create the PCI Configuration Address
    // Bit 31 must be 1 (Enable bit)
    // Bits 23-16: Bus
    // Bits 15-11: Slot (Device)
    // Bits 10-8: Function
    // Bits 7-0: Offset (Register)
    address = (uint32_t)((lbus << 16) | (lslot << 11) | (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    
    // Write out the address
    outl(PCI_CONFIG_ADDRESS, address);
    
    // Read the data
    return inl(PCI_CONFIG_DATA);
}

void pci_write_config_32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address;
    uint32_t lbus  = (uint32_t)bus;
    uint32_t lslot = (uint32_t)slot;
    uint32_t lfunc = (uint32_t)func;
    
    address = (uint32_t)((lbus << 16) | (lslot << 11) | (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

void init_pci(void) {
    // Scan all buses, slots, and functions (Naive brute-force scan)
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                
                // Read Vendor ID and Device ID (Offset 0x00)
                uint32_t vendor_device = pci_read_config_32(bus, slot, func, 0x00);
                
                uint16_t vendor_id = vendor_device & 0xFFFF;
                uint16_t device_id = vendor_device >> 16;
                
                // 0xFFFF means there is no device at this slot
                if (vendor_id == 0xFFFF) {
                    if (func == 0) {
                        // If function 0 is empty, the entire slot is empty
                        break;
                    }
                    continue; // Check other functions
                }
                
                // Is it our glorious Intel E1000 Gigabit Ethernet card?
                if (vendor_id == E1000_VENDOR_ID && device_id == E1000_DEVICE_ID) {
                    e1000_found = true;
                    e1000_device.bus = bus;
                    e1000_device.slot = slot;
                    e1000_device.func = func;
                    
                    // The Memory Mapped I/O Base Address is found in BAR0 (Base Address Register 0)
                    // BAR0 is located at offset 0x10
                    uint32_t bar0 = pci_read_config_32(bus, slot, func, 0x10);
                    
                    // Mask out the lower 4 bits (they are status bits) to get the true physical address
                    e1000_device.mmio_base = bar0 & 0xFFFFFFF0;
                    
                    // Enable Bus Mastering (Allows the network card to use DMA to read/write RAM without the CPU)
                    // Command Register is at offset 0x04. We need to set bit 2 (Bus Master Enable)
                    uint32_t command_reg = pci_read_config_32(bus, slot, func, 0x04);
                    command_reg |= (1 << 2); 
                    pci_write_config_32(bus, slot, func, 0x04, command_reg);
                    
                    return; // Found it, stop scanning!
                }
            }
        }
    }
}
