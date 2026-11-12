"""Validate incremental software drag frames inside MyOS on generated Live data."""
import subprocess
from hardware_smoke import VM
root='build/hardware/live-root.img'
subprocess.run(['build/myfs',root,'put','build/user/dragcheck','/bin/dragcheck'],check=True)
vm=None
try:
    vm=VM('desktop-drag',usb=False,legacy=True,memory='512M')
    vm.wait('New password:')
    vm.send('dragtest123','Confirm password:')
    vm.send('dragtest123','Login [root]:')
    vm.send('','Password:')
    vm.send('dragtest123','root:/')
    vm.send('dragcheck','DESKTOP DRAG PASS:')
    print('PASS: MyOS incremental software drag matches full composition for every frame')
finally:
    if vm:vm.stop()
    subprocess.run(['build/myfs',root,'rm','/bin/dragcheck'],check=True)
