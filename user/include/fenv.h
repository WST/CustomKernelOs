#ifndef FENV_H
#define FENV_H
/* Округление программ всегда к ближайшему. Флаги исключений не копятся. */
#define FE_INEXACT    1
#define FE_DIVBYZERO  4
#define FE_UNDERFLOW  8
#define FE_OVERFLOW   16
#define FE_INVALID    32
#define FE_ALL_EXCEPT 61
#define FE_TONEAREST  0
#define FE_DOWNWARD   0x400
#define FE_UPWARD     0x800
#define FE_TOWARDZERO 0xc00
int feclearexcept(int excepts);
int fetestexcept(int excepts);
int feraiseexcept(int excepts);
int fegetround(void);
#endif
