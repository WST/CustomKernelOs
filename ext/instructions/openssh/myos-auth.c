#include "includes.h"
#include "ssh.h"
#include "sshkey.h"
#include "packet.h"
#include "hostfile.h"
#include "auth.h"
int sys_auth_passwd(struct ssh *ssh,const char *password) {
    Authctxt *a=ssh->authctxt;
    return a->valid && password[0] && account(10,a->pw->pw_name,password)==0;
}
