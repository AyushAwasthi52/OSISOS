#ifndef IO_H
#define IO_H

#include <stdint.h>

/*
 * outb: Sends an 8-bit value to a specific hardware I/O port.
 * The CPU uses a special 'out' instruction to talk to motherboard chips.
 */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ( "outb %0, %1" : : "a"(val), "Nd"(port) );
}

/*
 * inb: Reads an 8-bit value from a specific hardware I/O port.
 * The CPU uses a special 'in' instruction to read from motherboard chips.
 */
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ( "inb %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

#endif
