"""Test the selected live size: allocation safety and early low-RAM rejection."""
import hashlib,json,pathlib,shutil,subprocess,tempfile,time,struct
import hardware_smoke as hw

B=pathlib.Path('build')
def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda:f.read(1024*1024),b''):h.update(chunk)
    return h.hexdigest()

def main():
    manifest=json.loads((B/'hardware/manifest.json').read_text())
    original=hw.IMAGE;before=digest(original)
    # Invalid sizes must fail before opening or truncating any output image.
    for size in ('0','63','513','-1','garbage'):
        p=subprocess.run(['python3','tools/make_usb_image.py','--root-mb',size],capture_output=True,text=True)
        assert p.returncode!=0 and '--root-mb' in p.stderr,(size,p.stderr)
    assert digest(original)==before
    root_mb=manifest['root_bytes']//1048576
    offset=(96 if root_mb>128 else 48)*1048576
    with tempfile.TemporaryDirectory(prefix='myos-live-root-') as directory:
        d=pathlib.Path(directory);root=d/'root.img';image=d/'usb.img'
        shutil.copyfile(B/'hardware/live-root.img',root)
        subprocess.run(['build/myfs',str(root),'put',str(B/'user/live-rootcheck'),'/bin/live-rootcheck'],check=True,stdout=subprocess.DEVNULL)
        # Never overwrite an installed file if the base fixture grows later.
        with root.open('r+b') as f:
            magic,version,blocks,bitmap_start=struct.unpack('<4I',f.read(16))
            assert magic==0x5346594d and offset//512<blocks
            f.seek(bitmap_start*512+(offset//512)//8)
            assert not (f.read(1)[0] & (1 << ((offset//512)%8))), 'sentinel block is already allocated'
            f.seek(offset);f.write(b'MYOS-LIVE-GUARD!\0')
        shutil.copyfile(original,image)
        with image.open('r+b') as out,root.open('rb') as source:
            out.seek(manifest['root_lba']*512);shutil.copyfileobj(source,out,1048576)
        hw.IMAGE=image
        vm=hw.VM(f'live-root-{root_mb}')
        try:
            vm.wait('New password:',timeout=max(90,root_mb//2))
            text=hw.clean(vm.log)
            assert f"bytes={manifest['root_bytes']}" in text,text[-3000:]
            vm.send('live123','Confirm password:');vm.send('live123','Login [root]:')
            vm.send('','Password:');vm.send('live123','root:/')
            vm.send(f'live-rootcheck {offset}','LIVE ROOT PASS:')
            assert 'PANIC' not in hw.clean(vm.log) and 'LIVE ROOT FAIL' not in hw.clean(vm.log)
        except Exception:
            dump=d/'failed-vga.bin'
            vm.qmp('human-monitor-command',{'command-line':f'pmemsave 0xb8000 4000 "{dump}"'})
            print('Boot VGA:',dump.read_bytes()[::2].decode('ascii',errors='replace'),flush=True)
            raise
        finally:vm.stop()
        print(f'Live root {root_mb} MiB: boot, high-RAM reservation and writes PASS',flush=True)
        # Enough RAM for the root itself, but less than the required working reserve.
        vm=hw.VM(f'live-root-{root_mb}-lowram',usb=False,memory=f'{root_mb+32}M')
        try:
            end=time.monotonic()+15
            while time.monotonic()<end:
                dump=d/'vga.bin'
                vm.qmp('human-monitor-command',{'command-line':f'pmemsave 0xb8000 4000 "{dump}"'})
                screen=dump.read_bytes()[::2].decode('ascii',errors='replace')
                if 'insufficient usable E820 RAM' in screen:break
                time.sleep(.1)
            else:raise AssertionError('Low-RAM boot did not show the E820 rejection: '+screen)
            assert '[BOOT]' not in hw.clean(vm.log)
        finally:vm.stop()
        hw.IMAGE=original
        assert digest(original)==before
        print(f'Live root {root_mb} MiB: insufficient working RAM rejected before kernel entry PASS',flush=True)

if __name__=='__main__':main()
