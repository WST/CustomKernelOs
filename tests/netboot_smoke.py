"""Boot the published live image through QEMU's iPXE ROM, with no local disk."""
import argparse,json,pathlib,shutil,subprocess,sys,tempfile,threading
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'tools'))
import netboot
from build_ipxe import TARGET
import hardware_smoke as hw

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--bootstrap',action='store_true');args=ap.parse_args()
    public=netboot.project_path(netboot.settings()['NETBOOT_DIRECTORY'])
    metadata=json.loads((public/'latest.json').read_text())
    with netboot.Server(('127.0.0.1',0),public) as server:
        thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        scratch=tempfile.TemporaryDirectory(prefix='myos-ipxe-usb-test-')
        original=hw.IMAGE
        if args.bootstrap:
            directory=pathlib.Path(scratch.name);embedded=directory/'test.ipxe'
            embedded.write_text(f'#!ipxe\ndhcp\nchain http://10.0.2.2:{server.server_port}/boot.ipxe\n')
            source=netboot.ROOT/'build/ipxe-src/src'
            subprocess.run(['make','-C',str(source),'-j4',TARGET,'EMBED='+str(embedded)],check=True)
            hw.IMAGE=directory/'ipxe.usb';shutil.copyfile(source/TARGET,hw.IMAGE)
            vm=hw.VM('netboot-bootstrap',extra=['-netdev','user,id=bootnet','-device','e1000,netdev=bootnet'])
        else:
            vm=hw.VM('netboot-http',usb=False,netboot_url=f'http://10.0.2.2:{server.server_port}/boot.ipxe')
        try:
            vm.wait('New password:',timeout=240)
            assert f"bytes={metadata['root_bytes']}" in hw.clean(vm.log)
            vm.send('net12345','Confirm password:');vm.send('net12345','Login [root]:')
            vm.send('','Password:');vm.send('net12345','root:/')
            vm.send('whoami','\nroot\n')
            print(('USB bootstrap + HTTP' if args.bootstrap else 'Network-only iPXE HTTP')+' boot, RAM root and interactive shell: PASS',flush=True)
        except Exception:
            if vm.p.poll() is not None:
                print('QEMU exit status:',vm.p.returncode,flush=True)
                raise
            dump=pathlib.Path(vm.temp.name)/'vga.bin'
            vm.qmp('human-monitor-command',{'command-line':f'pmemsave 0xb8000 4000 "{dump}"'})
            print('HTTP range reads:',server.range_reads,'last:',server.last_range,flush=True)
            print('Boot VGA:',dump.read_bytes()[::2].decode('ascii',errors='replace'),flush=True)
            print(vm.qmp('human-monitor-command',{'command-line':'info registers'}),flush=True)
            raise
        finally:
            vm.stop();server.shutdown();thread.join();hw.IMAGE=original;scratch.cleanup()

if __name__=='__main__':main()
