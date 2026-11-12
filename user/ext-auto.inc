/* Boot package selection is data, never a shell script. Install recipes retain
 * their ordinary dependency, fetch-retry and successful-install stamp logic. */
#define AUTO_CONFIG "/etc/ext/autoinstall.conf"
#define AUTO_STATUS "/var/lib/ext/autoinstall.status"
#define AUTO_LOG "/var/log/ext-autoinstall.log"
#define AUTO_UNIT "ext-autoinstall.service"
struct auto_packages {int count;char names[MAX_PKG][32];};
static int auto_index(const struct auto_packages *p,const char *name) {
    for(int i=0;i<p->count;i++)if(!strcmp(p->names[i],name))return i;
    return -1;
}
static FILE *auto_open_read(const char *path) {
    FILE *f=fopen(path,"r");
    if(!f && errno==ENOENT){char backup[160];if(snprintf(backup,sizeof(backup),"%s.previous",path)<(int)sizeof(backup))f=fopen(backup,"r");}
    return f;
}
static int auto_read(const char *path,struct auto_packages *p) {
    FILE *f=auto_open_read(path);p->count=0;
    if(!f)return errno==ENOENT?0:1;
    char line[256];int r,number=0;
    while((r=read_line(f,line,sizeof(line)))!=0) {
        char *args[MAX_ARG+1];number++;
        int n=r<0?-1:split_line(line,args);
        if(!n)continue;
        if(n!=1 || !valid_pkg(args[0])){dprintf(STDERR,"ext auto: %s:%d: expected one package name\n",path,number);fclose(f);return 1;}
        if(auto_index(p,args[0])>=0)continue;
        if(p->count==MAX_PKG){dprintf(STDERR,"ext auto: too many packages (maximum %d)\n",MAX_PKG);fclose(f);return 1;}
        strcpy(p->names[p->count++],args[0]);
    }
    r=ferror(f);fclose(f);return r?1:0;
}
#ifndef __LINUX_TEST__
#include "sys/stat.h"
/* Every mutating ext CLI operation holds this registration until exit.
 * Kernel registration is atomic and dead process entries are reclaimed. */
static int ext_lock(void) {
    if(account(4,NULL,NULL)!=0){dprintf(STDERR,"ext: authenticated root required\n");return 1;}
    int r=service_register("ext-install");
    if(r<0){dprintf(STDERR,"ext: another package operation is running, or lock unavailable (%s)\n",strerror(-r));return 1;}
    return 0;
}
static int auto_trusted(const char *path,bool directory,bool missing_ok) {
    struct stat s;
    if(lstat(path,&s)<0)return missing_ok && errno==ENOENT?0:1;
    if(s.st_uid || (s.st_mode&0022) || (directory?!S_ISDIR(s.st_mode):!S_ISREG(s.st_mode))) {
        dprintf(STDERR,"ext auto: %s must be root-owned, not writable by group/others, and not a symlink\n",path);return 1;
    }
    return 0;
}
static int auto_prepare(void) {
    const char *dirs[]={"/","/etc","/etc/ext","/var","/var/lib","/var/lib/ext","/var/log"};
    for(unsigned i=0;i<sizeof(dirs)/sizeof(*dirs);i++) {
        int r=!strcmp(dirs[i],"/")?0:mkdir(dirs[i],0755);
        if(r<0 && errno!=EEXIST){dprintf(STDERR,"ext auto: cannot create %s: %s\n",dirs[i],strerror(errno));return 1;}
        if(auto_trusted(dirs[i],true,false))return 1;
    }
    return auto_trusted(AUTO_CONFIG,false,true) || auto_trusted(AUTO_CONFIG ".previous",false,true) ||
        auto_trusted(AUTO_STATUS,false,true) || auto_trusted(AUTO_STATUS ".previous",false,true) || auto_trusted(AUTO_LOG,false,true);
}
/* MyFS rename does not overwrite an existing name. Preserve the previous
 * complete file until the new name is published. Readers also try .previous,
 * allowing recovery if a process stops between the two renames. */
static int auto_replace(const char *tmp,const char *path) {
    char backup[160];struct stat st;
    if(snprintf(backup,sizeof(backup),"%s.previous",path)>=(int)sizeof(backup) || auto_trusted(backup,false,true))return 1;
    bool old=lstat(path,&st)==0;
    if(!old && errno!=ENOENT)return 1;
    if(old) {
        int r=unlink(backup);if(r<0 && r!=-ENOENT)return 1;
        if(rename(path,backup)<0)return 1;
    }
    if(rename(tmp,path)<0){if(old)rename(backup,path);return 1;}
    unlink(backup);return 0;
}
static FILE *auto_open_output(const char *path,bool append) {
    /* Create private files even if the caller has an unrestricted umask. */
    int fd=open(path,O_WRONLY|O_CREAT|(append?O_APPEND:O_TRUNC),0600);
    if(fd<0)return NULL;
    FILE *f=fdopen(fd,append?"a":"w");if(!f)close(fd);return f;
}
static int auto_save(const struct auto_packages *p) {
    const char *tmp=AUTO_CONFIG ".tmp";
    if(auto_trusted(tmp,false,true))return 1;
    FILE *f=auto_open_output(tmp,false);if(!f)return 1;
    fputs("# Packages installed by ext-autoinstall.service at boot. One name per line.\n",f);
    for(int i=0;i<p->count;i++)fprintf(f,"%s\n",p->names[i]);
    int r=ferror(f);if(fclose(f))r=1;
    if(!r && chmod(tmp,0644)<0)r=1;
    if(!r)r=auto_replace(tmp,AUTO_CONFIG);
    if(r)unlink(tmp);
    return r;
}
static int auto_show_status(void) {
    struct auto_packages p;if(auto_read(AUTO_CONFIG,&p))return 1;
    printf("Selected packages: %d\n",p.count);
    for(int i=0;i<p.count;i++)printf("  %s: %s\n",p.names[i],stamp_exists(INST_DIR,p.names[i])?"installed":"pending");
    int pid=service_find("ext-install");if(pid)printf("Package operation active: pid=%d\n",pid);
    FILE *f=auto_open_read(AUTO_STATUS);
    if(!f){puts("No automatic installation results yet.");return 0;}
    puts("Last automatic run:");int c;while((c=fgetc(f))!=EOF)putchar(c);
    int r=ferror(f);fclose(f);return r?1:0;
}
static void auto_wait_network(FILE *log) {
    /* No adapter: proceed immediately so offline recipes still work. */
    struct net_info info;unsigned start=ticks();
    while(netinfo(&info)==0 && info.up && !info.ip && ticks()-start<30000)sleep_ms(250);
    if(netinfo(&info)<0 || !info.up || !info.ip)fputs("Network not ready; local recipes will still run. Fetch has bounded retries.\n",log);
}
static int auto_write_status(const struct auto_packages *p,const char **states,bool complete) {
    const char *tmp=AUTO_STATUS ".tmp";
    if(auto_trusted(tmp,false,true))return 1;
    FILE *f=auto_open_output(tmp,false);if(!f)return 1;
    fprintf(f,"run %s\n",complete?"complete":"incomplete");
    for(int i=0;i<p->count;i++)fprintf(f,"%s %s\n",p->names[i],states[i]);
    int r=ferror(f);if(fclose(f))r=1;
    if(!r && chmod(tmp,0644)<0)r=1;
    if(!r)r=auto_replace(tmp,AUTO_STATUS);
    if(r)unlink(tmp);
    return r;
}
static int auto_run(bool boot) {
    struct auto_packages *p=malloc(sizeof(*p));if(!p)return 1;
    int r=auto_read(AUTO_CONFIG,p);if(r){free(p);return 1;}
    const char *states[MAX_PKG];for(int i=0;i<p->count;i++)states[i]="pending";
    FILE *log=auto_open_output(AUTO_LOG,true);if(!log){free(p);return 1;}
    fprintf(log,"\nAutomatic package run pid=%d tick=%u selected=%d\n",getpid(),ticks(),p->count);
    r=auto_write_status(p,states,false);
    bool pending=false;
    for(int i=0;i<p->count;i++)if(!stamp_exists(INST_DIR,p->names[i]))pending=true;
    if(!r && pending && boot)auto_wait_network(log);
    int failed=0;
    for(int i=0;!r && i<p->count;i++) {
        const char *name=p->names[i];
        if(stamp_exists(INST_DIR,name)) {
            states[i]="skipped";fprintf(log,"%s skipped (already installed)\n",name);
        } else {
            states[i]="running";r=auto_write_status(p,states,false);if(r)break;
            fprintf(log,"%s installing\n",name);fflush(log);fflush(stdout);fflush(stderr);
            int pid=fork(),status=1;
            if(pid==0) {
                if(dup2(fileno(log),STDOUT)<0 || dup2(fileno(log),STDERR)<0 || chdir("/")<0)exit(1);
                exit(install_name(INST_DIR,name));
            }
            if(pid>0 && waitpid(pid,&status,0)<0)status=1;
            states[i]=status?"failed":"installed";if(status)failed++;
            fprintf(log,"%s %s exit=%d\n",name,states[i],status);
        }
        printf("ext auto: %s %s\n",name,states[i]);
        r=auto_write_status(p,states,false);
    }
    if(!r)r=auto_write_status(p,states,true);
    fprintf(log,"Automatic package run finished: failures=%d status_error=%d\n",failed,r);
    if(ferror(log))r=1;
    if(fclose(log))r=1;
    free(p);return r || failed?1:0;
}
static int auto_command(int argc,char **argv) {
    if(argc==3 && (!strcmp(argv[2],"list") || !strcmp(argv[2],"status")))return auto_show_status();
    bool run=argc>=3 && !strcmp(argv[2],"run");
    bool add=argc>=4 && !strcmp(argv[2],"add"),remove=argc>=4 && !strcmp(argv[2],"remove");
    if((!run && !add && !remove) || (run && (argc>4 || (argc==4 && strcmp(argv[3],"--boot"))))) {
        dprintf(STDERR,"usage: ext auto add|remove NAME... | ext auto list|status|run\n");return 2;
    }
    if(ext_lock() || auto_prepare())return 1;
    if(run)return auto_run(argc==4);
    struct auto_packages p;if(auto_read(AUTO_CONFIG,&p))return 1;
    for(int i=3;i<argc;i++) {
        if(!valid_pkg(argv[i])){dprintf(STDERR,"ext auto: invalid package name: %s\n",argv[i]);return 2;}
        int at=auto_index(&p,argv[i]);
        if(add) {
            char script[160];FILE *f;
            if(script_path(INST_DIR,argv[i],"setup.inst",script,sizeof(script))<0 || !(f=fopen(script,"r"))) {
                dprintf(STDERR,"ext auto: unknown package: %s\n",argv[i]);return 1;
            }
            fclose(f);
            if(at<0){if(p.count==MAX_PKG)return 1;strcpy(p.names[p.count++],argv[i]);}
        } else if(at>=0){if(at<p.count-1)memmove(p.names[at],p.names[at+1],(p.count-at-1)*sizeof(p.names[0]));p.count--;}
    }
    if(auto_save(&p)){dprintf(STDERR,"ext auto: cannot save configuration\n");return 1;}
    struct unit_info unit;
    int r=unitctl(UNIT_RELOAD,NULL,NULL,0);
    if(r>=0)r=unitctl(add?UNIT_ENABLE:!p.count?UNIT_DISABLE:UNIT_STATUS,AUTO_UNIT,&unit,0);
    if(r<0){dprintf(STDERR,"ext auto: list saved, but service configuration failed: %s\n",strerror(-r));return 1;}
    printf("ext auto: saved %d packages; boot service %s\n",p.count,unit.enabled?"enabled":"disabled");return 0;
}
#else
/* ext --selftest is built by the native MyOS compiler's Linux test backend. */
static int ext_lock(void){return 0;}
static int auto_command(int argc,char **argv){(void)argc;(void)argv;return 2;}
#endif
