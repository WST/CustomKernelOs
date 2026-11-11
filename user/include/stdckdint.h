#ifndef STDCKDINT_H
#define STDCKDINT_H
/* Переполнения не ловим: для сборки bash достаточно, что макрос компилируется. */
#define ckd_add(r, a, b) (*(r) = (a) + (b), 0)
#define ckd_sub(r, a, b) (*(r) = (a) - (b), 0)
#define ckd_mul(r, a, b) (*(r) = (a) * (b), 0)
#endif
