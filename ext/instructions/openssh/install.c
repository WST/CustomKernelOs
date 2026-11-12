/* Install defaults only on first use; retain keys/configuration on reinstall. */
#include <myos.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
static int create(const char *path,const char *text) {
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0644);
    if(fd<0){if(errno==EEXIST)return 0;perror(path);return 1;}
    size_t n=strlen(text),done=0;
    while(done<n){int ret=write(fd,text+done,n-done);if(ret<=0){perror(path);close(fd);unlink(path);return 1;}done+=ret;}
    if(close(fd)<0){perror(path);return 1;}return 0;
}
static int run(const char *path,char **argv) {
    int pid=fork(),status=1;
    if(pid<0){perror("openssh-setup: fork");return 1;}
    if(!pid){exec(path,argv);exit(127);}
    return waitpid(pid,&status,0)<0 || status;
}
static int service(void) {
    mkdir("/etc/services");mkdir("/etc/services/enabled");
    if(create("/etc/services/sshd.service",
        "[Service]\nType=simple\nUser=root\n"
        "ExecStart=/ext/bin/sshd -D -e\nRestart=on-failure\nStandardInput=null\n"))return 1;
    int ret=unitctl(UNIT_RELOAD,NULL,NULL,0);
    if(!ret)ret=unitctl(UNIT_ENABLE,"sshd.service",NULL,0);
    if(!ret)ret=unitctl(UNIT_START,"sshd.service",NULL,0);
    if(ret<0){fprintf(stderr,"openssh-setup: sshd.service: %s\n",strerror(-ret));return 1;}
    return 0;
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
    char *keys[]={"ssh-keygen","-A",NULL};
    if(run("/ext/bin/ssh-keygen",keys)){fprintf(stderr,"openssh-setup: host key generation failed (RDRAND required)\n");return 1;}
    if(chown("/etc/ssh/ssh_host_ed25519_key",0,0)<0 || chmod("/etc/ssh/ssh_host_ed25519_key",0600)<0){perror("host key permissions");return 1;}
    char *check[]={"/ext/bin/sshd","-t",NULL};
    if(run("/ext/bin/sshd",check)){fprintf(stderr,"openssh-setup: sshd configuration check failed\n");return 1;}
    if(service())return 1;
    puts("OpenSSH installed. sshd.service enabled and started. Use systemctl status sshd.");
    return 0;
}
