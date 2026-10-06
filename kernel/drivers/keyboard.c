#include "keyboard.h"

/*
 * This is a standard lookup table mapping PS/2 Keyboard Scancode Set 1 
 * into standard US QWERTY ASCII characters.
 * 
 * The index of the array is the raw hardware scancode. 
 * The value at that index is the actual character.
 */
const char kbd_us[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', /* 9 */
  '9', '0', '-', '=', '\b', /* Backspace */
  '\t',     /* Tab */
  'q', 'w', 'e', 'r',   /* 19 */
  't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', /* Enter key */
    0,      /* 29   - Control */
  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', /* 39 */
 '\'', '`',   0,        /* Left shift */
 '\\', 'z', 'x', 'c', 'v', 'b', 'n',            /* 49 */
  'm', ',', '.', '/',   0,              /* Right shift */
  '*',
    0,  /* Alt */
  ' ',  /* Space bar */
    0,  /* Caps lock */
    0,  /* 59 - F1 key ... > */
    0,   0,   0,   0,   0,   0,   0,   0,
    0,  /* < ... F10 */
    0,  /* 69 - Num lock*/
    0,  /* Scroll Lock */
    0,  /* Home key */
    0,  /* Up Arrow */
    0,  /* Page Up */
  '-',
    0,  /* Left Arrow */
    0,
    0,  /* Right Arrow */
  '+',
    0,  /* 79 - End key*/
    0,  /* Down Arrow */
    0,  /* Page Down */
    0,  /* Insert Key */
    0,  /* Delete Key */
    0,   0,   0,
    0,  /* F11 Key */
    0,  /* F12 Key */
    0, /* All other keys are undefined */
};

char keyboard_scancode_to_ascii(uint8_t scancode) {
    /* 
     * In Scancode Set 1, if the highest bit (0x80) is set, 
     * it means the key was released, not pressed. 
     * We only want to print when the key is pressed down.
     */
    if (scancode & 0x80) {
        return 0; 
    }
    
    // Safety check just in case
    if (scancode > 127) {
        return 0;
    }

    return kbd_us[scancode];
}
