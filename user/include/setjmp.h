#ifndef SETJMP_H
#define SETJMP_H
/* ebx, esi, edi, ebp, esp, адрес возврата */
typedef int jmp_buf[6];
int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);
#endif
