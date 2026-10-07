#include "shell.h"
#include <stdint.h>
#include <stdbool.h>

extern void terminal_print_string(const char* str);
extern void terminal_putchar(char c);
extern char keyboard_pop_char(void);

// Custom string functions since we don't have libc
static int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static void execute_command(char* cmd) {
    if (cmd[0] == '\0') {
        return;
    }
    
    if (strcmp(cmd, "help") == 0) {
        terminal_print_string("OSISOS Shell v1.0\n");
        terminal_print_string("Available commands:\n");
        terminal_print_string("  help  - Show this message\n");
        terminal_print_string("  ping  - Send a network packet\n");
        terminal_print_string("  clear - Clear the terminal\n");
        terminal_print_string("  echo  - Print text to screen\n");
    } else if (cmd[0] == 'e' && cmd[1] == 'c' && cmd[2] == 'h' && cmd[3] == 'o' && cmd[4] == ' ') {
        terminal_print_string(&cmd[5]);
        terminal_putchar('\n');
    } else if (strcmp(cmd, "clear") == 0) {
        // Scroll the terminal enough to clear it
        for (int i = 0; i < 25; i++) {
            terminal_putchar('\n');
        }
    } else if (strcmp(cmd, "ping") == 0) {
        terminal_print_string("Pinging network...\n");
        // We can hook into e1000 send here later
    } else {
        terminal_print_string("Unknown command: ");
        terminal_print_string(cmd);
        terminal_putchar('\n');
    }
}

void shell_main(void) {
    char input_buffer[256];
    int buf_idx = 0;

    terminal_print_string("\nWelcome to OSISOS Shell!\n");
    terminal_print_string("> ");

    while (1) {
        char c = keyboard_pop_char();
        if (c != 0) {
            if (c == '\n') {
                terminal_putchar('\n');
                input_buffer[buf_idx] = '\0';
                execute_command(input_buffer);
                buf_idx = 0;
                terminal_print_string("> ");
            } else if (c == '\b') {
                if (buf_idx > 0) {
                    buf_idx--;
                    terminal_putchar('\b');
                }
            } else {
                if (buf_idx < 255) {
                    input_buffer[buf_idx++] = c;
                    terminal_putchar(c);
                }
            }
        }
    }
}
