#include "timer.h"
#include "io.h"
#include <stddef.h>

/* We borrow our print function from kernel.c for now */
extern void kernel_print(const char* message, size_t color, size_t row);

/* Keep track of how many times the timer has fired */
uint32_t tick = 0;
uint32_t seconds_passed = 0;

/* 
 * This function is called from assembly (isr32) every time the timer fires.
 */
void timer_handler(void) {
    tick++;
    
    /* 
     * Since we set the frequency to 100Hz, 100 ticks = 1 second.
     * We will update the screen once every second to prove time is passing.
     */
    if (tick % 100 == 0) {
        seconds_passed++;
        
        // Print a simple heartbeat message on Row 2
        if (seconds_passed % 2 == 0) {
            kernel_print("System Uptime: Tick [TOCK]", 0x0E, 2); // Yellow text
        } else {
            kernel_print("System Uptime: [TICK] Tock", 0x0E, 2);
        }
    }

    /* We MUST send an End of Interrupt (EOI) to the Master PIC (Port 0x20) */
    outb(0x20, 0x20);
}

void init_timer(uint32_t frequency) {
    /* The hardware clock on all PCs runs at exactly 1,193,180 Hz */
    uint32_t divisor = 1193180 / frequency;

    /* 
     * Port 0x43 is the Mode/Command register. 
     * 0x36 sets the timer to: Channel 0, Lobyte/Hibyte access, Square Wave generator.
     */
    outb(0x43, 0x36);

    /* 
     * Port 0x40 is the Channel 0 data port.
     * We have to send our divisor split into two 8-bit bytes (low byte, then high byte).
     */
    uint8_t low  = (uint8_t)(divisor & 0xFF);
    uint8_t high = (uint8_t)((divisor >> 8) & 0xFF);
    
    outb(0x40, low);
    outb(0x40, high);
}
