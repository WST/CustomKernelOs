/* Соглашение о вызовах: этот файл собирают и cc, и gcc -O2, а main.c — всегда cc.
 * Структуры по значению и в ответе, char/short в ответе, сохранность ebx/esi/edi. */
#include "link.h"

struct big lib2_make(int base) {
    struct big b;
    for (int i = 0; i < 10; i++) b.a[i] = base + i;
    b.tag = 'x';
    return b;
}

int lib2_sum(struct big b) {
    int s = 0;
    for (int i = 0; i < 10; i++) s += b.a[i];
    return s + b.tag;
}

int lib2_call_back_make(void) {
    struct big b = main_make(3);
    return b.a[9] + b.tag;
}

#ifdef __MYOS_CC__
signed char lib2_schar(void) { return -128; }
unsigned char lib2_uchar(void) { return 200; }
short lib2_short(void) { return -2; }
unsigned short lib2_ushort(void) { return 65535; }
_Bool lib2_bool(void) { return 1; }
#else
/* gcc: старшие биты eax — мусор; по ABI значение только в al / ax */
__asm__(".text\n"
        ".globl lib2_schar\nlib2_schar:\n  movl $0x12345680, %eax\n  ret\n"
        ".globl lib2_uchar\nlib2_uchar:\n  movl $0xABCDEFC8, %eax\n  ret\n"
        ".globl lib2_short\nlib2_short:\n  movl $0x7777FFFE, %eax\n  ret\n"
        ".globl lib2_ushort\nlib2_ushort:\n movl $0x8000FFFF, %eax\n  ret\n"
        ".globl lib2_bool\nlib2_bool:\n    movl $0xFFFFFF01, %eax\n  ret\n");
#endif

/* у gcc -O2 a..e живут в ebx, esi, edi, ebp — f обязана их сберечь */
int lib2_stress(int (*f)(int)) {
    int a = 1, b = 2, c = 3, d = 4, e = 5;
    for (int i = 0; i < 10; i++) {
        int r = f(i);
        a += r;
        b ^= r * 3;
        c += a;
        d += b;
        e += c ^ d;
    }
    return a + b + c + d + e;
}

long long lib2_div64(long long a, long long b) { return a / b; }
