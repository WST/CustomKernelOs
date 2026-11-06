#include "myos.h"
int main(int argc,char **argv) {
    if(argc!=3 || strcmp(argv[2],"--yes")) {
        dprintf(STDERR,"usage: mkfs DEVICE --yes\nCreates MyFS; erases existing filesystem on DEVICE. Mounted devices are refused.\n"); return 2;
    }
    int r=mkfs(argv[1]);
    if(r<0) { dprintf(STDERR,"mkfs: %s: %s\n",argv[1],strerror(-r)); return 1; }
    printf("MyFS created on %s\n",argv[1]); return 0;
}
