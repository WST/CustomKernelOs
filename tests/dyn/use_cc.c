extern int extra;
extern int add(int, int);

int main(void) {
    if (extra != 5) return 1;
    extra = 1;
    if (add(1, 2) != 4) return 2;
    return 0;
}
