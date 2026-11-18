"""Native roots: sparse NVMe NSIDs, SATA coexistence, 4Kn refusal, SMP shutdown.
All disks are temporary regular files. No installation on host devices.
"""
import pathlib
import shutil
import struct
import sys
import tempfile
from disk_install_smoke import VM,run
from hardware_inventory_smoke import login
import hardware_smoke as hardware

B=pathlib.Path('build')


def main():
    sys.path.insert(0,str(pathlib.Path('tools').resolve()))
    from make_install_payload import build
    payload=bytearray(build(uefi=False).read_bytes())
    with tempfile.TemporaryDirectory(prefix='myos-nvme-boot-') as directory:
        temp=pathlib.Path(directory);root=temp/'root.img'
        with root.open('wb') as stream:stream.truncate(64*1048576)
        for d in ['/bin','/lib','/etc','/root','/home','/tmp']:
            run(B/'myfs',root,'mkdir',d)
        for name in ['sh','cat','stat','poweroff','sync']:
            run(B/'myfs',root,'put',B/'user'/name,'/bin/'+name)
        run(B/'myfs',root,'put',B/'ld.so','/lib/ld.so')
        run(B/'myfs',root,'put',B/'user/libc.so','/lib/libc.so')
        struct.pack_into('<I',payload,440,0x34567812)
        struct.pack_into('<II',payload,454,len(payload)//512,root.stat().st_size//512)
        for kind in ['nvme','sata']:
            image=temp/(kind+'.img')
            with image.open('wb') as dst,root.open('rb') as src:
                dst.write(payload);shutil.copyfileobj(src,dst)
            decoy=temp/(kind+'-decoy.img')
            with decoy.open('wb') as stream:stream.truncate(64*1048576)
            before=hardware.digest(decoy)
            storage=['-smp','4','-drive',f'if=none,id=root,format=raw,file={image}',
                     '-drive',f'if=none,id=decoy,format=raw,file={decoy}',
                     '-debugcon',f'file:{B}/nvme-bios-{kind}.log','-global','isa-debugcon.iobase=0x402']
            if kind=='nvme':
                # SeaBIOS uses the PCI controller boot priority, not a namespace path.
                storage+=['-device','nvme,id=nvm,serial=SPARSE,bootindex=1',
                          '-device','nvme-ns,bus=nvm,drive=decoy,nsid=19',
                          '-device','nvme-ns,bus=nvm,drive=root,nsid=1']
            else:
                fourk=temp/'4kn.img'
                with fourk.open('wb') as stream:stream.truncate(64*1048576)
                storage+=['-device','ich9-ahci,id=sata',
                          '-device','ide-hd,bus=sata.0,drive=root,bootindex=1',
                          '-device','nvme,id=nvm,serial=SECONDARY',
                          '-device','nvme-ns,bus=nvm,drive=decoy,nsid=7',
                          '-drive',f'if=none,id=fourk,format=raw,file={fourk}',
                          '-device','nvme-ns,bus=nvm,drive=fourk,nsid=19,logical_block_size=4096,physical_block_size=4096']
            vm=VM('nvme-boot-'+kind,storage,memory='128M')
            try:
                login(vm)
                vm.send('stat /dev/nvme0','/dev/nvme0: mode=0600')
                text=hardware.clean(vm.log)
                if kind=='nvme':assert 'ns 1:' in text and 'ns 19:' in text and '(unit 0)' in text,text[-6000:]
                else:assert 'unsupported/empty LBA format' in text and 'AHCI disk:' in text,text[-6000:]
                assert '4 CPU(s)' in text,text[-6000:]
                vm.send('echo native-saved > /root/check','root:/')
                vm.send('sync','root:/')
                vm.send('poweroff','Powering off...');vm.p.wait(timeout=25)
            finally:vm.stop()
            assert hardware.digest(decoy)==before,'Unselected namespace was written'
            print(kind+': sparse NSIDs, secondary namespace, 4 CPUs, clean NVMe shutdown: PASS',flush=True)


if __name__=='__main__':main()
