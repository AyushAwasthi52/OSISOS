.global gdt_flush

gdt_flush:
    /* Load the parameter passed to us from C (the pointer to the GDT) into eax */
    mov 4(%esp), %eax

    /* Tell the CPU to load the GDT from that pointer */
    lgdt (%eax)

    /* 
     * Now we need to update our segment registers to use the new GDT.
     * We loaded our Data Segment into the 3rd entry (offset 0x10).
     */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    /* 
     * To update the Code Segment (CS), we must perform a "far jump".
     * Our Code Segment is the 2nd entry (offset 0x08).
     */
    jmp $0x08, $flush
flush:
    ret

.global tss_flush
tss_flush:
    /* Load the index of our TSS structure into the TR (Task Register). 
     * The TSS is the 5th entry in the GDT, so its offset is 5 * 8 = 40 (0x28).
     * The bottom two bits are set to 3 for Ring 3 (User Mode), so 0x28 | 3 = 0x2B.
     */
    mov $0x2B, %ax
    ltr %ax
    ret
