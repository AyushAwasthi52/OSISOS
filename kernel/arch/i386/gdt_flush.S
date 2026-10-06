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
