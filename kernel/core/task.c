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

/* Wrapper to ensure new tasks always start with interrupts enabled */
void task_entry_wrapper(void (*entry_point)(void)) {
    /* Enable interrupts so the Timer and Keyboard can fire! */
    __asm__ volatile("sti");
    
    /* Jump into the actual task code */
    entry_point();
    
    /* If the task function ever finishes and returns, trap it safely */
    while(1) {
        __asm__ volatile("hlt");
    }
}

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
     * Construct the C function call stack for task_entry_wrapper(entry_point)
     */
    *(--stack) = (uint32_t) entry_point;        /* Argument passed to the wrapper */
    *(--stack) = 0;                             /* Fake return address for the wrapper */
    
    /* Construct the assembly stack for perform_task_switch */
    *(--stack) = (uint32_t) task_entry_wrapper; /* The EIP (perform_task_switch returns to here) */
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
