/* Deliberately small ABI used by the private OpenSSH POSIX overlay.
 * All addresses are checked before dereferencing. Existing socket ABI is unchanged.
 * op: 1 poll, 2 socketpair, 3 sendfd, 4 recvfd, 5 socknames,
 *     6 entropy, 7 fcntl, 8 identities, 9 shutdown. */
static int sys_sshctl(u32 op,u32 arg,u32 ptr) {
    struct task *p=task_process(task_current());
    if(op==1) {
        struct pollfd_abi {int fd;short events,revents;};
        if(arg>MAX_FDS || !user_buf_ok(ptr,arg*sizeof(struct pollfd_abi),true)) return -EINVAL;
        struct pollfd_abi *fds=(void*)ptr;int n=0;
        for(unsigned i=0;i<arg;i++) {
            int fd=fds[i].fd;fds[i].revents=0;
            if(fd<0) continue;
            struct file *f=get_fd(fd);
            fds[i].revents=f?file_ready(f,fds[i].events):32;
            if(fds[i].revents) n++;
        }
        return n;
    }
    if(op==2) {
        if(!user_buf_ok(ptr,8,true)) return -EFAULT;
        struct file *a,*b;int ret=local_pair_create(&a,&b);if(ret<0) return ret;
        int x=alloc_fd(a);if(x<0) {file_close(a);file_close(b);return x;}
        int y=alloc_fd(b);if(y<0) {p->fds[x]=NULL;file_close(a);file_close(b);return y;}
        ((int*)ptr)[0]=x;((int*)ptr)[1]=y;return 0;
    }
    if(op==10) {
        if(!p->authenticated || p->uid) return -EPERM;
        char path[FS_PATH_MAX];u32 ino;struct fs_stat st;
        if(!user_str(ptr,path,sizeof(path))) return -EFAULT;
        int ret=fs_resolve(p->cwd,path,&ino);if(ret) return fs_errno(ret);
        ret=fs_stat(ino,&st);if(ret) return fs_errno(ret);
        if(st.type!=FS_TYPE_DIR) return -ENOTDIR;
        p->fs_root=ino;p->cwd=ino;return 0;
    }
    if(op==6) {
        if(arg>256 || !user_buf_ok(ptr,arg,true)) return -EFAULT;
        u32 a,b,c,d;__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));
        if(!(c&(1u<<30))) return -38; /* No timing/PID fallback for cryptographic keys. */
        u8 *out=(void*)ptr;
        for(unsigned i=0;i<arg;) {
            unsigned value;u8 ok=0;
            for(int tries=0;tries<32 && !ok;tries++)
                __asm__ volatile(".byte 0x0f,0xc7,0xf0; setc %1":"=a"(value),"=qm"(ok)::"cc");
            if(!ok) return -EIO;
            for(int j=0;j<4 && i<arg;j++,i++) out[i]=value>>(j*8);
        }
        return 0;
    }
    if(op==12) {
        if(!user_buf_ok(ptr,12,true)) return -EFAULT;
        int *v=(void*)ptr;
        if(!arg) {v[0]=p->real_gid;v[1]=p->gid;v[2]=p->saved_gid;return 0;}
        if(!p->authenticated) return -EPERM;
        if(p->uid) for(int i=0;i<3;i++)
            if(v[i]!=-1 && (u32)v[i]!=p->gid && (u32)v[i]!=p->real_gid && (u32)v[i]!=p->saved_gid) return -EPERM;
        if(v[0]!=-1) p->real_gid=v[0];
        if(v[1]!=-1) p->gid=v[1];
        if(v[2]!=-1) p->saved_gid=v[2];
        return 0;
    }
    if(op==8) {
        if(!user_buf_ok(ptr,12,true)) return -EFAULT;
        int *v=(void*)ptr;
        if(!arg) {v[0]=p->real_uid;v[1]=p->uid;v[2]=p->saved_uid;return 0;}
        if(!p->authenticated) return -EPERM;
        if(p->uid) for(int i=0;i<3;i++)
            if(v[i]!=-1 && (u32)v[i]!=p->uid && (u32)v[i]!=p->real_uid && (u32)v[i]!=p->saved_uid) return -EPERM;
        if(v[0]!=-1) p->real_uid=v[0];
        if(v[1]!=-1) p->uid=v[1];
        if(v[2]!=-1) p->saved_uid=v[2];
        return 0;
    }
    struct file *f=get_fd(arg);if(!f) return -EBADF;
    if(op==11) {
        if(!user_buf_ok(ptr,8,true)) return -EFAULT;
        int *v=(void*)ptr;
        if(f->type==FILE_CONSOLE) {int old=console_get_mode();if(v[0]) console_set_mode(v[1]);v[1]=old;return 0;}
        if(f->type!=FILE_PTY_MASTER && f->type!=FILE_PTY_SLAVE) return -25;
        int old=f->tty->mode;if(v[0]) tty_set_mode(f->tty,v[1]);v[1]=old;return 0;
    }
    if(op==3) {struct file *passed=get_fd(ptr);return passed?local_sendfd(f,passed):-EBADF;}
    if(op==4) {int err;struct file *passed=local_recvfd(f,&err);if(!passed) return err;
        int fd=alloc_fd(passed);if(fd<0) file_close(passed);return fd;}
    if(op==5) {
        if(f->type!=FILE_SOCKET) return -88;
        if(!user_buf_ok(ptr,16,true)) return -EFAULT;
        u32 *v=(void*)ptr;v[0]=net_src_ip(f->sock->rip);v[1]=f->sock->lport;v[2]=f->sock->rip;v[3]=f->sock->rport;
        return 0;
    }
    if(op==7) {
        if(!user_buf_ok(ptr,8,false)) return -EFAULT;
        int *v=(void*)ptr;
        switch(v[0]) {
        case 1:return (p->cloexec>>arg)&1;
        case 2:if(v[1]&1) p->cloexec|=1u<<arg;else p->cloexec&=~(1u<<arg);return 0;
        case 3:return f->status_flags;
        case 4:f->status_flags=v[1]&04000;return 0;
        }
        return -EINVAL;
    }
    if(op==9) {if(f->type!=FILE_SOCKET) return -88;tcp_close(f->sock);return 0;}
    return -EINVAL;
}
