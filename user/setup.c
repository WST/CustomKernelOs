/* BIOS/CSM installation from Live USB. Never accepts a silent erase flag. */
#include "myos.h"
#include "sys/stat.h"
#include "unistd.h"
#include "utime.h"
#include "errno.h"
#include "stdio.h"
#include "string.h"

#define TARGET "/mnt/install"
#define PATH_CAP 1024
static unsigned source_dev, files;
static unsigned long long bytes;
static char data[32768];

static int failure(const char *operation,const char *path,int error) {
    dprintf(STDERR,"setup: %s %s: %s\n",operation,path,strerror(error));return -1;
}
static int join(char *out,const char *parent,const char *name) {
    if(snprintf(out,PATH_CAP,"%s%s%s",parent,!strcmp(parent,"/")?"":"/",name)>=PATH_CAP)
        return failure("path too long:",parent,ENAMETOOLONG);
    return 0;
}
static int copy_tree(const char *source,const char *dest,unsigned depth) {
    struct stat st;
    if(depth>64)return failure("directory depth:",source,ELOOP);
    if(lstat(source,&st))return failure("stat",source,errno);
    if(st.st_dev!=source_dev)return 0; /* /dev and other mounted filesystems */
    if(depth==1 && S_ISDIR(st.st_mode))printf("Copying %s...\n",source);
    if(S_ISLNK(st.st_mode)) {
        char link[PATH_CAP];int n=readlink(source,link,sizeof(link)-1);
        if(n<0)return failure("readlink",source,errno);
        if(n>=(int)sizeof(link)-1)return failure("symlink too long:",source,ENAMETOOLONG);
        link[n]=0;
        if(symlink(link,dest) || lchown(dest,st.st_uid,st.st_gid))return failure("symlink",dest,errno);
        files++;return 0;
    }
    if(S_ISDIR(st.st_mode)) {
        if(mkdir(dest,0700) && errno!=EEXIST)return failure("mkdir",dest,errno);
        struct dirent_info entry;
        for(int i=0;;i++) {
            int r=dirread(source,i,&entry);if(r<0)return failure("readdir",source,-r);if(!r)break;
            if(!strcmp(entry.name,".") || !strcmp(entry.name,".."))continue;
            char from[PATH_CAP],to[PATH_CAP];
            if(join(from,source,entry.name) || join(to,dest,entry.name))return -1;
            /* The kernel copies its private account database during commit. */
            if(!strcmp(from,"/etc/accounts"))continue;
            /* Keep mount points, without recursively copying their devices. */
            struct stat child;if(lstat(from,&child))return failure("stat",from,errno);
            if(child.st_dev!=source_dev) {
                if(S_ISDIR(child.st_mode) && mkdir(to,0755) && errno!=EEXIST)return failure("mkdir",to,errno);
                continue;
            }
            if(copy_tree(from,to,depth+1))return -1;
        }
    } else if(S_ISREG(st.st_mode)) {
        int in=open(source,O_RDONLY);if(in<0)return failure("open",source,errno);
        int out=open(dest,O_WRONLY|O_CREAT|O_TRUNC,0600);
        if(out<0){close(in);return failure("create",dest,errno);}
        unsigned copied=0;int error=0;
        for(;;) {
            int n=read(in,data,sizeof(data));if(n<0){error=errno;break;}if(!n)break;
            for(int off=0;off<n;) {int w=write(out,data+off,n-off);if(w<=0){error=w<0?errno:EIO;break;}off+=w;}
            if(error)break;
            copied+=n;
        }
        if(close(out) && !error)error=errno;
        close(in);
        if(!error && copied!=st.st_size)error=EIO;
        if(error)return failure("copy",source,error);
        bytes+=copied;files++;
        if(!(files%100))printf("Copied %u files (%u MiB)\n",files,(unsigned)(bytes/1048576));
    } else return failure("unsupported file:",source,EINVAL);
    if(chown(dest,st.st_uid,st.st_gid) || chmod(dest,st.st_mode&01777))return failure("metadata",dest,errno);
    struct utimbuf times={st.st_atime,st.st_mtime};
    if(utime(dest,&times))return failure("timestamp",dest,errno);
    return 0;
}
static int list(void) {
    int count=0;
    for(unsigned i=0;i<INSTALL_TARGETS;i++) {
        struct disk_install_info info;int r=disk_install(INSTALL_LIST,i,&info);if(r<0)continue;
        printf("%s  %u MiB  %s\n",info.device,info.sectors/2048,info.model);count++;
    }
    if(!count)puts("No unmounted SATA/AHCI or NVMe installation targets found.");
    return count;
}
int main(int argc,char **argv) {
    if(argc!=2){puts("Usage: setup --list | setup DEVICE\nExamples: /dev/sdc (SATA), /dev/nvme0 (NVMe namespace)\nBIOS/CSM and UEFI x64 (Secure Boot off). Erases the entire selected disk/namespace.");return 2;}
    if(geteuid()){dprintf(STDERR,"setup: root login required\n");return 1;}
    int r=disk_install(INSTALL_PROBE,0,NULL);
    if(r<0){failure("probe","SATA/AHCI or NVMe targets (boot from Live USB)",-r);return 1;}
    if(!strcmp(argv[1],"--list"))return list()?0:1;
    unsigned index=INSTALL_TARGETS;
    for(unsigned i=0;i<INSTALL_TARGETS;i++){struct disk_install_info info;if(!disk_install(INSTALL_LIST,i,&info) && !strcmp(info.device,argv[1]))index=i;}
    if(index==INSTALL_TARGETS){list();failure("invalid target:",argv[1],ENODEV);return 1;}
    struct disk_install_info info;
    r=disk_install(INSTALL_PLAN,index,&info);
    if(r<0){failure("plan",argv[1],-r);return 1;}
    printf("\nDisk: %s (%s), %u MiB\nMyFS root: %u MiB\n",info.device,info.model,info.sectors/2048,info.root_sectors/2048);
    puts("ALL DATA AND PARTITIONS ON THIS DISK WILL BE ERASED.\nOther operating systems will not be preserved.");
    char expected[40],answer[80];snprintf(expected,sizeof(expected),"erase %s",info.device);
    printf("Type exactly '%s' to install: ",expected);
    if(!fgets(answer,sizeof(answer),stdin)){disk_install(INSTALL_ABORT,index,&info);return 1;}
    answer[strcspn(answer,"\r\n")]=0;
    if(strcmp(answer,expected)){puts("Installation cancelled. Disk unchanged.");disk_install(INSTALL_ABORT,index,&info);return 1;}
    struct stat root;if(stat("/",&root)){failure("stat","/",errno);goto abort;}
    source_dev=root.st_dev;
    struct stat config;
    if(lstat("/etc",&config) || !S_ISDIR(config.st_mode) || config.st_dev!=source_dev) {
        failure("requires a directory on the root filesystem:","/etc",EINVAL);goto abort;
    }
    if(mkdir("/mnt",0755) && errno!=EEXIST){failure("mkdir","/mnt",errno);goto abort;}
    if(mkdir(TARGET,0700) && errno!=EEXIST){failure("mkdir",TARGET,errno);goto abort;}
    r=disk_install(INSTALL_PREPARE,index,&info);
    if(r<0){failure("prepare",info.device,-r);goto abort;}
    char partition[16];snprintf(partition,sizeof(partition),"%s%s1",info.device,!strncmp(info.device,"/dev/nvme",9)?"p":"");
    r=mount(partition,TARGET);if(r<0){failure("mount",partition,-r);goto abort;}
    puts("Copying the Live root filesystem...");
    int failed=copy_tree("/",TARGET,0);
    if(!failed) {
        /* Hardware-specific Live fstab entries must not select other disks. */
        if(remove(TARGET "/etc/fstab") && errno!=ENOENT)failed=failure("unlink","fstab",errno);
    }
    if(!failed) {
        int fd=open(TARGET "/etc/fstab",O_WRONLY|O_CREAT|O_TRUNC,0644);
        const char text[]="# Installed MyOS: root is selected by the boot disk ID.\n";
        if(fd<0)failed=failure("create","fstab",errno);
        else {if(write(fd,text,sizeof(text)-1)!=sizeof(text)-1)failed=failure("write","fstab",EIO);if(close(fd))failed=-1;}
    }
    r=umount(TARGET);if(r<0){failure("unmount",TARGET,-r);failed=-1;}
    if(failed)goto abort;
    puts("Checking filesystem and committing the boot loader...");
    r=disk_install(INSTALL_COMMIT,index,&info);
    if(r<0){failure("commit",info.device,-r);goto abort;}
    printf("Installation complete: %u files, %u MiB.\nShut down, remove Live USB and boot the disk in BIOS/CSM or UEFI x64 mode.\n",files,(unsigned)(bytes/1048576));
    return 0;
abort:
    disk_install(INSTALL_ABORT,index,&info);
    dprintf(STDERR,"setup: installation failed; the selected disk may contain an incomplete installation.\n");return 1;
}
