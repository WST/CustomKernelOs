#ifndef UTIL_H
#define UTIL_H
#define FMT_SCALED_STRSIZE 7
int pledge(const char *promises, const char *execpromises);
int fmt_scaled(long long number, char *result);
#endif
