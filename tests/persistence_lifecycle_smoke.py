"""Boot AHCI with an fstab-mounted disposable disk; test real busy references."""
import json,pathlib,shutil,struct,subprocess,tempfile,time
import hardware_smoke as hw
ROOT=pathlib.Path(__file__).resolve().parents[1]
B=ROOT/'build'
def run(*args):subprocess.run([str(a) for a in args],cwd=ROOT,check=True,stdout=subprocess.DEVNULL)
def main():
    run('make','-j4','build/os.img','build/user/storage_lifecycle_check','build/myfs')
    with tempfile.TemporaryDirectory(prefix='myos-storage-lifecycle-') as directory:
        d=pathlib.Path(directory);root=d/'root.img';disk=d/'data.img';log=d/'serial';sock=d/'qmp'
        manifest=json.loads((B/'hardware/manifest.json').read_text())
        with (B/'myos-usb.img').open('rb') as source,root.open('wb') as target:
            source.seek(manifest['root_lba']*512);target.write(source.read(manifest['root_bytes']))
        run(B/'myfs',root,'put',B/'user/storage_lifecycle_check','/bin/storage-lifecycle')
        run(B/'myfs',root,'mkdir','/mnt')
        fstab=d/'fstab';fstab.write_text('# root data disk\n/dev/sdc1 /mnt myfs defaults 0 0\n')
        run(B/'myfs',root,'put',fstab,'/etc/fstab')
        mbr=bytearray(512);mbr[510:]=b'\x55\xaa'
        with disk.open('wb') as out:
            out.truncate(8*1048576)
            for index,start in enumerate((2048,6144)):
                part=d/f'part{index}.img'
                with part.open('wb') as f:f.truncate(2*1048576)
                run(B/'myfs',part,'ls','/')
                struct.pack_into('<B3sB3sII',mbr,446+index*16,0,b'\0'*3,0x83,b'\0'*3,start,4096)
                out.seek(start*512);out.write(part.read_bytes())
            out.seek(0);out.write(mbr)
        # Use the common QMP helper for keyboard input on a standard kernel boot.
        old=hw.IMAGE;hw.IMAGE=B/'os.img'
        vm=hw.VM('storage-lifecycle',usb=False,extra=[
            '-device','ich9-ahci,id=ahci',
            '-drive',f'if=none,id=root,format=raw,file={root}',
            '-device','ide-hd,drive=root,bus=ahci.0',
            '-drive',f'if=none,id=data,format=raw,file={disk}',
            '-device','ide-hd,drive=data,bus=ahci.1'])
        try:
            vm.wait('New password:');vm.send('storage123','Confirm password:');vm.send('storage123','Login [root]:')
            vm.send('','Password:');vm.send('storage123','root:/')
            vm.send('storage-lifecycle','STORAGE LIFECYCLE PASS:')
        finally:vm.stop();hw.IMAGE=old
    print('STORAGE LIFECYCLE QEMU PASS: fstab and safe umount of AHCI data partitions')
if __name__=='__main__':main()
