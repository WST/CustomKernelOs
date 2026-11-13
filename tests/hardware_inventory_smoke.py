"""Verify discovery and the read-only ABI on disposable PCI/AHCI and USB boots."""
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import hardware_smoke as hardware

B = Path('build')


def run(*args):
    subprocess.run([str(a) for a in args], check=True, stdout=subprocess.DEVNULL)


def root_image(path, megabytes):
    with path.open('wb') as stream:
        stream.truncate(megabytes * 1048576)
    for directory in ['/bin', '/lib', '/etc', '/root', '/home', '/tmp']:
        run(B/'myfs', path, 'mkdir', directory)
    for name in ['sh', 'lspci', 'lsusb', 'lsports']:
        run(B/'myfs', path, 'put', B/'user'/name, '/bin/'+name)
    run(B/'myfs', path, 'put', B/'hwinfo-check', '/bin/hwinfo-check')
    run(B/'myfs', path, 'put', B/'ld.so', '/lib/ld.so')
    run(B/'myfs', path, 'put', B/'user/libc.so', '/lib/libc.so')


def live_image(root, output, temporary):
    # Same boot contract as tools/make_usb_image.py, with a minimal test root.
    import sys
    sys.path.insert(0, str(Path('tools').resolve()))
    from make_usb_image import live_limits
    phys, minimum, maximum, reserve = live_limits()
    kernel = (B/'hardware/kernel.bin').read_bytes()
    symbols = subprocess.check_output(['nm', '-n', str(B/'hardware/kernel.elf')], text=True)
    end = next(int(line.split()[0], 16) for line in symbols.splitlines() if line.endswith(' __bss_end'))
    assert end < phys
    sectors = (len(kernel)+511)//512
    root_lba = (33+sectors+2047)//2048*2048
    defines = [f'-DKERNEL_LBA=33', f'-DKERNEL_SECTORS={sectors}', f'-DKERNEL_END={end}',
               f'-DROOT_LBA={root_lba}', f'-DROOT_SECTORS={root.stat().st_size//512}',
               f'-DROOT_PHYS={phys}', f'-DROOT_MIN_BYTES={minimum*1048576}',
               f'-DROOT_MAX_BYTES={maximum*1048576}', f'-DROOT_RESERVE_BYTES={reserve*1048576}']
    for source, name in [('hardware.asm', 'mbr'), ('hardware-stage2.asm', 'stage2')]:
        run('nasm', '-f', 'bin', *defines, 'boot/'+source, '-o', temporary/name)
    with output.open('wb') as stream:
        stream.write((temporary/'mbr').read_bytes())
        stream.write((temporary/'stage2').read_bytes())
        stream.write(kernel)
        stream.seek(root_lba*512)
        stream.write(root.read_bytes())
        stream.truncate((stream.tell()+1048575)//1048576*1048576)


class VM(hardware.VM):
    def __init__(self, name, boot, extra, cpus, memory="256M"):
        self.temp = tempfile.TemporaryDirectory(prefix='myos-inventory-qmp-')
        self.log = Path(self.temp.name)/'serial.log'
        self.log.write_text('')
        self.log_artifact = B/(name+'.log')
        self.sockpath = self.temp.name+'/qmp'
        self.boot_timeout = 90
        self.err = (B/(name+'.stderr')).open('w')
        args = ['qemu-system-i386', '-cpu', 'max', '-smp', str(cpus), '-m', memory,
                '-display', 'none', '-no-reboot', '-nic', 'none',
                '-serial', 'file:'+str(self.log), '-qmp', 'unix:'+self.sockpath+',server=on,wait=off',
                '-drive', f'format=raw,file={boot},snapshot=on,index=0', *extra]
        self.p = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=self.err)
        self.s = socket.socket(socket.AF_UNIX)
        self.s.settimeout(5)
        deadline = time.monotonic()+10
        while not Path(self.sockpath).exists():
            if self.p.poll() is not None:
                self.err.close()
                raise AssertionError((B/(name+'.stderr')).read_text())
            if time.monotonic() > deadline:
                raise AssertionError('QMP socket missing')
            time.sleep(.05)
        self.s.connect(self.sockpath)
        self.f = self.s.makefile('rb')
        self.read_response(greeting=True)
        self.qmp('qmp_capabilities')


def login(vm):
    vm.wait('New password:')
    vm.send('hardware123', 'Confirm password:')
    vm.send('hardware123', 'Login [root]:')
    vm.send('', 'Password:')
    vm.send('hardware123', 'root:/')


def check(vm, mode):
    vm.send('hwinfo-check'+(' '+mode if mode else ''), 'HWINFO PASS')
    text = hardware.clean(vm.log)
    assert 'HWINFO FAIL' not in text and 'PANIC' not in text and 'EXCEPTION' not in text, text[-8000:]


def main():
    with tempfile.TemporaryDirectory(prefix='myos-inventory-') as directory:
        temporary = Path(directory)
        disk = temporary/'sata.img'
        root_image(disk, 32)
        for cpus in (1, 4):
            vm = VM('hardware-inventory-pci-'+str(cpus), B/'os.img', [
                '-device', 'pci-bridge,id=bridge,chassis_nr=1',
                '-device', 'e1000,bus=bridge,addr=03.0,multifunction=on',
                '-device', 'e1000,bus=bridge,addr=03.1',
                '-device', 'ich9-ahci,id=ahci',
                '-drive', f'if=none,id=rootdisk,format=raw,file={disk},snapshot=on',
                '-device', 'ide-hd,bus=ahci.0,drive=rootdisk'], cpus)
            try:
                login(vm)
                check(vm, '')
                if cpus == 1:
                    vm.send('lspci -v', 'Ethernet controller')
                    vm.send('lsports', 'Physical connector labels')
                    vm.send('lsusb', 'USB inventory unavailable')
                    text = hardware.clean(vm.log)
                    assert '01:03.1' in text and 'SATA ports' in text, text[-8000:]
                print(f'PCI bridge, multifunction, SATA and ABI validation, {cpus} CPU(s): PASS', flush=True)
            finally:
                vm.stop()
        root = temporary/'live-root.img'
        root_image(root, 64)
        sata = temporary/'live-sata.img'
        sata.write_bytes(disk.read_bytes())
        sata_before = hardware.digest(sata)
        image = temporary/'live.img'
        live_image(root, image, temporary)
        vm = VM('hardware-inventory-usb', image, [
            '-machine', 'pc,i8042=off', '-device', 'qemu-xhci,id=xhci',
            '-device', 'ich9-ahci,id=ahci',
            '-drive', f'if=none,id=livesata,format=raw,file={sata},snapshot=on',
            '-device', 'ide-hd,bus=ahci.0,drive=livesata',
            '-device', 'usb-kbd,bus=xhci.0', '-device', 'usb-mouse,bus=xhci.0,id=testmouse',
            '-device', 'usb-storage,bus=xhci.0,drive=usbfixture',
            '-drive', f'if=none,id=usbfixture,format=raw,file={disk},snapshot=on'], 4)
        try:
            login(vm)
            check(vm, 'usb')
            vm.send('lsusb -t', 'HID mouse')
            vm.send('lsports', 'Physical connector labels')
            assert 'not managed as a disk' in hardware.clean(vm.log)
            assert hardware.digest(sata) == sata_before
            vm.qmp('device_del', {'id': 'testmouse'})
            time.sleep(1)
            check(vm, 'removed')
            print('xHCI root ports, boot descriptors, USB HID input and disconnect status, 4 CPUs: PASS', flush=True)
        finally:
            vm.stop()


if __name__ == '__main__':
    main()
