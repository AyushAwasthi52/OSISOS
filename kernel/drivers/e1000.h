#ifndef E1000_H
#define E1000_H

#include <stdint.h>
#include <stdbool.h>

/* E1000 Register Offsets */
#define REG_CTRL    0x0000
#define REG_STATUS  0x0008
#define REG_EEPROM  0x0014
#define REG_CTRL_EXT 0x0018
#define REG_RCTRL   0x0100
#define REG_RXDESCLO 0x2800
#define REG_RXDESCHI 0x2804
#define REG_RXDESCLEN 0x2808
#define REG_RXDESCHEAD 0x2810
#define REG_RXDESCTAIL 0x2818
#define REG_TCTRL   0x0400
#define REG_TXDESCLO 0x3800
#define REG_TXDESCHI 0x3804
#define REG_TXDESCLEN 0x3808
#define REG_TXDESCHEAD 0x3810
#define REG_TXDESCTAIL 0x3818

#define E1000_NUM_TX_DESC 8
#define E1000_NUM_RX_DESC 32

/* TX Descriptor */
struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    volatile uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed));

/* RX Descriptor */
struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    volatile uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed));

extern uint8_t e1000_mac[6];

/* Initialize the E1000 driver (Requires MMIO base address from PCI) */
bool init_e1000(uint32_t mmio_base);

/* Send a raw Ethernet frame */
void e1000_send_packet(void* packet, uint16_t length);

/* Receive a raw Ethernet frame (returns NULL if none available) */
void* e1000_receive_packet(uint16_t* out_length);

#endif
