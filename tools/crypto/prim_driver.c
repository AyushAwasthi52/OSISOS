#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "x25519.h"
#include "chacha20poly1305.h"
static int unhex(const char* s, uint8_t* o) { int n = (int)strlen(s) / 2; for (int i = 0; i < n; i++) { unsigned v; sscanf(s + 2*i, "%2x", &v); o[i] = (uint8_t)v; } return n; }
static void hex(const uint8_t* b, int n) { for (int i = 0; i < n; i++) printf("%02x", b[i]); printf("\n"); }
int main(int argc, char** argv) {
    static uint8_t a[4096], b[4096], c[4096], d[4096], out[8192];
    if (!strcmp(argv[1], "x25519")) { unhex(argv[2], a); unhex(argv[3], b); x25519(out, a, b); hex(out, 32); }
    else if (!strcmp(argv[1], "base")) { unhex(argv[2], a); x25519_base(out, a); hex(out, 32); }
    else if (!strcmp(argv[1], "enc")) { unhex(argv[2], a); uint64_t n = strtoull(argv[3], 0, 10); int al = argc > 4 ? unhex(argv[4], b) : 0; int pl = argc > 5 ? unhex(argv[5], c) : 0; aead_encrypt(a, n, b, al, c, pl, out); hex(out, pl + 16); }
    else if (!strcmp(argv[1], "dec")) { unhex(argv[2], a); uint64_t n = strtoull(argv[3], 0, 10); int al = unhex(argv[4], b); int cl = unhex(argv[5], c); if (aead_decrypt(a, n, b, al, c, cl - 16, d)) hex(d, cl - 16); else printf("FAIL\n"); }
    return 0;
}
