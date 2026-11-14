/* USB Mass Storage Bulk-Only Transport, SCSI transparent, LUN 0, 512-byte media.
 * Only the disk ID + partition range supplied by our BIOS loader is exported.
 * No probing or writing of internal SATA/ATA disks in Live mode. */
static struct usbdev *root_storage;
static u32 root_generation,root_offset,storage_tag;
static bool root_ambiguous,root_sealed;
static struct mutex usb_lock;
static u32 bot_u32(const u8 *p) {return p[0]|(u32)p[1]<<8|(u32)p[2]<<16|(u32)p[3]<<24;}
static void bot_put(u8 *p,u32 v) {for(int i=0;i<4;i++)p[i]=v>>(i*8);}
static u32 bot_be32(const u8 *p) {return (u32)p[0]<<24|(u32)p[1]<<16|(u32)p[2]<<8|p[3];}
static void bot_beput(u8 *p,u32 v) {for(int i=0;i<4;i++)p[i]=v>>((3-i)*8);}
static bool bulk_transfer(struct usbdev *d,bool input,unsigned length) {
    if(poisoned || d->failed || d->removing || length>4096)return false;
    unsigned at=input?1:0,dci=d->bulk_dci[at];
    u32 ptr=enqueue(&d->bulk[at],(u32)d->storage_buffer,0,length,TYPE(1)|(1u<<5)|(1u<<2));
    ring_doorbell(d->slot,dci);waiting_slot=d->slot;waiting_dci=dci;
    unsigned code=completion(ptr,5000,false);waiting_slot=waiting_dci=0;
    if(code!=1 || wait_left) {d->failed=true;return false;}
    return true;
}
static int scsi(struct usbdev *d,const u8 *cdb,unsigned cdb_len,void *data,unsigned length,bool input) {
    u8 *b=d->storage_buffer;u32 tag=++storage_tag;
    memset(b,0,31);bot_put(b,0x43425355);bot_put(b+4,tag);bot_put(b+8,length);
    b[12]=input?0x80:0;b[14]=cdb_len;memcpy(b+15,cdb,cdb_len);
    if(!bulk_transfer(d,false,31))return -1;
    if(length) {
        if(!input)memcpy(b,data,length);
        if(!bulk_transfer(d,input,length))return -1;
        if(input)memcpy(data,b,length);
    }
    if(!bulk_transfer(d,true,13))return -1;
    if(bot_u32(b)!=0x53425355 || bot_u32(b+4)!=tag || bot_u32(b+8) || b[12]>1) {d->failed=true;return -1;}
    return b[12]?-1:0;
}
static bool configure_storage(struct usbdev *d,const u8 *descriptors,unsigned total,unsigned config) {
    unsigned iface=0;bool active=false;u32 packets[2]={0};
    const struct boot_info *boot=boot_info_get();
    if(!boot || boot->version<2 || !boot->root_disk_id)return false;
    for(unsigned pos=0;pos+2<=total;) {
        const u8 *p=descriptors+pos;unsigned len=p[0];if(len<2 || pos+len>total)return false;
        if(p[1]==4 && len>=9) {active=p[3]==0 && p[5]==8 && p[6]==6 && p[7]==0x50;if(active)iface=p[2];}
        else if(active && p[1]==5 && len>=7 && (p[3]&3)==2) {
            unsigned at=(p[2]&0x80)?1:0;d->bulk_dci[at]=(p[2]&15)*2+at;
            packets[at]=(p[4]|(u32)p[5]<<8)&0x7ff;
        }
        pos+=len;
    }
    if(!packets[0] || !packets[1] || packets[0]>1024 || packets[1]>1024)return false;
    d->storage_buffer=page();
    if(!d->storage_buffer || !ring_init(&d->bulk[0]) || !ring_init(&d->bulk[1]))return false;
    copy_slot(d);d->input[1]=1;unsigned maxdci=1;
    for(unsigned at=0;at<2;at++) {
        unsigned dci=d->bulk_dci[at];if(dci<2 || dci>31)return false;
        u32 *ep=ctx(d->input,dci+1);ep[1]=(packets[at]<<16)|((at?6:2)<<3)|(3<<1);
        ep[2]=(u32)d->bulk[at].base|1;ep[4]=512;
        d->input[1]|=1u<<dci;if(dci>maxdci)maxdci=dci;
    }
    ctx(d->input,1)[0]=(ctx(d->input,1)[0]&0x07ffffff)|(maxdci<<27);
    if(!command((u32)d->input,TYPE(12)|(d->slot<<24)) || !control(d,0,9,config,0,0))return false;
    (void)iface; /* LUN 0 is mandatory; optional GET_MAX_LUN need not succeed. */
    u8 capacity[8],cdb[16]={0x25};bool ready=false;
    for(unsigned retry=0;retry<5 && !d->failed;retry++) {
        if(!scsi(d,cdb,10,capacity,8,true)){ready=true;break;}
        u8 sense[18],request[6]={3,0,0,0,18,0};scsi(d,request,6,sense,18,true);sleep_ms(100);
    }
    if(!ready || bot_be32(capacity+4)!=512 || bot_be32(capacity)==0xffffffffu)return false;
    d->storage_sectors=bot_be32(capacity)+1;
    u8 mbr[512],read[10]={0x28,0,0,0,0,0,0,0,1,0};
    if(scsi(d,read,10,mbr,512,true))return false;
    if(mbr[510]!=0x55 || mbr[511]!=0xaa || bot_u32(mbr+440)!=boot->root_disk_id)return true;
    bool match=false;
    for(unsigned i=0;i<4;i++) {
        u8 *part=mbr+446+i*16;
        if(part[4]==0x7f && bot_u32(part+8)==boot->root_lba && bot_u32(part+12)==boot->root_bytes/512)match=true;
    }
    if(!match || boot->root_lba>=d->storage_sectors || boot->root_bytes/512>d->storage_sectors-boot->root_lba)return true;
    if(root_sealed)return true;
    if(root_storage){root_ambiguous=true;return true;}
    root_storage=d;root_generation=d->generation;root_offset=boot->root_lba;
    kprintf("USB persistent root: slot %u, partition LBA %u, %u sectors\n",d->slot,root_offset,boot->root_bytes/512);
    return true;
}
u32 xhci_root_sectors(void) {
    root_sealed=true;
    const struct boot_info *b=boot_info_get();
    return b && root_storage && !root_ambiguous && !root_storage->failed &&
        root_storage->generation==root_generation?b->root_bytes/512:0;
}
int xhci_root_io(u32 lba,void *buf,u32 count,bool write) {
    mutex_lock(&usb_lock);int r=-1;u32 n=xhci_root_sectors();
    if(!n || !count || count>32 || lba>=n || count>n-lba)goto done;
    for(u32 i=0;i<count;i++) {
        u8 cdb[10]={0};cdb[0]=write?0x2a:0x28;bot_beput(cdb+2,root_offset+lba+i);cdb[8]=1;
        if(scsi(root_storage,cdb,10,(u8 *)buf+i*512,512,!write))goto done;
    }
    r=0;
done:mutex_unlock(&usb_lock);return r;
}
int xhci_root_flush(void) {
    mutex_lock(&usb_lock);u8 cdb[10]={0x35};
    int r=xhci_root_sectors()?scsi(root_storage,cdb,10,NULL,0,false):-1;
    mutex_unlock(&usb_lock);return r;
}
