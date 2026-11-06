#ifndef UNISTD_H
#define UNISTD_H
#include "sys/types.h"
#include "myos.h"
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
extern char *optarg;
extern int optind, opterr, optopt;
int getopt(int argc, char *const argv[], const char *optstring);
#endif
