#include "editor.h"
#include "../fs/ofs.h"
#include <stdint.h>
#include <stdbool.h>

extern void terminal_print_string(const char* str);
extern void terminal_putchar(char c);
extern char keyboard_pop_char(void);

// Clear the screen by scrolling
static void clear_screen(void) {
    for (int i = 0; i < 25; i++) {
        terminal_putchar('\n');
    }
}

// Simple string copy
static void strcpy_safe(char* dest, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

void editor_start(const char* filename) {
    char file_buffer[512];
    int buf_idx = 0;
    
    // Clear buffer
    for (int i=0; i<512; i++) file_buffer[i] = '\0';

    clear_screen();
    terminal_print_string("=== OSISOS NANO === File: ");
    terminal_print_string(filename);
    terminal_print_string(" ===\n");
    terminal_print_string("Type your text. Press ESC to save and exit.\n\n");

    // Load existing file if any
    if (ofs_read_file(filename, file_buffer)) {
        // Count length and print
        while(file_buffer[buf_idx] != '\0' && buf_idx < 511) {
            terminal_putchar(file_buffer[buf_idx]);
            buf_idx++;
        }
    }

    while (1) {
        char c = keyboard_pop_char();
        if (c != 0) {
            if (c == 27) { // ESC key
                // Save file!
                file_buffer[buf_idx] = '\0';
                
                // We need to write it back.
                // ofs_write_file might fail if it already exists, unless we update OFS to support overwrite!
                terminal_print_string("\n\nSaving...\n");
                
                bool success = ofs_write_file(filename, file_buffer);
                if (success) {
                    terminal_print_string("Saved successfully!\n");
                } else {
                    terminal_print_string("Failed to save! (File might exist, and overwrite not supported yet)\n");
                }
                return;
            } else if (c == '\b') {
                if (buf_idx > 0) {
                    buf_idx--;
                    file_buffer[buf_idx] = '\0';
                    terminal_putchar('\b');
                }
            } else {
                if (buf_idx < 511) {
                    file_buffer[buf_idx++] = c;
                    terminal_putchar(c);
                }
            }
        }
        
        // Very slight delay
        for (volatile int d = 0; d < 10000; d++);
    }
}
