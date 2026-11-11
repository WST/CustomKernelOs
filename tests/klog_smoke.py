"""Validate the log syscall and absence of automatic disk writes on a disposable root."""
from pathlib import Path
import subprocess
import tempfile
import hardware_smoke as hardware

source=Path('build/myos-usb.img');before=hardware.digest(source)
with tempfile.TemporaryDirectory(prefix='myos-klog-') as directory:
    root=Path(directory)/'root.img';image=Path(directory)/'usb.img'
    data=source.read_bytes();offset=int.from_bytes(data[454:458],'little')*512
    size=int.from_bytes(data[458:462],'little')*512
    root.write_bytes(data[offset:offset+size])
    # Reuse an existing fixture inode: old development roots can have very
    # little inode headroom even with plenty of free data blocks.
    subprocess.run(['build/myfs',str(root),'put','build/user/klogcheck','/bin/clear'],check=True,stdout=subprocess.DEVNULL)
    subprocess.run(['build/myfs',str(root),'rm','/var/log/kernel.log'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    image.write_bytes(data)
    with image.open('r+b') as stream:stream.seek(offset);stream.write(root.read_bytes())
    hardware.IMAGE=image
    vm=hardware.VM('klog-syscall',usb=False)
    try:
        vm.wait('New password:');vm.send('log123','Confirm password:')
        vm.send('log123','Login [root]:');vm.send('','Password:');vm.send('log123','root:/')
        vm.send('clear','Kernel log syscall: independent snapshots and invalid pointer/length/cursor checks: PASS')
        vm.send('dmesg > /tmp/boot.log','root:/')
        vm.send('cat /tmp/boot.log','Kernel log: 16 KiB memory ring')
        start=len(hardware.clean(vm.log));vm.send('dmesg','Kernel log: 16 KiB memory ring')
        vm.wait('root:/',start)
        assert 'PANIC' not in hardware.clean(vm.log) and 'EXCEPTION' not in hardware.clean(vm.log)
        print('Kernel log syscall, independent readers, absent automatic file and explicit dmesg redirection: PASS',flush=True)
    finally:vm.stop()
assert hardware.digest(source)==before
