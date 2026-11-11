#include <myos.h>
#include <stdio.h>
static int remove_if_present(const char *path) {
    int r=unlink(path);if(r<0&&r!=-ENOENT){fprintf(stderr,"openssh uninstall: %s: %s\n",path,strerror(-r));return 1;}return 0;
}
int main(void) {
    if(account(4,NULL,NULL)!=0){fprintf(stderr,"openssh uninstall: root required\n");return 1;}
    const char *names[]={"ssh","sshd","sshd-session","sshd-auth","ssh-keygen","ssh-keyscan","scp","sftp","sftp-server","openssh-setup",NULL};
    for(int i=0;names[i];i++){char path[80];snprintf(path,sizeof(path),"/ext/bin/%s",names[i]);if(remove_if_present(path))return 1;}
    return 0;
}
