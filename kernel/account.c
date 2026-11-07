/* Kernel-owned persistent account database. */
#include "account.h"
#include "task.h"
#include "fs.h"
#include "string.h"
#include "syscall.h"
#include "timer.h"
#define ROR(x,n) (((x)>>(n))|((x)<<(32-(n))))
static u32 be32(const u8 *p) { return (u32)p[0]<<24|(u32)p[1]<<16|(u32)p[2]<<8|p[3]; }
static void put_be32(u8 *p,u32 x) { p[0]=x>>24;p[1]=x>>16;p[2]=x>>8;p[3]=x; }
typedef struct { u32 h[8]; u8 buf[64]; u32 n; u64 len; } sha256_ctx;

static const u32 K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha256_block(sha256_ctx *c, const u8 *p) {
    u32 w[64], a, b, d, e, f, g, h, cc, t1, t2;
    for (int i = 0; i < 16; i++) w[i] = be32(p + 4 * i);
    for (int i = 16; i < 64; i++) {
        u32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 64; i++) {
        t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_init(sha256_ctx *c) {
    static const u32 iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                               0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(c->h, iv, sizeof(iv));
    c->n = 0;
    c->len = 0;
}

static void sha256_update(sha256_ctx *c, const void *data, int n) {
    const u8 *p = data;
    c->len += n;
    while (n > 0) {
        if (c->n == 0 && n >= 64) { sha256_block(c, p); p += 64; n -= 64; continue; }
        int k = 64 - c->n;
        if (k > n) k = n;
        memcpy(c->buf + c->n, p, k);
        c->n += k; p += k; n -= k;
        if (c->n == 64) { sha256_block(c, c->buf); c->n = 0; }
    }
}

static void sha256_final(sha256_ctx *c, u8 out[32]) {          /* c не портится — можно продолжать */
    sha256_ctx t = *c;
    u64 bits = t.len * 8;
    u8 pad = 0x80, zero = 0, lenb[8];
    sha256_update(&t, &pad, 1);
    while (t.n != 56) sha256_update(&t, &zero, 1);
    for (int i = 0; i < 8; i++) lenb[i] = bits >> (56 - 8 * i);
    sha256_update(&t, lenb, 8);
    for (int i = 0; i < 8; i++) put_be32(out + 4 * i, t.h[i]);
}


#define USERS 16
struct entry { char name[24]; u8 salt[16], hash[32]; };
static struct { u32 magic; struct entry users[USERS]; } db;
static struct mutex lock;
static bool loaded;
static void wipe(void *p,unsigned n) { volatile u8 *v=p;while(n--) *v++=0; }
static int load(void) {
    if(loaded) return 0;
    u32 ino;int r=fs_resolve(FS_ROOT,"/etc/accounts",&ino);
    if(r==FS_ENOENT) { memset(&db,0,sizeof(db));db.magic=0x55535231;strcpy(db.users[0].name,"root");loaded=true;return 0; }
    if(r || fs_read(ino,0,&db,sizeof(db))!=(int)sizeof(db) || db.magic!=0x55535231) return -EIO;
    for(int i=0;i<USERS;i++) if(db.users[i].name[23]) return -EIO;
    if(strcmp(db.users[0].name,"root")) return -EIO;
    loaded=true;return 0;
}
static int save(void) {
    u32 ino;int r=fs_resolve(FS_ROOT,"/etc/accounts",&ino);
    if(r==FS_ENOENT) { u32 dir;fs_create(FS_ROOT,"/etc",FS_TYPE_DIR,&dir);r=fs_create(FS_ROOT,"/etc/accounts",FS_TYPE_FILE,&ino); }
    return r || fs_write(ino,0,&db,sizeof(db))!=(int)sizeof(db) ? -EIO:0;
}
bool account_private(u32 ino) { u32 a;return !fs_resolve(FS_ROOT,"/etc/accounts",&a) && a==ino; }
static bool bootstrap(void) { for(int i=0;i<32;i++) if(db.users[0].hash[i]) return false;return true; }
static void digest(struct entry *e,const char *pw,u8 out[32]) {
    sha256_ctx c;sha256_init(&c);sha256_update(&c,e->salt,16);sha256_update(&c,pw,strlen(pw));sha256_final(&c,out);
    for(int i=0;i<4096;i++) {sha256_init(&c);sha256_update(&c,out,32);sha256_update(&c,pw,strlen(pw));sha256_update(&c,e->salt,16);sha256_final(&c,out);}
    wipe(&c,sizeof(c));
}
int account_name(char *out) {
    mutex_lock(&lock);int r=load();struct task *t=task_current();
    if(!r) { if(!t->authenticated || t->uid>=USERS) r=-EPERM;else memcpy(out,db.users[t->uid].name,24); }
    mutex_unlock(&lock);return r;
}
/* Public names only: salts and password hashes stay in the kernel. */
int account_list(char *out, unsigned index) {
    mutex_lock(&lock); int r=load();
    if(!r) {
        r=-ENOENT;
        for(int i=0;i<USERS;i++) if(db.users[i].name[0]) {
            if(!index--) { memcpy(out,db.users[i].name,24); r=0; break; }
        }
    }
    mutex_unlock(&lock); return r;
}
/* 0 first setup, 1 login, 2 useradd, 3 passwd, 4 uid, 5 name, 6 logout */
int account_ctl(unsigned op,const char *name,const char *pw) {
    struct task *t=task_current();int r;mutex_lock(&lock);
    r=load();if(r) goto done;
    if(op==0) {r=bootstrap();goto done;}
    if(op==4) {r=t->authenticated?(int)t->uid:-EPERM;goto done;}
    if(op==6) {t->authenticated=false;t->uid=0;r=0;goto done;}
    int id=-1;for(int i=0;i<USERS;i++) if(!strcmp(db.users[i].name,name)) id=i;
    if(op==1) {
        u8 h[32];struct entry *e=&db.users[id<0?0:id];digest(e,pw,h);unsigned diff=0;
        for(int i=0;i<32;i++) diff|=h[i]^e->hash[i];
        wipe(h,32);r=-EPERM;
        if(id>=0 && !diff && !bootstrap()) {t->uid=id;t->authenticated=true;r=0;}
        goto done;
    }
    if(op!=2 && op!=3) {r=-EINVAL;goto done;}
    if(!(bootstrap() && op==3 && id==0) && (!t->authenticated || (op==2?t->uid!=0:(t->uid!=0 && id!=(int)t->uid)))) {r=-EPERM;goto done;}
    if(!pw[0]) {r=-EINVAL;goto done;}
    if(op==2) {
        if(id>=0) {r=-EEXIST;goto done;}
        if(!name[0]) {r=-EINVAL;goto done;}
        for(const char *s=name;*s;s++) if(!((*s>='a'&&*s<='z')||(*s>='0'&&*s<='9')||*s=='_'||*s=='-')) {r=-EINVAL;goto done;}
        for(int i=1;i<USERS;i++) if(!db.users[i].name[0]) {id=i;break;}
        if(id<0) {r=-ENOSPC;goto done;}
    }
    if(id<0) {r=-ENOENT;goto done;}
    struct entry old=db.users[id],*e=&db.users[id];strcpy(e->name,name);
    u32 seed,high;__asm__ volatile("rdtsc":"=a"(seed),"=d"(high));seed^=high^timer_ticks();
    for(int i=0;i<16;i++) {seed=seed*1664525+1013904223;e->salt[i]=seed>>24;}
    digest(e,pw,e->hash);r=save();if(r) *e=old;wipe(&old,sizeof(old));
done: mutex_unlock(&lock);return r;
}
