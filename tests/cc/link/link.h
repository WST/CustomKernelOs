/* Общие объявления для main.c, lib1.c и lib2.c (раздельная компиляция, tests/cclink.sh) */
struct big { int a[10]; char tag; };

/* lib1.c */
extern int initialized;
extern int common_counter;
extern int table[5];
extern const char *greeting;
extern int (*ops[2])(int, int);
int  lib1_helper(void);
int  lib1_get_initialized(void);
void lib1_bump(void);

/* lib2.c — его собирают и cc, и gcc */
struct big lib2_make(int base);
int  lib2_sum(struct big b);
int  lib2_call_back_make(void);
signed char    lib2_schar(void);
unsigned char  lib2_uchar(void);
short          lib2_short(void);
unsigned short lib2_ushort(void);
_Bool          lib2_bool(void);
int  lib2_stress(int (*f)(int));
long long lib2_div64(long long a, long long b);

/* main.c */
struct big main_make(int base);
