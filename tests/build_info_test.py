"""Check clean/dirty identity, archive fallback and reproducible timestamps."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

script=Path('tools/build_info.py').resolve()
with tempfile.TemporaryDirectory() as tmp:
    root=Path(tmp)
    (root/'VERSION').write_text('0.19\n')
    env=dict(os.environ, SOURCE_DATE_EPOCH='0')
    env.pop('MYOS_BUILD_COMMIT',None)
    def generate():
        subprocess.run([sys.executable,str(script),'identity.h'],cwd=root,env=env,check=True)
        return (root/'identity.h').read_text()
    first=generate()
    assert '"unknown"' in first and '1970-01-01 00:00:00 UTC' in first
    assert generate()==first
    def git(*args):
        return subprocess.check_output(['git',*args],cwd=root,text=True,stderr=subprocess.DEVNULL).strip()
    git('init')
    (root/'.gitignore').write_text('identity.h\n')
    git('add','VERSION','.gitignore')
    git('-c','user.name=Test','-c','user.email=test@example.invalid','commit','-m','fixture')
    commit=git('rev-parse','HEAD')
    assert '"'+commit+'"' in generate()
    (root/'VERSION').write_text('0.20\n')
    assert commit+'-dirty' in generate()
    (root/'VERSION').write_text('0.19\n')
    assert '"'+commit+'"' in generate()
    (root/'new-source.c').write_text('/* untracked */')
    assert commit+'-dirty' in generate()
print('Build metadata: PASS')
