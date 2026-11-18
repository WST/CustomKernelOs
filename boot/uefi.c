/* x64 UEFI application. Firmware services are never called by the i386 kernel.
 * Own all pages before ExitBootServices, retry its map key without doing I/O,
 * then enter the existing protected-mode kernel using private identity tables.
 */
#include <efi.h>
#include <efilib.h>
#include <string.h>
#include "uefi_trampoline.h"

typedef unsigned int u32;
typedef unsigned char u8;
struct boot_info {
    u32 magic,version,root_phys,root_bytes,font_phys,fb_phys,width,height,bpp,pitch;
    u32 bios_snapshot_magic,root_lba,root_disk_id,storage_flags,root_sectors;
    u32 firmware,rsdp,map_phys,map_count,arena_phys,arena_bytes,ap_safe;
};
struct config {
    char magic[8];u32 version,mode,disk_id,root_lba,root_sectors;
    u32 kernel_bytes,kernel_crc,kernel_end,width,height;
};
struct region {UINT64 base,length;u32 type,attributes;} __attribute__((packed));
_Static_assert(sizeof(struct boot_info)==88,"boot ABI");
_Static_assert(sizeof(struct region)==24,"memory map ABI");
static EFI_SYSTEM_TABLE *system;
static EFI_BOOT_SERVICES *services;
static struct {EFI_PHYSICAL_ADDRESS address;UINTN pages;} owned[8];
static unsigned owned_count;
static EFI_FILE *boot_root;
static void *map_buffer;
static EFI_GUID image_guid=LOADED_IMAGE_PROTOCOL,fs_guid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID block_guid=EFI_BLOCK_IO_PROTOCOL_GUID,path_guid=EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID gop_guid=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static EFI_GUID acpi2=ACPI_20_TABLE_GUID,acpi1=ACPI_TABLE_GUID;
static inline void outb(unsigned short p,u8 v){__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));}
static inline u8 inb(unsigned short p){u8 v;__asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p));return v;}
static int equal(const void *a,const void *b,UINTN n){const u8 *x=a,*y=b;while(n--)if(*x++!=*y++)return 0;return 1;}
static void message(const char *s) {
    CHAR16 wide[128];unsigned n=0;
    for(const char *p=s;*p;p++) {
        for(unsigned spin=0;spin<100000 && !(inb(0x3fd)&32);spin++);
        outb(0x3f8,*p);if(n<126)wide[n++]=(unsigned char)*p;
    }
    wide[n]=0;if(system && system->ConOut)system->ConOut->OutputString(system->ConOut,wide);
}
static u32 get32(const void *p){const u8 *b=p;return b[0]|(u32)b[1]<<8|(u32)b[2]<<16|(u32)b[3]<<24;}
static u32 crc32(const void *p,UINTN n){const u8 *b=p;u32 c=~0u;while(n--){c^=*b++;for(int i=0;i<8;i++)c=(c>>1)^(0xedb88320u&(0u-(c&1)));}return ~c;}
static EFI_STATUS allocate(UINTN pages,EFI_PHYSICAL_ADDRESS *address,BOOLEAN fixed) {
    if(owned_count==8)return EFI_OUT_OF_RESOURCES;
    EFI_STATUS r=services->AllocatePages(fixed?AllocateAddress:AllocateMaxAddress,EfiLoaderData,pages,address);
    if(!EFI_ERROR(r)){owned[owned_count].address=*address;owned[owned_count++].pages=pages;}
    return r;
}
static EFI_STATUS fail(EFI_STATUS error,const char *text) {
    message(text);
    if(boot_root){boot_root->Close(boot_root);boot_root=NULL;}
    if(map_buffer){services->FreePool(map_buffer);map_buffer=NULL;}
    while(owned_count){--owned_count;services->FreePages(owned[owned_count].address,owned[owned_count].pages);}
    return error;
}
static EFI_STATUS read_file(EFI_FILE *root,CHAR16 *path,void *buffer,UINTN size) {
    EFI_FILE *file;EFI_STATUS r=root->Open(root,&file,path,EFI_FILE_MODE_READ,0);
    if(EFI_ERROR(r))return r;
    UINTN n=size;r=file->Read(file,&n,buffer);file->Close(file);
    return EFI_ERROR(r)?r:n==size?EFI_SUCCESS:EFI_LOAD_ERROR;
}
static unsigned path_length(const u8 *p,BOOLEAN parent) {
    for(unsigned off=0;off<2048;) {
        unsigned length=p[off+2]|(unsigned)p[off+3]<<8;
        if(length<4 || length>2048-off)return 0;
        if(parent && p[off]==4 && p[off+1]==1)return off;
        if(p[off]==0x7f && p[off+1]==0xff)return parent?0:off;
        off+=length;
    }
    return 0;
}
static EFI_BLOCK_IO *boot_disk(EFI_HANDLE esp,const struct config *cfg,void *scratch) {
    u8 *esp_path;if(EFI_ERROR(services->HandleProtocol(esp,&path_guid,(void **)&esp_path)))return NULL;
    unsigned length=path_length(esp_path,TRUE);if(!length)return NULL;
    UINTN count=0;EFI_HANDLE *handles=NULL;
    if(EFI_ERROR(services->LocateHandleBuffer(ByProtocol,&block_guid,NULL,&count,&handles)))return NULL;
    EFI_BLOCK_IO *found=NULL;
    for(UINTN i=0;i<count;i++) {
        EFI_BLOCK_IO *io;u8 *path;
        if(EFI_ERROR(services->HandleProtocol(handles[i],&block_guid,(void **)&io)) ||
           !io->Media->MediaPresent || io->Media->LogicalPartition || io->Media->BlockSize!=512 ||
           io->Media->IoAlign>4096 || EFI_ERROR(services->HandleProtocol(handles[i],&path_guid,(void **)&path)))continue;
        if(path_length(path,FALSE)!=length || !equal(path,esp_path,length))continue;
        if(EFI_ERROR(io->ReadBlocks(io,io->Media->MediaId,0,512,scratch)))continue;
        const u8 *mbr=scratch;
        if(mbr[510]!=0x55 || mbr[511]!=0xaa || get32(mbr+440)!=cfg->disk_id ||
           get32(mbr+454)!=cfg->root_lba || get32(mbr+458)!=cfg->root_sectors ||
           (UINT64)cfg->root_lba+cfg->root_sectors-1>io->Media->LastBlock)continue;
        if(found){found=NULL;break;}found=io;
    }
    services->FreePool(handles);return found;
}
static BOOLEAN usable_mode(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *m) {
    return m->HorizontalResolution>=640 && m->HorizontalResolution<=2560 &&
           m->VerticalResolution>=480 && m->VerticalResolution<=1600 &&
           m->PixelsPerScanLine>=m->HorizontalResolution &&
           (m->PixelFormat==PixelBlueGreenRedReserved8BitPerColor ||
            (m->PixelFormat==PixelBitMask && m->PixelInformation.RedMask==0xff0000 &&
             m->PixelInformation.GreenMask==0xff00 && m->PixelInformation.BlueMask==0xff));
}
static EFI_STATUS graphics(struct boot_info *boot,const struct config *cfg) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_STATUS r=services->LocateProtocol(&gop_guid,NULL,(void **)&gop);if(EFI_ERROR(r))return r;
    unsigned choice=~0u,score=0;
    for(unsigned mode=0;mode<gop->Mode->MaxMode;mode++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;UINTN size;
        if(EFI_ERROR(gop->QueryMode(gop,mode,&size,&info)))continue;
        if(usable_mode(info)) {
            unsigned rank=info->HorizontalResolution*info->VerticalResolution;
            if(!cfg->width && mode==gop->Mode->Mode)rank+=5000000;
            if(cfg->width && info->HorizontalResolution==cfg->width && info->VerticalResolution==cfg->height)rank+=10000000;
            if(rank>score){choice=mode;score=rank;}
        }
        services->FreePool(info);
    }
    if(choice==~0u || EFI_ERROR(gop->SetMode(gop,choice)))return EFI_UNSUPPORTED;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *m=gop->Mode->Info;
    UINT64 bytes=(UINT64)m->PixelsPerScanLine*4*m->VerticalResolution;
    if(!usable_mode(m) || !gop->Mode->FrameBufferBase || (gop->Mode->FrameBufferBase&4095) ||
       gop->Mode->FrameBufferBase+bytes>0x100000000ULL || bytes>16u*1024*1024 ||
       bytes>gop->Mode->FrameBufferSize)return EFI_UNSUPPORTED;
    boot->fb_phys=gop->Mode->FrameBufferBase;boot->width=m->HorizontalResolution;
    boot->height=m->VerticalResolution;boot->bpp=32;boot->pitch=m->PixelsPerScanLine*4;
    return EFI_SUCCESS;
}
static u32 memory_type(u32 t) {
    if(t==EfiConventionalMemory || t==EfiLoaderCode || t==EfiLoaderData || t==EfiBootServicesCode || t==EfiBootServicesData)return 1;
    return t==EfiACPIReclaimMemory?3:t==EfiACPIMemoryNVS?4:2;
}
static BOOLEAN convert_map(void *map,UINTN bytes,UINTN stride,struct boot_info *boot) {
    struct region *out=(void *)(UINTN)boot->map_phys;unsigned count=0;
    if(stride<sizeof(EFI_MEMORY_DESCRIPTOR) || bytes%stride)return FALSE;
    for(UINTN off=0;off<bytes;off+=stride) {
        EFI_MEMORY_DESCRIPTOR *m=(void *)((u8 *)map+off);
        UINT64 base=m->PhysicalStart;
        if(base>=0x40000000ULL || !m->NumberOfPages)continue;
        UINT64 size=m->NumberOfPages>0x100000?0x40000000ULL:m->NumberOfPages*4096;
        if(size>0x40000000ULL-base)size=0x40000000ULL-base;
        u32 type=memory_type(m->Type);
        if(count && out[count-1].base+out[count-1].length==base && out[count-1].type==type)out[count-1].length+=size;
        else {if(count==128)return FALSE;out[count++]=(struct region){base,size,type,1};}
    }
    boot->map_count=count;return count!=0;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *st) {
    system=st;services=st->BootServices;owned_count=0;boot_root=NULL;map_buffer=NULL;
    outb(0x3f9,0);outb(0x3fb,0x80);outb(0x3f8,1);outb(0x3f9,0);outb(0x3fb,3);outb(0x3fa,0xc7);outb(0x3fc,0x0b);
    message("MyOS UEFI x64 loader\r\n");
    UINT64 cr4;__asm__ volatile("mov %%cr4,%0":"=r"(cr4));
    if(cr4&(1u<<12))return fail(EFI_UNSUPPORTED,"UEFI: 5-level paging is unsupported\r\n");
    EFI_LOADED_IMAGE *loaded;EFI_FILE_IO_INTERFACE *fs;EFI_FILE *root;
    if(EFI_ERROR(services->HandleProtocol(image,&image_guid,(void **)&loaded)) ||
       EFI_ERROR(services->HandleProtocol(loaded->DeviceHandle,&fs_guid,(void **)&fs)) ||
       EFI_ERROR(fs->OpenVolume(fs,&root)))return fail(EFI_NOT_FOUND,"UEFI: cannot open boot ESP\r\n");
    boot_root=root;
    u8 config_sector[512];struct config *cfg=(void *)config_sector;
    if(EFI_ERROR(read_file(root,L"\\EFI\\MYOS\\BOOT.CFG",config_sector,512)) ||
       !equal(cfg->magic,"MYOSEFI1",8) || cfg->version!=1 || (cfg->mode!=1 && cfg->mode!=2) ||
       !cfg->disk_id || cfg->root_lba<2048 || cfg->root_sectors<131072 || cfg->root_sectors>16777216 ||
       !cfg->kernel_bytes || cfg->kernel_bytes>0x400000 || cfg->kernel_end<0x100000+cfg->kernel_bytes || cfg->kernel_end>0x1000000) {
        return fail(EFI_LOAD_ERROR,"UEFI: invalid boot configuration\r\n");
    }
    EFI_PHYSICAL_ADDRESS arena=0x3fffffff;
    if(EFI_ERROR(allocate(32,&arena,FALSE)))return fail(EFI_OUT_OF_RESOURCES,"UEFI: no bootstrap memory\r\n");
    memset((void *)(UINTN)arena,0,32*4096);
    struct boot_info *boot=(void *)(UINTN)arena;
    *boot=(struct boot_info){.magic=0x534f594d,.version=4,.font_phys=arena+4096,
        .root_lba=cfg->root_lba,.root_disk_id=cfg->disk_id,.firmware=1,
        .map_phys=arena+8192,.arena_phys=arena,.arena_bytes=32*4096};
    EFI_PHYSICAL_ADDRESS ap=0x7000;
    if(!EFI_ERROR(allocate(1,&ap,TRUE)))boot->ap_safe=1;
    EFI_PHYSICAL_ADDRESS kernel=0x100000;
    if(EFI_ERROR(allocate((cfg->kernel_end-kernel+4095)/4096,&kernel,TRUE)) ||
       EFI_ERROR(read_file(root,L"\\EFI\\MYOS\\KERNEL.BIN",(void *)(UINTN)kernel,cfg->kernel_bytes)) ||
       crc32((void *)(UINTN)kernel,cfg->kernel_bytes)!=cfg->kernel_crc ||
       EFI_ERROR(read_file(root,L"\\EFI\\MYOS\\FONT.BIN",(void *)(UINTN)boot->font_phys,4096))) {
        return fail(EFI_LOAD_ERROR,"UEFI: kernel/font load or allocation failed\r\n");
    }
    if(EFI_ERROR(graphics(boot,cfg)))return fail(EFI_UNSUPPORTED,"UEFI: supported 32-bit GOP mode unavailable\r\n");
    EFI_BLOCK_IO *disk=boot_disk(loaded->DeviceHandle,cfg,(void *)(UINTN)(arena+45056));
    if(!disk)return fail(EFI_NOT_FOUND,"UEFI: boot disk identity mismatch\r\n");
    if(cfg->mode==1) {
        if(cfg->root_sectors>1048576 || (cfg->root_sectors&7))return fail(EFI_LOAD_ERROR,"UEFI: invalid Live root size\r\n");
        EFI_PHYSICAL_ADDRESS ram=0x3fffffff;
        if(EFI_ERROR(allocate((UINTN)cfg->root_sectors/8,&ram,FALSE)))return fail(EFI_OUT_OF_RESOURCES,"UEFI: insufficient Live root RAM\r\n");
        for(u32 lba=0;lba<cfg->root_sectors;) {
            u32 n=cfg->root_sectors-lba;if(n>128)n=128;
            if(EFI_ERROR(disk->ReadBlocks(disk,disk->Media->MediaId,(UINT64)cfg->root_lba+lba,n*512,(void *)(UINTN)(ram+(UINT64)lba*512)))) {
                return fail(EFI_DEVICE_ERROR,"UEFI: Live filesystem read failed\r\n");
            }
            lba+=n;
        }
        boot->root_phys=ram;boot->root_bytes=cfg->root_sectors*512;
    } else {boot->storage_flags=1;boot->root_sectors=cfg->root_sectors;}
    root->Close(root);boot_root=NULL;
    /* ACPI comes from the UEFI configuration table, never a BIOS RAM scan. */
    EFI_CONFIGURATION_TABLE *table=st->ConfigurationTable;void *rsdp=NULL;
    for(UINTN i=0;i<st->NumberOfTableEntries;i++)if(equal(&table[i].VendorGuid,&acpi2,16))rsdp=table[i].VendorTable;
    if(!rsdp)for(UINTN i=0;i<st->NumberOfTableEntries;i++)if(equal(&table[i].VendorGuid,&acpi1,16))rsdp=table[i].VendorTable;
    if(rsdp && equal(rsdp,"RSD PTR ",8)) {
        u32 bytes=((u8 *)rsdp)[15]>=2?get32((u8 *)rsdp+20):20;
        if(bytes>=20 && bytes<=256){memcpy((u8 *)boot+512,rsdp,bytes);boot->rsdp=arena+512;}
    }
    u8 *trampoline=(void *)(UINTN)(arena+12288);
    memcpy(trampoline,uefi_trampoline,sizeof(uefi_trampoline));
    *(UINT64 *)(trampoline+8)=arena+16384;*(u32 *)(trampoline+16)=arena;
    *(u32 *)(trampoline+20)=arena+32*4096;
    *(UINT64 *)(trampoline+24)=arena+12288+get32(uefi_trampoline+sizeof(uefi_trampoline)-8);
    *(UINT64 *)(trampoline+50)=arena+12288+get32(uefi_trampoline+sizeof(uefi_trampoline)-4);
    UINT64 *pml4=(void *)(UINTN)(arena+16384),*pdpt=(void *)(UINTN)(arena+20480);
    pml4[0]=(arena+20480)|3;
    for(unsigned i=0;i<4;i++) {
        UINT64 *pd=(void *)(UINTN)(arena+24576+i*4096);pdpt[i]=(UINTN)pd|3;
        for(unsigned j=0;j<512;j++)pd[j]=((UINT64)i*512+j)*0x200000|0x83;
    }
    UINTN capacity=65536;void *map;
    if(EFI_ERROR(services->AllocatePool(EfiLoaderData,capacity,&map)))return fail(EFI_OUT_OF_RESOURCES,"UEFI: no memory map buffer\r\n");
    map_buffer=map;
    message("UEFI: leaving boot services, entering i386 kernel\r\n");
    for(unsigned attempt=0;attempt<4;attempt++) {
        UINTN size=capacity,key,stride;u32 version;
        EFI_STATUS r=services->GetMemoryMap(&size,map,&key,&stride,&version);
        if(EFI_ERROR(r) || !convert_map(map,size,stride,boot))break;
        if(!EFI_ERROR(services->ExitBootServices(image,key))) {
            __asm__ volatile("cli; jmp *%0"::"r"(trampoline):"memory");__builtin_unreachable();
        }
    }
    /* Firmware may have partially shut down. No console/BS calls after retry. */
    for(;;)__asm__ volatile("cli; hlt");
}
