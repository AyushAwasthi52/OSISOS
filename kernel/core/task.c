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
