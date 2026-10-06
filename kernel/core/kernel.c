#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "io.h"
#include "keyboard.h"
#include "timer.h"
#include "multiboot.h"
#include "pmm.h"
#include "paging.h"
#include "kheap.h"
#include "task.h"

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

/* Prints a single character and advances the cursor */
void terminal_putchar(char c) {
    if (c == '\n') {
        terminal_col = 0;
        terminal_row++;
    } else if (c == '\b') {
        if (terminal_col > 0) {
            terminal_col--;
            size_t index = terminal_row * VGA_WIDTH + terminal_col;
            terminal_buffer[index] = (uint16_t) ' ' | (uint16_t) 0x0F << 8;
        }
    } else {
        size_t index = terminal_row * VGA_WIDTH + terminal_col;
        terminal_buffer[index] = (uint16_t) c | (uint16_t) 0x0F << 8;
        terminal_col++;
        if (terminal_col >= VGA_WIDTH) {
            terminal_col = 0;
            terminal_row++;
        }
    }
    if (terminal_row >= VGA_HEIGHT) {
        terminal_row = 3; 
    }
}

/* Helper to print strings sequentially */
void terminal_print_string(const char* str) {
    for (size_t i = 0; str[i] != '\0'; i++) {
        terminal_putchar(str[i]);
    }
}

/* Helper to convert an integer to a Hexadecimal string and print it */
void terminal_print_hex(uint32_t val) {
    terminal_print_string("0x");
    char buffer[9];
    buffer[8] = '\0';
    for (int i = 7; i >= 0; i--) {
        uint8_t nibble = val & 0x0F;
        if (nibble < 10) buffer[i] = '0' + nibble;
        else buffer[i] = 'A' + (nibble - 10);
        val >>= 4;
    }
    terminal_print_string(buffer);
}

/* Helper to convert an integer to a decimal string and print it */
void terminal_print_dec(uint32_t val) {
    if (val == 0) {
        terminal_putchar('0');
        return;
    }
    char buffer[11]; // Max uint32_t is 4294967295 (10 digits) + null terminator
    int i = 9;
    buffer[10] = '\0';
    while (val > 0) {
        buffer[i] = '0' + (val % 10);
        val /= 10;
        i--;
    }
    terminal_print_string(&buffer[i + 1]);
}

void isr0_handler(void) {
    kernel_print("EXCEPTION: Divide by Zero Caught! Halting System.", 0x0C, 2);
}

void keyboard_handler(void) {
    uint8_t scancode = inb(0x60);
    char ascii = keyboard_scancode_to_ascii(scancode);
    if (ascii != 0) {
        terminal_putchar(ascii);
    }
    outb(0x20, 0x20);
}

void task2_main(void) {
    char anim[] = {'|', '/', '-', '\\'};
    int i = 0;
    while(1) {
        /* Task 2 will independently control a Yellow Spinner in the bottom right corner */
        uint16_t* vga = (uint16_t*) 0xB8000;
        vga[24 * 80 + 78] = (uint16_t) anim[i] | 0x0E00; // Yellow text
        i = (i + 1) % 4;
        
        /* Slow it down */
        for(volatile int d = 0; d < 5000000; d++); 
        
        /* NOTE: We removed task_yield() entirely! Task 2 refuses to give up the CPU. */
    }
}

/* kernel_main now takes the parameters we pushed in boot.S */
void kernel_main(uint32_t magic, uint32_t multiboot_addr) {
    init_gdt();
    init_idt();
    pic_remap();
    init_timer(100);

    /* Clear the screen */
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t index = y * VGA_WIDTH + x;
            terminal_buffer[index] = (uint16_t) ' ' | (uint16_t) 0x0F << 8;
        }
    }

    kernel_print("OSISOS Kernel Booted Successfully!", 0x0F, 0);

    /* Verify that we were booted by a Multiboot-compliant bootloader (like GRUB) */
    if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        kernel_print("CRITICAL ERROR: Invalid Multiboot Magic Number!", 0x0C, 1);
        return; // Halt system
    }

    /* Cast the memory address GRUB gave us into our C structure pointer */
    multiboot_info_t* mbd = (multiboot_info_t*) multiboot_addr;

    /* Print the total system RAM */
    terminal_print_string("Total System RAM Installed: ");
    
    uint32_t total_memory_kb = mbd->mem_lower + mbd->mem_upper;
    uint32_t total_memory_mb = total_memory_kb / 1024;
    
    terminal_print_dec(total_memory_mb);
    terminal_print_string(" MB\n\n");

    /* Initialize the Physical Memory Manager */
    init_pmm(total_memory_kb);
    terminal_print_string("PMM Initialized. Reserved first 4MB for Kernel.\n");

    /* Initialize Virtual Memory (Paging) */
    init_paging();
    terminal_print_string("Paging Enabled! Virtual Memory is now active.\n");

    /* Initialize the Kernel Heap Allocator */
    init_kheap();
    terminal_print_string("Kernel Heap (kmalloc) Initialized.\n");
    
    /* Initialize Tasking (Establish PID 1) */
    init_tasking();
    terminal_print_string("Tasking Initialized. Kernel is now running as PID 1.\n\n");

    /* Spawn our very first new Task! */
    create_task(task2_main);
    terminal_print_string("Spawned Task 2 (PID 2) successfully!\n\n");
    terminal_print_string("Preemptive Multitasking Active! Look at the bottom right corner.");

    /* Enable interrupts */
    __asm__ volatile ("sti");

    char anim[] = {'|', '/', '-', '\\'};
    int i = 0;
    while (1) {
        /* Task 1 will independently control a Green Spinner in the bottom right corner */
        uint16_t* vga = (uint16_t*) 0xB8000;
        vga[24 * 80 + 76] = (uint16_t) anim[i] | 0x0A00; // Light Green text
        i = (i + 1) % 4;
        
        /* Slow it down */
        for(volatile int d = 0; d < 5000000; d++); 
        
        /* NOTE: We removed task_yield() entirely! Task 1 refuses to give up the CPU. */
    }
}
