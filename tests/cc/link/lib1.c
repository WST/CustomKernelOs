/* Данные и функции для main.c: static-имена, совпадающие с main.c, COMMON, указатели в данных */
#include "link.h"

static int helper(void) { return 10; }
int lib1_helper(void) { return helper(); }

int initialized = 42;
int lib1_get_initialized(void) { return initialized; }

int common_counter;
void lib1_bump(void) { common_counter++; }

int table[5] = { 10, 20, 30, 40, 50 };
const char *greeting = "hello from lib1";

static int add(int a, int b) { return a + b; }
static int mul(int a, int b) { return a * b; }
int (*ops[2])(int, int) = { add, mul };
