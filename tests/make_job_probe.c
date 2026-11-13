/* Test-only cc trampoline: exec retains the CPU assignment recorded here. */
#include "myos.h"
#include "stdio.h"
#include "stdlib.h"
static void path(char *out,const char *name) {
    const char *base=strrchr(name,'/'); if(base) name=base+1;
    snprintf(out,128,"/tmp/build-%s.probe",name);
}
int main(int argc,char **argv) {
    char file[128];
    if(argc==3 && !strcmp(argv[1],"end")) {
        path(file,argv[2]);FILE *f=fopen(file,"a");if(!f)return 1;
        fprintf(f,"%u\n",ticks());fclose(f);return 0;
    }
    if(argc==3 && !strcmp(argv[1],"check")) {
        unsigned begin[4],end[4],ids[4],peak=0,unique=0;
        for(unsigned i=0;i<4;i++) {
            char name[8];snprintf(name,sizeof(name),"%c.o",'a'+i);path(file,name);
            FILE *f=fopen(file,"r");if(!f)return 2;
            char text[96];int n=fread(text,1,sizeof(text)-1,f);text[n]=0;fclose(f);
            char *p=text;begin[i]=strtoul(p,&p,10);ids[i]=strtoul(p,&p,10);end[i]=strtoul(p,&p,10);
            if(end[i]<=begin[i]) { printf("JOB PROBE FAIL empty interval\n");return 3; }
            unsigned j;for(j=0;j<i;j++) if(ids[j]==ids[i]) break;
            if(j==i)unique++;
        }
        for(unsigned i=0;i<4;i++) {
            unsigned active=0;for(unsigned j=0;j<4;j++) if(begin[j]<=begin[i] && end[j]>begin[i])active++;
            if(active>peak)peak=active;
        }
        unsigned expected=atoi(argv[2]);
        if(peak!=expected || (expected>1 && unique<2)) {
            printf("JOB PROBE FAIL peak=%u expected=%u CPUs=%u\n",peak,expected,unique);return 4;
        }
        printf("PARALLEL BUILD PASS jobs=%u CPUs=%u\n",peak,unique);return 0;
    }
    const char *output=0;
    for(int i=1;i+1<argc;i++)if(!strcmp(argv[i],"-o"))output=argv[i+1];
    if(!output)return 5;
    unsigned a=1,b,c,d;__asm__ volatile("cpuid":"+a"(a),"=b"(b),"=c"(c),"=d"(d));
    path(file,output);FILE *f=fopen(file,"w");if(!f)return 6;
    fprintf(f,"%u %u\n",ticks(),b>>24);fclose(f);
    argv[0]="cc";exec("/bin/cc",argv);return 127;
}
