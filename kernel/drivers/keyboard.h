#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

/*
 * Takes a raw hardware scancode and returns the corresponding ASCII character.
 * Returns 0 if the scancode is not a printable character or is a key-release event.
 */
char keyboard_scancode_to_ascii(uint8_t scancode);

#endif
