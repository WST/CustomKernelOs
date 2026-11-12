#include "myos.h"
#include "unistd.h"
static int id(const char *s,unsigned *out) {
    if(!*s)return -1;unsigned n=0;int numeric=1;
    for(const char *p=s;*p;p++)if(*p<'0'||*p>'9')numeric=0;
    if(!numeric){int r=account(8,s,NULL);if(r<0)return -1;*out=r;return 0;}
    for(;*s;s++){unsigned v=*s-'0';if(n>(0xffffffffu-v)/10)return -1;n=n*10+v;}*out=n;return 0;
}
int main(int argc,char **argv) {
    if(argc<3){dprintf(STDERR,"Usage: chown OWNER[:GROUP] FILE...\n");return 1;}
    char spec[64];if(strlen(argv[1])>=sizeof(spec))return 1;strcpy(spec,argv[1]);
    char *group=strchr(spec,':');if(group)*group++=0;unsigned uid,gid=(unsigned)-1;
    if(id(spec,&uid) || (group && id(group,&gid))){dprintf(STDERR,"chown: invalid account or ID\n");return 1;}
    int status=0;for(int i=2;i<argc;i++)if(chown(argv[i],uid,gid)<0){dprintf(STDERR,"chown: %s: %s\n",argv[i],strerror(errno));status=1;}
    return status;
}
