"""Build a BIOS/CSM USB-HDD image without modifying disk.img or any device."""
import argparse
import pathlib
import subprocess
import hashlib
import json
import re
import shutil

def live_limits():
    header=pathlib.Path('kernel/bootinfo.h').read_text()
    def value(name):
        match=re.search(r'^#define '+name+r' (0x[0-9a-fA-F]+|[0-9]+)u?$',header,re.M)
        if not match: raise SystemExit('Missing live-root contract constant: '+name)
        return int(match.group(1),0)
    return tuple(value(name) for name in ('LIVE_ROOT_PHYS','LIVE_ROOT_MIN_MB','LIVE_ROOT_MAX_MB','LIVE_ROOT_RESERVE_MB'))

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    root_phys,root_min,root_max,reserve_mb=live_limits()
    ap.add_argument('--root-mb',type=int,default=256,help=f'RAM filesystem size in MiB ({root_min}..{root_max}, default: 256)')
    ap.add_argument('--nasm',default='nasm')
    ap.add_argument('--nm',default='nm')
    ap.add_argument('--video',default='auto',help='auto (EDID preferred) or WIDTHxHEIGHT')
    args=ap.parse_args()
    if not root_min<=args.root_mb<=root_max:
        ap.error(f'--root-mb must be between {root_min} and {root_max} MiB')
    video_defines=[]
    if args.video != 'auto':
        match=re.fullmatch(r'([0-9]+)x([0-9]+)',args.video)
        if not match:
            ap.error('--video must be auto or WIDTHxHEIGHT')
        width,height=map(int,match.groups())
        if not (640<=width<=2560 and 480<=height<=1600):
            ap.error('--video exceeds framebuffer limits (640..2560 x 480..1600)')
        video_defines=[f'-DVIDEO_WIDTH={width}',f'-DVIDEO_HEIGHT={height}']
    b=pathlib.Path('build/hardware'); b.mkdir(parents=True,exist_ok=True)
    kernel=(b/'kernel.bin').read_bytes()
    if not kernel or len(kernel)>root_phys-0x100000:
        raise SystemExit('Kernel is empty or overlaps the live root')
    symbols=subprocess.check_output([args.nm,'-n',str(b/'kernel.elf')],text=True)
    end=next(int(line.split()[0],16) for line in symbols.splitlines() if line.endswith(' __bss_end'))
    if end>root_phys:
        raise SystemExit('Kernel BSS overlaps the live root')
    live=b/'live-root.img'
    # Start a fresh, volatile filesystem, independent of the developer disk.
    with live.open('wb') as f:
        f.truncate(args.root_mb * 1024 * 1024)
    # Install onto the copy, leaving the user's original filesystem untouched.
    subprocess.run(['make','install',f'DISK={live}'],check=True)
    size=live.stat().st_size
    if size!=args.root_mb*1024*1024:
        raise SystemExit('Live root size changed during installation')
    # First boot gets its own account setup, independent of the developer image.
    subprocess.run(['build/myfs',str(live),'rm','/etc/accounts'],check=True,
                   stdout=subprocess.DEVNULL)
    # The developer disk's display preference must not override the USB menu.
    subprocess.run(['build/myfs',str(live),'rm','/etc/display'],check=True,
                   stdout=subprocess.DEVNULL)
    # Do not inherit an enabled graphical login that would own the console.
    subprocess.run(['build/myfs',str(live),'rm','/etc/services/enabled/desktop.service'],
                   check=True,stdout=subprocess.DEVNULL)
    sectors=(len(kernel)+511)//512
    root_lba=(33+sectors+2047)//2048*2048
    defines=[f'-DKERNEL_LBA=33',f'-DKERNEL_SECTORS={sectors}',f'-DKERNEL_END={end}',
             f'-DROOT_LBA={root_lba}',f'-DROOT_SECTORS={size//512}',
             f'-DROOT_PHYS={root_phys}',f'-DROOT_MIN_BYTES={root_min*1048576}',
             f'-DROOT_MAX_BYTES={root_max*1048576}',f'-DROOT_RESERVE_BYTES={reserve_mb*1048576}',*video_defines]
    for src,out in [('hardware.asm','mbr.bin'),('hardware-stage2.asm','stage2.bin')]:
        subprocess.run([args.nasm,'-f','bin',*defines,'boot/'+src,'-o',str(b/out)],check=True)
    output=pathlib.Path('build/myos-usb.img')
    with output.open('wb') as f:
        f.write((b/'mbr.bin').read_bytes()); f.write((b/'stage2.bin').read_bytes())
        f.write(kernel); f.seek(root_lba*512)
        with live.open('rb') as source: shutil.copyfileobj(source,f,1024*1024)
        total=(f.tell()+1048575)//1048576*1048576
        f.truncate(total)
    digest=hashlib.sha256()
    with output.open('rb') as source:
        for chunk in iter(lambda: source.read(1024*1024),b''): digest.update(chunk)
    manifest={'image':str(output),'bytes':output.stat().st_size,
              'sha256':digest.hexdigest(),
              'boot':'BIOS/CSM only','filesystem':'volatile RAM copy','video_request':args.video,
              'kernel_lba':33,'root_lba':root_lba,'root_bytes':size,'root_phys':root_phys,
              'root_mb':args.root_mb,'ram_reserve_mb':reserve_mb,
              'minimum_usable_end':root_phys+size+reserve_mb*1048576}
    (b/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps(manifest,indent=2))

if __name__=='__main__': main()
