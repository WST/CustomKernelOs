#include "myos.h"
int main(void){if(service_register("ext-install")<0)return 1;puts("EXT AUTO LOCK HELD");sleep_ms(5000);puts("EXT AUTO LOCK RELEASED");return 0;}
