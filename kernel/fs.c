/* VFS: opaque handles encode mount slot + local inode. /dev is virtual.
 * The recursive lock stays held across backend selection and blocking I/O. */
#include "fs.h"
#include "myfs.h"
#include "block.h"
#include "task.h"
#include "string.h"
#define DEVROOT 0xf0000001u
#define DEVBASE 0xf0000010u
#define DEVNULL 0xf0000100u
#define DEVZERO 0xf0000101u
#define SLOT(i) ((i)>>24)
#define LOCAL(i) ((i)&0xffffff)
#define HANDLE(s,i) (((u32)(s)<<24)|(i))
static struct { bool used; int device; u32 covered, parent; } mounts[4];
static struct mutex lock;
void fs_namespace_lock(void) { mutex_lock(&lock); }
void fs_namespace_unlock(void) { mutex_unlock(&lock); }
static bool ready;
static bool isdev(u32 i) { return i>=DEVROOT; }
static int select_inode(u32 i) {
    u32 s=SLOT(i); if(s>=4 || !mounts[s].used) return FS_ENODEV;
    myfs_select(s,mounts[s].device); return 0;
}
static u32 cross(u32 i) {
    for(int s=1;s<4;s++) if(mounts[s].used && mounts[s].covered==i) return HANDLE(s,FS_ROOT);
    return i;
}
static int stat_impl(u32 i,struct fs_stat *st) {
    if(!isdev(i)) { int r=select_inode(i); return r ? r : myfs_stat(LOCAL(i),st); }
    memset(st,0,sizeof(*st));
    if(i==DEVROOT) { st->type=FS_TYPE_DIR; return 0; }
    if(i==DEVNULL || i==DEVZERO) { st->type=FS_TYPE_CHAR; return 0; }
    const struct block_device *d=block_get((int)(i-DEVBASE)); if(!d) return FS_ENOENT;
    st->type=FS_TYPE_BLOCK; st->blocks=d->sectors;
    st->size=d->sectors>0x7fffff ? 0xffffffffu : d->sectors*512; return 0;
}
static int readdir_impl(u32 dir,u32 *pos,struct fs_dirent *out) {
    if(dir==DEVROOT) {
        u32 n=(*pos)++;
        if(n<(u32)block_count()) { int id=block_at(n); out->ino=DEVBASE+id; strcpy(out->name,block_get(id)->name); return 1; }
        if(n==(u32)block_count()) { out->ino=DEVNULL; strcpy(out->name,"null"); return 1; }
        if(n==(u32)block_count()+1) { out->ino=DEVZERO; strcpy(out->name,"zero"); return 1; }
        return 0;
    }
    if(isdev(dir)) return FS_ENOTDIR;
    int r=select_inode(dir); if(r) return r;
    if(dir==FS_ROOT && *pos==0) { *pos=1; out->ino=DEVROOT; strcpy(out->name,"dev"); return 1; }
    u32 p=dir==FS_ROOT ? *pos-1 : *pos;
    while((r=myfs_readdir(LOCAL(dir),&p,out))==1) {
        if(dir==FS_ROOT && !strcmp(out->name,"dev")) continue;
        out->ino=cross(HANDLE(SLOT(dir),out->ino)); break;
    }
    *pos=dir==FS_ROOT ? p+1 : p; return r;
}
static int lookup(u32 dir,const char *name,u32 *out) {
    struct fs_stat st; int r=stat_impl(dir,&st); if(r) return r;
    if(st.type!=FS_TYPE_DIR) return FS_ENOTDIR;
    if(!strcmp(name,".")) { *out=dir; return 0; }
    if(!strcmp(name,"..")) {
        if(dir==DEVROOT) { *out=FS_ROOT; return 0; }
        if(LOCAL(dir)==FS_ROOT && SLOT(dir)>0) { *out=mounts[SLOT(dir)].parent; return 0; }
        r=select_inode(dir); if(r) return r;
        r=myfs_resolve(LOCAL(dir),"..",out); if(!r) *out=HANDLE(SLOT(dir),*out); return r;
    }
    u32 pos=0; struct fs_dirent de;
    while((r=readdir_impl(dir,&pos,&de))==1) if(!strcmp(name,de.name)) { *out=de.ino; return 0; }
    return r<0 ? r : FS_ENOENT;
}
static int resolve_impl(u32 cwd,const char *path,u32 *out) {
    if(!ready) return FS_ENODEV;
    if(!path || !*path) return FS_ENOENT;
    u32 cur=*path=='/' ? FS_ROOT : cwd;
    while(*path) {
        while(*path=='/') path++;
        if(!*path) break;
        char name[FS_NAME_MAX+1]; u32 n=0;
        while(path[n] && path[n]!='/') { if(n==FS_NAME_MAX) return FS_ENAMETOOLONG; name[n]=path[n]; n++; }
        name[n]=0; path+=n; int r=lookup(cur,name,&cur); if(r) return r;
        if(*path=='/') { struct fs_stat st; r=stat_impl(cur,&st); if(r) return r; if(st.type!=FS_TYPE_DIR) return FS_ENOTDIR; }
    }
    *out=cur; return 0;
}
static int parent_impl(u32 cwd,const char *path,u32 *parent,char *name) {
    char buf[128]; u32 n=strlen(path); if(!n || n>=sizeof(buf)) return FS_EINVAL;
    memcpy(buf,path,n+1); while(n>1 && buf[n-1]=='/') buf[--n]=0;
    int slash=-1; for(u32 j=0;j<n;j++) if(buf[j]=='/') slash=j;
    const char *base=buf+slash+1;
    if(!*base || !strcmp(base,".") || !strcmp(base,"..")) return FS_EINVAL;
    if(strlen(base)>FS_NAME_MAX) return FS_ENAMETOOLONG;
    strcpy(name,base); int r=0;
    if(slash<0) *parent=cwd; else if(slash==0) *parent=FS_ROOT;
    else { buf[slash]=0; r=resolve_impl(cwd,buf,parent); }
    if(r) return r;
    struct fs_stat st; r=stat_impl(*parent,&st); return r ? r : st.type==FS_TYPE_DIR ? 0 : FS_ENOTDIR;
}
static bool protected(u32 ino) {
    if(isdev(ino) || LOCAL(ino)==FS_ROOT) return true;
    /* Prevent moving ancestors of active mounts. */
    for(int s=1;s<4;s++) if(mounts[s].used) {
        u32 p=mounts[s].covered;
        for(int depth=0;depth<64;depth++) { if(p==ino) return true; if(p==FS_ROOT || lookup(p,"..",&p)) break; }
    }
    return false;
}
static int getpath_impl(u32 ino,char *buf,u32 len) {
    char parts[64][FS_NAME_MAX+1]; int depth=0;
    while(ino!=FS_ROOT) {
        if(depth==64) return FS_ENAMETOOLONG;
        u32 parent; int r=lookup(ino,"..",&parent); if(r) return r;
        u32 pos=0; struct fs_dirent de; bool found=false;
        while(readdir_impl(parent,&pos,&de)==1) if(de.ino==ino) { strcpy(parts[depth++],de.name); found=true; break; }
        if(!found) return FS_ENOENT;
        ino=parent;
    }
    u32 n=1; for(int j=0;j<depth;j++) n+=strlen(parts[j])+(j!=0);
    if(n+1>len) return FS_ENAMETOOLONG;
    u32 p=0; buf[p++]='/';
    for(int j=depth-1;j>=0;j--) { u32 l=strlen(parts[j]); memcpy(buf+p,parts[j],l); p+=l; if(j) buf[p++]='/'; }
    buf[p]=0; return 0;
}
int fs_init(bool *formatted) {
    mutex_lock(&lock); ready=false; memset(mounts,0,sizeof(mounts)); *formatted=false;
    int r=block_init();
    if(!r) {
        r=FS_ENODEV;
        for(int d=block_partitioned()?1:0;d<5;d++) {
            if(!block_get(d)) continue;
            myfs_select(0,d); r=myfs_init(formatted,!block_partitioned());
            if(!r) { mounts[0].used=true; mounts[0].device=d; ready=true; break; }
        }
    } else r=FS_EIO;
    mutex_unlock(&lock); return r;
}
bool fs_ready(void) { return ready; }
int fs_format(void) {
    mutex_lock(&lock); int r=ready ? 0 : FS_ENODEV;
    for(int s=1;s<4;s++) if(mounts[s].used) r=FS_EBUSY;
    if(!r) { myfs_select(0,mounts[0].device); r=myfs_format(); }
    mutex_unlock(&lock); return r;
}
int fs_mount(u32 cwd,const char *device,const char *target) {
    mutex_lock(&lock); int d=block_find(device),r=0,slot=0; u32 ino=0,parent=0;
    if(d<0) r=FS_ENODEV;
    if(!r) r=resolve_impl(cwd,target,&ino);
    struct fs_stat st;
    if(!r) { r=stat_impl(ino,&st); if(!r && st.type!=FS_TYPE_DIR) r=FS_ENOTDIR; }
    if(!r && protected(ino)) r=FS_EBUSY;
    for(int s=0;!r && s<4;s++) if(mounts[s].used) {
        const struct block_device *a=block_get(d),*b=block_get(mounts[s].device);
        if(d/5==mounts[s].device/5 && a->start < b->start+b->sectors && b->start < a->start+a->sectors) r=FS_EBUSY;
    }
    if(!r) r=lookup(ino,"..",&parent);
    for(int s=1;s<4;s++) if(!mounts[s].used) { slot=s; break; }
    if(!r && !slot) r=FS_ENOSPC;
    if(!r) { bool formatted; myfs_select(slot,d); r=myfs_init(&formatted,false); }
    if(!r) { mounts[slot].used=true; mounts[slot].device=d; mounts[slot].covered=ino; mounts[slot].parent=parent; }
    mutex_unlock(&lock); return r;
}
int fs_mount_info(u32 index,char *device,char *target) {
    mutex_lock(&lock); int r=0;
    if(index<4 && mounts[index].used) {
        strcpy(device,"/dev/"); strcpy(device+5,block_get(mounts[index].device)->name);
        r=getpath_impl(HANDLE(index,FS_ROOT),target,128); r=r ? r : 1;
    }
    mutex_unlock(&lock); return r;
}
/* Administrative writes are restricted to unmounted disks/ranges. No generic
 * raw-write interface is exported to userspace. Slot 4 is a private formatter. */
static bool device_busy(int d,bool whole_disk) {
#ifndef HOST_TEST
    if(task_raw_device_busy(d/5)) return true;
#endif
    const struct block_device *a=block_get(d);
    for(int s=0;s<4;s++) if(mounts[s].used && mounts[s].device/5==d/5) {
        const struct block_device *b=block_get(mounts[s].device);
        if(whole_disk || (a->start<b->start+b->sectors && b->start<a->start+a->sectors)) return true;
    }
    return false;
}
int fs_mkfs(const char *device) {
    mutex_lock(&lock); int d=block_find(device),r=0;
    if(d<0) r=FS_ENODEV;
    else if(device_busy(d,false)) r=FS_EBUSY;
    /* Whole disks with a partition table must be explicitly reinitialized first. */
    else if(!block_get(d)->partition && block_disk_partitioned(d/5)) r=FS_EINVAL;
    else { myfs_select(4,d); r=myfs_format(); }
    mutex_unlock(&lock); return r;
}
static u32 mbr_u32(const u8 *p) { return p[0]|(u32)p[1]<<8|(u32)p[2]<<16|(u32)p[3]<<24; }
static void mbr_put(u8 *p,u32 v) { for(int i=0;i<4;i++) p[i]=v>>(8*i); }
int fs_partition(const char *device,u32 op,u32 slot,u32 start,u32 sectors) {
    mutex_lock(&lock); int d=block_find(device),r=0;
    if(d<0) r=FS_ENODEV;
    else if(block_get(d)->partition || op>2) r=FS_EINVAL;
    else if(device_busy(d,true)) r=FS_EBUSY;
    u8 buf[512];
    if(!r && block_read(d,0,buf)) r=FS_EIO;
    if(!r && op==0) { /* Explicit init: preserve boot code, clear primary entries. */
        if(buf[510]==0x55 && buf[511]==0xaa)
            for(int i=0;i<4;i++) if(buf[446+i*16+4]==0xee) r=FS_EINVAL;
        if(!r) { memset(buf+446,0,64); buf[510]=0x55; buf[511]=0xaa; }
    } else if(!r) {
        if(slot<1 || slot>4 || buf[510]!=0x55 || buf[511]!=0xaa) r=FS_EINVAL;
        for(int i=0;!r && i<4;i++) {
            const u8 *e=buf+446+i*16;
            if(e[4] && e[4]!=0x83) r=FS_EINVAL;
        }
        if(!r) {
            u8 *p=buf+446+(slot-1)*16;
            if(op==2) {
                if(!p[4]) r=FS_ENOENT; else memset(p,0,16);
            } else {
                u32 total=block_get(d)->sectors;
                if(p[4]) r=FS_EEXIST;
                else if(start<2048 || !sectors || sectors>2097152u || start>=total || sectors>total-start) r=FS_EINVAL;
                for(int i=0;!r && i<4;i++) {
                    const u8 *e=buf+446+i*16; if(!e[4]) continue;
                    u32 a=mbr_u32(e+8),n=mbr_u32(e+12);
                    if(!a || !n || a>=total || n>total-a || (start<a+n && a<start+sectors)) r=FS_EINVAL;
                }
                if(!r) { memset(p,0,16); p[1]=p[5]=0xfe; p[2]=p[3]=p[6]=p[7]=0xff; p[4]=0x83; mbr_put(p+8,start); mbr_put(p+12,sectors); }
            }
        }
    }
    if(!r && block_write(d,0,buf)) r=FS_EIO;
    if(!r && block_rescan(d/5)) r=FS_EIO;
    mutex_unlock(&lock); return r;
}
int fs_resolve(u32 cwd,const char *path,u32 *ino) { mutex_lock(&lock); int r=resolve_impl(cwd,path,ino); mutex_unlock(&lock); return r; }
int fs_stat(u32 ino,struct fs_stat *st) { mutex_lock(&lock); int r=stat_impl(ino,st); mutex_unlock(&lock); return r; }
int fs_readdir(u32 dir,u32 *pos,struct fs_dirent *out) { mutex_lock(&lock); int r=readdir_impl(dir,pos,out); mutex_unlock(&lock); return r; }
int fs_getpath(u32 ino,char *buf,u32 len) { mutex_lock(&lock); int r=getpath_impl(ino,buf,len); mutex_unlock(&lock); return r; }
int fs_create(u32 cwd,const char *path,int type,u32 *out) {
    mutex_lock(&lock); u32 parent=0,ino; char name[28]; int r=parent_impl(cwd,path,&parent,name);
    if(!r && (isdev(parent) || (parent==FS_ROOT && !strcmp(name,"dev")))) r=FS_EROFS;
    if(!r && type!=FS_TYPE_DIR && type!=FS_TYPE_FILE) r=FS_EINVAL;
    if(!r) { r=select_inode(parent); if(!r) r=myfs_create(LOCAL(parent),name,type,&ino); if(!r && out) *out=HANDLE(SLOT(parent),ino); }
    mutex_unlock(&lock); return r;
}
int fs_remove(u32 cwd,const char *path) {
    mutex_lock(&lock); u32 ino,parent; char name[28]; int r=resolve_impl(cwd,path,&ino);
    if(!r && protected(ino)) r=FS_EBUSY;
    if(!r) r=parent_impl(cwd,path,&parent,name);
    if(!r) { r=select_inode(parent); if(!r) r=myfs_remove(LOCAL(parent),name); }
    mutex_unlock(&lock); return r;
}
int fs_rename(u32 cwd,const char *src,const char *dst) {
    mutex_lock(&lock); u32 ino,sp,dp,dest; char sn[28],dn[28]; int r=resolve_impl(cwd,src,&ino);
    if(!r && protected(ino)) r=FS_EBUSY;
    if(!r) r=parent_impl(cwd,src,&sp,sn);
    if(!r) {
        struct fs_stat st;
        if(!resolve_impl(cwd,dst,&dest) && !stat_impl(dest,&st) && st.type==FS_TYPE_DIR) { dp=dest; strcpy(dn,sn); }
        else r=parent_impl(cwd,dst,&dp,dn);
    }
    if(!r && (isdev(dp) || (dp==FS_ROOT && !strcmp(dn,"dev")))) r=FS_EROFS;
    if(!r && SLOT(sp)!=SLOT(dp)) r=FS_EXDEV;
    if(!r) {
        char destpath[128]; r=select_inode(dp);
        if(!r) r=myfs_getpath(LOCAL(dp),destpath,sizeof(destpath));
        if(!r) { u32 n=strlen(destpath),l=strlen(dn);
            if(n+l+2>sizeof(destpath)) r=FS_ENAMETOOLONG;
            else { if(n>1) destpath[n++]='/'; strcpy(destpath+n,dn); r=myfs_rename(LOCAL(sp),sn,destpath); }
        }
    }
    mutex_unlock(&lock); return r;
}
int fs_truncate(u32 ino) {
    mutex_lock(&lock); int r;
    if(ino==DEVNULL || ino==DEVZERO) r=0; else if(isdev(ino)) r=FS_EROFS;
    else { r=select_inode(ino); if(!r) r=myfs_truncate(LOCAL(ino)); }
    mutex_unlock(&lock); return r;
}
int fs_read(u32 ino,u32 off,void *buf,u32 n) {
    mutex_lock(&lock); int r=0;
    if(ino==DEVNULL) r=0; else if(ino==DEVZERO) { memset(buf,0,n); r=n; }
    else if(ino==DEVROOT) r=FS_EISDIR;
    else if(isdev(ino)) {
        if(n>0xffffffffu-off) n=0xffffffffu-off;
        int d=(int)(ino-DEVBASE); const struct block_device *dev=block_get(d);
        if(!dev) r=FS_ENODEV;
        else {
            u32 done=0; u8 sector[512];
            while(done<n && off/512<dev->sectors) {
                if(block_read(d,off/512,sector)) { r=FS_EIO; break; }
                u32 part=512-off%512; if(part>n-done) part=n-done;
                memcpy((u8*)buf+done,sector+off%512,part); done+=part;
                if(off>0xffffffffu-part) break;
                off+=part;
            }
            if(done) r=done;
        }
    } else { r=select_inode(ino); if(!r) r=myfs_read(LOCAL(ino),off,buf,n); }
    mutex_unlock(&lock); return r;
}
int fs_write(u32 ino,u32 off,const void *buf,u32 n) {
    mutex_lock(&lock); int r;
    if(ino==DEVNULL || ino==DEVZERO) r=n; else if(isdev(ino)) r=FS_EROFS;
    else { r=select_inode(ino); if(!r) r=myfs_write(LOCAL(ino),off,buf,n); }
    mutex_unlock(&lock); return r;
}
void fs_info(struct fs_info *info) { mutex_lock(&lock); memset(info,0,sizeof(*info)); if(ready) { myfs_select(0,mounts[0].device); myfs_info(info); } mutex_unlock(&lock); }
const char *fs_strerror(int e) {
    if(e==FS_EROFS) return "read-only filesystem/device";
    if(e==FS_EBUSY) return "mountpoint or device busy";
    if(e==FS_EXDEV) return "cross-device rename";
    return myfs_strerror(e);
}


