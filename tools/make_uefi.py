"""Build a UEFI x64 loader and compact FAT32 ESP (no physical devices)."""
import pathlib
import struct
import subprocess
import zlib

DIRECTORY=pathlib.Path('build/uefi')
ESP_START=2048
ESP_SECTORS=131072
ROOT_START=ESP_START+ESP_SECTORS


def run(*args):
    subprocess.run([str(a) for a in args],check=True)


def loader(nasm='nasm'):
    import fcntl
    DIRECTORY.mkdir(parents=True,exist_ok=True)
    with (DIRECTORY/'.loader-lock').open('w') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX)
        return _loader(nasm)


def _loader(nasm):
    DIRECTORY.mkdir(parents=True,exist_ok=True)
    run(nasm,'-f','bin','boot/uefi-transition.asm','-o',DIRECTORY/'trampoline.bin')
    blob=(DIRECTORY/'trampoline.bin').read_bytes()
    (DIRECTORY/'uefi_trampoline.h').write_text('static const unsigned char uefi_trampoline[]={'+','.join(map(str,blob))+'};\n')
    run('gcc','-m64','-Os','-ffreestanding','-fno-builtin','-fpic','-fshort-wchar','-mno-red-zone',
        '-fno-stack-protector','-fno-strict-aliasing','-DGNU_EFI_USE_MS_ABI','-Wall','-Wextra',
        '-I/usr/include/efi','-I/usr/include/efi/x86_64','-I'+str(DIRECTORY),
        '-c','boot/uefi.c','-o',DIRECTORY/'loader.o')
    run('ld','-nostdlib','-znocombreloc','-T','/usr/lib/elf_x86_64_efi.lds','-shared','-Bsymbolic',
        '/usr/lib/crt0-efi-x86_64.o',DIRECTORY/'loader.o','-L/usr/lib','-lefi','-lgnuefi','-o',DIRECTORY/'loader.so')
    run('objcopy','-j','.text','-j','.sdata','-j','.data','-j','.rodata','-j','.dynamic','-j','.dynsym',
        '-j','.rel','-j','.rela','-j','.reloc','-O','pei-x86-64','--subsystem=10',DIRECTORY/'loader.so',DIRECTORY/'BOOTX64.EFI')
    return (DIRECTORY/'BOOTX64.EFI').read_bytes()


def font():
    from PIL import Image,ImageFont,ImageDraw
    glyphs=bytearray(4096)
    face=ImageFont.truetype('fonts/UbuntuMono-R.ttf',14)
    for c in range(32,256):
        character=bytes([c]).decode('cp437')
        image=Image.new('1',(8,16));draw=ImageDraw.Draw(image)
        draw.text((0,-1),character,font=face,fill=1)
        for y in range(16):
            glyphs[c*16+y]=sum((1<<(7-x)) for x in range(8) if image.getpixel((x,y)))
    return bytes(glyphs)


def esp(kernel,kernel_end,mode=2,disk_id=0,root_sectors=0,width=0,height=0,nasm='nasm'):
    """Return only allocated FAT sectors; the remaining free data area is sparse.
    All files use 8.3 names, one-sector clusters, and deterministic chains.
    """
    app=loader(nasm)
    config=bytearray(512)
    struct.pack_into('<8s10I',config,0,b'MYOSEFI1',1,mode,disk_id,ROOT_START,root_sectors,
                     len(kernel),zlib.crc32(kernel),kernel_end,width,height)
    files=[('BOOTX64 EFI',app),('KERNEL  BIN',kernel),('BOOT    CFG',bytes(config)),('FONT    BIN',font()),
           ('FONT    LIC',pathlib.Path('fonts/UFL.txt').read_bytes())]
    reserved=32;fat_sectors=1024;data_start=reserved+2*fat_sectors
    next_cluster=5;chains=[]
    for name,contents in files:
        count=(len(contents)+511)//512
        chains.append((name,contents,next_cluster,count));next_cluster+=count
    image=bytearray((data_start+next_cluster-2)*512)
    def entry(name,cluster,size,attributes=0x20):
        item=bytearray(32);item[:11]=name.encode('ascii');item[11]=attributes
        for offset in (16,18,24):struct.pack_into('<H',item,offset,0x21) # 1980-01-01
        struct.pack_into('<H',item,20,cluster>>16);struct.pack_into('<H',item,26,cluster&65535)
        struct.pack_into('<I',item,28,size);return item
    boot=bytearray(512);boot[:3]=b'\xeb\x58\x90';boot[3:11]=b'MYOSUEFI'
    struct.pack_into('<HBHBHHBHHHII',boot,11,512,1,reserved,2,0,0,0xf8,0,63,255,ESP_START,ESP_SECTORS)
    struct.pack_into('<IHHIHH',boot,36,fat_sectors,0,0,2,1,6)
    boot[64]=0x80;boot[66]=0x29;struct.pack_into('<I',boot,67,0x4d594f53)
    boot[71:82]=b'MYOS EFI   ';boot[82:90]=b'FAT32   ';boot[510:]=b'\x55\xaa'
    image[:512]=boot;image[6*512:7*512]=boot
    info=bytearray(512);struct.pack_into('<I',info,0,0x41615252)
    struct.pack_into('<III',info,484,0x61417272,ESP_SECTORS-data_start-(next_cluster-1),next_cluster+1)
    struct.pack_into('<I',info,508,0xaa550000)
    image[512:1024]=info;image[7*512:8*512]=info
    fat=bytearray(fat_sectors*512)
    for c,v in [(0,0x0ffffff8),(1,0xffffffff),(2,0x0fffffff),(3,0x0fffffff),(4,0x0fffffff)]:struct.pack_into('<I',fat,c*4,v)
    # Clusters 2/3/4 are root/EFI/BOOT; MYOS directory follows the file chains.
    myos_cluster=next_cluster;image.extend(bytes(512));struct.pack_into('<I',fat,myos_cluster*4,0x0fffffff)
    def cluster(c):return (data_start+c-2)*512
    image[cluster(2):cluster(2)+32]=entry('MYOS EFI   ',0,0,0x08)
    image[cluster(2)+32:cluster(2)+64]=entry('EFI        ',3,0,0x10)
    image[cluster(3):cluster(3)+32]=entry('.          ',3,0,0x10)
    image[cluster(3)+32:cluster(3)+64]=entry('..         ',0,0,0x10)
    image[cluster(3)+64:cluster(3)+96]=entry('BOOT       ',4,0,0x10)
    image[cluster(3)+96:cluster(3)+128]=entry('MYOS       ',myos_cluster,0,0x10)
    image[cluster(4):cluster(4)+32]=entry('.          ',4,0,0x10)
    image[cluster(4)+32:cluster(4)+64]=entry('..         ',3,0,0x10)
    image[cluster(myos_cluster):cluster(myos_cluster)+32]=entry('.          ',myos_cluster,0,0x10)
    image[cluster(myos_cluster)+32:cluster(myos_cluster)+64]=entry('..         ',3,0,0x10)
    config_lba=0
    for index,(name,contents,first,count) in enumerate(chains):
        for c in range(first,first+count):struct.pack_into('<I',fat,c*4,c+1 if c<first+count-1 else 0x0fffffff)
        start=cluster(first);image[start:start+len(contents)]=contents
        directory=4 if index==0 else myos_cluster
        slot=2 if index==0 else 1+index
        image[cluster(directory)+slot*32:cluster(directory)+(slot+1)*32]=entry(name,first,len(contents))
        if index==2:config_lba=ESP_START+start//512
    for copy in range(2):image[(reserved+copy*fat_sectors)*512:(reserved+(copy+1)*fat_sectors)*512]=fat
    assert len(image)%512==0 and len(image)<ESP_SECTORS*512
    return bytes(image),config_lba


if __name__=='__main__':
    loader()
