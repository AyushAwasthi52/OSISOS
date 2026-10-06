#include "task.h"
#include "../mm/kheap.h"
#include <stddef.h>

/* Global pointers to manage the scheduler queue */
volatile task_t* current_task = NULL;
volatile task_t* ready_queue = NULL;

/* The next available Process ID */
static uint32_t next_pid = 1;

/* We import the global page directory we created during init_paging() */
extern uint32_t* kernel_page_directory;

/* Inline Assembly helper to read the current Stack Pointer */
static inline uint32_t read_esp(void) {
    uint32_t esp;
    __asm__ volatile("mov %%esp, %0" : "=r"(esp));
    return esp;
}

/* Inline Assembly helper to read the current Base Pointer */
static inline uint32_t read_ebp(void) {
    uint32_t ebp;
    __asm__ volatile("mov %%ebp, %0" : "=r"(ebp));
    return ebp;
}

void init_tasking(void) {
    /* Disable hardware interrupts while we mess with critical OS data structures */
    __asm__ volatile("cli");
    
    /* 
     * We allocate the very first Process Control Block (PCB) out of our newly built Kernel Heap!
     * This PCB will represent the main boot thread of the kernel itself.
     */
    current_task = (task_t*) kmalloc(sizeof(task_t));
    
    /* Fill out its identity card */
    current_task->pid = next_pid++;
    current_task->esp = read_esp();
    current_task->ebp = read_ebp();
    current_task->page_directory = kernel_page_directory;
    current_task->next = NULL;
    
    /* Put it at the head of the ready queue */
    ready_queue = current_task;
    
    /* Safe to turn interrupts back on! */
    __asm__ volatile("sti");
}

/* External reference to our new assembly context switcher */
extern void perform_task_switch(uint32_t new_esp, uint32_t new_cr3, uint32_t* old_esp_ptr);

void create_task(void (*entry_point)(void)) {
    __asm__ volatile("cli");
    
    /* Allocate the PCB */
    task_t* new_task = (task_t*) kmalloc(sizeof(task_t));
    new_task->pid = next_pid++;
    new_task->page_directory = kernel_page_directory;
    new_task->next = NULL;

    /* Allocate a 4KB stack exclusively for this task */
    uint32_t stack_base = (uint32_t) kmalloc(4096);
    uint32_t* stack = (uint32_t*)(stack_base + 4096); /* Top of the stack */

    /* 
     * We must "fake" the stack so it looks exactly as if this task had called 
     * `perform_task_switch` in the past and is waiting to return!
     * `perform_task_switch` pops 4 registers, then pops EIP (the return address).
     */
    *(--stack) = (uint32_t) entry_point; /* The EIP (The function it will run!) */
    *(--stack) = 0; /* EBX */
    *(--stack) = 0; /* ESI */
    *(--stack) = 0; /* EDI */
    *(--stack) = 0; /* EBP */

    new_task->esp = (uint32_t) stack;
    
    /* Add this task to the end of the Scheduler's Ready Queue */
    task_t* tmp = (task_t*)ready_queue;
    while (tmp->next != NULL) {
        tmp = tmp->next;
    }
    tmp->next = new_task;
    
    __asm__ volatile("sti");
}

void task_yield(void) {
    __asm__ volatile("cli");
    
    /* If there is no next task, just keep running the current one */
    if (current_task == NULL || current_task->next == NULL) {
        __asm__ volatile("sti");
        return;
    }
    
    /* Grab the current task */
    task_t* old_task = (task_t*)current_task;
    
    /* Move to the next task in the queue */
    current_task = current_task->next;
    
    /* Put the old task at the very end of the queue (Round-Robin) */
    task_t* tmp = (task_t*)current_task;
    while (tmp->next != NULL) {
        tmp = tmp->next;
    }
    tmp->next = old_task;
    old_task->next = NULL;
    
    /* EXECUTE THE CONTEXT SWITCH! */
    perform_task_switch(current_task->esp, (uint32_t)current_task->page_directory, &old_task->esp);
    
    __asm__ volatile("sti");
}
