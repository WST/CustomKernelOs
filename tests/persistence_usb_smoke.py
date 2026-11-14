"""Real xHCI BOT boot: persist a save across SIGKILL, without touching user images."""
import hashlib,pathlib,shutil,tempfile
import hardware_smoke as hw

def main():
    original=hw.IMAGE;before=hw.digest(original)
    with tempfile.TemporaryDirectory(prefix='myos-persistent-usb-') as directory:
        image=pathlib.Path(directory)/'usb.img';shutil.copyfile(original,image);hw.IMAGE=image
        for iteration in range(2):
            vm=hw.VM('persistent-usb-'+str(iteration),persistent=True)
            try:
                vm.wait('USB persistent root:')
                vm.wait('USB boot disk (persistent)')
                if not iteration:
                    vm.wait('New password:');vm.send('persist123','Confirm password:');vm.send('persist123','Login [root]:')
                else:
                    vm.wait('Login [root]:')
                    assert 'New password:' not in hw.clean(vm.log)
                vm.send('','Password:');vm.send('persist123','root:/')
                if not iteration:
                    vm.send('echo saved-to-usb > /persist.txt','root:/')
                    vm.send('sync','root:/')
                vm.send('cat /persist.txt','saved-to-usb')
                assert 'PANIC' not in hw.clean(vm.log)
                # Kill the host VM process: no guest shutdown path is called.
                vm.p.kill();vm.p.wait(timeout=5)
            finally:vm.stop()
        hw.IMAGE=original
        assert hw.digest(original)==before
    print('LIVE USB PERSISTENCE PASS: xHCI BOT, SYNCHRONIZE CACHE, password and saved file survive SIGKILL/reboot')

if __name__=='__main__':main()
