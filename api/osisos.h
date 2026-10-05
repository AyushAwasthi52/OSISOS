#ifndef OSISOS_H
#define OSISOS_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

// ---------------------------------------------------------
// OSISOS Core API
// ---------------------------------------------------------

// --- Process Management ---
typedef int osisos_pid_t;

osisos_pid_t osisos_process_create(const char* path, char* const argv[]);
int osisos_process_kill(osisos_pid_t pid, int sig);
int osisos_process_wait(osisos_pid_t pid, int* status);
int osisos_process_list(void);
int osisos_process_info(osisos_pid_t pid);

// --- Memory Management ---
typedef struct {
    size_t total_ram;
    size_t free_ram;
    size_t used_ram;
} osisos_mem_stats_t;

int osisos_mem_stats(osisos_mem_stats_t* stats);
int osisos_mem_process_stats(osisos_pid_t pid);
int osisos_mem_limit_set(osisos_pid_t pid, size_t limit_bytes);

// --- Filesystem Management ---
int osisos_fs_open(const char* path, int flags, int mode);
ssize_t osisos_fs_read(int fd, void* buf, size_t count);
ssize_t osisos_fs_write(int fd, const void* buf, size_t count);
int osisos_fs_stat(const char* path, void* statbuf);
int osisos_fs_mkdir(const char* path, int mode);
int osisos_fs_unlink(const char* path);

// --- IPC ---
int osisos_ipc_pipe(int pipefd[2]);
int osisos_ipc_msg_send(int msqid, const void* msgp, size_t msgsz, int msgflg);
ssize_t osisos_ipc_msg_recv(int msqid, void* msgp, size_t msgsz, long msgtyp, int msgflg);
int osisos_ipc_shm_create(const char* name, size_t size);

// --- Concurrency ---
typedef void* osisos_thread_t;
typedef void* osisos_mutex_t;
typedef void* osisos_sem_t;

int osisos_thread_create(osisos_thread_t* thread, void *(*start_routine) (void *), void *arg);
int osisos_mutex_lock(osisos_mutex_t* mutex);
int osisos_mutex_unlock(osisos_mutex_t* mutex);
int osisos_sem_wait(osisos_sem_t* sem);
int osisos_sem_post(osisos_sem_t* sem);

// --- Networking ---
int osisos_net_if_list(void);
int osisos_net_if_up(const char* ifname);
int osisos_net_if_down(const char* ifname);
int osisos_net_socket(int domain, int type, int protocol);
int osisos_net_connect(int sockfd, const void* addr, socklen_t addrlen);
ssize_t osisos_net_send(int sockfd, const void* buf, size_t len, int flags);
ssize_t osisos_net_recv(int sockfd, void* buf, size_t len, int flags);

// --- Users & Security ---
int osisos_user_create(const char* username, const char* password);
int osisos_user_auth(const char* username, const char* password);
int osisos_perm_check(const char* path, int mode);

// --- Service Management ---
int osisos_service_start(const char* service_name);
int osisos_service_stop(const char* service_name);
int osisos_service_status(const char* service_name);

#endif // OSISOS_H
