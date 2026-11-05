// tests/js/gen.js — генераторы и async/await (вывод сравнивается с gen.expected)
function* g(n) { for (let i = 0; i < n; i++) { const r = yield i * 10; if (r) console.log('got', r); } return 'end'; }
var it = g(3); console.log(JSON.stringify(it.next()), JSON.stringify(it.next('hello')), JSON.stringify(it.next()), JSON.stringify(it.next()), JSON.stringify(it.next()));
console.log([...g(4)], Array.from(g(2)));
function* inner() { yield 'a'; yield 'b'; } function* outer() { yield 1; yield* inner(); yield* [8, 9]; yield 2; }
console.log([...outer()].join(','));
var obj = { *[Symbol.iterator]() { yield 1; yield 2; } }; for (const x of obj) console.log('obj', x);
class C { *items() { yield* [3, 4]; } } console.log([...new C().items()]);
function* fin() { try { yield 1; yield 2; } finally { console.log('finally ran'); } }
var f = fin(); f.next(); console.log(JSON.stringify(f.return(42)), JSON.stringify(f.next()));
function* thr() { try { yield 1; } catch (e) { console.log('caught', e); yield 'recovered'; } }
var t = thr(); t.next(); console.log(JSON.stringify(t.throw('boom')));
function* fib() { let [a, b] = [0, 1]; for (;;) { yield a; [a, b] = [b, a + b]; } }
var out = []; for (const x of fib()) { if (x > 50) break; out.push(x); } console.log(out.join(' '));
// async
var log = [];
async function delay(v) { await null; return v * 2; }
async function main() { log.push('start'); var a = await delay(1); log.push('a=' + a); var b = await Promise.all([delay(2), delay(3)]); log.push('b=' + b); try { await Promise.reject(new Error('x')); } catch (e) { log.push('caught ' + e.message); } return 'done'; }
main().then(v => { log.push(v); console.log(log.join(' | ')); });
log.push('sync after main()');
var arrow = async () => { await 0; return 7; }; arrow().then(v => console.log('arrow', v));
for (var i = 0; i < 300; i++) (async function (k) { await k; if (k == 299) console.log('300 asyncs ok'); })(i);
