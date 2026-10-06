.global idt_flush
.global isr0

/* Load the IDT */
idt_flush:
    mov 4(%esp), %eax  /* Get the pointer to the IDT, passed as a parameter. */
    lidt (%eax)        /* Load the IDT pointer. */
    ret

/* 
 * Interrupt Service Routine for Exception 0 (Divide by Zero)
 * The CPU automatically jumps here when a divide by zero happens.
 */
isr0:
    /* Disable interrupts while we handle this */
    cli 
    
    /* Call the C function to print the error message */
    call isr0_handler 
    
/* We halt because a divide by zero in the kernel is fatal right now. */
1:  hlt 
    jmp 1b

/*
 * Interrupt Service Routine for Hardware Interrupt 32 (System Timer)
 */
.global isr32
isr32:
    cli             /* Disable other interrupts while we process the timer */
    pusha           /* Save all general-purpose registers (eax, ebx, ecx, etc.) */
    
    call timer_handler
    
    popa            /* Restore all general-purpose registers */
    sti             /* Re-enable interrupts */
    iret            /* Return from the interrupt and resume whatever the CPU was doing */

/*
 * Interrupt Service Routine for Hardware Interrupt 33 (Keyboard)
 */
.global isr33
isr33:
    cli             /* Disable other interrupts while we process the keyboard */
    pusha           /* Save all general-purpose registers (eax, ebx, ecx, etc.) */
    
    call keyboard_handler
    
    popa            /* Restore all general-purpose registers */
    sti             /* Re-enable interrupts */
    iret            /* Return from the interrupt and resume whatever the CPU was doing */

/*
 * Interrupt Service Routine for System Calls (Interrupt 128 / 0x80)
 */
.global isr128
isr128:
    cli             
    pusha           

    /* Pass the System Call parameters to our C handler!
     * The C function will look like:
     * uint32_t syscall_handler(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx)
     */
    push %edx
    push %ecx
    push %ebx
    push %eax
    
    call syscall_handler
    
    /* Clean up the 4 parameters we just pushed */
    add $16, %esp
    
    /* 
     * The C function returns its result in %eax.
     * We need to overwrite the old EAX value that pusha saved on the stack,
     * so that when we call popa, the new returned value goes back into EAX!
     * EAX is the 8th register pushed by pusha (8 * 4 = 32 bytes down, wait...)
     * No, EAX is pushed FIRST.
     * pusha pushes: EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI
     * So EAX is at the VERY TOP (highest memory address).
     * [esp+28] is where EAX is located.
     */
    mov %eax, 28(%esp)

    popa
    sti
    iret
