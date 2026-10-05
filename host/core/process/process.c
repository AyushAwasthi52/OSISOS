#include "osisos.h"
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdio.h>

// Creates a new process to run a specific command
osisos_pid_t osisos_process_create(const char* path, char* const argv[]) {
    // fork() creates a duplicate of the current process.
    // It returns 0 to the child process, and the child's PID to the parent.
    pid_t pid = fork();

    if (pid < 0) {
        // Fork failed (e.g., out of memory or process limits reached)
        perror("fork failed");
        return -1;
    }

    if (pid == 0) {
        // This code block only runs in the CHILD process.
        // execvp replaces the child process's memory space with the new program.
        if (execvp(path, argv) == -1) {
            perror("execvp failed");
            _exit(1); // Exit immediately if exec fails so we don't have two shells
        }
    }

    // This code block only runs in the PARENT process.
    // We return the child's process ID so the OSISOS system can track it.
    return (osisos_pid_t)pid;
}

// Waits for a specific process to finish execution
int osisos_process_wait(osisos_pid_t pid, int* status) {
    // waitpid pauses the parent process until the specified child PID exits.
    if (waitpid((pid_t)pid, status, 0) == -1) {
        perror("waitpid failed");
        return -1;
    }
    return 0;
}

// Sends a signal to a process (like SIGTERM or SIGKILL)
int osisos_process_kill(osisos_pid_t pid, int sig) {
    // The kill() system call sends a signal to a process.
    if (kill((pid_t)pid, sig) == -1) {
        perror("kill failed");
        return -1;
    }
    return 0;
}
