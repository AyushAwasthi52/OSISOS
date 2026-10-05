#ifndef PIC_H
#define PIC_H

/* 
 * Remaps the hardware interrupts from the PIC.
 * By default, the PIC maps IRQ 0-7 to IDT entries 8-15.
 * We must remap them to IDT entries 32-47 to avoid colliding with CPU exceptions.
 */
void pic_remap(void);

#endif
