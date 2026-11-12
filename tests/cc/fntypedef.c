/* typedef функции и список прототипов: static handler a, b; */
typedef int handler(int);

static handler a, b;

static int a(int x) { return x + 1; }
static int b(int x) { return x + 2; }

int main(void) {
    if (a(3) != 4) return 1;
    if (b(3) != 5) return 2;
    return 0;
}
