"""Explicit fixed-mode fallback when runtime BIOS state is unavailable."""
import pathlib
import re
import tempfile
import hardware_smoke as hw

def main():
    original=hw.IMAGE;before=hw.digest(original)
    with tempfile.TemporaryDirectory(prefix='myos-fixed-video-') as temp:
        image=pathlib.Path(temp)/'fixed-video.img'
        data=bytearray(original.read_bytes())
        # Zero only the loader's optional snapshot marker. Kernel and root stay intact.
        marker=bytes.fromhex('66 c7 06 28 60 56 42 56 49')
        start=data.find(marker,512,33*512)
        assert start>=512
        data[start+5:start+9]=bytes(4);image.write_bytes(data)
        hw.IMAGE=image
        vm=hw.VM('video-fixed-fallback',usb=False)
        try:
            vm.wait('New password:')
            assert 'VBE runtime: BIOS snapshot missing; fixed boot resolution' in hw.clean(vm.log)
            vm.send('fixedtest123','Confirm password:')
            vm.send('fixedtest123','Login [root]:')
            vm.send('','Password:')
            vm.send('fixedtest123','root:/')
            vm.send('mode','Mode switching: driver unavailable')
            assert re.search(r'Modes:\s+\[1280x800\]\n',hw.clean(vm.log))
            vm.send('mode 1920x1080','video BIOS runtime unavailable')
            vm.send('dmesg','VBE runtime: BIOS snapshot missing')
            assert 'EXCEPTION' not in hw.clean(vm.log)
        finally:vm.stop();hw.IMAGE=original
    assert hw.digest(original)==before
    print('Fixed framebuffer fallback: only current mode + actionable mode error + dmesg reason: OK')

if __name__=='__main__':main()
