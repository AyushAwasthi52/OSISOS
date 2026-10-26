#ifndef RCON_H
#define RCON_H

#include <stdint.h>
#include <stdbool.h>

/*
 * OSISOS remote console ("rcon"): a password-protected shell over TCP.
 *
 *   port 2324  SECURE   Noise_NK_25519_ChaChaPoly_SHA256: encrypted + server-authenticated.
 *                       Use tools/osisos_ssh.py (or any Noise NK client). `rcon on` enables it.
 *   port 2323  PLAIN    unencrypted telnet-style text (nc / telnet). Legacy; `rcon plain on`.
 *
 * Safe defaults: BOTH are disabled until an administrator enables them at the console; remote logins are
 * refused while the account still has the default password; one session at a time; its own login-throttle
 * counters (network guessing cannot lock the local console); idle timeout; every login, failure and command
 * is written to the kernel log (`dmesg`).
 *
 * Execution model: the network task handles sockets, the handshake and line editing; commands run in the
 * SHELL task (so blocking commands such as ping/fetch work), with their output redirected into the session.
 */

#define RCON_PORT          2323          /* plaintext */
#define RCON_SECURE_PORT   2324          /* Noise */
#define RCON_LINE_MAX      200
#define RCON_IDLE_TICKS    (300 * 100)
#define RCON_PROLOGUE      "OSISOS-RCON-1"

#define RCON_PLAIN   0
#define RCON_SECURE  1

void rcon_init(void);                                  /* read the /etc flags, start listeners that were enabled */
void rcon_poll(void);                                  /* network task: accept, handshake, read, prompt */
void rcon_set(int mode, bool on, bool persist);
bool rcon_new_key(void);                               /* replace the server identity (clients must re-pin) */
bool rcon_get_pubkey(uint8_t out[32]);                 /* false if no key exists yet */
bool rcon_key_persistent(void);

/* shell task side */
bool rcon_take_command(char* out, int max);
void rcon_exec_begin(void);
void rcon_exec_end(void);

typedef struct {
    bool     plain_enabled, secure_enabled;
    bool     session_active, session_secure;
    uint32_t peer_ip;
    char     user[24];
    uint32_t idle_secs;
} rcon_status_t;
void rcon_get_status(rcon_status_t* out);

#endif
