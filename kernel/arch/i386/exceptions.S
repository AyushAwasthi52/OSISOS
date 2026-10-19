/*
 * exceptions.S - entry stubs for all 32 CPU exception vectors.
 *
 * Some exceptions push an error code (8, 10-14, 17, 21, 30); for the others we push a
 * dummy 0 so every exception presents the same stack frame to exception_handler():
 *
 *   [esp]  -> gs fs es ds | edi esi ebp esp ebx edx ecx eax | int_no err_code | eip cs eflags [useresp ss]
 */
.macro EXC_NOERR n
.global exc_stub_\n
exc_stub_\n:
    push $0
    push $\n
    jmp exc_common
.endm

.macro EXC_ERR n
.global exc_stub_\n
exc_stub_\n:
    push $\n
    jmp exc_common
.endm

EXC_NOERR 0
EXC_NOERR 1
EXC_NOERR 2
EXC_NOERR 3
EXC_NOERR 4
EXC_NOERR 5
EXC_NOERR 6
EXC_NOERR 7
EXC_ERR   8
EXC_NOERR 9
EXC_ERR   10
EXC_ERR   11
EXC_ERR   12
EXC_ERR   13
EXC_ERR   14
EXC_NOERR 15
EXC_NOERR 16
EXC_ERR   17
EXC_NOERR 18
EXC_NOERR 19
EXC_NOERR 20
EXC_ERR   21
EXC_NOERR 22
EXC_NOERR 23
EXC_NOERR 24
EXC_NOERR 25
EXC_NOERR 26
EXC_NOERR 27
EXC_NOERR 28
EXC_NOERR 29
EXC_ERR   30
EXC_NOERR 31

exc_common:
    pusha
    push %ds
    push %es
    push %fs
    push %gs
    mov $0x10, %ax          /* kernel data segment */
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    cld
    push %esp               /* argument: pointer to the saved register frame */
    call exception_handler
    add $4, %esp
    pop %gs
    pop %fs
    pop %es
    pop %ds
    popa
    add $8, %esp            /* drop int_no and err_code */
    iret

.section .rodata
.global exc_stubs
.align 4
exc_stubs:
.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    .long exc_stub_\n
.endr
.section .note.GNU-stack,"",@progbits
