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
    // Since kmalloc returns a pointer just past a 12-byte header (which is 4-byte aligned),
    // we must manually 16-byte align the pointer by allocating extra space!
    void* unaligned_tx = kmalloc(sizeof(struct e1000_tx_desc) * E1000_NUM_TX_DESC + 16);
    tx_descs = (struct e1000_tx_desc*) (((uint32_t)unaligned_tx + 15) & ~15);
    
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


static void init_rx(void) {
    void* unaligned_rx = kmalloc(sizeof(struct e1000_rx_desc) * E1000_NUM_RX_DESC + 16);
    rx_descs = (struct e1000_rx_desc*) (((uint32_t)unaligned_rx + 15) & ~15);
    
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        // Allocate a 2KB buffer for each packet. Align it for safety!
        void* unaligned_buf = kmalloc(2048 + 16);
        rx_descs[i].addr = (uint64_t)(uint32_t) (((uint32_t)unaligned_buf + 15) & ~15);
        rx_descs[i].status = 0;
    }
    
    // Initialize the Multicast Table Array (MTA)
    // The E1000 requires this 128-entry array to be zeroed, otherwise it may drop packets!
    for (int i = 0; i < 128; i++) {
        e1000_write_reg(0x5200 + (i * 4), 0);
    }
    
    e1000_write_reg(REG_RXDESCLO, (uint32_t)rx_descs);
    e1000_write_reg(REG_RXDESCHI, 0);
    e1000_write_reg(REG_RXDESCLEN, E1000_NUM_RX_DESC * 16);
    e1000_write_reg(REG_RXDESCHEAD, 0);
    e1000_write_reg(REG_RXDESCTAIL, E1000_NUM_RX_DESC - 1);
    
    // Enable RX (1), Unicast Promiscuous (3), Multicast Promiscuous (4), Broadcast Accept (15), Strip Ethernet CRC (26)
    e1000_write_reg(REG_RCTRL, (1 << 1) | (1 << 3) | (1 << 4) | (1 << 15) | (1 << 26));
}

bool init_e1000(uint32_t mmio_base) {
    mmio_addr = mmio_base;
    
    for (uint32_t i = 0; i < 32; i++) {
        map_page(mmio_base + (i * 4096), mmio_base + (i * 4096));
    }
    
    read_mac_address();
    
    uint32_t ctrl = e1000_read_reg(REG_CTRL);
    e1000_write_reg(REG_CTRL, ctrl | (1 << 6));
    
    init_tx();
    init_rx();
    
    return true;
}

static uint16_t rx_curr = 0;

void* e1000_receive_packet(uint16_t* out_length) {
    if (rx_descs[rx_curr].status & 0x01) {
        // A packet is here!
        *out_length = rx_descs[rx_curr].length;
        void* packet_buffer = (void*)(uint32_t)rx_descs[rx_curr].addr;
        
        // Reset the descriptor so the hardware can use it again
        rx_descs[rx_curr].status = 0;
        
        // Tell hardware we consumed it
        e1000_write_reg(REG_RXDESCTAIL, rx_curr);
        
        // Advance
        rx_curr = (rx_curr + 1) % E1000_NUM_RX_DESC;
        return packet_buffer;
    }
    return NULL; // No packet available
}

void e1000_send_packet(void* packet, uint16_t length) {
    tx_descs[tx_tail].addr = (uint64_t)(uint32_t)packet;
    tx_descs[tx_tail].length = length;
    tx_descs[tx_tail].cmd = (1 << 0) | (1 << 1) | (1 << 3); 
    tx_descs[tx_tail].status = 0; 
    
    uint16_t old_tail = tx_tail;
    tx_tail = (tx_tail + 1) % E1000_NUM_TX_DESC;
    
    e1000_write_reg(REG_TXDESCTAIL, tx_tail);
    
    // Do NOT wait for the hardware to finish sending (status & 1).
    // In QEMU multicast mode, this can sometimes hang if the link is not ready!
}
