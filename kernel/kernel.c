#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "io.h"

/* VGA Text Mode constants */
static const size_t VGA_WIDTH = 80;
static const size_t VGA_HEIGHT = 25;
uint16_t* terminal_buffer = (uint16_t*) 0xB8000;
size_t terminal_row = 3; // Keep track of which row we are printing on

/* Helper function to print a string to the screen */
void kernel_print(const char* message, size_t color, size_t row) {
    for (size_t i = 0; message[i] != '\0'; i++) {
        size_t index = row * VGA_WIDTH + i;
        terminal_buffer[index] = (uint16_t) message[i] | (uint16_t) color << 8;
    }
}

/* This function is called from assembly (isr0) when a divide by zero occurs */
void isr0_handler(void) {
    kernel_print("EXCEPTION: Divide by Zero Caught! Halting System.", 0x0C, 2);
}

/* 
 * This function is called from assembly (isr33) when a key is pressed or released.
 */
void keyboard_handler(void) {
    /* The keyboard sends its data to I/O port 0x60 */
    uint8_t scancode = inb(0x60);

    /* Very basic check to ensure we only print when a key is PRESSED (not released) */
    if (!(scancode & 0x80)) {
        kernel_print("A Key was Pressed!", 0x0A, terminal_row); // 0x0A is Light Green
        
        // Move to the next row, and wrap around if we hit the bottom
        terminal_row++;
        if (terminal_row >= VGA_HEIGHT) {
            terminal_row = 3;
        }
    }

    /* We MUST tell the PIC that we finished handling the interrupt, or it will stop sending them. */
    /* Command 0x20 is End of Interrupt (EOI). We send it to the Master PIC's command port (0x20). */
    outb(0x20, 0x20);
}

void kernel_main(void) {
    /* 1. Initialize Memory Layout (GDT) */
    init_gdt();
    
    /* 2. Initialize Interrupt Table (IDT) */
    init_idt();
    
    /* 3. Remap the PIC so hardware interrupts don't collide with exceptions */
    pic_remap();

    /* Clear the screen */
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t index = y * VGA_WIDTH + x;
            terminal_buffer[index] = (uint16_t) ' ' | (uint16_t) 0x0F << 8;
        }
    }

    kernel_print("OSISOS Kernel Booted Successfully!", 0x0F, 0);
    kernel_print("PIC Remapped. Waiting for keyboard input...", 0x0B, 1); // 0x0B is Cyan

    /* 
     * 4. Enable Hardware Interrupts! 
     * `sti` (Set Interrupt Flag) tells the CPU to start listening to the PIC.
     */
    __asm__ volatile ("sti");

    /* 
     * 5. Infinite loop. The CPU will just spin here, and jump to `keyboard_handler`
     * instantly whenever a key is pressed, then return here.
     */
    while (1) {
        __asm__ volatile ("hlt"); // hlt puts CPU to sleep until next interrupt saves power
    }
}
