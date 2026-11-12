"""Build a BIOS iPXE USB bootstrap with an embedded MyOS HTTP script URL."""
from env_config import settings
import argparse,hashlib,json,pathlib,shutil,subprocess,urllib.parse

ROOT=pathlib.Path(__file__).resolve().parents[1]
REVISION='fc23df6fb245eec9f68d5ae3f95469875c3dcade'
REPOSITORY='https://github.com/ipxe/ipxe.git'
TARGET='bin-i386-pcbios/ipxe.usb'

def script_url(value):
    try:
        uri=urllib.parse.urlsplit(value)
        if (uri.scheme not in ('http','https') or not uri.hostname or uri.port==0 or
            uri.username is not None or uri.password is not None or uri.query or uri.fragment or
            any(ord(c)<=32 or ord(c)>=127 or c in '$ {}\\' for c in value)):
            raise ValueError
    except ValueError:raise argparse.ArgumentTypeError('Use an HTTP(S) URL without credentials, spaces, variables, query or fragment')
    if uri.path in ('','/'):return value.rstrip('/')+'/boot.ipxe'
    return value

def main():
    config=settings()
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--url',default=config['NETBOOT_URL'],type=script_url,help='HTTP server URL, e.g. http://192.168.2.6:8080/boot.ipxe')
    ap.add_argument('--jobs',type=int,default=config['IPXE_JOBS'])
    args=ap.parse_args()
    if args.jobs<1:ap.error('--jobs must be positive')
    probe=subprocess.run(['gcc','-x','c','-fsyntax-only','-'],input='#include <lzma.h>\n',text=True,capture_output=True)
    if probe.returncode:ap.error('iPXE needs liblzma headers; install liblzma-dev (or xz-devel) in the build environment')
    build=ROOT/'build';build.mkdir(exist_ok=True)
    source=build/'ipxe-src';artifact=build/'netboot-bootstrap';artifact.mkdir(exist_ok=True)
    embedded=artifact/'bootstrap.ipxe'
    embedded.write_text('#!ipxe\n:retry\necho MyOS network boot: '+args.url+'\n'
                        'dhcp || goto failed\nchain --autofree '+args.url+' || goto failed\n'
                        ':failed\necho Network/server unavailable or boot failed.\n'
                        'prompt Press any key to retry\ngoto retry\n')
    def run(*cmd):subprocess.run([str(x) for x in cmd],check=True)
    if not (source/'.git').is_dir():
        source.mkdir(exist_ok=True);run('git','init',source)
        run('git','-C',source,'remote','add','origin',REPOSITORY)
    head=subprocess.run(['git','-C',str(source),'rev-parse','HEAD'],capture_output=True,text=True)
    if head.returncode or head.stdout.strip()!=REVISION:
        run('git','-C',source,'fetch','--depth=1','origin',REVISION)
        run('git','-C',source,'checkout','--detach','FETCH_HEAD')
    # Use a pinned upstream source. EMBED is rebuilt when the server URL changes.
    run('make','-C',source/'src',f'-j{args.jobs}',TARGET,'EMBED='+str(embedded))
    output=build/'myos-netboot-usb.img';shutil.copyfile(source/'src'/TARGET,output)
    metadata={'image':str(output.relative_to(ROOT)),'bytes':output.stat().st_size,
              'sha256':hashlib.sha256(output.read_bytes()).hexdigest(),
              'ipxe_revision':REVISION,'source':REPOSITORY,'script_url':args.url,'boot':'BIOS/CSM only','architecture':'i386'}
    (artifact/'manifest.json').write_text(json.dumps(metadata,indent=2)+'\n')
    print(json.dumps(metadata,indent=2))
    print('Write this bootstrap to the test USB once; future MyOS updates only replace the server publication.')

if __name__=='__main__':main()
