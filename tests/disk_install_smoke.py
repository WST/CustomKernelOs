"""Install from Live USB, remove it, boot AHCI and verify persistent writes.
Only disposable regular disk-image files are passed to QEMU.
"""
import json
import argparse
import pathlib
import socket
import struct
import shutil
import subprocess
import sys
import tempfile
import time
import hardware_smoke as hardware
from hardware_inventory_smoke import live_image, login

B = pathlib.Path('build')
def uefi_firmware(temp,name):
    ovmf=pathlib.Path('/usr/share/OVMF');variables=temp/(name+'-vars.fd')
    shutil.copyfile(ovmf/'OVMF_VARS_4M.fd',variables)
    return ['-machine','q35','-fw_cfg','name=opt/ovmf/X-PciMmio64Mb,string=0',
            '-drive',f'if=pflash,format=raw,unit=0,file={ovmf}/OVMF_CODE_4M.fd,readonly=on',
            '-drive',f'if=pflash,format=raw,unit=1,file={variables}']


def run(*args):
    subprocess.run([str(a) for a in args], check=True, stdout=subprocess.DEVNULL)


class VM(hardware.VM):
    def __init__(self, name, args, memory='256M',binary='qemu-system-i386'):
        self.temp = tempfile.TemporaryDirectory(prefix='myos-setup-qmp-')
        self.log = pathlib.Path(self.temp.name)/'serial.log'
        self.log.write_text('')
        self.log_artifact = B/(name+'.log')
        self.sockpath = self.temp.name+'/qmp'
        self.boot_timeout = 300
        self.err = (B/(name+'.stderr')).open('w')
        self.p = subprocess.Popen([binary, '-cpu', 'max', '-m', memory,
            '-display', 'none', '-no-reboot', '-nic', 'none',
            '-serial', 'file:'+str(self.log), '-qmp', 'unix:'+self.sockpath+',server=on,wait=off',
            *args], stdout=subprocess.DEVNULL, stderr=self.err)
        self.s = socket.socket(socket.AF_UNIX)
        self.s.settimeout(5)
        deadline = time.monotonic()+10
        while not pathlib.Path(self.sockpath).exists():
            if self.p.poll() is not None:
                self.err.close()
                raise AssertionError((B/(name+'.stderr')).read_text())
            if time.monotonic()>deadline: raise AssertionError('QMP socket missing')
            time.sleep(.05)
        self.s.connect(self.sockpath)
        self.f = self.s.makefile('rb')
        self.read_response(greeting=True)
        self.qmp('qmp_capabilities')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--storage',choices=['ahci','nvme'],default='ahci')
    parser.add_argument('--firmware',choices=['bios','uefi'],default='bios')
    args=parser.parse_args()
    nvme=args.storage=='nvme'
    uefi=args.firmware=='uefi'
    binary='qemu-system-x86_64' if uefi else 'qemu-system-i386'
    device='/dev/nvme0' if nvme else '/dev/sdc'
    label=('uefi-' if uefi else '')+('nvme-install' if nvme else 'disk-install')
    sys.path.insert(0, str(pathlib.Path('tools').resolve()))
    from make_install_payload import build
    payload = build()
    with tempfile.TemporaryDirectory(prefix='myos-setup-') as directory:
        temp = pathlib.Path(directory)
        root = temp/'root.img'
        with root.open('wb') as stream: stream.truncate(64*1048576)
        for d in ['/bin', '/lib', '/etc', '/root', '/home', '/tmp', '/mnt', '/boot', '/boot/install']:
            run(B/'myfs',root,'mkdir',d)
        for name in ['sh','setup','cat','mkdir','sync','stat','poweroff','installercheck']:
            run(B/'myfs',root,'put',B/'user'/name,'/bin/'+name)
        run(B/'myfs',root,'put',B/'ld.so','/lib/ld.so')
        run(B/'myfs',root,'put',B/'user/libc.so','/lib/libc.so')
        run(B/'myfs',root,'put',payload,'/boot/install/payload.bin')
        fixture=temp/'fixture';fixture.write_bytes(bytes(range(256))*1024)
        run(B/'myfs',root,'put',fixture,'/root/binary')
        source=temp/'usb.img'
        live_image(root,source,temp,uefi=uefi)
        source_before=hardware.digest(source)
        disk=temp/'target.img'
        with disk.open('wb') as stream: stream.truncate(192*1048576)
        before=hardware.digest(disk)
        decoy=temp/'decoy.img'
        with decoy.open('wb') as stream:stream.truncate(80*1048576)
        decoy_before=hardware.digest(decoy)
        if nvme:
            storage=([] if uefi else ['-device','ich9-ahci,id=ahci'])+['-drive',f'if=none,id=sata,format=raw,file={decoy}',
                     '-device','ide-hd,bus='+('ide.0' if uefi else 'ahci.0')+',drive=sata',
                     '-drive',f'if=none,id=target,format=raw,file={disk}',
                     '-device','nvme,drive=target,serial=MYOSNVME,bootindex=2']
        else:
            storage=([] if uefi else ['-device','ich9-ahci,id=ahci'])+['-drive',f'if=none,id=target,format=raw,file={disk}',
                     '-device','ide-hd,bus='+('ide.0' if uefi else 'ahci.0')+',drive=target,bootindex=2']
        vm=VM(label+'-live',(uefi_firmware(temp,'live') if uefi else [])+[
            '-device','qemu-xhci,id=xhci',
            '-drive',f'if=none,id=stick,format=raw,file={source},snapshot=on',
            '-device','usb-storage,bus=xhci.0,drive=stick,bootindex=1',*storage],binary=binary)
        try:
            login(vm)
            vm.send('setup --list',device)
            vm.send('setup '+device,"Type exactly 'erase "+device+"'")
            vm.send('cancel','Disk unchanged.')
            assert hardware.digest(disk)==before, 'Cancelled install wrote target'
            vm.send('installercheck create','INSTALL FIXTURE READY')
            vm.send('setup '+device,"Type exactly 'erase "+device+"'")
            vm.send('erase '+device,'Installation complete:')
            assert hardware.digest(source)==source_before, 'Live source image changed'
            vm.send('poweroff','Powering off...')
            vm.p.wait(timeout=25)
        finally:
            vm.stop()
        assert hardware.digest(decoy)==decoy_before,'Unselected SATA disk changed'
        with disk.open('rb') as stream: header=stream.read(512)
        assert header[400:408]==b'MYOSINST' and header[510:]==b'\x55\xaa'
        start,size=struct.unpack_from('<II',header,454)
        expected_start=struct.unpack_from('<I',payload.read_bytes(),412)[0]
        assert start==expected_start and size==(192*2048-expected_start)
        partition=temp/'installed-root.img'
        with disk.open('rb') as src,partition.open('wb') as dst:
            src.seek(start*512);dst.write(src.read(size*512))
        extracted=temp/'copied-binary'
        run(B/'myfs',partition,'get','/root/binary',extracted)
        assert extracted.read_bytes()==fixture.read_bytes()
        shutil.copyfile(disk,B/(label+'-target.img'))
        print('Live USB: cancellation unchanged, copy, checked boot commit: PASS',flush=True)
        # Native root must work with less RAM than the size of the partition.
        for attempt in range(2):
            # Port 0 is a different disk: boot root must be selected by disk ID.
            with decoy.open('wb') as stream: stream.truncate(64*1048576)
            if nvme:
                storage=['-drive',f'if=none,id=decoy,format=raw,file={decoy}',
                         '-device','nvme,drive=decoy,serial=OTHER',
                         '-drive',f'if=none,id=root,format=raw,file={disk}',
                         '-device','nvme,drive=root,serial=ROOT,bootindex=1']
            else:
                storage=([] if uefi else ['-device','ich9-ahci,id=ahci'])+[
                         '-drive',f'if=none,id=decoy,format=raw,file={decoy}',
                         '-device','ide-hd,bus='+('ide.0' if uefi else 'ahci.0')+',drive=decoy',
                         '-drive',f'if=none,id=root,format=raw,file={disk}',
                         '-device','ide-hd,bus='+('ide.1' if uefi else 'ahci.1')+',drive=root,bootindex=1']
            vm=VM(label+'-boot-'+str(attempt),(uefi_firmware(temp,'boot-'+str(attempt)) if uefi else [])+storage,memory='256M' if uefi else '128M',binary=binary)
            try:
                vm.wait('INSTALLED MODE: native disk root')
                vm.wait('Login [root]:');vm.send('','Password:');vm.send('hardware123','root:/')
                if attempt==0:
                    vm.send('echo installed-persistent > /root/saved','root:/')
                    vm.send('sync','root:/')
                    vm.send('poweroff','Powering off...')
                    vm.p.wait(timeout=25)
                else:
                    vm.send('cat /root/saved','installed-persistent')
                    vm.send('installercheck','INSTALL METADATA PASS')
            finally:
                vm.stop()
        print(args.firmware+' '+args.storage+': disk-only boot, controller/port reorder, durable save/reboot: PASS',flush=True)


if __name__=='__main__': main()
