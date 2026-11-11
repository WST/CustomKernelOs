extern int extra;
extern int gadd(int, int);

static int write_str(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return __syscall(1, 1, (int)s, n);
}

static int main(void) {
    if (extra != 3) { write_str("gccso bad\n"); return 1; }
    extra = 1;
    if (gadd(1, 2) != 4) { write_str("gccso bad\n"); return 2; }
    write_str("gccso ok\n");
    return 0;
}

int __libc_start(int argc, char **argv) {
    (void)argc;
    (void)argv;
    __syscall(0, main());
    return 1;
}
