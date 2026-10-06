#ifndef TASK_H
#define TASK_H

#include <stdint.h>

/* Process Control Block (PCB) 
 * This structure holds the entire "identity" of a running program.
 */
typedef struct task {
    uint32_t pid;                /* Process ID */
    uint32_t esp;                /* Stack Pointer - Where the program's stack is */
    uint32_t ebp;                /* Base Pointer */
    uint32_t* page_directory;    /* The translation dictionary for this program's memory */
    struct task* next;           /* Pointer to the next task in the scheduler queue */
} task_t;

/* Capture the current boot thread and turn it into PID 1 */
void init_tasking(void);

#endif
