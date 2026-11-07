#include "types.h"
int account_ctl(unsigned op,const char *name,const char *password);
int account_name(char *out);
int account_list(char *out, unsigned index);
bool account_private(u32 ino);
