#ifndef AUTH_H
#define AUTH_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Authentication: salted, iterated password hashes (PBKDF2-HMAC-SHA256) stored in the OFS file
 * /etc/shadow, plus login throttling.
 *
 * - First boot (no /etc/shadow): account "admin" is created with the default password "osisos"
 *   and flagged "must change" - the shell warns until `passwd` is run.
 * - 3 consecutive failures start a lockout that doubles each time (5 s, 10 s, ... max 5 min);
 *   while locked, passwords are not even checked, so guessing is rate-limited.
 * - Unknown user names cost the same time as wrong passwords (no user enumeration by timing).
 * - Failures and successes are written to the kernel log (`dmesg`).
 */

#define AUTH_MIN_PASSWORD 6
#define AUTH_MAX_PASSWORD 63

typedef enum { AUTH_OK = 0, AUTH_BAD, AUTH_LOCKED } auth_result_t;

void          auth_init(void);
auth_result_t auth_verify(const char* user, const char* password, uint32_t* wait_secs);
/* Returns true if the new hash was also saved to disk. */
bool          auth_set_password(const char* user, const char* new_password, bool* persisted);
bool          auth_has_default_password(const char* user);
bool          auth_selftest(void);        /* SHA-256 / PBKDF2 known-answer tests */

#endif
