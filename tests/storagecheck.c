/* AHCI write/close/read plus persisted-file verification after a fresh boot. */
#include "myos.h"
#include "stdio.h"
#define SECTORS 192
static unsigned char data[512],actual[512];
static void pattern(int sector){for(int i=0;i<512;i++)data[i]=(sector*17+i*13)&255;}
int main(int argc,char **argv){
    (void)argv;const char *path="/root/storage-check.bin";
    if(argc==1){
        int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);
        if(fd<0){perror("storage create");return 1;}
        for(int sector=0;sector<SECTORS;sector++){
            pattern(sector);if(write(fd,data,512)!=512){perror("storage write");close(fd);return 2;}
        }
        if(close(fd)<0){perror("storage close");return 3;}
    }
    for(int pass=0;pass<2;pass++){
        int fd=open(path,O_RDONLY);if(fd<0){perror("storage open");return 4;}
        for(int sector=0;sector<SECTORS;sector++){
            pattern(sector);if(read(fd,actual,512)!=512 || memcmp(data,actual,512)){printf("STORAGE mismatch sector %d\n",sector);close(fd);return 5;}
        }
        if(close(fd)<0){perror("storage read close");return 6;}
    }
    puts("STORAGE DATA PASS");return 0;
}
