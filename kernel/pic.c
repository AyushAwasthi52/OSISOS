#include "pic.h"
#include "io.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

void pic_remap(void) {
    /* 
     * In IBM PC architecture, there are two PIC chips:
     * Master PIC handles IRQ 0-7. Slave PIC handles IRQ 8-15.
     * We send standard initialization command 0x11 to both chips.
     */
    outb(PIC1_COMMAND, 0x11);
    outb(PIC2_COMMAND, 0x11);
    
    /* 
     * Here is the crucial remap:
     * We tell the Master PIC to start mapping its IRQs to IDT entry 32.
     * We tell the Slave PIC to start mapping its IRQs to IDT entry 40.
     */
    outb(PIC1_DATA, 0x20); // 0x20 is Hex for 32
    outb(PIC2_DATA, 0x28); // 0x28 is Hex for 40
    
    /* Tell Master PIC that there is a Slave PIC at IRQ2 (0x04) */
    outb(PIC1_DATA, 0x04);
    /* Tell Slave PIC its cascade identity (0x02) */
    outb(PIC2_DATA, 0x02);
    
    /* Set both chips to 8086 mode */
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);
    
    /* 
     * Unmask ONLY the keyboard interrupt (IRQ 1) on the Master PIC.
     * Binary 11111101 is Hex 0xFD. This sets bit 1 to '0' (unmasked) and all others to '1' (masked).
     * We mask the entire Slave PIC (0xFF).
     * 
     * If we set this to 0x00, the System Timer (IRQ 0) fires 18 times a second.
     * Because we haven't written a handler for IRQ 0 yet, the CPU would triple-fault immediately!
     */
    outb(PIC1_DATA, 0xFD);
    outb(PIC2_DATA, 0xFF);
}
