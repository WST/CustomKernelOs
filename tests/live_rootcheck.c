#include "myos.h"
#define CHECK(x) do { if (!(x)) { printf("LIVE ROOT FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static int marker(int fd,int offset) {
    char data[16];
    return lseek(fd,offset,SEEK_SET)==offset && read(fd,data,sizeof(data))==sizeof(data) &&
           !memcmp(data,"MYOS-LIVE-GUARD!",16);
}
int main(int argc,char **argv) {
    CHECK(argc==2);
    int offset=atoi(argv[1]),fd=open("/dev/sdb",O_READ);
    CHECK(fd>=0 && marker(fd,offset));
    /* Allocations beyond the old 64 MiB reservation must not overwrite the root. */
    void *chunks[12];
    for(int i=0;i<12;i++) {
        chunks[i]=malloc(8*1024*1024);CHECK(chunks[i]);
        memset(chunks[i],0x5a+i,8*1024*1024);
        CHECK(marker(fd,offset));
    }
    for(int i=0;i<12;i++) free(chunks[i]);
    CHECK(close(fd)==0);
    int f=open("/tmp/live-write",O_WRITE);CHECK(f>=0);
    CHECK(write(f,"live-ok",7)==7 && close(f)==0);
    char data[8]={0};f=open("/tmp/live-write",O_READ);CHECK(f>=0);
    CHECK(read(f,data,7)==7 && !strcmp(data,"live-ok") && close(f)==0);
    CHECK(unlink("/tmp/live-write")==0);
    puts("LIVE ROOT PASS: root survives 96 MiB allocation pressure and filesystem writes");
    return 0;
}
