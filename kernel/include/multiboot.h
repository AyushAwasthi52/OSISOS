#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include <stdint.h>

/* The magic number passed by a Multiboot-compliant bootloader in the eax register */
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

/* 
 * The Multiboot Information structure. 
 * GRUB builds this structure in memory and passes a pointer to it in the ebx register.
 * We use packed so the compiler doesn't misalign the bytes.
 */
struct multiboot_info {
    uint32_t flags;         // Flags indicating which fields are valid
    uint32_t mem_lower;     // Amount of lower memory (in KB)
    uint32_t mem_upper;     // Amount of upper memory (in KB)
    uint32_t boot_device;   // Indicates which disk we booted from
    uint32_t cmdline;       // Pointer to the command line string passed to the kernel
    uint32_t mods_count;    // Number of modules loaded with the kernel
    uint32_t mods_addr;     // Pointer to the module structures
    uint32_t syms[4];       // Symbol table info (for debugging)
    uint32_t mmap_length;   // Length of the memory map buffer
    uint32_t mmap_addr;     // Pointer to the memory map buffer
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
} __attribute__((packed));

typedef struct multiboot_info multiboot_info_t;

#endif
