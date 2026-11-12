#include <fenv.h>
int feclearexcept(int excepts) { (void)excepts; return 0; }
int fetestexcept(int excepts) { (void)excepts; return 0; }
int feraiseexcept(int excepts) { (void)excepts; return 0; }
int fegetround(void) { return FE_TONEAREST; }
