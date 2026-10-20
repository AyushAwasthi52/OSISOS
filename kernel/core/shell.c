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

#include "ofs.h"
#include "rtc.h"
#include "../mm/pmm.h"
#include "task.h"
#include "editor.h"
#include "io.h"
#include "net.h"
#include "auth.h"

extern void send_ping_packet(void);
extern void terminal_print_dec(uint32_t val);
extern uint32_t seconds_passed;

// Helper to print a 2-digit zero-padded number
static void print_2digit(uint8_t val) {
    if (val < 10) {
        terminal_putchar('0');
    }
    terminal_print_dec(val);
}

// State variables for login
static bool is_logged_in = false;
static bool entering_password = false;
static char username[32];
static char password[AUTH_MAX_PASSWORD + 1];

// `passwd` flow: 0 = idle, 1 = asking for the current password, 2 = new password, 3 = confirm
static int  pw_stage = 0;
static char pw_new[AUTH_MAX_PASSWORD + 1];

extern bool fs_shell_command(const char* line);
extern bool extras_command(const char* line);

static void execute_command(char* cmd) {
    if (cmd[0] == '\0') {
        return;
    }
    
    /* sysinfo, dmesg, fortune, snake, matrix, easter eggs (see core/extras.c) */
    if (extras_command(cmd)) {
        return;
    }

    /* rm, rmdir, mv, df (see fs/fscmd.c) */
    if (fs_shell_command(cmd)) {
        return;
    }

    /* ifconfig, arp, ping <ip>, netstat, nc, http (see net/netcmd.c) */
    if (net_shell_command(cmd)) {
        return;
    }

    if (strcmp(cmd, "help") == 0) {
        terminal_print_string("OSISOS Shell v1.0\n");
        terminal_print_string("Available commands:\n");
        terminal_print_string("  help          - Show this message\n");
        terminal_print_string("  clear         - Clear the terminal\n");
        terminal_print_string("  echo <text>   - Print text to screen\n");
        terminal_print_string("  ls            - List files on disk\n");
        terminal_print_string("  cat <file>    - Read file contents\n");
        terminal_print_string("  rm/rmdir/mv/df - delete, move files, disk usage\n");
        terminal_print_string("  sysinfo, dmesg, selftest - system info, kernel log, self-check\n");
        terminal_print_string("  fortune, cowsay, snake, matrix, sl - fun (there may be secrets)\n");
        terminal_print_string("  passwd        - change your password\n");
        terminal_print_string("  crash <kind>  - DEBUG: fault the kernel to see the panic screen\n");
        terminal_print_string("  ping          - Send a raw ethernet test packet\n");
        terminal_print_string("  ping <host>   - ICMP echo (host = IP or DNS name)\n");
        terminal_print_string("  nslookup <n>  - DNS lookup      dns [ip] - show/set DNS server\n");
        terminal_print_string("  dhcp [renew]  - DHCP status / renew lease\n");
        terminal_print_string("  fetch <host> <port> <url> <ofs-path> - download into OFS\n");
        terminal_print_string("  httpd [root <dir>] - web server status/log (docroot /www)\n");
        terminal_print_string("  ifconfig      - Show/set IP address (ifconfig <ip> <mask> <gw>)\n");
        terminal_print_string("  arp           - Show ARP cache\n");
        terminal_print_string("  netstat       - List TCP sockets\n");
        terminal_print_string("  netdrop <pct> - Simulate packet loss (test TCP retransmit)\n");
        terminal_print_string("  nc <host> <port> [text] - TCP client\n");
        terminal_print_string("  http <host> [port] [path] - HTTP GET\n");
        terminal_print_string("  date          - Show current date and time\n");
        terminal_print_string("  free          - Show memory statistics\n");
        terminal_print_string("  ps            - List running processes\n");
        terminal_print_string("  uptime        - Show system uptime\n");
        terminal_print_string("  edit <file>   - Open text editor\n");
        terminal_print_string("  mkdir <dir>   - Create a directory\n");
        terminal_print_string("  cd <dir>      - Change directory\n");
        terminal_print_string("  logout        - Log out of the current session\n");
        terminal_print_string("  reboot        - Reboot the system\n");
        terminal_print_string("  shutdown      - Power off the system\n");
    } else if (cmd[0] == 'e' && cmd[1] == 'c' && cmd[2] == 'h' && cmd[3] == 'o' && cmd[4] == ' ') {
        terminal_print_string(&cmd[5]);
        terminal_putchar('\n');
    } else if (cmd[0] == 'm' && cmd[1] == 'k' && cmd[2] == 'd' && cmd[3] == 'i' && cmd[4] == 'r' && cmd[5] == ' ') {
        char* dirname = &cmd[6];
        if (ofs_mkdir(dirname)) {
            terminal_print_string("Directory created.\n");
        } else {
            terminal_print_string("mkdir: cannot create directory '");
            terminal_print_string(dirname);
            terminal_print_string("': File exists or no space\n");
        }
    } else if (cmd[0] == 'c' && cmd[1] == 'd' && cmd[2] == ' ') {
        char* dirname = &cmd[3];
        if (!ofs_change_dir(dirname)) {
            terminal_print_string("cd: ");
            terminal_print_string(dirname);
            terminal_print_string(": No such file or directory\n");
        }
    } else if (cmd[0] == 'e' && cmd[1] == 'd' && cmd[2] == 'i' && cmd[3] == 't' && cmd[4] == ' ') {
        char* filename = &cmd[5];
        editor_start(filename);
    } else if (strcmp(cmd, "clear") == 0) {
        // Scroll the terminal enough to clear it
        for (int i = 0; i < 25; i++) {
            terminal_putchar('\n');
        }
    } else if (strcmp(cmd, "date") == 0) {
        rtc_time_t t;
        read_rtc(&t);
        terminal_print_dec(t.year);
        terminal_putchar('-');
        print_2digit(t.month);
        terminal_putchar('-');
        print_2digit(t.day);
        terminal_putchar(' ');
        print_2digit(t.hour);
        terminal_putchar(':');
        print_2digit(t.minute);
        terminal_putchar(':');
        print_2digit(t.second);
        terminal_print_string(" UTC\n");
    } else if (strcmp(cmd, "free") == 0) {
        uint32_t total = pmm_get_total_memory() / 1024;
        uint32_t free = pmm_get_free_memory() / 1024;
        uint32_t used = total - free;
        terminal_print_string("Total Memory: ");
        terminal_print_dec(total);
        terminal_print_string(" KB\nUsed Memory:  ");
        terminal_print_dec(used);
        terminal_print_string(" KB\nFree Memory:  ");
        terminal_print_dec(free);
        terminal_print_string(" KB\n");
    } else if (strcmp(cmd, "ps") == 0) {
        task_list_all();
    } else if (strcmp(cmd, "uptime") == 0) {
        terminal_print_string("System Uptime: ");
        terminal_print_dec(seconds_passed);
        terminal_print_string(" seconds\n");
    } else if (strcmp(cmd, "ls") == 0) {
        ofs_list_files();
    } else if (cmd[0] == 'c' && cmd[1] == 'a' && cmd[2] == 't' && cmd[3] == ' ') {
        char* filename = &cmd[4];
        char file_buffer[512];
        if (ofs_read_file(filename, file_buffer)) {
            terminal_print_string(file_buffer);
            terminal_putchar('\n');
        } else {
            terminal_print_string("cat: ");
            terminal_print_string(filename);
            terminal_print_string(": No such file or directory\n");
        }
    } else if (strcmp(cmd, "ping") == 0) {
        terminal_print_string("Pinging network...\n");
        send_ping_packet();
    } else if (strcmp(cmd, "passwd") == 0) {
        pw_stage = 1;
        terminal_print_string("Changing password for ");
        terminal_print_string(username);
        terminal_print_string(".\nCurrent password: ");
    } else if (strcmp(cmd, "logout") == 0) {
        for (int i = 0; i < 25; i++) terminal_putchar('\n');
        terminal_print_string("==================================\n");
        terminal_print_string("      Welcome to OSISOS v1.0      \n");
        terminal_print_string("==================================\n\n");
        terminal_print_string("login: ");
        is_logged_in = false;
        entering_password = false;
    } else if (strcmp(cmd, "reboot") == 0) {
        terminal_print_string("Rebooting...\n");
        uint8_t good = 0x02;
        while (good & 0x02) good = inb(0x64);
        outb(0x64, 0xFE);
        __asm__ volatile ("cli; hlt");
    } else if (strcmp(cmd, "shutdown") == 0) {
        terminal_print_string("Shutting down...\n");
        outw(0x604, 0x2000);
        outw(0xB004, 0x2000);
        __asm__ volatile ("cli; hlt");
    } else {
        terminal_print_string("Unknown command: ");
        terminal_print_string(cmd);
        terminal_putchar('\n');
    }
}

static void wipe_buf(char* p, int n) { volatile char* v = p; while (n-- > 0) *v++ = 0; }

static void print_prompt(void) {
    terminal_print_string(username);
    terminal_print_string("@osisos> ");
}

static void print_wait_message(uint32_t secs) {
    terminal_print_string("Too many failed attempts. Try again in ");
    terminal_print_dec(secs);
    terminal_print_string(" s.\n");
}

// One line of input while in the `passwd` dialogue.
static void password_dialogue(const char* line) {
    if (pw_stage == 1) {
        uint32_t wait = 0;
        auth_result_t r = auth_verify(username, line, &wait);
        if (r == AUTH_OK) {
            pw_stage = 2;
            terminal_print_string("New password: ");
        } else {
            pw_stage = 0;
            terminal_print_string("Authentication failed.\n");
            if (r == AUTH_LOCKED || wait) print_wait_message(wait);
            print_prompt();
        }
    } else if (pw_stage == 2) {
        int n = 0;
        while (line[n]) n++;
        if (n < AUTH_MIN_PASSWORD || n > AUTH_MAX_PASSWORD) {
            pw_stage = 0;
            terminal_print_string("Password must be 6 to 63 characters. Not changed.\n");
            print_prompt();
        } else {
            for (int k = 0; k <= n; k++) pw_new[k] = line[k];
            pw_stage = 3;
            terminal_print_string("Retype new password: ");
        }
    } else {
        bool same = true;
        int k = 0;
        while (line[k] || pw_new[k]) { if (line[k] != pw_new[k]) { same = false; break; } k++; }
        if (!same) {
            terminal_print_string("Passwords do not match. Not changed.\n");
        } else {
            bool saved = false;
            if (auth_set_password(username, pw_new, &saved)) {
                terminal_print_string("Password updated.");
                if (!saved) terminal_print_string(" (WARNING: no disk, it will be lost at reboot)");
                terminal_print_string("\n");
            } else {
                terminal_print_string("Could not change the password.\n");
            }
        }
        wipe_buf(pw_new, sizeof(pw_new));
        pw_stage = 0;
        print_prompt();
    }
}

void shell_main(void) {
    char input_buffer[256];
    int buf_idx = 0;

    auth_init();                                   // load /etc/shadow (creates the default account on first boot)

    terminal_print_string("\n==================================\n");
    terminal_print_string("      Welcome to OSISOS v1.0      \n");
    terminal_print_string("==================================\n\n");
    terminal_print_string("login: ");

    while (1) {
        char c = keyboard_pop_char();
        if (c != 0) {
            if (c == '\n') {
                input_buffer[buf_idx] = '\0';

                if (!is_logged_in) {
                    if (!entering_password) {
                        // Store the user name (bounded: the typed line can be up to 255 characters)
                        int k = 0;
                        while (k < (int)sizeof(username) - 1 && k < buf_idx) { username[k] = input_buffer[k]; k++; }
                        username[k] = '\0';
                        entering_password = true;
                        terminal_print_string("\npassword: ");
                    } else {
                        int k = 0;
                        while (k < (int)sizeof(password) - 1 && k < buf_idx) { password[k] = input_buffer[k]; k++; }
                        password[k] = '\0';
                        terminal_putchar('\n');

                        uint32_t wait = 0;
                        auth_result_t r = auth_verify(username, password, &wait);
                        wipe_buf(password, sizeof(password));      // never keep a typed password around
                        if (r == AUTH_OK) {
                            is_logged_in = true;
                            terminal_print_string("\nLogin successful. Type 'help' for commands.\n");
                            if (auth_has_default_password(username))
                                terminal_print_string("WARNING: this account still uses the default password. Run 'passwd' now.\n");
                            print_prompt();
                        } else {
                            if (r == AUTH_LOCKED) print_wait_message(wait);
                            else {
                                terminal_print_string("\nLogin incorrect.\n");
                                if (wait) print_wait_message(wait);
                            }
                            terminal_print_string("\nlogin: ");
                            entering_password = false;
                        }
                    }
                } else {
                    terminal_putchar('\n');
                    if (pw_stage) {
                        password_dialogue(input_buffer);
                    } else {
                        execute_command(input_buffer);
                        if (pw_stage == 0 && is_logged_in) print_prompt();
                    }
                }
                wipe_buf(input_buffer, sizeof(input_buffer));      // typed passwords must not linger on the stack
                buf_idx = 0;
            } else if (c == '\b') {
                if (buf_idx > 0) {
                    buf_idx--;
                    terminal_putchar('\b'); // Handle backspace visually
                }
            } else {
                if (buf_idx < 255) {
                    input_buffer[buf_idx++] = c;
                    if ((entering_password && !is_logged_in) || pw_stage) {
                        terminal_putchar('*'); // Mask password
                    } else {
                        terminal_putchar(c);
                    }
                }
            }
        }
        
        // Very slight delay
        for (volatile int d = 0; d < 10000; d++);
    }
}
