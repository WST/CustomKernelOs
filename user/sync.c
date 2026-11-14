#include "myos.h"
int main(void) {int r=sync();if(r<0){dprintf(STDERR,"sync: %s\n",strerror(-r));return 1;}return 0;}
