"""USB-only boot: all screen corners and wheel, before any mode change."""
import time
from hardware_smoke import VM,B,IMAGE,digest,clean

def move(vm,dx,dy):
    for _ in range(35):
        vm.qmp('input-send-event',{'events':[{'type':'rel','data':{'axis':'x','value':dx}},
                                          {'type':'rel','data':{'axis':'y','value':dy}}]})
        time.sleep(.025)
    time.sleep(.2)

def wheel(vm,button):
    for _ in range(3):
        vm.qmp('input-send-event',{'events':[{'type':'btn','data':{'button':button,'down':True}}]})
        time.sleep(.04)
        vm.qmp('input-send-event',{'events':[{'type':'btn','data':{'button':button,'down':False}}]})
        time.sleep(.04)
    time.sleep(.2)

def main():
    before=digest(IMAGE)
    for version in (2,1):
        vm=VM('usb-mouse-'+str(version),cpus=2,usb_version=version,
              extra=['-global','VGA.xres=1280','-global','VGA.yres=1024'])
        try:
            vm.wait('New password:')
            log=clean(vm.log)
            assert 'USB mouse report protocol: id=0, wheel=yes' in log,log[-5000:]
            path=B/'usb-mouse-size.ppm'
            vm.qmp('screendump',{'filename':str(path.resolve())})
            width,height=map(int,path.read_bytes().split(b'\n',3)[1].split())
            assert width==1280 and height==1024
            move(vm,100,100);assert vm.mouse_state()[:2]==[width-1,height-1],vm.mouse_state()
            move(vm,-100,-100);assert vm.mouse_state()[:2]==[0,0],vm.mouse_state()
            initial=vm.mouse_state()[4]
            wheel(vm,'wheel-up');up=vm.mouse_state()[4]
            assert (up-initial)&0xffffffff==3,(initial,up)
            wheel(vm,'wheel-down');assert vm.mouse_state()[4]==initial,vm.mouse_state()
            print('USB-only boot + corners 1280x1024 + wheel up/down, USB',version,'OK',flush=True)
        finally:vm.stop()
    assert digest(IMAGE)==before

if __name__=='__main__':main()
