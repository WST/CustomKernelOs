#include "myos.h"
struct mod_info { char name[16]; unsigned index, refs; };
int main(void) {
    int n = modctl(5, 0, 0);
    if (n < 0) { dprintf(2, "kldstat: %d\n", n); return 1; }
    printf("id  refs  name\n");
    for (int i = 0; i < n; i++) {
        struct mod_info info;
        if (modctl(3, i, &info) < 0) break;
        printf("%-3u %-5u %s\n", info.index, info.refs, info.name);
    }
    return 0;
}
