#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sha256.h"
static void hex(const uint8_t* b, int n) { for (int i = 0; i < n; i++) printf("%02x", b[i]); printf("\n"); }
int main(int argc, char** argv) {
    uint8_t out[32];
    if (argc >= 3 && !strcmp(argv[1], "sha"))  { sha256(argv[2], strlen(argv[2]), out); hex(out, 32); }
    if (argc >= 4 && !strcmp(argv[1], "hmac")) { hmac_sha256(argv[2], strlen(argv[2]), argv[3], strlen(argv[3]), out); hex(out, 32); }
    if (argc >= 5 && !strcmp(argv[1], "pbkdf2")) { pbkdf2_hmac_sha256(argv[2], strlen(argv[2]), argv[3], strlen(argv[3]), (uint32_t)atoi(argv[4]), out); hex(out, 32); }
    return 0;
}
