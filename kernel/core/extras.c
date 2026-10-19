/*
 * extras.c - system info, kernel log viewer, and some fun.
 *
 *   sysinfo (neofetch)   dmesg   about
 *   fortune   cowsay <text>
 *   matrix   snake   sl          (full-screen; the network keeps running underneath)
 *   + a handful of easter eggs: try  sudo make me a sandwich,  xyzzy,  42,  brew coffee
 *
 * Full-screen programs save the VGA text buffer and restore it on exit, and never
 * touch row 0 (the kernel's clock display).
 */
#include "net.h"
#include "dhcp.h"
#include "httpd.h"
#include "ofs.h"
#include "pmm.h"
#include "klog.h"

extern uint16_t* terminal_buffer;
extern size_t    terminal_row, terminal_col;
extern void terminal_print_string(const char* str);
extern void terminal_putchar(char c);
extern void terminal_print_dec(uint32_t val);
extern char keyboard_pop_char(void);
extern uint32_t seconds_passed;

#define VGA_W 80
#define VGA_H 25

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static void pr(const char* s) { terminal_print_string(s); }
static void prd(uint32_t v)   { terminal_print_dec(v); }

static int pr_ip(uint32_t ip) {
    int n = 0;
    for (int shift = 24; shift >= 0; shift -= 8) {
        uint32_t o = (ip >> shift) & 255;
        char tmp[4]; int k = 0;
        if (o == 0) tmp[k++] = '0';
        while (o) { tmp[k++] = (char)('0' + o % 10); o /= 10; }
        while (k) { terminal_putchar(tmp[--k]); n++; }
        if (shift) { terminal_putchar('.'); n++; }
    }
    return n;
}

static bool eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static bool starts(const char* s, const char* p) {
    while (*p) { if (*s++ != *p++) return false; }
    return true;
}

static uint32_t rng_state;
static uint32_t rnd(void) {
    if (!rng_state) { uint32_t lo, hi; __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi)); rng_state = lo ^ 0x9E3779B9u; }
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

static uint16_t saved_screen[VGA_W * VGA_H];
static size_t saved_row, saved_col;

static void screen_save(void) {
    for (int i = 0; i < VGA_W * VGA_H; i++) saved_screen[i] = terminal_buffer[i];
    saved_row = terminal_row; saved_col = terminal_col;
}
static void screen_restore(void) {
    for (int i = 0; i < VGA_W * VGA_H; i++) terminal_buffer[i] = saved_screen[i];
    terminal_row = saved_row; terminal_col = saved_col;
}
static void cell(int x, int y, char c, uint8_t attr) {
    if (x < 0 || x >= VGA_W || y < 0 || y >= VGA_H) return;
    terminal_buffer[y * VGA_W + x] = (uint16_t)(uint8_t)c | ((uint16_t)attr << 8);
}
static void text_at(int x, int y, const char* s, uint8_t attr) { for (; *s; s++, x++) cell(x, y, *s, attr); }
static void clear_rows(int from, int to, uint8_t attr) {
    for (int y = from; y <= to; y++) for (int x = 0; x < VGA_W; x++) cell(x, y, ' ', attr);
}

/* sleep up to `ticks` (10 ms each); returns the key pressed meanwhile, or 0 */
static char wait_key(uint32_t ticks) {
    uint32_t start = net_now();
    for (;;) {
        char k = keyboard_pop_char();
        if (k) return k;
        if ((uint32_t)(net_now() - start) >= ticks) return 0;
        __asm__ volatile ("sti; hlt");
    }
}

/* ------------------------------------------------------------------ */
/* sysinfo                                                             */
/* ------------------------------------------------------------------ */

static void cpu_name(char* out) {                       /* out: 49 bytes */
    uint32_t a, b, c, d;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u));
    if (a >= 0x80000004u) {
        for (uint32_t i = 0; i < 3; i++) {
            __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000002u + i));
            memcpy(out + i * 16,      &a, 4); memcpy(out + i * 16 + 4,  &b, 4);
            memcpy(out + i * 16 + 8,  &c, 4); memcpy(out + i * 16 + 12, &d, 4);
        }
        out[48] = '\0';
        int s = 0; while (out[s] == ' ') s++;
        if (s) { int i = 0; while ((out[i] = out[i + s])) i++; }
    } else {
        __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0));
        memcpy(out, &b, 4); memcpy(out + 4, &d, 4); memcpy(out + 8, &c, 4); out[12] = '\0';
    }
}

static const char* logo[] = {
    "    .---------.     ",
    " ===|  O S    |===  ",
    " ===|   I S   |===  ",
    " ===|    O S  |===  ",
    "    '---------'     ",
    "   a tiny server OS ",
    "                    ",
    "                    ",
    "                    ",
    "                    ",
};

static void sysinfo_line(int i) {
    static char cpu[49];
    switch (i) {
        case 0: pr("admin@osisos"); break;
        case 1: pr("------------"); break;
        case 2: pr("OS:      OSISOS (i386, from scratch)"); break;
        case 3: { if (!cpu[0]) cpu_name(cpu); pr("CPU:     "); pr(cpu); break; }
        case 4: { uint32_t tot = pmm_get_total_memory() / (1024 * 1024), fr = pmm_get_free_memory() / (1024 * 1024);
                  pr("Memory:  "); prd(tot - fr); pr(" / "); prd(tot); pr(" MB"); break; }
        case 5: { uint32_t s = seconds_passed;
                  pr("Uptime:  "); if (s >= 3600) { prd(s / 3600); pr("h "); } prd((s / 60) % 60); pr("m "); prd(s % 60); pr("s"); break; }
        case 6: { pr("Network: "); pr_ip(netif.ip); pr(" via "); pr(dhcp_state_name()); break; }
        case 7: {
            if (ofs_available()) {
                ofs_usage_t u; ofs_usage(&u);
                pr("Disk:    OFS v2, "); prd(u.files); pr(" files, "); prd(u.used_blocks / 2); pr(" / "); prd(u.total_blocks / 2); pr(" KB");
            } else {
                pr("Disk:    none");
            }
            break;
        }
        case 8: { httpd_stats_t st; httpd_get_stats(&st);
                  pr("Web:     "); prd(st.requests); pr(" requests, "); prd(st.bytes_sent / 1024); pr(" KB served"); break; }
        case 9: { pr("TCP:     "); prd(net_stats.tcp_rx); pr(" in / "); prd(net_stats.tcp_tx); pr(" out, "); prd(net_stats.tcp_retx); pr(" retransmits"); break; }
        default: break;
    }
}

static void cmd_sysinfo(void) {
    pr("\n");
    for (int i = 0; i < 10; i++) { pr(logo[i]); sysinfo_line(i); pr("\n"); }
}

/* ------------------------------------------------------------------ */
/* dmesg, about, fortune, cowsay                                       */
/* ------------------------------------------------------------------ */

static void cmd_dmesg(void) {
    static char buf[2048];
    if (klog_read(buf, sizeof(buf), 20) == 0) { pr("(kernel log is empty)\n"); return; }
    pr(buf);
}

static void cmd_about(void) {
    pr("OSISOS - a lightweight server OS written from scratch.\n");
    pr("  kernel: GDT/IDT, paging, preemptive scheduler, PIT, PS/2, ATA, e1000\n");
    pr("  net:    Ethernet, ARP, IPv4, ICMP, UDP, TCP, DHCP, DNS, HTTP server\n");
    pr("  disk:   OFS v2 (128 inodes, extents, delete/rename, v1 migration)\n");
    pr("  try:    sysinfo  dmesg  selftest  fortune  snake  matrix  sl\n");
}

static const char* fortunes[] = {
    "There are only two hard things in computer science: cache invalidation, naming things, and off-by-one errors.",
    "I would tell you a UDP joke, but you might not get it.",
    "Hi, I'd like to hear a TCP joke.  Hello, would you like to hear a TCP joke?  Yes, I'd like to hear a TCP joke.  OK, I'll tell you a TCP joke.",
    "It works on my machine. -- every developer, right before shipping the machine.",
    "A SYN packet walks into a bar. SYN-ACK, says the bartender. ACK, says the packet. They got along fine.",
    "Premature optimization is the root of all evil. -- Donald Knuth",
    "Simplicity is prerequisite for reliability. -- Edsger Dijkstra",
    "Talk is cheap. Show me the code. -- Linus Torvalds",
    "The network is reliable. (First fallacy of distributed computing.)",
    "127.0.0.1 - there's no place like it.",
    "To understand recursion, you must first understand recursion.",
    "Segmentation fault (core dumped) - the kernel's way of saying 'we need to talk'.",
    "Real programmers count from zero, and so do their bugs.",
    "There is no cloud. It's just someone else's computer. This one is yours.",
};

static void cmd_fortune(void) {
    pr("\n"); pr(fortunes[rnd() % (sizeof(fortunes) / sizeof(fortunes[0]))]); pr("\n");
}

static void cmd_cowsay(const char* text) {
    if (!*text) text = "Moo! Give me something to say.";
    int len = (int)net_strlen(text);
    if (len > 60) len = 60;
    pr(" "); for (int i = 0; i < len + 2; i++) pr("_"); pr("\n");
    pr("< "); for (int i = 0; i < len; i++) terminal_putchar(text[i]); pr(" >\n");
    pr(" "); for (int i = 0; i < len + 2; i++) pr("-"); pr("\n");
    pr("        \\   ^__^\n         \\  (oo)\\_______\n            (__)\\       )\\/\\\n                ||----w |\n                ||     ||\n");
}

/* ------------------------------------------------------------------ */
/* matrix                                                              */
/* ------------------------------------------------------------------ */

static void cmd_matrix(void) {
    static int y[VGA_W], len[VGA_W], delay[VGA_W];
    screen_save();
    clear_rows(1, VGA_H - 1, 0x00);
    for (int x = 0; x < VGA_W; x++) { y[x] = -(int)(rnd() % 20); len[x] = 5 + (int)(rnd() % 12); delay[x] = (int)(rnd() % 25); }

    text_at(2, VGA_H - 1, " any key to leave the matrix ", 0x02);
    for (;;) {
        for (int x = 0; x < VGA_W; x++) {
            if (delay[x] > 0) { delay[x]--; continue; }
            y[x]++;
            int hy = y[x];
            if (hy >= 1 && hy < VGA_H - 1) cell(x, hy, (char)(33 + rnd() % 94), 0x0F);                     /* bright head */
            if (hy - 1 >= 1 && hy - 1 < VGA_H - 1) terminal_buffer[(hy - 1) * VGA_W + x] = (terminal_buffer[(hy - 1) * VGA_W + x] & 0x00FF) | (0x0A << 8);
            int mid = hy - len[x] / 2;
            if (mid >= 1 && mid < VGA_H - 1) terminal_buffer[mid * VGA_W + x] = (terminal_buffer[mid * VGA_W + x] & 0x00FF) | (0x02 << 8);
            int ty = hy - len[x];
            if (ty >= 1 && ty < VGA_H - 1) cell(x, ty, ' ', 0x00);
            if (ty >= VGA_H - 1) { y[x] = -(int)(rnd() % 8); len[x] = 5 + (int)(rnd() % 12); delay[x] = (int)(rnd() % 30); }
        }
        if (wait_key(6)) break;
    }
    screen_restore();
}

/* ------------------------------------------------------------------ */
/* snake                                                               */
/* ------------------------------------------------------------------ */

#define SN_X0 1
#define SN_X1 78
#define SN_Y0 3
#define SN_Y1 23
#define SN_MAX ((SN_X1 - SN_X0 + 1) * (SN_Y1 - SN_Y0 + 1))

static void cmd_snake(void) {
    static uint8_t sx[SN_MAX], sy[SN_MAX];
    static uint8_t grid[VGA_H][VGA_W];
    screen_save();
    clear_rows(1, VGA_H - 1, 0x00);
    for (int y = 0; y < VGA_H; y++) for (int x = 0; x < VGA_W; x++) grid[y][x] = 0;

    for (int x = 0; x < VGA_W; x++) { cell(x, SN_Y0 - 1, '#', 0x08); cell(x, SN_Y1 + 1, '#', 0x08); }
    for (int y = SN_Y0 - 1; y <= SN_Y1 + 1; y++) { cell(0, y, '#', 0x08); cell(VGA_W - 1, y, '#', 0x08); }

    int head = 2, tail = 0, len = 3;                    /* ring buffer: indices tail..head */
    int cx = 38, cy = (SN_Y0 + SN_Y1) / 2;
    for (int i = 0; i < 3; i++) { sx[i] = (uint8_t)(cx - 2 + i); sy[i] = (uint8_t)cy; grid[cy][cx - 2 + i] = 1; cell(cx - 2 + i, cy, 0xDB, 0x02); }
    cell(cx, cy, 0xDB, 0x0A);

    int dx = 1, dy = 0, score = 0, fx = 0, fy = 0;
    bool over = false, quit = false;

    #define PLACE_FOOD() do { do { fx = SN_X0 + (int)(rnd() % (SN_X1 - SN_X0 + 1)); fy = SN_Y0 + (int)(rnd() % (SN_Y1 - SN_Y0 + 1)); } while (grid[fy][fx]); cell(fx, fy, 0x04, 0x0C); } while (0)
    PLACE_FOOD();

    for (;;) {
        text_at(1, 1, "SNAKE   WASD = steer   Q = quit   score: ", 0x0E);
        { char tmp[11]; int n = 0; uint32_t v = (uint32_t)score; if (!v) tmp[n++] = '0'; while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
          int x = 43; while (n) cell(x++, 1, tmp[--n], 0x0F); cell(x, 1, ' ', 0x0F); }

        uint32_t step = (uint32_t)(10 - score / 4); if (step < 4) step = 4;       /* speeds up as you grow */
        uint32_t t0 = net_now();
        while ((uint32_t)(net_now() - t0) < step) {
            char k = keyboard_pop_char();
            if (k == 'q') { quit = true; break; }
            if (k == 'w' && dy == 0)  { dx = 0; dy = -1; }
            if (k == 's' && dy == 0)  { dx = 0; dy = 1; }
            if (k == 'a' && dx == 0)  { dx = -1; dy = 0; }
            if (k == 'd' && dx == 0)  { dx = 1; dy = 0; }
            __asm__ volatile ("sti; hlt");
        }
        if (quit) break;

        int nx = sx[head] + dx, ny = sy[head] + dy;
        if (nx < SN_X0 || nx > SN_X1 || ny < SN_Y0 || ny > SN_Y1) { over = true; break; }
        bool eat = (nx == fx && ny == fy);
        if (!eat) {                                      /* the tail cell is free this turn */
            grid[sy[tail]][sx[tail]] = 0; cell(sx[tail], sy[tail], ' ', 0x00);
            tail = (tail + 1) % SN_MAX; len--;
        }
        if (grid[ny][nx]) { over = true; break; }       /* ran into itself */

        cell(sx[head], sy[head], 0xDB, 0x02);
        head = (head + 1) % SN_MAX;
        sx[head] = (uint8_t)nx; sy[head] = (uint8_t)ny; len++;
        grid[ny][nx] = 1; cell(nx, ny, 0xDB, 0x0A);

        if (eat) { score++; if (len >= SN_MAX) break; PLACE_FOOD(); }
    }

    if (over || len >= SN_MAX) {
        text_at(27, 12, "                         ", 0x4F);
        text_at(27, 13, "  G A M E   O V E R      ", 0x4F);
        text_at(27, 14, "                         ", 0x4F);
        { const char* m = "  score: "; text_at(27, 14, m, 0x4F);
          char tmp[11]; int n = 0; uint32_t v = (uint32_t)score; if (!v) tmp[n++] = '0'; while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
          int x = 27 + 9; while (n) cell(x++, 14, tmp[--n], 0x4F); }
        text_at(27, 15, " press any key           ", 0x4F);
        while (!wait_key(100)) { }
    }
    screen_restore();
    #undef PLACE_FOOD
}

/* ------------------------------------------------------------------ */
/* sl - a train for people who mistype `ls`                            */
/* ------------------------------------------------------------------ */

static void cmd_sl(void) {
    static const char* train[] = {
        "       o O                         ",
        "     ____   ______________  ____   ",
        "  __|OSIS|__|  OSISOS  []  ||    |  ",
        " |  ____  ___  ____  ___  ___|___|>>",
        " |_/ ()\\_/ () \\_() \\_/ ()\\_/ () \\_|  ",
        "   \\__/   \\__/   \\__/   \\__/   \\__/   ",
    };
    const int tw = 38, rows = 6, y0 = 10;
    screen_save();
    clear_rows(1, VGA_H - 1, 0x00);
    for (int x = 0; x < VGA_W; x++) cell(x, y0 + rows, '=', 0x08);
    for (int pos = VGA_W; pos > -tw; pos--) {
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < tw; c++) {
                int x = pos + c;
                if (x >= 0 && x < VGA_W) cell(x, y0 + r, train[r][c] ? train[r][c] : ' ', r == 0 ? 0x07 : 0x0E);
            }
            if (pos + tw < VGA_W) cell(pos + tw, y0 + r, ' ', 0x00);
        }
        if (wait_key(3)) break;
    }
    screen_restore();
    pr("\n(you meant ls, didn't you?)\n");
}

/* ------------------------------------------------------------------ */
/* crash: deliberately fault the kernel (to see/test the panic screen)  */
/* ------------------------------------------------------------------ */

extern void kpanic(const char* msg);

static void cmd_crash(const char* kind) {
    if (eq(kind, "null"))      { *(volatile int*)0 = 1; }
    else if (eq(kind, "div0")) {   /* C division by zero is undefined behaviour and the compiler deletes it: use the real instruction */
        __asm__ volatile ("xor %%edx, %%edx; mov $1, %%eax; xor %%ecx, %%ecx; div %%ecx" : : : "eax", "ecx", "edx", "cc");
    }
    else if (eq(kind, "ud"))   { __asm__ volatile ("ud2"); }
    else if (eq(kind, "gpf"))  { __asm__ volatile ("mov $0x7F8, %%ax; mov %%ax, %%ds" : : : "ax"); }
    else if (eq(kind, "page")) { *(volatile int*)0x1F000000 = 1; }          /* beyond the 32 MB identity map */
    else if (eq(kind, "panic")){ kpanic("manual panic requested with `crash panic`"); }
    else { pr("usage: crash null|div0|ud|gpf|page|panic   (DEBUG: crashes the kernel on purpose)\n"); }
}

/* ------------------------------------------------------------------ */
/* dispatcher                                                          */
/* ------------------------------------------------------------------ */

static void fake_wipe(void) {
    pr("Deleting everything");
    for (int i = 0; i < 6; i++) { pr("."); wait_key(25); }
    pr("\n\nJust kidding. Nothing was deleted.\nPlease never type that on a machine you care about.\n");
}

bool extras_command(const char* line) {
    while (*line == ' ') line++;
    if (!*line) return false;

    if (eq(line, "sysinfo") || eq(line, "neofetch")) { cmd_sysinfo(); return true; }
    if (eq(line, "dmesg"))    { cmd_dmesg();   return true; }
    if (eq(line, "about") || eq(line, "credits")) { cmd_about(); return true; }
    if (eq(line, "fortune"))  { cmd_fortune(); return true; }
    if (eq(line, "matrix"))   { cmd_matrix();  return true; }
    if (eq(line, "snake"))    { cmd_snake();   return true; }
    if (eq(line, "sl"))       { cmd_sl();      return true; }
    if (eq(line, "crash") || starts(line, "crash ")) { cmd_crash(line[5] ? line + 6 : ""); return true; }
    if (eq(line, "cowsay") || starts(line, "cowsay ")) { cmd_cowsay(line[6] ? line + 7 : ""); return true; }

    /* easter eggs */
    if (eq(line, "rm -rf /") || eq(line, "rm -rf /*") || eq(line, "rm -rf *") || eq(line, "rm -rf ~")) { fake_wipe(); return true; }
    if (eq(line, "sudo make me a sandwich")) { pr("Okay.\n"); return true; }
    if (eq(line, "make me a sandwich"))      { pr("What? Make it yourself.\n"); return true; }
    if (eq(line, "sudo") || starts(line, "sudo ")) { pr("admin is not in the sudoers file. This incident will be reported.\n(It won't. Nobody is listening.)\n"); return true; }
    if (eq(line, "xyzzy"))   { pr("Nothing happens.\n"); return true; }
    if (eq(line, "plugh"))   { pr("A hollow voice says \"Fool.\"\n"); return true; }
    if (eq(line, "42"))      { pr("The Answer to the Ultimate Question of Life, the Universe, and Everything.\n"); return true; }
    if (eq(line, "hello") || eq(line, "hi")) { pr("Hello, world! (the kernel says hi back)\n"); return true; }
    if (eq(line, "brew coffee") || eq(line, "make coffee")) {
        pr("418 I'm a teapot (RFC 2324).\nThe web server agrees:  http <your-ip> 80 /teapot\n"); return true;
    }
    return false;
}
