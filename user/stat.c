#include "myos.h"
#include "sys/stat.h"
int main(int argc,char **argv) {
    if(argc<2){dprintf(STDERR,"Usage: stat FILE...\n");return 1;}int status=0;
    for(int i=1;i<argc;i++){struct stat s;if(lstat(argv[i],&s)<0){dprintf(STDERR,"stat: %s: %s\n",argv[i],strerror(errno));status=1;continue;}
        printf("%s: mode=%04o uid=%u gid=%u size=%u inode=%u\n",argv[i],s.st_mode&07777,s.st_uid,s.st_gid,s.st_size,s.st_ino);}
    return status;
}
