"""UEFI x64 -> i386 kernel, native root, GOP, ACPI, and persistent files."""
import pathlib
import argparse
import shutil
import struct
import sys
import tempfile
from disk_install_smoke import VM,run,uefi_firmware
from hardware_inventory_smoke import login
import hardware_smoke as hardware

B=pathlib.Path('build')
OVMF=pathlib.Path('/usr/share/OVMF')


def native_image(root,image,payload,disk_id=0x23456789):
    data=bytearray(payload)
    start=struct.unpack_from('<I',data,412)[0];sectors=root.stat().st_size//512
    struct.pack_into('<I',data,440,disk_id)
    struct.pack_into('<II',data,454,start,sectors)
    data[462:478]=struct.pack('<B3sB3sII',0,b'\xfe\xff\xff',0xef,b'\xfe\xff\xff',2048,131072)
    cfg=struct.unpack_from('<I',data,436)[0]*512
    struct.pack_into('<IIII',data,cfg+12,2,disk_id,start,sectors)
    with image.open('wb') as out,root.open('rb') as src:
        out.write(data);out.seek(start*512);shutil.copyfileobj(src,out)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--storage',choices=['nvme','ahci'],default='nvme')
    storage=parser.parse_args().storage
    sys.path.insert(0,str(pathlib.Path('tools').resolve()))
    from make_install_payload import build
    payload=build().read_bytes()
    with tempfile.TemporaryDirectory(prefix='myos-uefi-') as directory:
        temp=pathlib.Path(directory);root=temp/'root.img'
        with root.open('wb') as stream:stream.truncate(64*1048576)
        for d in ['/bin','/lib','/etc','/root','/home','/tmp']:
            run(B/'myfs',root,'mkdir',d)
        for name in ['sh','cat','stat','poweroff','sync']:
            run(B/'myfs',root,'put',B/'user'/name,'/bin/'+name)
        run(B/'myfs',root,'put',B/'ld.so','/lib/ld.so')
        run(B/'myfs',root,'put',B/'user/libc.so','/lib/libc.so')
        image=temp/'native.img';native_image(root,image,payload)
        for attempt in range(2):
            args=uefi_firmware(temp,'native-'+str(attempt))+[
                '-smp','4','-drive',f'if=none,id=root,format=raw,file={image}',
                '-device','nvme,drive=root,serial=UEFIROOT,bootindex=1' if storage=='nvme' else 'ide-hd,bus=ide.0,drive=root,bootindex=1']
            vm=VM('uefi-native-'+storage+'-'+str(attempt),args,memory='256M',binary='qemu-system-x86_64')
            try:
                vm.wait('Boot: UEFI x64, firmware services exited')
                if not attempt:login(vm)
                else:
                    vm.wait('Login [root]:');vm.send('','Password:');vm.send('hardware123','root:/')
                    vm.send('cat /root/uefi-save','uefi-persistent')
                text=hardware.clean(vm.log)
                assert 'UEFI GOP framebuffer' in text and ('NVMe disk:' if storage=='nvme' else 'AHCI disk:') in text,text[-6000:]
                if not attempt:
                    vm.send('echo uefi-persistent > /root/uefi-save','root:/')
                    ppm=B/('uefi-native-'+storage+'.ppm');png=ppm.with_suffix('.png')
                    vm.qmp('screendump',{'filename':str(ppm.resolve())})
                    hardware.save_png(ppm,png)
                vm.send('sync','root:/');vm.send('poweroff','Powering off...');vm.p.wait(timeout=25)
            finally:vm.stop()
        print('UEFI x64 '+storage+': GOP, native root, saved file, two boots and clean shutdown: PASS',flush=True)


if __name__=='__main__':main()
