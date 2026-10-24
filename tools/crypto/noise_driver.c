#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "noise.h"
#include "x25519.h"
static int unhex(const char* s, uint8_t* o) { int n = (int)strlen(s) / 2; for (int i = 0; i < n; i++) { unsigned v; sscanf(s + 2*i, "%2x", &v); o[i] = (uint8_t)v; } return n; }
static void hex(const char* tag, const uint8_t* b, int n) { printf("%s ", tag); for (int i = 0; i < n; i++) printf("%02x", b[i]); printf("\n"); fflush(stdout); }
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    uint8_t priv[32], pub[32], pro[64]; int pl = unhex(argv[2], pro);
    unhex(argv[1], priv); x25519_base(pub, priv);
    noise_hs hs; noise_resp_init(&hs, priv, pub, pro, pl);
    static char line[20000]; static uint8_t buf[10000], out[10000];
    while (fgets(line, sizeof line, stdin)) {
        char* sp = strchr(line, ' '); if (!sp) continue; *sp++ = 0; sp[strcspn(sp, "\r\n")] = 0;
        int n = unhex(sp, buf);
        if (!strcmp(line, "m1")) {
            if (!noise_resp_read_msg1(&hs, buf, n)) { printf("err\n"); fflush(stdout); continue; }
            uint8_t e[32]; FILE* f = fopen("/dev/urandom", "rb"); if (!f || fread(e, 1, 32, f) != 32) return 3; fclose(f);
            uint8_t m2[48]; noise_resp_write_msg2(&hs, e, m2); hex("m2", m2, 48);
        } else if (!strcmp(line, "dec")) { if (noise_decrypt(&hs.recv, buf, n, out)) hex("pt", out, n - 16); else { printf("err\n"); fflush(stdout); } }
        else if (!strcmp(line, "enc")) { noise_encrypt(&hs.send, buf, n, out); hex("ct", out, n + 16); }
    }
    return 0;
}
