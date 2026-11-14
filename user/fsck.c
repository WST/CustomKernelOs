#include "myos.h"
int main(int argc,char **argv) {
    if(argc!=2){dprintf(STDERR,"usage: fsck DEVICE\n");return 2;}
    int r=fsck(argv[1]);if(r<0){dprintf(STDERR,"fsck: %s\n",strerror(-r));return 1;}return 0;
}
