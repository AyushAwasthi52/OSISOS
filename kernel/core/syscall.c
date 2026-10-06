#include "syscall.h"

// External kernel functions we want to expose to User Mode
extern void kernel_print(const char* message, size_t color, size_t row);

/*
 * The global System Call Handler.
 * When a User Mode program executes `int 0x80`, the CPU drops into Ring 0
 * and lands here. The arguments are passed in registers.
 */
uint32_t syscall_handler(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx) {
    /* Print a Green 'K' to prove we entered the Kernel Syscall Handler! */
    uint16_t* vga = (uint16_t*) 0xB8000;
    vga[15 * 80 + 6] = 'K' | 0x0A00;

    switch(eax) {
        case 1: 
            /* 
             * System Call 1: Print to Screen
             * ebx = (const char*) message
             * ecx = color
             * edx = row
             */
            kernel_print((const char*)ebx, ecx, edx);
            return 0; // Success
            
        case 2:
            /* 
             * System Call 2: Ping
             * Returns 42 to prove it works
             */
            return 42;
            
        default:
            /* Unknown System Call */
            return -1;
    }
}
