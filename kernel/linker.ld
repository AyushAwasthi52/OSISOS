/* 
 * linker.ld - OSISOS Kernel Linker Script
 * Tells the compiler where to put the different sections of our code in memory.
 */
ENTRY(_start)

SECTIONS
{
    /* 
     * The bootloader conventionally loads the kernel at 1 MiB (1048576 bytes).
     * Loading below 1 MiB is dangerous as it's reserved for BIOS/hardware memory maps.
     */
    . = 1M;

    /* 
     * The Multiboot header MUST be very early in the binary so GRUB can find it.
     * We put it first in the .text (code) section.
     */
    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
        *(.text)
    }

    /* Read-only data (e.g., our "OSISOS Kernel Booted..." string) */
    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata)
    }

    /* Initialized data */
    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
    }

    /* 
     * Uninitialized data and our stack.
     * We defined our stack in the .bss section in boot.S.
     */
    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }
}
