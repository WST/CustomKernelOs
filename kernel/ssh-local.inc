/* Full duplex local streams and single-descriptor SCM_RIGHTS, for OpenSSH.
 * Included by file.c so local streams reuse the regular pipe implementation. */
struct local_pair {
    struct pipe p[2];
    struct file *rights[2][16];
    unsigned head[2],count[2],ends;
};
static void local_close(struct file *f) {
    struct local_pair *l=f->local; unsigned s=f->mode;
    l->p[s].readers--; l->p[1-s].writers--;
    task_wakeup(&l->p[s]); task_wakeup(&l->p[1-s]); task_wakeup(l);
    while(l->count[s]) { struct file *p=l->rights[s][l->head[s]];
        l->head[s]=(l->head[s]+1)%16;l->count[s]--;file_close(p); }
    if (!--l->ends) kfree(l);
}
int local_pair_create(struct file **a, struct file **b) {
    struct local_pair *l=kcalloc(1,sizeof(*l));
    struct file *x=kcalloc(1,sizeof(*x)),*y=kcalloc(1,sizeof(*y));
    if(!l || !x || !y) {kfree(l);kfree(x);kfree(y);return -ENOMEM;}
    l->ends=2;
    for(int i=0;i<2;i++) l->p[i].readers=l->p[i].writers=1;
    x->type=y->type=FILE_LOCAL;x->refs=y->refs=1;x->local=y->local=l;
    x->mode=0;y->mode=1;*a=x;*b=y;return 0;
}
int local_sendfd(struct file *f, struct file *passed) {
    if(f->type!=FILE_LOCAL || passed->type==FILE_LOCAL) return -EINVAL;
    struct local_pair *l=f->local;unsigned s=1-f->mode;
    if(!l->p[s].readers) return -EPIPE;
    if(l->count[s]==16) return -EAGAIN;
    l->rights[s][(l->head[s]+l->count[s])%16]=file_dup(passed);
    l->count[s]++;task_wakeup(l);return 0;
}
struct file *local_recvfd(struct file *f,int *err) {
    if(f->type!=FILE_LOCAL) {*err=-EINVAL;return NULL;}
    struct local_pair *l=f->local;unsigned s=f->mode;
    if(!l->count[s]) {*err=-EAGAIN;return NULL;}
    struct file *p=l->rights[s][l->head[s]];
    l->head[s]=(l->head[s]+1)%16;l->count[s]--;*err=0;return p;
}
int file_ready(struct file *f,int events) {
    int ready=0;
    switch(f->type) {
    case FILE_DISK: return events&(1|4);
    case FILE_CONSOLE: return (events&4) | ((console_readable()) ? (events&1):0);
    case FILE_LOCAL: {
        struct local_pair *l=f->local;unsigned s=f->mode;
        if(l->p[s].count || !l->p[s].writers) ready|=1;
        if(l->p[1-s].count<PIPE_SIZE) ready|=4;
        if(!l->p[s].writers) ready|=16;
        if(!l->p[1-s].readers) ready|=8;
        break;
    }
    case FILE_PIPE:
        if(!f->mode) { if(f->pipe->count || !f->pipe->writers) ready|=1;
            if(!f->pipe->writers) ready|=16; }
        else {if(f->pipe->count<PIPE_SIZE) ready|=4;if(!f->pipe->readers) ready|=8;}
        break;
    case FILE_SOCKET: {
        struct sock *s=f->sock;
        if(s->type==SOCK_UDP) {ready=4;if(s->qn) ready|=1;break;}
        if(s->state==TCP_LISTEN) {if(s->aq_n) ready|=1;break;}
        if(s->rx_len || s->peer_fin || s->err || s->state==TCP_CLOSED) ready|=1;
        if((s->state==TCP_ESTABLISHED || s->state==TCP_CLOSE_WAIT) && s->tx_len<SOCK_TXBUF) ready|=4;
        if(s->peer_fin || s->state==TCP_CLOSED) ready|=16;
        if(s->err) ready|=8;
        break;
    }
    case FILE_PTY_MASTER:
        if(f->tty->out.count || !f->tty->slaves) ready|=1;
        if(f->tty->in.count<TTY_BUF) ready|=4;
        if(!f->tty->slaves) ready|=16;
        break;
    case FILE_PTY_SLAVE:
        if(f->tty->in.count || f->tty->eof || !f->tty->masters) ready|=1;
        if(f->tty->out.count<TTY_BUF) ready|=4;
        if(!f->tty->masters) ready|=16;
        break;
    }
    return ready&(events|8|16);
}
