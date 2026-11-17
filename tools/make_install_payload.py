"""Build the compact BIOS/UEFI installer payload; writes only a regular file."""
import argparse
import pathlib
import struct
import subprocess
import zlib
from make_usb_image import live_limits


def build(nasm='nasm', nm='nm', video_defines=(), uefi=True):
    directory = pathlib.Path('build/hardware')
    kernel = (directory / 'kernel.bin').read_bytes()
    symbols = subprocess.check_output([nm, '-n', str(directory / 'kernel.elf')], text=True)
    end = next(int(line.split()[0], 16) for line in symbols.splitlines() if line.endswith(' __bss_end'))
    root_phys, minimum, maximum, reserve = live_limits()
    if not kernel or end > root_phys:
        raise ValueError('Invalid hardware kernel layout')
    sectors = (len(kernel) + 511) // 512
    bios_sectors = (33 + sectors + 2047) // 2048 * 2048
    if uefi:
        from make_uefi import esp,ROOT_START,ESP_START,ESP_SECTORS
        if bios_sectors>ESP_START:raise ValueError('Kernel overlaps ESP')
        root_lba=ROOT_START
    else:root_lba=bios_sectors
    defines = ['-DINSTALLED_ROOT=1', '-DKERNEL_LBA=33', f'-DKERNEL_SECTORS={sectors}',
               f'-DKERNEL_END={end}', '-DROOT_DISK_ID=0', f'-DROOT_LBA={root_lba}',
               '-DROOT_SECTORS=0', f'-DROOT_PHYS={root_phys}',
               f'-DROOT_MIN_BYTES={minimum*1048576}', f'-DROOT_MAX_BYTES={maximum*1048576}',
               f'-DROOT_RESERVE_BYTES={reserve*1048576}', *video_defines]
    prefix='install' if uefi else 'install-bios'
    for source, output in [('hardware.asm', prefix+'-mbr.bin'), ('hardware-stage2.asm', prefix+'-stage2.bin')]:
        subprocess.run([nasm, '-f', 'bin', *defines, 'boot/' + source, '-o', str(directory/output)], check=True)
    mbr = (directory/(prefix+'-mbr.bin')).read_bytes()
    stage2 = (directory/(prefix+'-stage2.bin')).read_bytes()
    assert len(mbr) == 512 and len(stage2) == 32*512
    payload = bytearray(mbr + stage2 + kernel)
    payload.extend(bytes(bios_sectors*512 - len(payload)))
    if uefi:
        efi,config_lba=esp(kernel,end,nasm=nasm)
        payload.extend(efi)
        struct.pack_into('<I',payload,408,2)
        struct.pack_into('<IIII',payload,424,len(payload)//512,ESP_START,ESP_SECTORS,config_lba)
    struct.pack_into('<I', payload, 420, zlib.crc32(payload[512:]))
    output = directory/(prefix+'-payload.bin')
    output.write_bytes(payload)
    return output


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nasm', default='nasm')
    parser.add_argument('--nm', default='nm')
    args = parser.parse_args()
    print(build(args.nasm, args.nm))
