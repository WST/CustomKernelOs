#include "myos.h"
int main(int argc,char **argv) {
    if(argc!=2){dprintf(STDERR,"usage: umount DIRECTORY|DEVICE\n");return 2;}
    int r=umount(argv[1]);if(r<0){dprintf(STDERR,"umount: %s\n",strerror(-r));return 1;}return 0;
}
