.global jump_usermode

/* 
 * void jump_usermode(uint32_t user_eip, uint32_t user_esp); 
 * 
 * C Calling Convention:
 * [esp+8] user_esp
 * [esp+4] user_eip
 * [esp]   return address (which we will overwrite!)
 */
jump_usermode:
    /* Disable interrupts while we construct the fake stack */
    cli

    /* Read arguments from the stack */
    mov 4(%esp), %ebx /* ebx = user_eip */
    mov 8(%esp), %ecx /* ecx = user_esp */

    /* Set up the Data Segment registers for Ring 3 */
    /* Our User Data segment is the 5th entry in the GDT (offset 0x20).
     * To request Ring 3 privilege, we logically OR it with 3: 0x20 | 3 = 0x23 
     */
    mov $0x23, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    /* 
     * Construct the IRET stack!
     * The `iret` instruction expects the stack to look exactly like this:
     * [SS, ESP, EFLAGS, CS, EIP]
     * We push them in reverse order so they pop correctly.
     */
    
    /* 1. SS (Stack Segment) - User Data (0x23) */
    push $0x23
    
    /* 2. ESP (User Stack Pointer) */
    push %ecx

    /* 3. EFLAGS. We must enable interrupts (IF bit = 0x200) in the user mode EFLAGS! */
    pushf
    pop %eax
    or $0x200, %eax
    push %eax

    /* 4. CS (Code Segment) - User Code.
     * Our User Code segment is the 4th entry in the GDT (offset 0x18).
     * 0x18 | 3 = 0x1B 
     */
    push $0x1B

    /* 5. EIP (Instruction Pointer) - Where user mode starts executing */
    push %ebx

    /* 
     * BOOM! Execute iret.
     * The CPU pops all 5 values, realizes we are downgrading to Ring 3, 
     * and securely switches privileges!
     */
    iret
