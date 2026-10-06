#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>

/* The C handler for all system calls */
uint32_t syscall_handler(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx);

/* 
 * A convenient wrapper function that User Mode programs can call 
 * to trigger a system call from C code.
 */
static inline uint32_t syscall(uint32_t sys_num, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a" (ret)
                     : "a" (sys_num), "b" (arg1), "c" (arg2), "d" (arg3)
                     : "memory"); /* Tell GCC that memory might be read/written! */
    return ret;
}

#endif
