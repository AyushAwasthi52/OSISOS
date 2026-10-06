.global load_page_directory
.global enable_paging

/* 
 * Takes one argument: the physical address of the page directory.
 * Loads it into the CR3 register, which tells the MMU where to find the translation dictionary.
 */
load_page_directory:
    push %ebp
    mov %esp, %ebp
    
    mov 8(%ebp), %eax
    mov %eax, %cr3
    
    mov %ebp, %esp
    pop %ebp
    ret

/*
 * Sets the Paging bit (bit 31) in the CR0 register to turn on Virtual Memory.
 */
enable_paging:
    push %ebp
    mov %esp, %ebp
    
    mov %cr0, %eax
    or $0x80000000, %eax
    mov %eax, %cr0
    
    mov %ebp, %esp
    pop %ebp
    ret
