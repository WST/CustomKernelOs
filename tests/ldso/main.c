extern int extra;
extern int add(int a, int b);
void *dlopen(const char *path, int flags);
void *dlsym(void *handle, const char *name);
char *dlerror(void);

static int write_str(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return __syscall(1, 1, (int)s, n);
}

static int main(void) {
    void *hb;
    int (*twice)(int);
    if (extra != 5) { write_str("extra bad\n"); return 1; }
    extra = 1;
    if (add(1, 2) != 4) { write_str("add bad\n"); return 2; }
    hb = dlopen("/lib/libb.so", 2);
    if (!hb) { write_str("dlopen bad\n"); return 3; }
    twice = dlsym(hb, "twice");
    if (!twice || twice(3) != 7) { write_str("dlsym bad\n"); return 4; }
    if (dlopen("/lib/no-such.so", 2)) { write_str("missing opened\n"); return 5; }
    if (!dlerror()) { write_str("dlerror empty\n"); return 6; }
    if (dlerror()) { write_str("dlerror stuck\n"); return 7; }
    if (dlsym(hb, "no_such")) { write_str("missing sym\n"); return 8; }
    if (!dlerror()) { write_str("sym error empty\n"); return 9; }
    write_str("add ok\n");
    return 0;
}

int __libc_start(int argc, char **argv) {
    (void)argc;
    (void)argv;
    __syscall(0, main());
    return 1;
}
