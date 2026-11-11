unsigned gfx_mix(unsigned dst, unsigned src, int a);
int write(int fd, const void *buf, unsigned n);

int main(void) {
    if (gfx_mix(1, 2, 0) != 1) return 1;
    write(1, "gfx ok\n", 7);
    return 0;
}
