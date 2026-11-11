/* Install defaults only on first use; retain keys/configuration on reinstall. */
#include <myos.h>
#include <stdio.h>
static int create(const char *path,const char *text) {
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL);
    if(fd<0){if(errno==EEXIST)return 0;perror(path);return 1;}
    size_t n=strlen(text),done=0;
    while(done<n){int ret=write(fd,text+done,n-done);if(ret<=0){perror(path);close(fd);unlink(path);return 1;}done+=ret;}
    if(close(fd)<0){perror(path);return 1;}return 0;
}
int main(void) {
    if(account(4,NULL,NULL)!=0){fprintf(stderr,"openssh-setup: root required\n");return 1;}
    mkdir("/etc/ssh");mkdir("/var/empty");mkdir("/run");
    if(create("/etc/ssh/ssh_config","Host *\n    AddressFamily inet\n    Compression no\n    ControlMaster no\n"))return 1;
    if(create("/etc/ssh/sshd_config",
        "# MyOS port: see /ext/instructions/openssh/README.txt\n"
        "Port 22\nAddressFamily inet\nListenAddress 0.0.0.0\n"
        "HostKey /etc/ssh/ssh_host_ed25519_key\n"
        "PermitRootLogin prohibit-password\nPasswordAuthentication yes\n"
        "PermitEmptyPasswords no\nKbdInteractiveAuthentication no\n"
        "LoginGraceTime 0\nMaxStartups 2\nMaxSessions 1\n"
        "AllowTcpForwarding no\nAllowAgentForwarding no\nX11Forwarding no\n"
        "Subsystem sftp /ext/bin/sftp-server\n"))return 1;
    int pid=fork(),status=1;
    if(pid<0){perror("openssh-setup: fork");return 1;}
    if(!pid){char *av[]={"ssh-keygen","-A",NULL};exec("/ext/bin/ssh-keygen",av);exit(127);}
    if(waitpid(pid,&status,0)<0||status){fprintf(stderr,"openssh-setup: host key generation failed (RDRAND required)\n");return 1;}
    puts("OpenSSH installed. Configure /etc/ssh/sshd_config, start as root: /ext/bin/sshd -D -e");
    return 0;
}
