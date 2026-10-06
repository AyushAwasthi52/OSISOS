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
