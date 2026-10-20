/*
 * panic.c - CPU exception handling and the kernel panic screen.
 *
 * Any CPU exception lands in exception_handler() (via arch/i386/exceptions.S). The kernel
 * cannot safely continue after one, so we:
 *   1. write the details to the kernel log,
 *   2. paint a panic screen straight into VGA memory (no dependency on the terminal code,
 *      which may be what crashed): exception, decoded cause, registers, stack words,
 *      recent log lines,
 *   3. count down and reboot (R = now, H = halt and keep the screen).
 *
 * Interrupts are off for the whole thing, so the timer and scheduler are frozen; timing
 * uses the RTC seconds register and the keyboard is polled directly.
 */
#include "io.h"
#include "klog.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PANIC_REBOOT_SECS 15

typedef struct {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_pusha, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;
    uint32_t useresp, ss;                      /* only valid when the fault came from ring 3 */
} regs_t;

extern uint32_t seconds_passed;

static const char* exc_names[32] = {
    "Divide Error", "Debug", "Non-Maskable Interrupt", "Breakpoint", "Overflow",
    "BOUND Range Exceeded", "Invalid Opcode", "Device Not Available", "Double Fault",
    "Coprocessor Segment Overrun", "Invalid TSS", "Segment Not Present", "Stack-Segment Fault",
    "General Protection Fault", "Page Fault", "Reserved", "x87 Floating-Point Error",
    "Alignment Check", "Machine Check", "SIMD Floating-Point Exception", "Virtualization Exception",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Security Exception", "Reserved"
};

/* ------------------------------------------------------------------ */
/* direct VGA output                                                   */
/* ------------------------------------------------------------------ */

static volatile uint16_t* const vga = (volatile uint16_t*)0xB8000;
static int row, col;
static uint8_t attr = 0x4F;                    /* white on red */

static void put(char c) {
    if (c == '\n') { col = 0; row++; return; }
    if (row >= 25) return;
    vga[row * 80 + col] = (uint16_t)(uint8_t)c | ((uint16_t)attr << 8);
    if (++col >= 80) { col = 0; row++; }
}
static void puts_(const char* s) { while (*s) put(*s++); }
static void hex(uint32_t v) {
    static const char* d = "0123456789ABCDEF";
    for (int s = 28; s >= 0; s -= 4) put(d[(v >> s) & 15]);
}
static void dec(uint32_t v) {
    char t[11]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) put(t[--n]);
}
static void reg(const char* name, uint32_t v) { puts_(name); put('='); hex(v); puts_("  "); }
static void at(int r, int c) { row = r; col = c; }

static void fill(uint8_t a) {
    attr = a;
    for (int i = 0; i < 80 * 25; i++) vga[i] = (uint16_t)' ' | ((uint16_t)a << 8);
}

/* ------------------------------------------------------------------ */
/* time + keyboard without interrupts                                  */
/* ------------------------------------------------------------------ */

static uint8_t rtc_seconds(void) {
    for (int i = 0; i < 100000; i++) {         /* wait out an in-progress update */
        outb(0x70, 0x0A);
        if (!(inb(0x71) & 0x80)) break;
    }
    outb(0x70, 0x00);
    return inb(0x71);
}

static int poll_key(void) {                    /* returns a set-1 make code, or 0 */
    if (inb(0x64) & 1) {
        uint8_t sc = inb(0x60);
        if (!(sc & 0x80)) return sc;
    }
    return 0;
}

static void reboot_now(void) {
    for (int i = 0; i < 100000 && (inb(0x64) & 0x02); i++) { }
    outb(0x64, 0xFE);                          /* pulse the CPU reset line via the 8042 */
    for (;;) __asm__ volatile ("cli; hlt");
}

static void halt_forever(void) { for (;;) __asm__ volatile ("cli; hlt"); }

static bool in_panic;

static void panic_finish(void) {
    int bottom = 23;
    at(bottom, 1);
    attr = 0x4E;
    puts_(" R = reboot now    H = halt and keep this screen ");
    uint8_t last = rtc_seconds();
    int left = PANIC_REBOOT_SECS;
    for (;;) {
        at(bottom - 1, 1); attr = 0x4F;
        puts_("Rebooting in "); dec((uint32_t)left); puts_(" s ...  ");
        int waited = 0;
        while (waited < 1) {
            int k = poll_key();
            if (k == 0x13) reboot_now();       /* R */
            if (k == 0x23) {                   /* H */
                at(bottom - 1, 1); puts_("System halted.                    ");
                halt_forever();
            }
            uint8_t now = rtc_seconds();
            if (now != last) { last = now; waited++; }
        }
        if (--left <= 0) reboot_now();
    }
}

/* ------------------------------------------------------------------ */
/* the panic screens                                                   */
/* ------------------------------------------------------------------ */

static void draw_log_tail(void) {
    static char buf[600];
    klog_read(buf, sizeof(buf), 6);
    at(15, 1); attr = 0x4E; puts_("Last kernel log lines:");
    attr = 0x4F;
    at(16, 1);
    int lines = 0;
    for (const char* p = buf; *p && lines < 6; p++) {
        if (*p == '\n') { lines++; at(16 + lines, 1); continue; }
        if (col < 78) put(*p);
    }
}

void exception_handler(regs_t* r) {
    __asm__ volatile ("cli");                  /* (already off for interrupt gates; be explicit) */
    if (in_panic) halt_forever();              /* a fault inside the panic code itself */
    in_panic = true;

    uint32_t cr0, cr2, cr3;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    bool user = (r->cs & 3) == 3;
    const char* name = r->int_no < 32 ? exc_names[r->int_no] : "Unknown";

    klog("PANIC: %s (#%u) err=%u eip=%u cs=%u cr2=%u%s", name, r->int_no, r->err_code, r->eip, r->cs, cr2, user ? " [user mode]" : "");

    fill(0x4F);
    at(0, 2); attr = 0x1F; puts_("  K E R N E L   P A N I C  "); attr = 0x4F;

    at(2, 1); puts_(name); puts_(" (#"); dec(r->int_no); puts_(")   error code "); hex(r->err_code);
    at(3, 1);
    if (r->int_no == 14) {
        if (cr2 < 0x1000) puts_("NULL pointer dereference");
        else if (!(r->err_code & 1)) puts_("access to an unmapped address");
        else puts_("page protection violation");
        puts_(r->err_code & 2 ? " (write" : " (read"); puts_(r->err_code & 4 ? ", user mode)" : ", kernel mode)");
        if (r->err_code & 16) puts_(" [instruction fetch]");
        puts_(" at "); hex(cr2);
    } else if (r->int_no == 0)  puts_("Division by zero");
    else if (r->int_no == 6)  puts_("Invalid opcode: the CPU hit an instruction it cannot execute (bad jump?)");
    else if (r->int_no == 13) puts_("Privilege or segment violation (selector/error code above)");
    else if (r->int_no == 8)  puts_("Double fault: an exception happened while handling another one");
    else if (r->int_no == 12) puts_("Stack fault: likely stack overflow");
    else puts_("See the Intel SDM, vol. 3, chapter 6");
    if (user) puts_("  [fault came from user mode]");

    at(5, 1); reg("EIP", r->eip); reg("CS", r->cs); reg("EFLAGS", r->eflags);
    at(6, 1); reg("EAX", r->eax); reg("EBX", r->ebx); reg("ECX", r->ecx); reg("EDX", r->edx);
    at(7, 1); reg("ESI", r->esi); reg("EDI", r->edi); reg("EBP", r->ebp);
    uint32_t esp = user ? r->useresp : (uint32_t)&r->useresp;       /* the CPU pushes ESP only for ring 3 */
    reg("ESP", esp);
    at(8, 1); reg("CR0", cr0); reg("CR2", cr2); reg("CR3", cr3);

    at(10, 1); attr = 0x4E; puts_("Stack (top 8 words):"); attr = 0x4F;
    at(11, 1);
    if (esp >= 0x1000 && esp < 0x2000000 - 64) {
        const uint32_t* sp = (const uint32_t*)esp;
        for (int i = 0; i < 8; i++) { hex(sp[i]); put(' '); if (i == 3) at(12, 1); }
    } else puts_("(stack pointer looks invalid)");

    at(14, 1); puts_("Uptime: "); dec(seconds_passed); puts_(" s");
    draw_log_tail();
    panic_finish();
}

/* Software panic for unrecoverable conditions detected by kernel code. */
void kpanic(const char* msg) {
    /* Called as an ordinary function with interrupts ON: turn them off first, or the timer would keep
     * redrawing the clock over the panic screen, the scheduler would keep switching tasks, and the
     * keyboard IRQ would eat the R/H keypress before we can poll it. */
    __asm__ volatile ("cli");
    if (in_panic) halt_forever();
    in_panic = true;
    klog("PANIC: %s", msg);
    fill(0x4F);
    at(0, 2); attr = 0x1F; puts_("  K E R N E L   P A N I C  "); attr = 0x4F;
    at(2, 1); puts_(msg);
    at(14, 1); puts_("Uptime: "); dec(seconds_passed); puts_(" s");
    draw_log_tail();
    panic_finish();
}
