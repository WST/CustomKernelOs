"""Verify real overlap, graph ordering, job caps and draining with MyOS make on Linux."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile

binary=Path(sys.argv[1]).resolve()
helper=r"""import fcntl,json,sys,time
from pathlib import Path
name=sys.argv[1]
def update(event):
 with open('state','a+') as f:
  fcntl.flock(f,fcntl.LOCK_EX);f.seek(0);s=f.read()
  state=json.loads(s) if s else {'active':0,'peak':0,'events':[]}
  state['active']+=1 if event=='start' else -1
  state['peak']=max(state['peak'],state['active'])
  state['events'].append([event,name])
  f.seek(0);f.truncate();json.dump(state,f);f.flush()
update('start')
time.sleep(.06 if name=='bad' else .2)
if name!='bad':Path(name).write_text(name)
update('end')
raise SystemExit(7 if name=='bad' else 0)
"""

def case(jobs):
 with tempfile.TemporaryDirectory(prefix='myos-make-parallel-') as tmp:
  root=Path(tmp);(root/'helper.py').write_text(helper)
  (root/'Makefile').write_text('all: final\nfinal: a b c d\n\tpython3 helper.py final\n'+''.join(f'{n}: shared\n\tpython3 helper.py {n}\n' for n in 'abcd')+'shared:\n\tpython3 helper.py shared\n')
  def run(*args,ok=True):
   result=subprocess.run([str(binary),*args],cwd=root,capture_output=True,text=True,timeout=15)
   assert (result.returncode==0)==ok,(args,result.stdout,result.stderr)
   return result
  run('-j'+str(jobs))
  state=json.loads((root/'state').read_text());events=state['events']
  assert state['peak']==jobs and state['active']==0,state
  assert events.count(['start','shared'])==1,state
  for name in 'abcd':
   assert events.index(['end','shared'])<events.index(['start',name]),state
   assert events.index(['end',name])<events.index(['start','final']),state
  run('--jobs='+str(jobs))
  assert json.loads((root/'state').read_text())==state
  for flag in ['-j0','-j17','-jbad','--jobs=0','--jobs=999999999999999']:
   run(flag,ok=False)
  (root/'Makefile').write_text('all: bad sibling queued downstream\nbad:\n\tpython3 helper.py bad\nsibling:\n\tpython3 helper.py sibling\nqueued:\n\tpython3 helper.py queued\ndownstream: bad\n\tpython3 helper.py downstream\n')
  (root/'state').unlink()
  run('-j2',ok=False)
  failed=json.loads((root/'state').read_text())
  assert failed['active']==0 and ['end','sibling'] in failed['events'],failed
  assert not (root/'queued').exists() and not (root/'downstream').exists(),failed
  (root/'Makefile').write_text('all: shared broken\nbroken: broken\n')
  run('-j2',ok=False)
  (root/'Makefile').write_text('all: ignored\nignored:\n\t-exit 7\n\tpython3 helper.py ignored\n')
  run('-j2')
  assert (root/'ignored').exists()
  (root/'ignored').unlink();run('-n','-j4')
  assert not (root/'ignored').exists()
  print(f'make -j{jobs}: overlap, shared dependencies, ordering, limits, failure drain and dry run: PASS',flush=True)

for jobs in (1,2,4):case(jobs)
