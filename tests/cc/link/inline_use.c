extern int ext_inline_add(int a, int b);

int main(void) { return ext_inline_add(20, 22) == 42 ? 0 : 1; }
