#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gdt.h"
#include "idt.h"

/* VGA Text Mode constants */
static const size_t VGA_WIDTH = 80;
static const size_t VGA_HEIGHT = 25;
uint16_t* terminal_buffer = (uint16_t*) 0xB8000;

/* Helper function to print a string to the screen */
void kernel_print(const char* message, size_t color, size_t row) {
    for (size_t i = 0; message[i] != '\0'; i++) {
        size_t index = row * VGA_WIDTH + i;
        terminal_buffer[index] = (uint16_t) message[i] | (uint16_t) color << 8;
    }
}

/* This function is called from assembly (isr0) when a divide by zero occurs */
void isr0_handler(void) {
    kernel_print("EXCEPTION: Divide by Zero Caught! Halting System.", 0x0C, 2); // 0x0C is Red text
}

void kernel_main(void) {
    /* Initialize GDT and IDT */
    init_gdt();
    init_idt();

    /* Clear the screen */
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t index = y * VGA_WIDTH + x;
            terminal_buffer[index] = (uint16_t) ' ' | (uint16_t) 0x0F << 8;
        }
    }

    kernel_print("OSISOS Kernel Booted Successfully!", 0x0F, 0);
    kernel_print("Triggering Divide by Zero...", 0x0E, 1); // 0x0E is Yellow text

    /* 
     * Deliberately trigger a Divide by Zero.
     * We use volatile to prevent GCC from optimizing it away at compile-time.
     */
    volatile int a = 1;
    volatile int b = 0;
    int c = a / b;
    (void)c; // Prevent unused variable warning
}
