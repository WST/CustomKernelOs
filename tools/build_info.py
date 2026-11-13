"""Generate kernel identity; SOURCE_DATE_EPOCH supports reproducible builds."""
import datetime
import json
import os
from pathlib import Path
import subprocess
import sys

GIT_TIMEOUT = 3

def git(*args):
    try:
        return subprocess.check_output(
            ['git', '--no-optional-locks', *args], stderr=subprocess.DEVNULL,
            text=True, timeout=GIT_TIMEOUT).strip()
    except subprocess.TimeoutExpired:
        print('build_info: git ' + args[0] + ' timed out; continuing without complete Git metadata', file=sys.stderr)
        return None
    except (OSError, subprocess.CalledProcessError):
        return None

commit = os.environ.get('MYOS_BUILD_COMMIT') or git('rev-parse', 'HEAD') or 'unknown'
if commit != 'unknown':
    state = git('status', '--porcelain', '--untracked-files=normal')
    if state is None:
        commit += '-state-unknown'
    elif state:
        commit += '-dirty'
epoch = os.environ.get('SOURCE_DATE_EPOCH')
now = datetime.datetime.fromtimestamp(int(epoch), datetime.timezone.utc) if epoch else datetime.datetime.now(datetime.timezone.utc)
values = {'MYOS_VERSION': Path('VERSION').read_text().strip(),
          'MYOS_COMMIT': commit, 'MYOS_BUILD_TIME': now.strftime('%Y-%m-%d %H:%M:%S UTC')}
for key, limit in [('MYOS_VERSION', 31), ('MYOS_COMMIT', 63), ('MYOS_BUILD_TIME', 31)]:
    if len(values[key].encode('utf-8')) > limit:
        raise SystemExit(key + ' exceeds kernel field size')
data = ''.join('#define %s %s\n' % (k, json.dumps(v)) for k, v in values.items())
out = Path(sys.argv[1])
if not out.exists() or out.read_text() != data:
    out.write_text(data)
