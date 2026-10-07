#ifndef TASK_H
#define TASK_H

#include <stdint.h>

/* Process Control Block (PCB) 
 * This structure holds the entire "identity" of a running program.
 */
typedef struct task {
    uint32_t pid;                /* Process ID */
    uint32_t esp;                /* Stack Pointer - Where the program's stack is right now */
    uint32_t ebp;                /* Base Pointer */
    uint32_t kernel_stack;       /* Top of the kernel stack (for TSS) */
    uint32_t* page_directory;    /* The translation dictionary for this program's memory */
    struct task* next;           /* Pointer to the next task in the scheduler queue */
} task_t;

/* Capture the current boot thread and turn it into PID 1 */
void init_tasking(void);

/* Create a brand new Task with its own stack */
void create_task(void (*entry_point)(void));

/* Voluntarily yield the CPU to the next Task in the queue */
void task_yield(void);

/* Jump down to Ring 3 (User Mode) */
void jump_usermode(uint32_t user_eip, uint32_t user_esp);

/* Print all running tasks */
void task_list_all(void);

#endif
