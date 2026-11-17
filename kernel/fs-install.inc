/* Explicit, privileged installation into an unmounted AHCI/NVMe whole disk.
 * No generic raw-write syscall is exposed. Boot code is committed last. */
#include "../user/lib/installer.h"
#ifndef HOST_TEST
#include "ahci.h"
#include "nvme.h"
#endif
static struct {
    struct disk_install_info info;
    u32 mbr_crc,payload_crc,payload_ino;
    u32 payload_sectors,esp_start,esp_sectors,config_lba;
    int device;
} installation;
static u32 install_crc(u32 crc,const void *data,u32 n) {
    const u8 *p=data;
    while(n--){crc^=*p++;for(int i=0;i<8;i++)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1)));}
    return crc;
}
static int install_payload(u8 header[512],u32 *ino,u32 *checksum) {
    struct fs_stat st;int r=resolve_impl(FS_ROOT,INSTALL_PAYLOAD,ino,true);if(r)return r;
    if((r=stat_impl(*ino,&st)))return r;
    if(st.type!=FS_TYPE_FILE || st.uid || (st.mode&0022) || st.size<1048576 || st.size>16u*1048576)return FS_EINVAL;
    if(fs_read(*ino,0,header,512)!=512 || memcmp(header+400,"MYOSINST",8) ||
       (mbr_u32(header+408)!=1 && mbr_u32(header+408)!=2) || header[510]!=0x55 || header[511]!=0xaa)return FS_EINVAL;
    u32 start=mbr_u32(header+412),kernel=mbr_u32(header+416);
    if(mbr_u32(header+408)==1) {
        if(start<2048 || start>32768 || (start&2047) || st.size!=start*512 || !kernel || kernel>start-33)return FS_EINVAL;
    } else {
        u32 count=mbr_u32(header+424),cfg=mbr_u32(header+436);
        if(start!=133120 || count<2048 || count>32768 || st.size!=count*512 ||
           mbr_u32(header+428)!=2048 || mbr_u32(header+432)!=131072 ||
           cfg<2048 || cfg>=count || !kernel || kernel>2048-33)return FS_EINVAL;
        u8 config[512];
        if(fs_read(*ino,cfg*512,config,512)!=512 || memcmp(config,"MYOSEFI1",8) ||
           mbr_u32(config+8)!=1 || mbr_u32(config+20)!=start)return FS_EINVAL;
    }
    u32 crc=~0u;u8 sector[512];
    for(u32 off=512;off<st.size;off+=512){if(fs_read(*ino,off,sector,512)!=512)return FS_EIO;crc=install_crc(crc,sector,512);}
    *checksum=~crc;
    return *checksum==mbr_u32(header+420)?0:FS_EIO;
}
static int install_target(unsigned index,int *device) {
    if(index>=BLOCK_UNITS-1)return FS_ENODEV;
    *device=(int)(index+1)*5;
    if(!block_get(*device))return FS_ENODEV;
#ifndef HOST_TEST
    if(!ahci_present(index+1) && !nvme_present(index+1))return FS_ENODEV;
#endif
    return device_busy(*device,true)?FS_EBUSY:0;
}
static void install_partition(u8 *mbr,const struct disk_install_info *info,bool efi) {
    memset(mbr+446,0,64);
    mbr_put(mbr+440,info->token);
    u8 *entry=mbr+446;entry[0]=0x80;entry[1]=entry[5]=0xfe;
    entry[2]=entry[3]=entry[6]=entry[7]=0xff;entry[4]=0x7f;
    mbr_put(entry+8,info->root_start);mbr_put(entry+12,info->root_sectors);
    if(efi && installation.esp_start) {
        entry+=16;entry[1]=entry[5]=0xfe;entry[2]=entry[3]=entry[6]=entry[7]=0xff;entry[4]=0xef;
        mbr_put(entry+8,installation.esp_start);mbr_put(entry+12,installation.esp_sectors);
    }
    mbr[510]=0x55;mbr[511]=0xaa;
}
/* The account database is never returned through a userspace read buffer. */
static int install_accounts(int device) {
    u32 source;struct fs_stat st;int r=resolve_impl(FS_ROOT,"/etc/accounts",&source,false);
    if(r==FS_ENOENT)return 0;
    if(r || (r=stat_impl(source,&st)))return r;
    if(st.type!=FS_TYPE_FILE || st.size>65536)return FS_EINVAL;
    myfs_select(4,device);u32 dir,dest;
    if((r=myfs_resolve(FS_ROOT,"/etc",&dir)))return r;
    r=myfs_create(dir,"accounts",FS_TYPE_FILE,&dest);
    if(r==FS_EEXIST) {
        struct fs_stat existing;
        r=myfs_resolve(dir,"accounts",&dest);
        if(!r)r=myfs_stat(dest,&existing);
        if(!r && existing.type!=FS_TYPE_FILE)r=FS_EINVAL;
        if(!r)r=myfs_truncate(dest);
    }
    if(r)return r;
    if((r=myfs_setattr(dest,0600,0,0)))return r;
    u8 data[512];
    for(u32 offset=0;offset<st.size;offset+=512) {
        u32 n=st.size-offset;if(n>512)n=512;
        if(fs_read(source,offset,data,n)!=(int)n){r=FS_EIO;break;}
        myfs_select(4,device);
        if(myfs_write(dest,offset,data,n)!=(int)n){r=FS_EIO;break;}
    }
    memset(data,0,sizeof(data));myfs_select(4,device);
    if(!r)r=myfs_set_time(dest,st.mtime);
    return r;
}
int fs_disk_install(unsigned op,unsigned index,struct disk_install_info *info) {
    mutex_lock(&lock);int r=0,d=0;u8 sector[512];
    if(op==INSTALL_PROBE) {
        if(installation.info.state==INSTALL_COPYING){r=FS_EBUSY;goto done;}
#ifndef HOST_TEST
        int sata=ahci_installer_probe(),nvme=nvme_installer_probe();
        if(sata && nvme){r=FS_ENODEV;goto done;}
#endif
        for(int disk=1;disk<BLOCK_UNITS;disk++)block_rescan(disk);
        goto done;
    }
    if(op>INSTALL_ABORT){r=FS_EINVAL;goto done;}
    if(op==INSTALL_ABORT) {
        if(installation.device!=(int)(index+1)*5 || info->token!=installation.info.token){r=FS_EINVAL;goto done;}
        memset(&installation,0,sizeof(installation));goto done;
    }
    if((r=install_target(index,&d)))goto done;
    if(op==INSTALL_LIST) {
        memset(info,0,sizeof(*info));strcpy(info->device,"/dev/");strcpy(info->device+5,block_get(d)->name);
#ifndef HOST_TEST
        strcpy(info->model,nvme_present(index+1)?nvme_model(index+1):ahci_unit_model(index+1));
#endif
        info->sectors=block_get(d)->sectors;goto done;
    }
    if(op==INSTALL_PLAN) {
        if(installation.info.state==INSTALL_COPYING){r=FS_EBUSY;goto done;}
        struct disk_install_info target;memset(&target,0,sizeof(target));
        if((r=fs_disk_install(INSTALL_LIST,index,&target)))goto done;
        if(block_read(d,0,sector)){r=FS_EIO;goto done;}
        /* GPT/extended layouts are outside this BIOS/MBR installer's contract. */
        if(sector[510]==0x55 && sector[511]==0xaa)for(int i=0;i<4;i++) {
            u8 type=sector[446+i*16+4];if(type==0xee || type==5 || type==0x0f || type==0x85){r=FS_EINVAL;goto done;}
        }
        installation.mbr_crc=~install_crc(~0u,sector,512);
        if((r=install_payload(sector,&installation.payload_ino,&installation.payload_crc)))goto done;
        bool efi=mbr_u32(sector+408)==2;
        installation.payload_sectors=efi?mbr_u32(sector+424):mbr_u32(sector+412);
        installation.esp_start=efi?mbr_u32(sector+428):0;
        installation.esp_sectors=efi?mbr_u32(sector+432):0;
        installation.config_lba=efi?mbr_u32(sector+436):0;
        target.root_start=mbr_u32(sector+412);
        if(target.sectors<=target.root_start){r=FS_ENOSPC;goto done;}
        target.root_sectors=target.sectors-target.root_start;
        if(target.root_sectors>FS_MAX_BLOCKS)target.root_sectors=FS_MAX_BLOCKS;
        if(target.root_sectors<64u*2048u){r=FS_ENOSPC;goto done;}
        target.token=rng_next();if(!target.token)target.token=1;
        target.state=INSTALL_PLANNED;installation.info=target;installation.device=d;*info=target;goto done;
    }
    if(installation.device!=d || !info->token || info->token!=installation.info.token){r=FS_EINVAL;goto done;}
    if(op==INSTALL_PREPARE) {
        if(installation.info.state!=INSTALL_PLANNED){r=FS_EINVAL;goto done;}
        if((r=fs_sync()))goto done;
        if(block_read(d,0,sector) || ~install_crc(~0u,sector,512)!=installation.mbr_crc){r=FS_EBUSY;goto done;}
        memset(sector,0,512);sector[0]=0xfa;sector[1]=0xf4;sector[2]=0xeb;sector[3]=0xfd;
        install_partition(sector,&installation.info,false);
        if(block_write_through(d,0,sector) || block_flush_device(d)){r=FS_EIO;goto done;}
        installation.info.state=INSTALL_COPYING;
        if(block_rescan(d/5)){r=FS_EIO;goto done;}
        myfs_select(4,d+1);r=myfs_format();if(!r)r=myfs_unmount();
        *info=installation.info;goto done;
    }
    if(op==INSTALL_COMMIT) {
        if(installation.info.state!=INSTALL_COPYING){r=FS_EINVAL;goto done;}
        u32 ino,crc;u8 mbr[512];
        if((r=install_payload(mbr,&ino,&crc)))goto done;
        if(ino!=installation.payload_ino || crc!=installation.payload_crc){r=FS_EBUSY;goto done;}
        if(block_flush_device(d) || block_cache_invalidate_disk(d/5)){r=FS_EIO;goto done;}
        myfs_select(4,d+1);bool formatted;r=myfs_init(&formatted,false);if(r)goto done;
        const char *required[]={"/bin/sh","/lib/ld.so","/lib/libc.so"};
        for(unsigned n=0;n<3 && !r;n++) {
            u32 file;struct fs_stat st;u8 magic[4];
            r=myfs_resolve(FS_ROOT,required[n],&file);
            if(!r)r=myfs_stat(file,&st);
            if(!r && (st.type!=FS_TYPE_FILE || st.size<52 || myfs_read(file,0,magic,4)!=4 || memcmp(magic,"\177ELF",4)))r=FS_EINVAL;
        }
        if(!r)r=install_accounts(d+1);
        myfs_select(4,d+1);
        int unmount=myfs_unmount();if(!r)r=unmount;if(r)goto done;
        if((r=fs_sync()))goto done;
        u32 expected=~0u;
        for(u32 block=1;block<installation.payload_sectors;block++) {
            if(fs_read(ino,block*512,sector,512)!=512){r=FS_EIO;goto done;}
            if(block==installation.config_lba) {
                mbr_put(sector+12,2);mbr_put(sector+16,installation.info.token);
                mbr_put(sector+20,installation.info.root_start);mbr_put(sector+24,installation.info.root_sectors);
            }
            expected=install_crc(expected,sector,512);
            if(block_write(d,block,sector)){r=FS_EIO;goto done;}
        }
        if(block_flush_device(d) || block_cache_invalidate_disk(d/5)){r=FS_EIO;goto done;}
        crc=~0u;
        for(u32 block=1;block<installation.payload_sectors;block++) {
            if(block_read(d,block,sector)){r=FS_EIO;goto done;}
            crc=install_crc(crc,sector,512);
        }
        if(crc!=expected){r=FS_EIO;goto done;}
        mbr_put(mbr+420,~expected);
        install_partition(mbr,&installation.info,true);
        /* Durable data + checked loader first; enabling BIOS boot is the last write. */
        if(block_write_through(d,0,mbr) || block_flush_device(d) || block_cache_invalidate_disk(d/5) || block_read(d,0,sector) || memcmp(mbr,sector,512)){r=FS_EIO;goto done;}
        installation.info.state=INSTALL_DONE;*info=installation.info;goto done;
    }
    r=FS_EINVAL;
done:mutex_unlock(&lock);return r;
}
