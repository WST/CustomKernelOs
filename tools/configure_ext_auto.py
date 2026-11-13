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

def build_jobs(value):
    if value=='auto':return value
    if not re.fullmatch(r'[0-9]+',value) or not 1<=int(value)<=16:
        raise ValueError('EXT_BUILD_JOBS must be auto or 1..16')
    return str(int(value))


def seed_build_jobs(disk,value):
    value=build_jobs(value)
    def fs(*args,check=True):return subprocess.run(['build/myfs',str(disk),*args],check=check,capture_output=True,text=True)
    if fs('stat','/etc/ext/build-jobs',check=False).returncode==0:
        print('ext build: preserving existing job limit');return
    fs('mkdir','/etc/ext')
    with tempfile.TemporaryDirectory(prefix='myos-ext-jobs-') as directory:
        path=pathlib.Path(directory)/'build-jobs';path.write_text(value+'\n')
        fs('put',str(path),'/etc/ext/build-jobs')
        fs('chown','0','0','/etc/ext/build-jobs');fs('chmod','0644','/etc/ext/build-jobs')
    print('ext build: jobs='+value)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--disk',required=True);args=parser.parse_args()
    try:
        config=settings();jobs=build_jobs(config['EXT_BUILD_JOBS']);selected=packages(config['EXT_AUTOINSTALL_PACKAGES'])
        seed_build_jobs(args.disk,jobs)
        seed(args.disk,selected)
    except (ValueError,subprocess.CalledProcessError) as error:parser.exit(2,str(error)+'\n')
if __name__=='__main__':main()
