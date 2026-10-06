#include "e1000.h"
#include "kheap.h"
#include "paging.h"

uint8_t e1000_mac[6];
static uint32_t mmio_addr;

struct e1000_tx_desc* tx_descs;
struct e1000_rx_desc* rx_descs;
uint16_t tx_tail = 0;

/* Read from E1000 MMIO Register */
static uint32_t e1000_read_reg(uint16_t offset) {
    return *((volatile uint32_t*)(mmio_addr + offset));
}

/* Write to E1000 MMIO Register */
static void e1000_write_reg(uint16_t offset, uint32_t value) {
    *((volatile uint32_t*)(mmio_addr + offset)) = value;
}

/* Read MAC address from the EEPROM */
static void read_mac_address(void) {
    uint32_t eeprom_reg = e1000_read_reg(REG_EEPROM);
    
    // Some E1000s don't have an EEPROM and just expose the MAC at 0x5400
    // QEMU's E1000 puts the MAC at 0x5400 (RAL/RAH)
    uint32_t mac_low = e1000_read_reg(0x5400);
    uint32_t mac_high = e1000_read_reg(0x5404);
    
    e1000_mac[0] = mac_low & 0xFF;
    e1000_mac[1] = (mac_low >> 8) & 0xFF;
    e1000_mac[2] = (mac_low >> 16) & 0xFF;
    e1000_mac[3] = (mac_low >> 24) & 0xFF;
    e1000_mac[4] = mac_high & 0xFF;
    e1000_mac[5] = (mac_high >> 8) & 0xFF;
}

static void init_tx(void) {
    // Allocate 16-byte aligned memory for the descriptors
    tx_descs = (struct e1000_tx_desc*) kmalloc(sizeof(struct e1000_tx_desc) * E1000_NUM_TX_DESC);
    
    // Initialize all descriptors
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        tx_descs[i].addr = 0;
        tx_descs[i].cmd = 0;
        tx_descs[i].status = 1; // Mark as done initially
    }
    
    // Set the Base Address of the TX ring
    e1000_write_reg(REG_TXDESCLO, (uint32_t)tx_descs);
    e1000_write_reg(REG_TXDESCHI, 0);
    
    // Set Length (bytes)
    e1000_write_reg(REG_TXDESCLEN, E1000_NUM_TX_DESC * 16);
    
    // Set Head and Tail
    e1000_write_reg(REG_TXDESCHEAD, 0);
    e1000_write_reg(REG_TXDESCTAIL, 0);
    
    // Enable TX, pad short packets, append CRC, 15 retries
    e1000_write_reg(REG_TCTRL, (1 << 1) | (1 << 3) | (0x0F << 4) | (0x3F << 12));
}

bool init_e1000(uint32_t mmio_base) {
    mmio_addr = mmio_base;
    
    // Map the MMIO physical address into our Virtual Page Tables!
    // The E1000 MMIO space is 128KB, so we need to map 32 pages (32 * 4096 = 131072)
    for (uint32_t i = 0; i < 32; i++) {
        map_page(mmio_base + (i * 4096), mmio_base + (i * 4096));
    }
    
    // Read the MAC address
    read_mac_address();
    
    // Turn on the Link
    uint32_t ctrl = e1000_read_reg(REG_CTRL);
    e1000_write_reg(REG_CTRL, ctrl | (1 << 6)); // Set Link Up
    
    // Initialize Transmit
    init_tx();
    
    return true;
}

void e1000_send_packet(void* packet, uint16_t length) {
    // Place the packet physical address in the next TX descriptor
    tx_descs[tx_tail].addr = (uint64_t)(uint32_t)packet;
    tx_descs[tx_tail].length = length;
    
    // Command: End of Packet (1), Insert FCS (2), Report Status (8)
    tx_descs[tx_tail].cmd = (1 << 0) | (1 << 1) | (1 << 3); 
    tx_descs[tx_tail].status = 0; // Clear the done status
    
    // Advance the tail pointer
    uint16_t old_tail = tx_tail;
    tx_tail = (tx_tail + 1) % E1000_NUM_TX_DESC;
    
    // Tell the hardware that there is a new packet ready to send!
    e1000_write_reg(REG_TXDESCTAIL, tx_tail);
    
    // Wait until the hardware marks the packet as sent (status bit 0 becomes 1)
    while (!(tx_descs[old_tail].status & 1));
}
