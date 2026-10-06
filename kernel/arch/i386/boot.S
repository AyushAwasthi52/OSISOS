/* 
 * boot.S - OSISOS Kernel Entry Point
 * Defines the Multiboot header and the initial stack.
 */

/* 1. Multiboot Header Constants */
.set ALIGN,    1<<0             /* Align loaded modules on page boundaries */
.set MEMINFO,  1<<1             /* Provide memory map to the kernel */
.set FLAGS,    ALIGN | MEMINFO  /* Multiboot 'flags' field */
.set MAGIC,    0x1BADB002       /* 'Magic number' lets bootloader find the header */
.set CHECKSUM, -(MAGIC + FLAGS) /* Checksum to prove we are Multiboot compliant */

/* 
 * 2. Multiboot Header 
 * Must be within the first 8 KiB of the kernel executable, 32-bit aligned.
 */
.section .multiboot
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM

/* 
 * 3. Kernel Stack
 * In C, function calls push local variables and return addresses to a stack.
 * We must allocate our own stack memory because the bootloader doesn't give us one.
 */
.section .bss
.align 16
stack_bottom:
.skip 16384 /* 16 KiB */
stack_top:

/* 
 * 4. Kernel Entry Point
 * This is where the bootloader jumps after loading the kernel into memory.
 */
.section .text
.global _start
.type _start, @function
_start:
    /* Set up the stack pointer (esp) to point to our allocated stack space. */
    mov $stack_top, %esp

    /* 
     * In the standard C calling convention (cdecl), function arguments are 
     * pushed to the stack in reverse order.
     * GRUB stores a pointer to the multiboot_info struct in %ebx.
     * GRUB stores the multiboot magic number in %eax.
     * So we push %ebx (argument 2) then push %eax (argument 1).
     */
    push %ebx
    push %eax

    /* Call the high-level C code: kernel_main(magic, multiboot_info_addr) */
    call kernel_main

    /* 
     * If kernel_main unexpectedly returns, put the CPU into an infinite loop.
     * cli: Clear Interrupts (disables hardware interrupts)
     * hlt: Halt the CPU until the next interrupt arrives
     */
    cli
1:  hlt
    jmp 1b

/* Set the size of the _start symbol for debugging tools */
.size _start, . - _start
