"""Observe PIT and USB mouse delivery during one large console write()."""
from pathlib import Path
import re
import subprocess
import tempfile
import time
import hardware_smoke as hardware

source=Path('build/myos-usb.img')
original=hardware.digest(source)
with tempfile.TemporaryDirectory(prefix='myos-console-load-') as directory:
    temporary=Path(directory)
    root=temporary/'root.img';image=temporary/'usb.img'
    # Use the root embedded in the selected image, preserving the boot/kernel.
    # The root is the active MBR partition (not necessarily the newest layout).
    data=source.read_bytes();offset=int.from_bytes(data[454:458],'little')*512
    size=int.from_bytes(data[458:462],'little')*512
    root.write_bytes(data[offset:offset+size])
    subprocess.run(['build/myfs',str(root),'put','build/user/consoleload','/bin/consoleload'],check=True,stdout=subprocess.DEVNULL)
    image.write_bytes(data)
    with image.open('r+b') as stream: stream.seek(offset);stream.write(root.read_bytes())
    hardware.IMAGE=image
    vm=hardware.VM('console-load')
    try:
        vm.wait('New password:');vm.send('load123','Confirm password:')
        vm.send('load123','Login [root]:');vm.send('','Password:');vm.send('load123','root:/')
        symbols=subprocess.check_output(['nm','-n','build/hardware/kernel.elf'],text=True)
        address=next(int(line.split()[0],16) for line in symbols.splitlines() if line.endswith(' b ticks'))
        def ticks():
            result=vm.qmp('human-monitor-command',{'command-line':f'xp /1uw 0x{address:x}'})
            return int(result.split(':',1)[1].strip())
        vm.send('consoleload','CONSOLE LOAD READY')
        initial=vm.mouse_state()
        vm.key('g');vm.wait('CONSOLE LOAD BEGIN')
        vm.qmp('input-send-event',{'events':[{'type':'rel','data':{'axis':'x','value':40}},
                                                   {'type':'rel','data':{'axis':'y','value':20}}]})
        vm.key('x')
        timer_during=False;mouse_during=False;previous_tick=None;started=time.monotonic()
        while time.monotonic()-started<35:
            text=hardware.clean(vm.log)
            assert 'PANIC' not in text and 'EXCEPTION' not in text,text[-1000:]
            body=text.split('CONSOLE LOAD BEGIN\n',1)[1]
            if 'CONSOLE LOAD END' in body: break
            # Keep sampling before the final 512 bytes. Completion/scheduling
            # after write() must not count as servicing input during write().
            if len(body)<512*1024-512:
                tick=ticks();moved=vm.mouse_state()[3]!=initial[3]
                after=hardware.clean(vm.log).split('CONSOLE LOAD BEGIN\n',1)[1]
                if len(after)<512*1024-512:
                    timer_during|=previous_tick is not None and tick>previous_tick
                    mouse_during|=moved
                    previous_tick=tick
            time.sleep(.01)
        else: raise AssertionError('console load timed out')
        result=re.search(r'CONSOLE LOAD END bytes=(\d+) ms=(\d+) key=(-?\d+)',text)
        assert result and int(result[1])==512*1024 and int(result[3])==ord('x'),text[-1000:]
        print(result.group(),flush=True)
        print(f'During write: PIT advanced={timer_during}, USB mouse delivered={mouse_during}',flush=True)
        assert timer_during and mouse_during
    finally: vm.stop()
assert hardware.digest(source)==original
