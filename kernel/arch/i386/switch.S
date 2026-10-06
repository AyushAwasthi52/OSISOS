.global perform_task_switch

/* 
 * void perform_task_switch(uint32_t new_esp, uint32_t new_cr3, uint32_t* old_esp_ptr);
 * 
 * Standard C Calling Convention (Arguments pushed right-to-left):
 * [esp+12] old_esp_ptr (Pointer to the variable where we save the old stack)
 * [esp+8]  new_cr3     (The new page directory)
 * [esp+4]  new_esp     (The new stack pointer)
 * [esp]    return addr (The instruction pointer to return to)
 */
perform_task_switch:
    /* 1. Save the registers that C doesn't save automatically (Callee-Saved) */
    push %ebx
    push %esi
    push %edi
    push %ebp

    /* We just pushed 4 registers (16 bytes). The arguments are now shifted by 16 bytes! */
    /* [esp+28] = old_esp_ptr */
    /* [esp+24] = new_cr3 */
    /* [esp+20] = new_esp */

    /* 2. Save the current ESP into the current task's PCB */
    mov 28(%esp), %edi
    mov %esp, (%edi)

    /* 3. Load the NEW task's Page Directory into CR3 */
    mov 24(%esp), %eax
    mov %eax, %cr3

    /* 4. Load the NEW task's Stack Pointer (The actual Context Switch!) */
    mov 20(%esp), %esp

    /* 5. Restore the registers for the NEW task */
    pop %ebp
    pop %edi
    pop %esi
    pop %ebx

    /* 
     * 6. Return! 
     * The `ret` instruction pops the top of the stack and jumps to it (EIP).
     * Since we just switched stacks, this instantly jumps to wherever the NEW task left off!
     */
    ret
