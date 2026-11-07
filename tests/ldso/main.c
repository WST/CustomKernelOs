extern int extra;
extern int add(int a, int b);
void *dlopen(const char *path, int flags);
void *dlsym(void *handle, const char *name);

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
    write_str("add ok\n");
    return 0;
}

int __libc_start(int argc, char **argv) {
    (void)argc;
    (void)argv;
    __syscall(0, main());
    return 1;
}
