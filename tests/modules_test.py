"""Module graph regressions through the browser DOM host."""
import pathlib
import subprocess
import tempfile

def run(files, expected, forbidden=()):
    with tempfile.TemporaryDirectory(dir='build') as directory:
        root = pathlib.Path(directory)
        for name, source in files.items():
            (root / name).write_text(source)
        result = subprocess.run(['build/dom-host', str(root / 'page.html'), 'wait:30'],
                                text=True, capture_output=True, timeout=20)
        assert result.returncode == 0, result.stderr
        for marker in expected:
            assert marker in result.stdout, (marker, result.stdout)
        for marker in forbidden:
            assert marker not in result.stdout, (marker, result.stdout)
        return result.stdout

text = subprocess.run(['build/dom-host', 'tests/web/modules.html', 'wait:30'],
                      capture_output=True, text=True, timeout=20).stdout
for marker in ['modules-ok:42', 'live 2 2 2 1', 'inline-live 3', 'dynamic lazy-ok', 'isolated undefined']:
    assert marker in text, text
assert 'JS error' not in text, text
assert text.count('[fetch GET http://test.local/dir/module-main.js -> ok]') == 1, text

run({'page.html': '<script type=module src="a.js"></script><p>after</p>',
     'a.js': 'import {b} from "./b.js"; export function a(){return "A";} console.log("cycle",b());',
     'b.js': 'import {a} from "./a.js"; export function b(){return a()+"B";}'}, ['cycle AB'], ['JS error'])

run({'page.html': '<script type=module src="a.js"></script><script type=module src="good.js"></script>',
     'a.js': 'import {missing} from "./b.js"; console.log("BAD-SIDE-EFFECT");',
     'b.js': 'console.log("BAD-DEPENDENCY-SIDE-EFFECT"); export const exists=1;',
     'good.js': 'console.log("independent-ok");'},
    ['Missing module export: missing', 'independent-ok'], ['BAD-SIDE-EFFECT', 'BAD-DEPENDENCY-SIDE-EFFECT'])

run({'page.html': '<script type=module>import {x} from "./a.js"; x=9;</script>',
     'a.js': 'export let x=1;'}, ['read-only'])

run({'page.html': '<script type=module>import f from "./a.js"; console.log("named-default",f());</script>',
     'a.js': 'export default function named(){return 42;} export {named};'}, ['named-default 42'], ['JS error'])

run({'page.html': '<script type=module>import("./missing.js").catch(e=>console.log("rejected",e.name));</script>'},
    ['rejected TypeError'], ['JS error'])

run({'page.html': '<script type=module>import x from "package";</script>'}, ['Bare module specifier: package'])
run({'page.html': '<script type=module>import "./data.txt";</script>', 'data.txt':'export const x=1;'},
    ['Invalid module MIME type'])
print('module graph tests: OK')
async_result = subprocess.run(['build/dom-host', 'tests/web/async-generators.html', 'wait:150'], capture_output=True, text=True, timeout=20)
assert '[console] async-generators-ok' in async_result.stdout and 'ASYNC-FAIL' not in async_result.stdout, async_result.stdout
print('async generator tests: OK')
blob_result = subprocess.run(['build/dom-host', 'tests/web/blob.html', 'wait:50'], capture_output=True, text=True, timeout=20)
assert '[console] blob-ok' in blob_result.stdout and 'BLOB-FAIL' not in blob_result.stdout, blob_result.stdout
print('Blob tests: OK')
web_result = subprocess.run(['build/dom-host', 'tests/web/web-api.html', 'wait:250'], capture_output=True, text=True, timeout=20)
assert '[console] web-api-ok' in web_result.stdout and 'WEB-FAIL' not in web_result.stdout, web_result.stdout
assert 'should-not-fetch' not in web_result.stdout and 'also-not-fetched' not in web_result.stdout, web_result.stdout
print('Web API tests: OK')
run({'page.html': '<script nomodule>throw new Error("NO-MODULE-FALLBACK");</script>'
                  '<script type=MODULE>console.log("uppercase-module-ok");</script>'},
    ['uppercase-module-ok'], ['JS error', 'NO-MODULE-FALLBACK'])
