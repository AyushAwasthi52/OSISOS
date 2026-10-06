#include "gdt.h"

// Let's declare an external assembly function that actually loads the GDT pointer
extern void gdt_flush(uint32_t);
// Declare the assembly function to load the TSS
extern void tss_flush(void);

// Our GDT, with 6 entries now (Null, Kernel Code, Kernel Data, User Code, User Data, TSS)
gdt_entry_t gdt_entries[6];
gdt_ptr_t   gdt_ptr;
tss_entry_t tss_entry;

// A helper function to easily set up a GDT entry
static void gdt_set_gate(int32_t num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt_entries[num].base_low    = (base & 0xFFFF);
    gdt_entries[num].base_middle = (base >> 16) & 0xFF;
    gdt_entries[num].base_high   = (base >> 24) & 0xFF;

    gdt_entries[num].limit_low   = (limit & 0xFFFF);
    gdt_entries[num].granularity = (limit >> 16) & 0x0F;

    gdt_entries[num].granularity |= gran & 0xF0;
    gdt_entries[num].access      = access;
}

// A helper function to initialize the TSS entry
static void write_tss(int32_t num, uint16_t ss0, uint32_t esp0) {
    // Compute the base and limit of our TSS structure
    uint32_t base = (uint32_t) &tss_entry;
    uint32_t limit = base + sizeof(tss_entry_t);

    // Add the TSS descriptor to the GDT (Access: 0xE9, Granularity: 0x00)
    gdt_set_gate(num, base, limit, 0xE9, 0x00);

    // Ensure the TSS is initially totally empty
    for (uint32_t i = 0; i < sizeof(tss_entry_t); i++) {
        ((uint8_t*)&tss_entry)[i] = 0;
    }

    // Set the kernel stack segment and stack pointer.
    // When an interrupt happens in User Mode, the CPU reads this to know where to jump!
    tss_entry.ss0  = ss0;
    tss_entry.esp0 = esp0;

    // Set the User Mode segments
    // 0x13 is (0x10 | 3) -> Data Segment | Ring 3 Privilege
    tss_entry.cs   = 0x0B; // (0x08 | 3) -> Code Segment | Ring 3 Privilege
    tss_entry.ss = tss_entry.ds = tss_entry.es = tss_entry.fs = tss_entry.gs = 0x13; 
}

void init_gdt(void) {
    // Set the limits and base for our GDT pointer
    gdt_ptr.limit = (sizeof(gdt_entry_t) * 6) - 1;
    gdt_ptr.base  = (uint32_t)&gdt_entries;

    // Entry 0: The Null Segment (Required by the CPU)
    gdt_set_gate(0, 0, 0, 0, 0);

    // Entry 1: The Kernel Code Segment (Ring 0)
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);

    // Entry 2: The Kernel Data Segment (Ring 0)
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    // Entry 3: The User Code Segment (Ring 3) -> Access 0xFA
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);

    // Entry 4: The User Data Segment (Ring 3) -> Access 0xF2
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    // Entry 5: The Task State Segment (TSS)
    // We initialize it with Kernel Data Segment (0x10) and an empty stack pointer for now.
    write_tss(5, 0x10, 0x0);

    // Tell the CPU where our new GDT is located and reload segment registers
    gdt_flush((uint32_t)&gdt_ptr);
    
    // Tell the CPU where the TSS is located!
    tss_flush();
}

void set_kernel_stack(uint32_t stack) {
    // This is called by the Context Switcher so the CPU always knows 
    // where the CURRENT task's kernel stack is located!
    tss_entry.esp0 = stack;
}
