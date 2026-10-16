#include "e1000.h"
#include "kheap.h"
#include "paging.h"

uint8_t e1000_mac[6];
static uint32_t mmio_addr;

struct e1000_tx_desc* tx_descs;
struct e1000_rx_desc* rx_descs;
uint16_t tx_tail = 0;
static uint8_t* tx_bufs[E1000_NUM_TX_DESC];   /* driver-owned copy of each in-flight frame */

#include <stddef.h>
extern void* memcpy(void* dst, const void* src, size_t n);

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
        void* b = kmalloc(2048 + 16);
        tx_bufs[i] = b ? (uint8_t*) (((uint32_t)b + 15) & ~15) : 0;
        tx_descs[i].addr = (uint64_t)(uint32_t) tx_bufs[i];
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
        rx_descs[i].addr = unaligned_buf ? (uint64_t)(uint32_t) (((uint32_t)unaligned_buf + 15) & ~15) : 0;
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

    /* kmalloc() returns NULL when the heap is exhausted: refuse to run DMA on address 0 */
    if (!tx_descs || !rx_descs) return false;
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) if (!tx_bufs[i]) return false;
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) if (!rx_descs[i].addr) return false;

    return true;
}

static uint16_t rx_curr = 0;

void* e1000_receive_packet(uint16_t* out_length) {
    while (rx_descs[rx_curr].status & 0x01) {
        uint16_t idx = rx_curr;
        uint8_t status = rx_descs[idx].status;
        uint8_t errors = rx_descs[idx].errors;
        uint16_t len = rx_descs[idx].length;
        void* packet_buffer = (void*)(uint32_t)rx_descs[idx].addr;

        rx_descs[idx].status = 0;
        e1000_write_reg(REG_RXDESCTAIL, idx);   /* hands back everything up to (not incl.) idx */
        rx_curr = (rx_curr + 1) % E1000_NUM_RX_DESC;

        /* Only whole, error-free frames. The buffer stays valid until the NEXT call,
         * because descriptor idx is not given back to the hardware until then. */
        if ((status & 0x02) && errors == 0) {
            *out_length = len;
            return packet_buffer;
        }
    }
    return NULL;
}

void e1000_send_packet(void* packet, uint16_t length) {
    if (length > 2048) return;

    /* Wait (bounded) until the hardware is done with this descriptor. */
    for (uint32_t spin = 0; spin < 1000000 && !(tx_descs[tx_tail].status & 0x01); spin++);

    /* Copy: the caller may reuse its buffer as soon as we return. */
    memcpy(tx_bufs[tx_tail], packet, length);
    tx_descs[tx_tail].addr = (uint64_t)(uint32_t) tx_bufs[tx_tail];
    tx_descs[tx_tail].length = length;
    tx_descs[tx_tail].cmd = (1 << 0) | (1 << 1) | (1 << 3);   /* EOP | IFCS | RS */
    tx_descs[tx_tail].status = 0;

    tx_tail = (tx_tail + 1) % E1000_NUM_TX_DESC;
    e1000_write_reg(REG_TXDESCTAIL, tx_tail);
}
