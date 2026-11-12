/* Version 3 attributes use a sparse two-level table, preserving 64-byte inodes
 * and both legacy data layouts. Zero records have explicit legacy defaults.
 * Table blocks are allocated from the ordinary bitmap and cannot be file data. */
#define PERMISSIONS_MAGIC 0x5045524du
struct disk_permissions {u32 magic,mode,uid,gid;};
#define PERMISSIONS_PER_BLOCK (BS/sizeof(struct disk_permissions))
struct permissions_cache_entry {int slot;u32 block,age;u8 bytes[BS];};
static struct permissions_cache_entry *permissions_cache;
static u32 permissions_age;
static void permissions_cache_clear(void) {
    if(permissions_cache)memset(permissions_cache,0,8*sizeof(*permissions_cache));
    permissions_age=0;
}
static bool permissions_block_valid(u32 b) {
    return b>=sb.data_start && b<sb.total_blocks && bit_get(b);
}
static int permissions_read(u32 b,void *out) {
    if(!permissions_block_valid(b))return FS_EIO;
    if(!permissions_cache){permissions_cache=bitmap_alloc(8*sizeof(*permissions_cache));if(!permissions_cache)return FS_ENOSPC;permissions_cache_clear();}
    if(!++permissions_age){permissions_cache_clear();permissions_age=1;}
    unsigned slot=0;u32 age=0xffffffffu;
    for(unsigned i=0;i<8;i++) {
        if(permissions_cache[i].age && permissions_cache[i].slot==selected && permissions_cache[i].block==b) {
            permissions_cache[i].age=permissions_age;memcpy(out,permissions_cache[i].bytes,BS);return 0;
        }
        if(permissions_cache[i].age<age){slot=i;age=permissions_cache[i].age;}
    }
    int ret=bread(b,out);if(ret)return ret;
    permissions_cache[slot].slot=selected;permissions_cache[slot].block=b;
    permissions_cache[slot].age=permissions_age;memcpy(permissions_cache[slot].bytes,out,BS);return 0;
}
static int permissions_write(u32 b,const void *in) {
    int ret=bwrite(b,in);
    for(unsigned i=0;permissions_cache && i<8;i++)if(permissions_cache[i].slot==selected && permissions_cache[i].block==b) {
        if(ret)permissions_cache[i].age=0;else memcpy(permissions_cache[i].bytes,in,BS);
    }
    return ret;
}
static int permissions_block(u32 ino,bool alloc,u32 *out) {
    *out=0;
    if(sb.version!=3 || !sb.permissions_root) {
        if(!alloc)return 0;
        u32 root=balloc();if(!root)return FS_ENOSPC;
        struct superblock old=sb;
        if(sb.version!=3)sb.layout_version=sb.version;
        sb.version=3;sb.permissions_root=root;
        if(bwrite(0,&sb)){sb=old;bfree(root);return FS_EIO;}
    }
    u32 pointers[NINDIRECT];
    u32 index=ino/PERMISSIONS_PER_BLOCK;
    if(index/NINDIRECT>=NINDIRECT)return FS_EINVAL;
    int ret=permissions_read(sb.permissions_root,pointers);if(ret)return ret;
    u32 leaf=pointers[index/NINDIRECT];
    if(!leaf) {
        if(!alloc)return 0;
        leaf=balloc();if(!leaf)return FS_ENOSPC;
        pointers[index/NINDIRECT]=leaf;
        if((ret=permissions_write(sb.permissions_root,pointers))){bfree(leaf);return ret;}
    }
    if((ret=permissions_read(leaf,pointers)))return ret;
    u32 block=pointers[index%NINDIRECT];
    if(!block && alloc) {
        block=balloc();if(!block)return FS_ENOSPC;
        pointers[index%NINDIRECT]=block;
        if((ret=permissions_write(leaf,pointers))){bfree(block);return ret;}
    }
    *out=block;return 0;
}
static int permissions_store(u32 ino,u32 mode,u32 uid,u32 gid) {
    u32 block;int ret=permissions_block(ino,true,&block);if(ret)return ret;
    struct disk_permissions records[PERMISSIONS_PER_BLOCK];
    if((ret=permissions_read(block,records)))return ret;
    records[ino%PERMISSIONS_PER_BLOCK]=(struct disk_permissions){PERMISSIONS_MAGIC,mode,uid,gid};
    return permissions_write(block,records);
}
static int myfs_read_impl(u32 ino,u32 off,void *buf,u32 n);
static int permissions_load(u32 ino,const struct inode *in,u32 *mode,u32 *uid,u32 *gid) {
    u32 block;int ret=permissions_block(ino,false,&block);if(ret)return ret;
    if(block) {
        struct disk_permissions records[PERMISSIONS_PER_BLOCK];
        if((ret=permissions_read(block,records)))return ret;
        struct disk_permissions *p=&records[ino%PERMISSIONS_PER_BLOCK];
        if(p->magic==PERMISSIONS_MAGIC){*mode=p->mode;*uid=p->uid;*gid=p->gid;return 0;}
        if(p->magic)return FS_EIO;
    }
    *uid=*gid=0;*mode=in->type==FS_TYPE_DIR?0755:in->type==FS_TYPE_LINK?0777:0644;
    if(in->type==FS_TYPE_FILE && in->size>=4) {
        u8 magic[4];int n=myfs_read_impl(ino,0,magic,4);if(n<0)return n;
        if(n==4 && !memcmp(magic,"\177ELF",4))*mode=0755;
    }
    return 0;
}
int myfs_setattr(u32 ino,u32 mode,u32 uid,u32 gid) {
    struct inode in;int ret=iread(ino,&in);if(ret)return ret;
    if(in.type==FS_TYPE_FREE)return FS_ENOENT;
    return permissions_store(ino,mode,uid,gid);
}
