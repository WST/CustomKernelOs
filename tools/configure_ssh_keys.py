"""Add explicitly selected Ed25519 public keys to the image's root account."""
import argparse
import base64
import binascii
import pathlib
import re
import shlex
import struct
import subprocess
import sys
import tempfile
from env_config import settings,project_path
TYPE='ssh-ed25519'
TARGET='/root/.ssh/authorized_keys'

def public_key(line):
    if len(line.encode('utf-8'))>8192 or any(ord(c)<32 and c!='\t' for c in line):
        raise ValueError('SSH public key contains invalid control characters or is too long')
    parts=line.strip().split(None,2)
    if len(parts)<2 or parts[0]!=TYPE:
        raise ValueError('Expected an ssh-ed25519 public key; private keys, options and other key types are not accepted')
    try:blob=base64.b64decode(parts[1],validate=True)
    except (ValueError,binascii.Error):raise ValueError('Invalid SSH public key base64') from None
    prefix=struct.pack('>I',len(TYPE))+TYPE.encode()+struct.pack('>I',32)
    if len(blob)!=len(prefix)+32 or not blob.startswith(prefix):
        raise ValueError('Invalid Ed25519 SSH wire-format key')
    encoded=base64.b64encode(blob).decode()
    return TYPE+' '+encoded+(' '+parts[2] if len(parts)==3 else ''),encoded

def source_path(value):
    # Allow Windows .pub paths when the build itself runs inside WSL.
    normalized=value.replace(chr(92),'/')
    if sys.platform.startswith('linux') and re.match(r'^[A-Za-z]:/',normalized):
        return pathlib.Path('/mnt/'+normalized[0].lower()+'/'+normalized[3:])
    return project_path(value)

def selected(config):
    lines=[]
    inline=config['SSH_AUTHORIZED_KEY'].strip()
    if inline:lines.append(inline)
    path=config['SSH_AUTHORIZED_KEY_FILE'].strip()
    if path:
        source=source_path(path)
        if source.stat().st_size>1024*1024:raise ValueError('SSH public key file exceeds 1 MiB')
        contents=source.read_text(encoding='utf-8-sig')
        file_lines=[line.strip() for line in contents.splitlines() if line.strip() and not line.lstrip().startswith('#')]
        if not file_lines:raise ValueError('SSH public key file contains no keys')
        lines.extend(file_lines)
    keys=[];seen=set()
    for line in lines:
        text,identity=public_key(line)
        if identity not in seen:keys.append((text,identity));seen.add(identity)
    if len(keys)>64:raise ValueError('At most 64 SSH public keys may be selected')
    return keys

def existing_identity(line):
    if not line.strip() or line.lstrip().startswith('#'):return None
    try:
        lexer=shlex.shlex(line,posix=True);lexer.whitespace_split=True;lexer.commenters=''
        first=next(lexer)
        if first!=TYPE:first=next(lexer) # authorized_keys options precede the key type
        return public_key(TYPE+' '+next(lexer))[1] if first==TYPE else None
    except (ValueError,StopIteration):return None

def install(disk,keys):
    if not keys:return 0
    def fs(*args,check=True):return subprocess.run(['build/myfs',str(disk),*args],check=check,capture_output=True)
    def metadata(path):
        result=fs('stat',path,check=False)
        if result.returncode:
            if b'not found' in result.stderr or b'no such' in result.stderr:return None
            raise ValueError('Cannot inspect image path '+path+': '+result.stderr.decode(errors='replace'))
        match=re.search(rb'\btype=(\d+)\b',result.stdout)
        if not match:raise ValueError('Rebuild build/myfs: file type metadata is required')
        return int(match[1])
    # Inspect the complete fixed path before writing; never follow a symlink.
    kinds={path:metadata(path) for path in ('/root','/root/.ssh',TARGET)}
    for path,kind in kinds.items():
        expected=1 if path==TARGET else 2
        if kind is not None and kind!=expected:raise ValueError('Refusing nonstandard SSH key path: '+path)
    old=fs('cat',TARGET).stdout if kinds[TARGET] is not None else b''
    identities={existing_identity(line) for line in old.decode('utf-8',errors='replace').splitlines()}
    additions=[text for text,identity in keys if identity not in identities]
    data=old
    if additions:data+=(b'\n' if data and not data.endswith(b'\n') else b'')+('\n'.join(additions)+'\n').encode('utf-8')
    for directory in ('/root','/root/.ssh'):
        fs('mkdir',directory);fs('chown','0','0',directory);fs('chmod','0700',directory)
    if additions or kinds[TARGET] is None:
        with tempfile.TemporaryDirectory(prefix='myos-ssh-public-') as folder:
            source=pathlib.Path(folder)/'authorized_keys';source.write_bytes(data)
            fs('put',str(source),TARGET)
    fs('chown','0','0',TARGET);fs('chmod','0600',TARGET)
    print('SSH authorized keys: added '+str(len(additions))+' for root (existing entries preserved)')
    return len(additions)

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--disk',required=True);args=parser.parse_args()
    try:install(args.disk,selected(settings()))
    except (ValueError,OSError,subprocess.CalledProcessError) as error:parser.exit(2,str(error)+'\n')
if __name__=='__main__':main()
