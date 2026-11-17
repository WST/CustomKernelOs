#include "myos.h"
#include "unistd.h"
#include "sys/stat.h"
#include "utime.h"
#define CHECK(c) do {if(!(c)){dprintf(STDERR,"INSTALL CHECK FAIL line %d: %s errno=%d\n",__LINE__,#c,errno);return 1;}}while(0)
int main(int argc,char **argv) {
    (void)argv;
    struct stat st;
    if(argc>1) {
        CHECK(!symlink("/root/binary","/root/link"));
        CHECK(!lchown("/root/link",123,456));
        CHECK(!chmod("/root/binary",0640));
        CHECK(!chown("/root/binary",42,43));
        struct utimbuf when={1700000000,1700000000};CHECK(!utime("/root/binary",&when));
        puts("INSTALL FIXTURE READY");return 0;
    }
    CHECK(!lstat("/root/link",&st) && S_ISLNK(st.st_mode) && st.st_uid==123 && st.st_gid==456);
    CHECK(!stat("/root/binary",&st) && st.st_uid==42 && st.st_gid==43 && (st.st_mode&0777)==0640 && st.st_mtime==1700000000);
    char link[64];int n=readlink("/root/link",link,sizeof(link)-1);CHECK(n>0);link[n]=0;CHECK(!strcmp(link,"/root/binary"));
    puts("INSTALL METADATA PASS");return 0;
}
