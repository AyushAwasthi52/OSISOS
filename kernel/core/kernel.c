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
#include "ide.h"
#include "ofs.h"
#include "pci.h"
#include "e1000.h"

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

/* Scrolls the terminal up by one line */
void terminal_scroll(void) {
    // Move all lines up by one
    for (size_t y = 1; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            terminal_buffer[(y - 1) * VGA_WIDTH + x] = terminal_buffer[y * VGA_WIDTH + x];
        }
    }
    // Clear the last line
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        terminal_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = (uint16_t) ' ' | (uint16_t) 0x0F << 8;
    }
    terminal_row = VGA_HEIGHT - 1;
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
        } else if (terminal_row > 0) {
            terminal_row--;
            terminal_col = VGA_WIDTH - 1;
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
        terminal_scroll();
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
        // Push to buffer instead of printing directly!
        keyboard_push_char(ascii);
    }
    outb(0x20, 0x20);
}

#include "shell.h"
#include "syscall.h"

/* This function will run entirely in Ring 3 (User Space)! */
void user_program(void) {
    char anim[] = {'|', '/', '-', '\\'};
    int i = 0;
    
    /* We must mark buf as volatile, otherwise the C compiler will erase our writes to it! */
    volatile char buf[2] = {0, 0};
    
    while(1) {
        buf[0] = anim[i];
        
        /* SYSCALL 1: Print String (Pointer, Color, Row) */
        syscall(1, (uint32_t)buf, 0x0E, 24);
        
        i = (i + 1) % 4;
        
        /* Slow it down */
        for(volatile int d = 0; d < 5000000; d++); 
    }
}

void send_ping_packet(void) {
    if (!e1000_found) return;
    
    uint8_t* unaligned_eth = (uint8_t*) kmalloc(64 + 16);
    uint8_t* eth_frame = (uint8_t*) (((uint32_t)unaligned_eth + 15) & ~15);
    // Determine the exact MAC of the OTHER machine!
    eth_frame[0] = 0x52;
    eth_frame[1] = 0x54;
    eth_frame[2] = 0x00;
    eth_frame[3] = 0x12;
    eth_frame[4] = 0x34;
    // If I am .56, talk to .57. If I am .57, talk to .56!
    eth_frame[5] = (e1000_mac[5] == 0x56) ? 0x57 : 0x56;
    
    for(int j=0; j<6; j++) eth_frame[6+j] = e1000_mac[j]; // Source
    eth_frame[12] = 0x13; // Custom EtherType High
    eth_frame[13] = 0x37; // Custom EtherType Low
    
    const char* msg = "PING!";
    for(int j=0; j<6; j++) eth_frame[14+j] = msg[j]; // Payload
    
    e1000_send_packet(eth_frame, 64);
    terminal_print_string("Packet sent!\n");
}

/* 
 * This is Task 2! 
 * It runs in kernel mode (Ring 0) and handles incoming network packets.
 */
void network_task(void) {
    while(1) {
        if (e1000_found) {
            uint16_t rx_len;
            uint8_t* rx_packet = (uint8_t*) e1000_receive_packet(&rx_len);
            
            if (rx_packet != NULL) {
                // Check if it's our custom protocol
                if (rx_len >= 14 && rx_packet[12] == 0x13 && rx_packet[13] == 0x37) {
                    terminal_print_string("\n[Network] MSG: ");
                    // Print payload (starts at byte 14)
                    for (int p = 14; p < rx_len && rx_packet[p] != '\0'; p++) {
                        terminal_putchar((char)rx_packet[p]);
                    }
                    terminal_print_string("\n> ");
                }
            }
        }
        
        /* Yield to let shell run smoothly */
        for(volatile int d = 0; d < 100000; d++); 
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
    terminal_print_string("Tasking Initialized. Kernel is now running as PID 1.\n");

    /* Initialize IDE Hard Drive */
    if (init_ide()) {
        terminal_print_string("IDE Hard Drive detected and initialized successfully!\n");
        
        /* Initialize Virtual File System */
        if (!init_ofs()) {
            terminal_print_string("Disk not formatted. Formatting OSISOS-FS...\n");
            ofs_format();
            
            /* Write a test file! */
            if (ofs_write_file("hello.txt", "Welcome to the OSISOS File System!")) {
                terminal_print_string("Created 'hello.txt' successfully.\n");
            }
        } else {
            terminal_print_string("OSISOS-FS detected and loaded.\n");
        }
        
    } else {
        terminal_print_string("WARNING: No IDE Hard Drive detected!\n\n");
    }

    /* Initialize PCI Bus and scan for network cards */
    init_pci();
    if (e1000_found) {
        terminal_print_string("Intel E1000 Gigabit Ethernet Card detected via PCI!\n");
        terminal_print_string("MMIO Base Address: ");
        terminal_print_hex(e1000_device.mmio_base);
        terminal_print_string("\n");
        
        if (init_e1000(e1000_device.mmio_base)) {
            terminal_print_string("E1000 Initialized! MAC Address: ");
            for (int i = 0; i < 6; i++) {
                // Print a single byte in hex
                const char* hex = "0123456789ABCDEF";
                terminal_putchar(hex[(e1000_mac[i] >> 4) & 0x0F]);
                terminal_putchar(hex[e1000_mac[i] & 0x0F]);
                if (i < 5) terminal_putchar(':');
            }
            terminal_print_string("\n\n");
        }
    } else {
        terminal_print_string("WARNING: No E1000 Network Card found on PCI Bus!\n\n");
    }

    /* Spawn our network task! */
    create_task(network_task);
    terminal_print_string("Spawned Background Network Task (PID 2)!\n");

    /* Enable interrupts */
    __asm__ volatile ("sti");

    /* Launch the interactive shell on the main kernel task */
    shell_main();
}
