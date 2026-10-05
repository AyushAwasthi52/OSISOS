/* 
 * kernel.c - OSISOS Kernel Main
 * We use standard types from stdint.h/stddef.h which are provided by the 
 * compiler in a freestanding environment (they don't rely on the host OS).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* VGA Text Mode constants */
static const size_t VGA_WIDTH = 80;
static const size_t VGA_HEIGHT = 25;

/* 
 * kernel_main is called from boot.S.
 * The bootloader has placed us in 32-bit Protected Mode.
 */
void kernel_main(void) {
    /* 
     * The VGA text buffer is mapped directly to physical memory address 0xB8000.
     * We can create a pointer to this address and write to it to display text.
     */
    uint16_t* terminal_buffer = (uint16_t*) 0xB8000;

    /*
     * VGA text entries are 16 bits:
     * - The lower 8 bits are the ASCII character.
     * - The upper 8 bits are the color (e.g., 0x0F is white text on black background).
     */
    const char* message = "OSISOS Kernel Booted Successfully!";
    size_t color = 0x0F;

    /* Clear the screen first */
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t index = y * VGA_WIDTH + x;
            terminal_buffer[index] = (uint16_t) ' ' | (uint16_t) color << 8;
        }
    }

    /* Print our boot message */
    for (size_t i = 0; message[i] != '\0'; i++) {
        terminal_buffer[i] = (uint16_t) message[i] | (uint16_t) color << 8;
    }
}
