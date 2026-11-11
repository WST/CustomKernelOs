"""Real runtime BIOS calls, console/desktop resize and SMP in the hardware guest."""
import pathlib
from hardware_smoke import VM, clean, digest, IMAGE, save_png, B

def dimensions(vm,name):
    p=B/(name+'.ppm')
    vm.qmp('screendump',{'filename':str(p.resolve())})
    return tuple(map(int,p.read_bytes().split(b'\n',3)[1].split()))

def main():
    before=digest(IMAGE)
    vm=VM('video-runtime',usb=False,cpus=2)
    try:
        vm.wait('New password:')
        log=clean(vm.log)
        print(log[log.find('VBE runtime:'):log.find('[BOOT] FPU')],flush=True)
        assert 'validated modes' in log, log[-6000:]
        assert 'VBE audit:' in log and 'largest reported=' in log and 'list_complete=1' in log
        assert '1920x1080 32 bpp accepted' in log
        vm.send('videotest123','Confirm password:')
        vm.send('videotest123','Login [root]:')
        vm.send('','Password:')
        vm.send('videotest123','root:/')
        for w,h in [(800,600),(1920,1080),(1024,768),(2560,1440),(800,600)]:
            vm.send(f'mode {w}x{h}',f'Switched to {w}x{h}')
            assert dimensions(vm,'video-runtime')==(w,h)
            assert f'to {w}x{h} pitch=' in clean(vm.log)
            vm.send('whoami','\nroot\n')
            vm.check_console_pixels("video-runtime-scroll")
            print('Runtime switch',w,h,'console pixels OK',flush=True)
        vm.send('mode 2552x1599','is not supported')
        assert dimensions(vm,'video-runtime')==(800,600)
        vm.send('mode','Usage: mode WIDTHxHEIGHT')
        # The script starts desktop, waits for it to acquire the screen, then
        # resizes from another process while the desktop is alive.
        vm.send("echo 'desktop &' > /runtime.sh",'root:/')
        vm.send("echo 'sleep 2' >> /runtime.sh",'root:/')
        vm.send("echo 'mode 1280x720' >> /runtime.sh",'root:/')
        vm.send('sh /runtime.sh','Switched to 1280x720')
        import time;time.sleep(3)
        assert dimensions(vm,'video-runtime-desktop')==(1280,720)
        save_png(B/'video-runtime-desktop.ppm',B/'video-runtime-desktop.png')
        assert 'EXCEPTION' not in clean(vm.log)
    finally:vm.stop()
    assert digest(IMAGE)==before
    print('Runtime VBE + repeated resize + console pixel comparison + desktop + SMP: OK')

if __name__=='__main__':main()
