#include "auth.h"
#include "sha256.h"
#include "klog.h"
#include "ofs.h"
#include <stddef.h>

extern uint32_t tick;                      /* 100 Hz */

#define AUTH_ITERATIONS 4096
#define MAX_USERS       4
#define SHADOW_PATH     "/etc/shadow"

typedef struct {
    bool     used;
    bool     must_change;
    char     name[24];
    uint32_t iters;
    uint8_t  salt[16];
    uint8_t  hash[32];
} user_t;

static user_t   users[MAX_USERS];
static bool     persistent;
static uint32_t fails_ch[2];
static uint32_t locked_until_ch[2];         /* tick, per channel */

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static size_t slen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }

static bool seq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* constant-time comparison */
static bool ct_equal(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

static void wipe(void* p, size_t n) { volatile uint8_t* v = (volatile uint8_t*)p; while (n--) *v++ = 0; }

static uint8_t pool[32];

/* Salts only need to be unique, not secret. Mix whatever entropy a bare VM offers. */
static void random_bytes(uint8_t* out, size_t n) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    uint32_t t = *(volatile uint32_t*)&tick;
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, pool, sizeof(pool));
    sha256_update(&c, &lo, 4); sha256_update(&c, &hi, 4); sha256_update(&c, &t, 4);
    sha256_update(&c, &c, 16);                            /* stack address / layout */
    sha256_final(&c, pool);
    for (size_t i = 0; i < n; i++) out[i] = pool[i % 32];
}

static void hex_encode(const uint8_t* in, size_t n, char* out) {
    static const char* d = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[i*2] = d[in[i] >> 4]; out[i*2+1] = d[in[i] & 15]; }
    out[n*2] = '\0';
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool hex_decode(const char* in, size_t n_bytes, uint8_t* out) {
    for (size_t i = 0; i < n_bytes; i++) {
        int h = hexval(in[i*2]), l = (h >= 0) ? hexval(in[i*2+1]) : -1;
        if (h < 0 || l < 0) return false;
        out[i] = (uint8_t)(h * 16 + l);
    }
    return true;
}

static void make_hash(user_t* u, const char* password) {
    pbkdf2_hmac_sha256(password, slen(password), u->salt, sizeof(u->salt), u->iters, u->hash);
}

/* ------------------------------------------------------------------ */
/* persistence: one line per user:  name:iters:salt-hex:hash-hex:must_change */
/* ------------------------------------------------------------------ */

static bool save_shadow(void) {
    if (!ofs_available()) return false;
    static char buf[512];
    size_t n = 0;
    for (int i = 0; i < MAX_USERS; i++) {
        if (!users[i].used) continue;
        char salt[33], hash[65], iters[12];
        hex_encode(users[i].salt, 16, salt);
        hex_encode(users[i].hash, 32, hash);
        uint32_t v = users[i].iters; int k = 0; char t[11]; int tn = 0;
        do { t[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (tn) iters[k++] = t[--tn];
        iters[k] = '\0';
        const char* parts[9] = { users[i].name, ":", iters, ":", salt, ":", hash, ":", users[i].must_change ? "1\n" : "0\n" };
        for (int p = 0; p < 9; p++) { size_t l = slen(parts[p]); if (n + l >= sizeof(buf)) return false; for (size_t j = 0; j < l; j++) buf[n++] = parts[p][j]; }
    }
    return ofs_put(SHADOW_PATH, buf, (uint32_t)n);
}

static bool load_shadow(void) {
    if (!ofs_available()) return false;
    int idx = ofs_lookup(SHADOW_PATH);
    if (idx < 0) return false;
    static char buf[513];
    int n = ofs_inode_read(idx, 0, buf, 512);
    if (n <= 0) return false;
    buf[n] = '\0';

    int count = 0;
    for (char* line = buf; *line && count < MAX_USERS; ) {
        char* f[5]; int nf = 0;
        char* p = line;
        f[nf++] = p;
        while (*p && *p != '\n') { if (*p == ':' && nf < 5) { *p = '\0'; f[nf++] = p + 1; } p++; }
        if (*p == '\n') *p++ = '\0';
        line = p;
        if (nf != 5 || !f[0][0] || slen(f[0]) >= sizeof(users[0].name) || slen(f[2]) != 32 || slen(f[3]) != 64) return false;

        user_t* u = &users[count];
        u->used = true;
        for (size_t i = 0; i <= slen(f[0]); i++) u->name[i] = f[0][i];
        u->iters = 0;
        for (const char* d = f[1]; *d >= '0' && *d <= '9'; d++) u->iters = u->iters * 10 + (uint32_t)(*d - '0');
        if (u->iters == 0 || u->iters > 1000000) return false;
        if (!hex_decode(f[2], 16, u->salt) || !hex_decode(f[3], 32, u->hash)) return false;
        u->must_change = (f[4][0] == '1');
        count++;
    }
    return count > 0;
}

static void create_default_admin(void) {
    for (int i = 0; i < MAX_USERS; i++) users[i].used = false;
    user_t* u = &users[0];
    u->used = true;
    u->must_change = true;
    const char* nm = "admin";
    for (int i = 0; i <= 5; i++) u->name[i] = nm[i];
    u->iters = AUTH_ITERATIONS;
    random_bytes(u->salt, 16);
    make_hash(u, "osisos");
}

void auth_init(void) {
    if (load_shadow()) { persistent = true; klog("auth: loaded %s", SHADOW_PATH); return; }

    bool had_file = ofs_available() && ofs_lookup(SHADOW_PATH) >= 0;
    create_default_admin();
    if (ofs_available()) {
        if (ofs_lookup("/etc") < 0) ofs_mkdir("etc");                 /* cwd is "/" at boot */
        persistent = save_shadow();
    }
    klog(had_file ? "auth: %s was unreadable, recreated the default admin account" : "auth: first boot, created default admin account (%s)",
         persistent ? "saved to /etc/shadow" : "NOT persistent: no disk");
}

/* ------------------------------------------------------------------ */
/* login                                                               */
/* ------------------------------------------------------------------ */

static void safe_name(const char* in, char* out) {                    /* for logging attacker-controlled text */
    int n = 0;
    for (; in[n] && n < 16; n++) out[n] = (in[n] >= 32 && in[n] < 127) ? in[n] : '?';
    out[n] = '\0';
}

auth_result_t auth_verify(const char* user, const char* password, uint32_t* wait_secs) {
    return auth_verify_ch(AUTH_CH_CONSOLE, user, password, wait_secs);
}

auth_result_t auth_verify_ch(int channel, const char* user, const char* password, uint32_t* wait_secs) {
    if (channel < 0 || channel > 1) channel = 1;
    uint32_t* fails = &fails_ch[channel];
    uint32_t* locked_until_p = &locked_until_ch[channel];
    const char* chname = channel == AUTH_CH_CONSOLE ? "console" : "remote";
    uint32_t now = *(volatile uint32_t*)&tick;
#define locked_until (*locked_until_p)
    if (locked_until && (int32_t)(now - locked_until) < 0) {
        if (wait_secs) *wait_secs = (locked_until - now) / 100 + 1;
        return AUTH_LOCKED;
    }

    user_t* u = NULL;
    for (int i = 0; i < MAX_USERS; i++) if (users[i].used && seq(users[i].name, user)) { u = &users[i]; break; }

    /* Do the same amount of work whether or not the user exists. */
    static const uint8_t dummy_salt[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    uint8_t calc[32];
    if (u) pbkdf2_hmac_sha256(password, slen(password), u->salt, 16, u->iters, calc);
    else   pbkdf2_hmac_sha256(password, slen(password), dummy_salt, 16, AUTH_ITERATIONS, calc);

    bool ok = u && ct_equal(calc, u->hash, 32);
    wipe(calc, sizeof(calc));

    char nm[17];
    safe_name(user, nm);
    if (ok) {
        *fails = 0;
        locked_until = 0;
        klog("auth: login ok for '%s' (%s)", nm, chname);
        return AUTH_OK;
    }

    (*fails)++;
    if (*fails >= 3) {
        uint32_t secs = 5u << (*fails - 3 > 6 ? 6 : *fails - 3);          /* 5, 10, 20, ... */
        if (secs > 300) secs = 300;
        locked_until = now + secs * 100;
        if (wait_secs) *wait_secs = secs;
        klog("auth: FAILED login #%u for '%s' (%s) - locked for %u s", *fails, nm, chname, secs);
    } else {
        klog("auth: FAILED login #%u for '%s' (%s)", *fails, nm, chname);
    }
    return AUTH_BAD;
#undef locked_until
}

bool auth_set_password(const char* user, const char* new_password, bool* persisted) {
    size_t len = slen(new_password);
    if (len < AUTH_MIN_PASSWORD || len > AUTH_MAX_PASSWORD) return false;
    for (int i = 0; i < MAX_USERS; i++) {
        if (users[i].used && seq(users[i].name, user)) {
            random_bytes(users[i].salt, 16);                           /* fresh salt on every change */
            users[i].iters = AUTH_ITERATIONS;
            make_hash(&users[i], new_password);
            users[i].must_change = false;
            bool saved = save_shadow();
            persistent = saved;
            if (persisted) *persisted = saved;
            klog("auth: password changed for '%s' (%s)", user, saved ? "saved" : "not saved: no disk");
            return true;
        }
    }
    return false;
}

bool auth_has_default_password(const char* user) {
    for (int i = 0; i < MAX_USERS; i++)
        if (users[i].used && seq(users[i].name, user)) return users[i].must_change;
    return false;
}

bool auth_selftest(void) {
    static const uint8_t abc[32] = { 0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
                                     0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad };
    static const uint8_t pb1[32] = { 0x12,0x0f,0xb6,0xcf,0xfc,0xf8,0xb3,0x2c,0x43,0xe7,0x22,0x52,0x56,0xc4,0xf8,0x37,
                                     0xa8,0x65,0x48,0xc9,0x2c,0xcc,0x35,0x48,0x08,0x05,0x98,0x7c,0xb7,0x0b,0xe1,0x7b };
    uint8_t out[32];
    sha256("abc", 3, out);
    if (!ct_equal(out, abc, 32)) return false;
    pbkdf2_hmac_sha256("password", 8, "salt", 4, 1, out);
    return ct_equal(out, pb1, 32);
}
