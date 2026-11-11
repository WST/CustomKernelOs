#include "myos.h"
#include "sys/stat.h"
int main(int argc,char **argv) {
    if(argc<3){dprintf(STDERR,"Usage: chmod OCTAL FILE...\n");return 1;}
    unsigned mode=0;for(char *p=argv[1];*p;p++){if(*p<'0'||*p>'7'||mode>01777/8){dprintf(STDERR,"chmod: invalid mode\n");return 1;}mode=mode*8+*p-'0';}
    if(!argv[1][0] || mode>01777){dprintf(STDERR,"chmod: invalid mode\n");return 1;}
    int status=0;for(int i=2;i<argc;i++)if(chmod(argv[i],mode)<0){dprintf(STDERR,"chmod: %s: %s\n",argv[i],strerror(errno));status=1;}
    return status;
}
