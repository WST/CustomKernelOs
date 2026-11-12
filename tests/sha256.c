/* SHA-256("abc") через libcrypto. Собирает gcc хоста и линкует со статическим
 * архивом: общую библиотеку на хосте не запустить, у неё интерпретатор /lib/ld.so.
 * Точка входа своя, потому что _start из cc в эту связку не попадает. */
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    unsigned char md[32];
    static const unsigned char expect[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    int i;
    SHA256((const unsigned char *)"abc", 3, md);
    for (i = 0; i < 32; i++) if (md[i] != expect[i]) return 1;
    printf("sha256: ok\n");
    return 0;
}
void _start(void) { exit(main()); }
