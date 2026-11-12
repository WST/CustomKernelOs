/* ls [path...] — содержимое каталогов; файлы показываются как есть */
#include "myos.h"
#include "sys/stat.h"

static bool long_format;
static void entry(const struct dirent_info *e,const char *shown,const char *path) {
    if(long_format){struct stat st;if(lstat(path,&st)<0){dprintf(STDERR,"ls: %s: %s\n",path,strerror(errno));return;}
        char bits[11];bits[0]=S_ISDIR(st.st_mode)?'d':S_ISLNK(st.st_mode)?'l':S_ISCHR(st.st_mode)?'c':S_ISBLK(st.st_mode)?'b':'-';
        for(int i=0;i<9;i++)bits[i+1]=(st.st_mode&(1u<<(8-i)))?"rwx"[i%3]:'-';
        if(st.st_mode&S_ISVTX)bits[9]=bits[9]=='x'?'t':'T';
        bits[10]=0;
        printf("%s %u %u %8u %s\n",bits,st.st_uid,st.st_gid,st.st_size,shown);return;
    }
    if (e->type == 2) printf("   <DIR>  " ANSI_BLUE ANSI_BOLD "%s/" ANSI_RESET "\n", shown);
    else if(e->type==3) printf(" <BLOCK>  %s\n",shown);
    else if(e->type==4) printf("  <CHAR>  %s\n",shown);
    else if(e->type==5) printf("   <LNK>  %s\n", shown);
    else              printf("%8u  %s\n", e->size, shown);
}

static int list_dir(const char *path, bool title) {
    struct dirent_info e;
    if (title) printf("%s:\n", path);
    int status;
    for(int i=0;(status=dirread(path,i,&e))==1;i++){
        char full[1024];snprintf(full,sizeof(full),"%s/%s",path,e.name);entry(&e,e.name,full);
    }
    if(status<0){dprintf(STDERR,"ls: %s: %s\n",path,strerror(-status));return 1;}
    return 0;
}

int main(int argc, char **argv) {
    int first=1;if(argc>1 && !strcmp(argv[1],"-l")){long_format=true;first++;}
    if(argc<=first)return list_dir(".",false);
    int rc = 0, dirs = 0;
    struct dirent_info e;
    /* сначала файлы, потом каталоги — как в настоящем ls */
    for (int i = first; i < argc; i++) {
        int r = dirstat(argv[i], &e);
        if (r < 0) { dprintf(STDERR, "ls: %s: %s\n", argv[i], strerror(-r)); rc = 1; continue; }
        if (e.type == 2) dirs++;
        else entry(&e,argv[i],argv[i]);
    }
    for (int i = first; i < argc; i++)
        if (dirstat(argv[i], &e) == 0 && e.type == 2) rc|=list_dir(argv[i],argc>first+1);
    (void)dirs;
    return rc;
}
