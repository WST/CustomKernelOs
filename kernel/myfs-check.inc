#ifdef HOST_TEST
int printf(const char *,...);
#define CHECK_ERROR() do {printf("MyFS check failed at %d\n",__LINE__);r=FS_EIO;goto done;} while(0)
#else
#define CHECK_ERROR() do {r=FS_EIO;goto done;} while(0)
#endif
/* Mount-time consistency pass. Validate every pointer before repairing anything;
 * ambiguous ownership/corrupt namespaces fail closed instead of freeing data.
 * Bitmap repair and unreachable-inode reclamation are individually journalled. */
static bool check_slash(const char *p){while(*p)if(*p++=='/')return true;return false;}
static u8 *check_bits;
static int check_claim(u32 b) {
    u32 end=sb.version>=4?sb.journal_start:sb.total_blocks;
    if(b<sb.data_start || b>=end || (check_bits[b/8]&(1u<<(b%8))))return FS_EIO;
    check_bits[b/8]|=1u<<(b%8);return 0;
}
static int check_tree(u32 b,unsigned depth) {
    if(!b)return 0;
    if(check_claim(b))return FS_EIO;
    if(depth) {
        u32 pointers[NINDIRECT];if(bread(b,pointers))return FS_EIO;
        for(unsigned i=0;i<NINDIRECT;i++)if(check_tree(pointers[i],depth-1))return FS_EIO;
    }
    return 0;
}
static int check_readdir(u32 ino,u32 size,u32 *pos,struct fs_dirent *out) {
 while(*pos<size) {
  if(legacy()) {struct dirent_v1 d;if(size-*pos<sizeof(d) || read_v1(ino,*pos,&d)!=1)return FS_EIO;*pos+=sizeof(d);if(!d.ino)continue;out->ino=d.ino;memcpy(out->name,d.name,sizeof(d.name));}
  else {struct dirent_v2 h;char name[FS_NAME_MAX+1];if(read_v2(ino,*pos,&h,name)!=1 || h.reclen>size-*pos)return FS_EIO;*pos+=h.reclen;if(!h.ino)continue;if(strlen(name)!=h.namelen)return FS_EIO;out->ino=h.ino;memcpy(out->name,name,h.namelen+1u);}
  return 1;
 }return 0;
}
static int myfs_check_impl(bool repair) {
    u32 bytes=sb.bitmap_blocks*BS;
    check_bits=bitmap_alloc(bytes);
    u32 *parents=bitmap_alloc(sb.inode_count*sizeof(u32));
    u32 *queue=bitmap_alloc(sb.inode_count*sizeof(u32));
    int r=0;
    if(!check_bits || !parents || !queue){r=FS_ENOSPC;goto done;}
    memset(check_bits,0,bytes);memset(parents,0,sb.inode_count*sizeof(u32));
    for(u32 b=0;b<sb.data_start;b++)check_bits[b/8]|=1u<<(b%8);
    if(sb.version>=4)for(u32 b=sb.journal_start;b<sb.total_blocks;b++)check_bits[b/8]|=1u<<(b%8);
    if(sb.version>=3 && sb.permissions_root && check_tree(sb.permissions_root,2)){CHECK_ERROR();}
    if(sb.times_root && check_tree(sb.times_root,2)){CHECK_ERROR();}
    struct inode in;
    for(u32 ino=FS_ROOT;ino<sb.inode_count;ino++) {
        if(iread(ino,&in)){CHECK_ERROR();}
        if(in.type==FS_TYPE_FREE)continue;
        if(in.type!=FS_TYPE_FILE && in.type!=FS_TYPE_DIR && in.type!=FS_TYPE_LINK){CHECK_ERROR();}
        if(in.size>max_file_blks()*BS){CHECK_ERROR();}
        for(u32 i=0;i<n_direct();i++)if(check_tree(in.direct[i],0)){CHECK_ERROR();}
        if(check_tree(in.indirect,1) || check_tree(in.dindirect,2) || (!legacy() && check_tree(in.direct[11],3))){CHECK_ERROR();}
    }
    /* Breadth-first namespace walk detects cycles, dangling and duplicate links. */
    u32 head=0,tail=1;queue[0]=FS_ROOT;parents[FS_ROOT]=FS_ROOT;
    while(head<tail) {
        u32 ino=queue[head++];
        if(iread(ino,&in) || !in.type || !in.links){CHECK_ERROR();}
        if(ino==FS_ROOT && in.type!=FS_TYPE_DIR){CHECK_ERROR();}
        if(in.type!=FS_TYPE_DIR)continue;
        bool dot=false,dotdot=false;u32 pos=0;struct fs_dirent de;int n;
        while((n=check_readdir(ino,in.size,&pos,&de))>0) {
            if(!de.ino || de.ino>=sb.inode_count){CHECK_ERROR();}
            if(!strcmp(de.name,".")){if(dot || de.ino!=ino){CHECK_ERROR();}dot=true;continue;}
            if(!strcmp(de.name,"..")){if(dotdot || de.ino!=parents[ino]){CHECK_ERROR();}dotdot=true;continue;}
            if(!de.name[0] || check_slash(de.name) || parents[de.ino] || tail>=sb.inode_count){CHECK_ERROR();}
            parents[de.ino]=ino;queue[tail++]=de.ino;
        }
        if(n<0 || !dot || !dotdot){CHECK_ERROR();}
    }
    if(!repair) {
        for(u32 b=sb.total_blocks-JOURNAL_BLOCKS;b<sb.total_blocks;b++)if(check_bits[b/8]&(1u<<(b%8))){r=FS_ENOSPC;goto done;}
        goto done;
    }
    for(u32 sec=0;sec<sb.bitmap_blocks;sec++)if(memcmp(bitmap+sec*BS,check_bits+sec*BS,BS)) {
        r=transaction_begin();if(r)goto done;
        memcpy(bitmap+sec*BS,check_bits+sec*BS,BS);
        bitmap_mark(sec*BITS_PER_BLK);
        r=transaction_end(bitmap_flush());if(r)goto done;
    }
    available_blocks=available_inodes=0;
    for(u32 b=sb.data_start;b<sb.total_blocks;b++)if(!bit_get(b))available_blocks++;
    for(u32 ino=FS_ROOT;ino<sb.inode_count;ino++) {
        if(iread(ino,&in)){CHECK_ERROR();}
        if(!in.type){available_inodes++;continue;}
        if(parents[ino])continue;
        r=transaction_begin();if(r)goto done;
        in.links=0;r=iwrite(ino,&in);
        if(!r)r=myfs_reap_orphan(ino);
        r=transaction_end(r);if(r)goto done;
    }
done:
    bitmap_free(check_bits);check_bits=NULL;bitmap_free(parents);bitmap_free(queue);
    return r;
}
