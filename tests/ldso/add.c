int extra = 5;
int *pextra = &extra;

int add(int a, int b) {
    return a + b + *pextra;
}
