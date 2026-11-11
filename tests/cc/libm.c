/* libc, без которой Lua 5.4 в режиме C89 не собирается: setjmp, %.14g, время. */
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails;

static void expect(int cond, const char *msg) {
    if (!cond) { printf("fail %s\n", msg); fails++; }
}

static jmp_buf jb;

static void jump(void) { longjmp(jb, 0); }

int main(void) {
    volatile int n = 0;
    int r = setjmp(jb);
    if (r == 0) { n = 7; jump(); }
    expect(r == 1 && n == 7, "setjmp");

    char buf[64];
    snprintf(buf, sizeof buf, "%.14g", 0.1);
    expect(strcmp(buf, "0.1") == 0, buf);
    snprintf(buf, sizeof buf, "%.14g", 1024.0);
    expect(strcmp(buf, "1024") == 0, buf);

    char *end = NULL;
    expect(strtod("1.5", &end) == 1.5 && *end == 0, "strtod");
    expect(strtod("inf", &end) > 1e300, "inf");

    expect(pow(2, 10) == 1024, "pow10");
    expect(pow(2, -3) == 0.125, "pow-3");
    expect(floor(-1.5) == -2, "floor");
    expect(ceil(1.5) == 2, "ceil");
    expect(fmod(5.5, 2) == 1.5, "fmod");
    expect(sqrt(9) == 3, "sqrt");
    expect(sin(0) == 0, "sin0");
    expect(cos(0) == 1, "cos0");
    expect(fabs(log(1)) < 1e-12, "log1");

    double inf = 1.0 / 0.0;
    double nan = 0.0 / 0.0;
    expect(inf == inf && !(inf != inf), "inf eq");
    expect(nan != nan && !(nan == nan), "nan ne");
    expect(inf != 0.0 && nan != 0.0, "not zero");
    if (!inf || !nan) { printf("fail if(nan)\n"); fails++; }
    snprintf(buf, sizeof buf, "%12p", (void *)0x10);
    expect(strcmp(buf, "  0x00000010") == 0, buf);

    time_t t = 0;
    struct tm *tm = gmtime(&t);
    expect(tm && tm->tm_year == 70 && tm->tm_mon == 0 && tm->tm_mday == 1, "gmtime");
    expect(tm && tm->tm_wday == 4 && tm->tm_hour == 0, "wday");
    expect(tm && strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", tm) > 0, "strftime");
    expect(strcmp(buf, "1970-01-01 00:00:00") == 0, buf);
    expect(tm && mktime(tm) == 0, "mktime");

    if (!fails) printf("libm: ok\n");
    return fails != 0;
}
