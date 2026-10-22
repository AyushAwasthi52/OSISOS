#ifndef RCON_H
#define RCON_H

#include <stdint.h>
#include <stdbool.h>

/*
 * OSISOS remote console ("rcon"): a password-protected shell over plain TCP, port 2323.
 * Works with `nc <ip> 2323` and with a telnet client.
 *
 *   *** The protocol is UNENCRYPTED. Use it only on a network you trust. ***
 *
 * Safe defaults: disabled until an administrator runs `rcon on` at the console; remote logins are
 * refused while the account still has the default password; one session at a time; its own
 * login-throttle counters (network guessing cannot lock the local console); 5 minute idle timeout;
 * every login, failure and command is written to the kernel log (`dmesg`).
 *
 * Execution model: the network task handles sockets and line editing; commands run in the SHELL task
 * (so blocking commands such as ping/fetch work), with their output redirected into the session.
 */

#define RCON_PORT        2323
#define RCON_LINE_MAX    200
#define RCON_IDLE_TICKS  (300 * 100)

void rcon_init(void);                                  /* read /etc/rcon, start listening if enabled */
void rcon_poll(void);                                  /* network task: accept, read, prompt */
void rcon_enable(bool on, bool persist);

/* shell task side */
bool rcon_take_command(char* out, int max);            /* a command is waiting? */
void rcon_exec_begin(void);                            /* start capturing terminal output */
void rcon_exec_end(void);                              /* flush, stop capturing, prompt again */

typedef struct {
    bool     enabled;
    bool     session_active;
    uint32_t peer_ip;
    char     user[24];
    uint32_t idle_secs;
} rcon_status_t;
void rcon_get_status(rcon_status_t* out);

#endif
