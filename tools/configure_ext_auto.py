"""Seed ext boot selection on a generated filesystem; preserve existing choices."""
import argparse
import pathlib
import re
import subprocess
import tempfile
from env_config import settings

def packages(value):
    result=[]
    for name in value.split():
        if not re.fullmatch(r'[A-Za-z0-9_+.-]{1,31}',name) or name in ('.','..'):
            raise ValueError('Invalid EXT_AUTOINSTALL_PACKAGES name: '+repr(name))
        if not (pathlib.Path('ext/instructions')/name/'setup.inst').is_file():
            raise ValueError('Unknown EXT_AUTOINSTALL_PACKAGES package: '+name)
        if name not in result:result.append(name)
    if len(result)>64:raise ValueError('EXT_AUTOINSTALL_PACKAGES supports at most 64 packages')
    return result

def seed(disk,selected):
    def fs(*args,check=True):
        return subprocess.run(['build/myfs',str(disk),*args],check=check,capture_output=True,text=True)
    existing=fs('cat','/etc/ext/autoinstall.conf',check=False)
    if existing.returncode==0:
        print('ext auto: preserving existing boot package configuration');return
    # Distinguish a missing file from an unreadable/corrupt existing path.
    present=fs('stat','/etc/ext/autoinstall.conf',check=False)
    if present.returncode==0:raise ValueError('Existing ext auto configuration cannot be read')
    for directory in ('/etc/ext','/var/lib/ext','/var/log'):
        fs('mkdir',directory)
        fs('chown','0','0',directory);fs('chmod','0755',directory)
    with tempfile.TemporaryDirectory(prefix='myos-ext-auto-') as folder:
        path=pathlib.Path(folder)/'autoinstall.conf'
        path.write_text('# Packages installed at boot; one package per line.\n'+''.join(n+'\n' for n in selected))
        fs('put',str(path),'/etc/ext/autoinstall.conf')
        fs('chown','0','0','/etc/ext/autoinstall.conf');fs('chmod','0644','/etc/ext/autoinstall.conf')
        if selected:
            marker=pathlib.Path(folder)/'enabled';marker.write_text('')
            fs('put',str(marker),'/etc/services/enabled/ext-autoinstall.service')
    print('ext auto: selected '+(', '.join(selected) if selected else 'none (disabled)'))

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--disk',required=True);args=parser.parse_args()
    try:seed(args.disk,packages(settings()['EXT_AUTOINSTALL_PACKAGES']))
    except (ValueError,subprocess.CalledProcessError) as error:parser.exit(2,str(error)+'\n')
if __name__=='__main__':main()
