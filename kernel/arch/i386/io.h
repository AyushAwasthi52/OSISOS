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

/* 16-bit word I/O for IDE Hard Drives */
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ( "outw %0, %1" : : "a"(val), "Nd"(port) );
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ( "inw %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

/* Read multiple 16-bit words at once (very fast) */
static inline void insw(uint16_t port, void* buffer, uint32_t count) {
    __asm__ volatile ( "rep insw" 
                       : "+D"(buffer), "+c"(count) 
                       : "d"(port) 
                       : "memory" );
}

/* 32-bit dword I/O for the PCI Bus */
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ( "outl %0, %1" : : "a"(val), "Nd"(port) );
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ( "inl %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

#endif
