#include <myos.h>
#include <stdio.h>
static int remove_if_present(const char *path) {
    int r=unlink(path);if(r<0&&r!=-ENOENT){fprintf(stderr,"openssh uninstall: %s: %s\n",path,strerror(-r));return 1;}return 0;
}
int main(void) {
    if(account(4,NULL,NULL)!=0){fprintf(stderr,"openssh uninstall: root required\n");return 1;}
    int ret=unitctl(UNIT_RELOAD,NULL,NULL,0);
    if(ret<0){fprintf(stderr,"openssh uninstall: daemon-reload: %s\n",strerror(-ret));return 1;}
    ret=unitctl(UNIT_STATUS,"sshd.service",NULL,0);
    if(ret!=-ENOENT) {
        if(!ret)ret=unitctl(UNIT_DISABLE,"sshd.service",NULL,0);
        if(!ret)ret=unitctl(UNIT_STOP,"sshd.service",NULL,0);
        if(ret<0){fprintf(stderr,"openssh uninstall: sshd.service: %s\n",strerror(-ret));return 1;}
    }
    if(remove_if_present("/etc/services/sshd.service") || remove_if_present("/etc/services/enabled/sshd.service"))return 1;
    ret=unitctl(UNIT_RELOAD,NULL,NULL,0);if(ret<0)return 1;
    const char *names[]={"ssh","sshd","sshd-session","sshd-auth","ssh-keygen","ssh-keyscan","scp","sftp","sftp-server","openssh-setup",NULL};
    for(int i=0;names[i];i++){char path[80];snprintf(path,sizeof(path),"/ext/bin/%s",names[i]);if(remove_if_present(path))return 1;}
    return 0;
}
