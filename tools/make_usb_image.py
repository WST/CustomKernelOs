"""Build a BIOS/CSM USB-HDD image without modifying disk.img or any device."""
import argparse
import pathlib
import shutil
import subprocess
import hashlib
import json

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--nasm',default='nasm')
    ap.add_argument('--nm',default='nm')
    ap.add_argument('--video',default='auto',help='auto (EDID preferred) or WIDTHxHEIGHT')
    args=ap.parse_args()
    video_defines=[]
    if args.video != 'auto':
        import re
        match=re.fullmatch(r'([0-9]+)x([0-9]+)',args.video)
        if not match:
            ap.error('--video must be auto or WIDTHxHEIGHT')
        width,height=map(int,match.groups())
        if not (640<=width<=2560 and 480<=height<=1600):
            ap.error('--video exceeds framebuffer limits (640..2560 x 480..1600)')
        video_defines=[f'-DVIDEO_WIDTH={width}',f'-DVIDEO_HEIGHT={height}']
    b=pathlib.Path('build/hardware'); b.mkdir(parents=True,exist_ok=True)
    kernel=(b/'kernel.bin').read_bytes()
    if not kernel or len(kernel)>15*1024*1024:
        raise SystemExit('Kernel is empty or overlaps the live root')
    live=b/'live-root.img'
    if not pathlib.Path('disk.img').is_file():
        raise SystemExit('disk.img is missing: run make first')
    shutil.copyfile('disk.img',live)
    # Install onto the copy, leaving the user's original filesystem untouched.
    subprocess.run(['make','install',f'DISK={live}'],check=True)
    size=live.stat().st_size
    if not size or size%512 or size>64*1024*1024:
        raise SystemExit('Live root must be sector aligned and at most 64 MiB')
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
    symbols=subprocess.check_output([args.nm,'-n',str(b/'kernel.elf')],text=True)
    end=next(int(line.split()[0],16) for line in symbols.splitlines() if line.endswith(' __bss_end'))
    defines=[f'-DKERNEL_LBA=33',f'-DKERNEL_SECTORS={sectors}',f'-DKERNEL_END={end}',
             f'-DROOT_LBA={root_lba}',f'-DROOT_SECTORS={size//512}',*video_defines]
    for src,out in [('hardware.asm','mbr.bin'),('hardware-stage2.asm','stage2.bin')]:
        subprocess.run([args.nasm,'-f','bin',*defines,'boot/'+src,'-o',str(b/out)],check=True)
    output=pathlib.Path('build/myos-usb.img')
    with output.open('wb') as f:
        f.write((b/'mbr.bin').read_bytes()); f.write((b/'stage2.bin').read_bytes())
        f.write(kernel); f.seek(root_lba*512); f.write(live.read_bytes())
        total=(f.tell()+1048575)//1048576*1048576
        f.truncate(total)
    manifest={'image':str(output),'bytes':output.stat().st_size,
              'sha256':hashlib.sha256(output.read_bytes()).hexdigest(),
              'boot':'BIOS/CSM only','filesystem':'volatile RAM copy','video_request':args.video,
              'kernel_lba':33,'root_lba':root_lba,'root_bytes':size}
    (b/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps(manifest,indent=2))

if __name__=='__main__': main()
