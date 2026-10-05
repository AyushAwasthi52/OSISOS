#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "io.h"
#include "keyboard.h"

/* VGA Text Mode constants */
static const size_t VGA_WIDTH = 80;
static const size_t VGA_HEIGHT = 25;
uint16_t* terminal_buffer = (uint16_t*) 0xB8000;

/* Keep track of the cursor position globally */
size_t terminal_row = 3; 
size_t terminal_col = 0;

/* Helper function to print a full string at a specific row */
void kernel_print(const char* message, size_t color, size_t row) {
    for (size_t i = 0; message[i] != '\0'; i++) {
        size_t index = row * VGA_WIDTH + i;
        terminal_buffer[index] = (uint16_t) message[i] | (uint16_t) color << 8;
    }
}

/* 
 * Prints a single character to the screen and advances the cursor.
 * Handles newlines and basic backspacing!
 */
void terminal_putchar(char c) {
    if (c == '\n') {
        terminal_col = 0;
        terminal_row++;
    } else if (c == '\b') {
        if (terminal_col > 0) {
            terminal_col--;
            // Erase the character by printing a space over it
            size_t index = terminal_row * VGA_WIDTH + terminal_col;
            terminal_buffer[index] = (uint16_t) ' ' | (uint16_t) 0x0F << 8;
        }
    } else {
        size_t index = terminal_row * VGA_WIDTH + terminal_col;
        terminal_buffer[index] = (uint16_t) c | (uint16_t) 0x0F << 8;
        terminal_col++;
        // Wrap to the next line if we hit the edge
        if (terminal_col >= VGA_WIDTH) {
            terminal_col = 0;
            terminal_row++;
        }
    }

    // Basic wrap around if we hit the bottom of the screen (for now)
    if (terminal_row >= VGA_HEIGHT) {
        terminal_row = 3; 
    }
}

void isr0_handler(void) {
    kernel_print("EXCEPTION: Divide by Zero Caught! Halting System.", 0x0C, 2);
}

void keyboard_handler(void) {
    uint8_t scancode = inb(0x60);

    /* Convert the raw scancode to an ASCII character */
    char ascii = keyboard_scancode_to_ascii(scancode);
    
    /* If it is a printable character (not 0), print it to the screen! */
    if (ascii != 0) {
        terminal_putchar(ascii);
    }

    /* End of Interrupt */
    outb(0x20, 0x20);
}

void kernel_main(void) {
    init_gdt();
    init_idt();
    pic_remap();

    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t index = y * VGA_WIDTH + x;
            terminal_buffer[index] = (uint16_t) ' ' | (uint16_t) 0x0F << 8;
        }
    }

    kernel_print("OSISOS Kernel Booted Successfully!", 0x0F, 0);
    kernel_print("Keyboard driver loaded. Type freely below!", 0x0B, 1);

    __asm__ volatile ("sti");

    while (1) {
        __asm__ volatile ("hlt");
    }
}
