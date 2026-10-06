#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

/*
 * Initializes the Programmable Interval Timer (PIT).
 * The frequency determines how many times per second the timer interrupts the CPU.
 */
void init_timer(uint32_t frequency);

#endif
