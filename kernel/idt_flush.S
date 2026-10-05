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
