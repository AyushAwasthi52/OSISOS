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

/* Import the TSS updater from gdt.c */
extern void set_kernel_stack(uint32_t stack);

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
    __asm__ volatile("cli");
    
    current_task = (task_t*) kmalloc(sizeof(task_t));
    current_task->pid = next_pid++;
    current_task->esp = read_esp();
    current_task->ebp = read_ebp();
    /* For PID 1 (Boot thread), we just approximate the initial stack base */
    current_task->kernel_stack = 0x80000; 
    current_task->page_directory = kernel_page_directory;
    current_task->next = NULL;
    
    ready_queue = current_task;
    __asm__ volatile("sti");
}

extern void perform_task_switch(uint32_t new_esp, uint32_t new_cr3, uint32_t* old_esp_ptr);

void task_entry_wrapper(void (*entry_point)(void)) {
    __asm__ volatile("sti");
    entry_point();
    while(1) {
        __asm__ volatile("hlt");
    }
}

void create_task(void (*entry_point)(void)) {
    __asm__ volatile("cli");
    
    task_t* new_task = (task_t*) kmalloc(sizeof(task_t));
    new_task->pid = next_pid++;
    new_task->page_directory = kernel_page_directory;
    new_task->next = NULL;

    uint32_t stack_base = (uint32_t) kmalloc(4096);
    uint32_t* stack = (uint32_t*)(stack_base + 4096); 
    
    /* Save the top of the stack so we can give it to the TSS later! */
    new_task->kernel_stack = (uint32_t) stack;

    *(--stack) = (uint32_t) entry_point;
    *(--stack) = 0;
    
    *(--stack) = (uint32_t) task_entry_wrapper;
    *(--stack) = 0; /* EBX */
    *(--stack) = 0; /* ESI */
    *(--stack) = 0; /* EDI */
    *(--stack) = 0; /* EBP */

    new_task->esp = (uint32_t) stack;
    
    task_t* tmp = (task_t*)ready_queue;
    while (tmp->next != NULL) {
        tmp = tmp->next;
    }
    tmp->next = new_task;
    
    __asm__ volatile("sti");
}

void task_yield(void) {
    __asm__ volatile("cli");
    
    if (current_task == NULL || current_task->next == NULL) {
        __asm__ volatile("sti");
        return;
    }
    
    task_t* old_task = (task_t*)current_task;
    current_task = current_task->next;
    
    task_t* tmp = (task_t*)current_task;
    while (tmp->next != NULL) {
        tmp = tmp->next;
    }
    tmp->next = old_task;
    old_task->next = NULL;
    
    /* UPDATE THE TSS! 
     * Tell the CPU where the new task's kernel stack is. If a User Mode program 
     * triggers an interrupt, the CPU will jump exactly to this stack. 
     */
    set_kernel_stack(current_task->kernel_stack);
    
    /* EXECUTE THE CONTEXT SWITCH! */
    perform_task_switch(current_task->esp, (uint32_t)current_task->page_directory, &old_task->esp);
    
    __asm__ volatile("sti");
}
