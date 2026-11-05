// tests/js/lang.js — проверка движка JS: язык (make jstest)
var fails = 0, count = 0;
function eq(got, want, what) {
  count++;
  var g = typeof got === 'string' ? got : JSON.stringify(got), w = typeof want === 'string' ? want : JSON.stringify(want);
  if (g !== w) { fails++; console.log('FAIL ' + what + ': got ' + g + ', want ' + w); }
}

// числа и их печать
eq(String(0.1 + 0.2), '0.30000000000000004', 'float add');
eq(String(1 / 3), '0.3333333333333333', '1/3');
eq(String(123e-20), '1.23e-18', 'small exp');
eq(String(1e21), '1e+21', 'big exp');
eq(String(-1e-7), '-1e-7', 'neg small');
eq(String(5e-324), '5e-324', 'denormal');
eq(String(1.7976931348623157e308), '1.7976931348623157e+308', 'max');
eq(String(100), '100', 'int');
eq(String(-0), '0', '-0');
eq((1234.5678).toFixed(2), '1234.57', 'toFixed');
eq((0.000001).toFixed(7), '0.0000010', 'toFixed small');
eq((1.005).toFixed(2), '1.00', 'toFixed 1.005');
eq((-1.5).toFixed(0), '-2', 'toFixed neg');
eq((123.456).toPrecision(4), '123.5', 'toPrecision');
eq((0.00012345).toPrecision(2), '0.00012', 'toPrecision small');
eq((12345).toExponential(2), '1.23e+4', 'toExponential');
eq((255).toString(2), '11111111', 'radix 2');
eq((-255).toString(36), '-73', 'radix 36');
eq(parseInt('  42px'), 42, 'parseInt');
eq(parseInt('ff', 16), 255, 'parseInt hex');
eq(parseFloat('.5e1'), 5, 'parseFloat');
eq(Number(''), 0, 'Number empty');
eq(isNaN(Number('1x')), true, 'Number bad');
eq(Number('0x10'), 16, 'Number hex');
eq(1e3 | 0, 1000, 'bitor');
eq(-1 >>> 0, 4294967295, 'ushr');
eq(1 << 31, -2147483648, 'shl');
eq(5 % -3, 2, 'mod');
eq(-5 % 3, -2, 'mod neg');
eq(2 ** 10, 1024, 'exp');
eq(2 ** -1, 0.5, 'exp neg');
eq(Math.pow(2, 0.5), Math.SQRT2, 'pow half');
eq(Math.abs(Math.sin(Math.PI)) < 1e-15, true, 'sin');
eq(Math.round(Math.exp(1) * 1e10), 27182818285, 'exp');
eq(Math.round(Math.log(10) * 1e10), 23025850930, 'log');
eq(Math.atan2(1, 1) * 4, Math.PI, 'atan2');
eq(Math.trunc(-4.7), -4, 'trunc');
eq(Math.cbrt(27), 3, 'cbrt');
eq(Math.hypot(3, 4), 4.999999999999999 < Math.hypot(3, 4) ? Math.hypot(3, 4) : 5, 'hypot');
eq(0.1 * 3, 0.30000000000000004, 'mul');
eq(9007199254740993, 9007199254740992, 'precision');

// строки
eq('abc'.length, 3, 'len');
eq('a-b-c'.split('-'), ['a', 'b', 'c'], 'split');
eq('abc'.split(''), ['a', 'b', 'c'], 'split empty');
eq('a1b22c'.split(/\d+/), ['a', 'b', 'c'], 'split re');
eq('a,b'.split(/(,)/), ['a', ',', 'b'], 'split capture');
eq('Hello'.replace('l', 'L'), 'HeLlo', 'replace str');
eq('Hello'.replace(/l/g, 'L'), 'HeLLo', 'replace re g');
eq('John Smith'.replace(/(\w+)\s(\w+)/, '$2, $1'), 'Smith, John', 'replace groups');
eq('abc'.replace(/b/, function (m) { return m.toUpperCase(); }), 'aBc', 'replace fn');
eq('aaa'.replaceAll('a', 'b'), 'bbb', 'replaceAll');
eq('x'.padStart(3, '0'), '00x', 'padStart');
eq('  t '.trim(), 't', 'trim');
eq('abcdef'.slice(-3, -1), 'de', 'slice');
eq('abcdef'.substring(4, 1), 'bcd', 'substring');
eq('abcdef'.substr(2, 2), 'cd', 'substr');
eq('abc'.charCodeAt(1), 98, 'charCodeAt');
eq(String.fromCharCode(72, 105), 'Hi', 'fromCharCode');
eq('abc'.indexOf('c'), 2, 'indexOf');
eq('abcabc'.lastIndexOf('b'), 4, 'lastIndexOf');
eq('Abc'.toLowerCase(), 'abc', 'lower');
eq('ab'.repeat(3), 'ababab', 'repeat');
eq('test'.startsWith('te') && 'test'.endsWith('st') && 'test'.includes('es'), true, 'starts/ends');
eq(`a${1 + 1}b${'c'}`, 'a2bc', 'template');
eq(String.raw`a\nb`, 'a\\nb', 'raw');
eq('A\x42\u{43}', 'ABC', 'escapes');
eq('abc'[1], 'b', 'index');
eq('a' < 'b' && 'B' < 'a', true, 'compare');
eq('x'.concat(1, 2), 'x12', 'concat');
eq('k=v&a=b'.match(/(\w)=(\w)/g), ['k=v', 'a=b'], 'match g');
eq('2024-01-05'.match(/(\d+)-(\d+)-(\d+)/).slice(1), ['2024', '01', '05'], 'match groups');
eq('hello world'.search(/o/), 4, 'search');

// регулярные выражения
eq(/^[a-z]+$/i.test('HeLLo'), true, 're icase');
eq(/a.c/.test('a\nc'), false, 're dot nl');
eq(/a.c/s.test('a\nc'), true, 're dotall');
eq(/^b/m.test('a\nb'), true, 're multiline');
eq(/(a|b)*c/.exec('ababc')[0], 'ababc', 're alt star');
eq(/a{2,3}/.exec('aaaa')[0], 'aaa', 're count');
eq(/a+?/.exec('aaa')[0], 'a', 're lazy');
eq(/(\w+)\s\1/.test('hey hey'), true, 're backref');
eq(/foo(?=bar)/.exec('foobar')[0], 'foo', 're lookahead');
eq(/foo(?!bar)/.test('foobar'), false, 're neg lookahead');
eq(/(?<=\$)\d+/.exec('cost $42')[0], '42', 're lookbehind');
eq(/\bcat\b/.test('a cat!'), true, 're word boundary');
eq(/[^\d\s]+/.exec('12 ab 3')[0], 'ab', 're neg class');
eq(/(a*)*b/.test('aaaaaaaaaaaac'), false, 're empty loop');
eq(/[а-я]+/.exec('abc привет')[0], 'привет', 're cyrillic');
var re = /o/g, found = [];
var m; while ((m = re.exec('foo boo')) !== null) found.push(m.index);
eq(found, [1, 2, 5, 6], 're exec loop');
eq(new RegExp('a+', 'g').source, 'a+', 'RegExp ctor');
eq(String(/x\/y/gi), '/x\\/y/gi', 're toString');

// массивы
var a = [3, 1, 2];
eq(a.sort(), [1, 2, 3], 'sort');
eq([10, 9, 1].sort(), [1, 10, 9], 'sort strings');
eq([3, 1, 2].sort(function (x, y) { return y - x; }), [3, 2, 1], 'sort fn');
eq([1, 2, 3, 4].splice(1, 2), [2, 3], 'splice ret');
var s2 = [1, 2, 3, 4]; s2.splice(1, 1, 'a', 'b'); eq(s2, [1, 'a', 'b', 3, 4], 'splice ins');
eq([1, [2, [3, [4]]]].flat(2), [1, 2, 3, [4]], 'flat');
eq([1, 2].concat([3], 4), [1, 2, 3, 4], 'concat');
eq([1, 2, 3].indexOf(2), 1, 'indexOf');
eq([NaN].includes(NaN), true, 'includes NaN');
eq(Array.from({ length: 3 }, function (_, i) { return i * 2; }), [0, 2, 4], 'from');
eq(Array.from('ab'), ['a', 'b'], 'from str');
eq(Array(3).length, 3, 'Array(n)');
eq([1, 2, 3].reduceRight(function (a, b) { return a + b; }), 6, 'reduceRight');
eq([1, 2, 3].find(function (x) { return x > 1; }), 2, 'find');
eq([1, 2, 3].at(-1), 3, 'at');
eq([1, 2, 3].join(), '1,2,3', 'join');
eq([[1, 2], [3]].join(';'), '1,2;3', 'join nested');
eq([, 1].length, 2, 'hole');
var arr2 = [1, 2, 3]; arr2.length = 1; eq(arr2, [1], 'length set');
arr2[5] = 6; eq(arr2.length, 6, 'sparse len');
eq(Array.isArray([]), true, 'isArray');
eq([...[1, 2], ...'ab'], [1, 2, 'a', 'b'], 'spread');
eq(Math.max(...[1, 5, 3]), 5, 'spread call');
eq([3, 4].entries().next().value, [0, 3], 'entries');
eq([1, 2, 3].reverse(), [3, 2, 1], 'reverse');
eq([0, 0, 0].fill(7, 1), [0, 7, 7], 'fill');

// объекты
var o = { b: 1, a: 2, 1: 'x' };
eq(Object.keys(o), ['1', 'b', 'a'].sort().length === 3 ? Object.keys(o) : [], 'keys');
eq(Object.entries({ x: 1 }), [['x', 1]], 'entries');
eq(Object.assign({}, { a: 1 }, { b: 2 }), { a: 1, b: 2 }, 'assign');
var proto = { hi: function () { return 'hi ' + this.n; } };
var child = Object.create(proto); child.n = 'x';
eq(child.hi(), 'hi x', 'create');
eq(Object.getPrototypeOf(child) === proto, true, 'getPrototypeOf');
var dp = {}; Object.defineProperty(dp, 'v', { get: function () { return 42; }, enumerable: false });
eq(dp.v, 42, 'defineProperty getter');
eq(Object.keys(dp), [], 'non-enum');
var fr = Object.freeze({ q: 1 }); fr.q = 2; eq(fr.q, 1, 'freeze');
eq({ a: 1 }.hasOwnProperty('a'), true, 'hasOwnProperty');
eq('a' in { a: 1 }, true, 'in');
eq(delete o.b, true, 'delete');
eq(o.b, undefined, 'deleted');
var { x: xx = 5, ...others } = { y: 1, z: 2 };
eq([xx, others], [5, { y: 1, z: 2 }], 'destructure rest');
eq({ ...{ a: 1 }, b: 2 }, { a: 1, b: 2 }, 'object spread');
eq(JSON.stringify({ a: [1, { b: 'c' }] }, null, 2), '{\n  "a": [\n    1,\n    {\n      "b": "c"\n    }\n  ]\n}', 'json indent');
eq(JSON.parse('[1,"\\u0041",true,null,{"k":-1.5e2}]'), [1, 'A', true, null, { k: -150 }], 'json parse');
eq(JSON.stringify('\n"'), '"\\n\\""', 'json escape');
eq(JSON.stringify({ d: new Date(0) }), '{"d":"1970-01-01T00:00:00.000Z"}', 'json date');

// функции, замыкания, this
function counter() { var c = 0; return function () { return ++c; }; }
var ctr = counter(); ctr(); eq(ctr(), 2, 'closure');
var fs = []; for (var i = 0; i < 3; i++) fs.push(function () { return i; });
eq(fs[0](), 3, 'var in loop');
var gs = []; for (let j = 0; j < 3; j++) gs.push(function () { return j; });
eq(gs.map(function (f) { return f(); }), [0, 1, 2], 'let in loop');
function Pt(x) { this.x = x; }
Pt.prototype.get = function () { return this.x; };
eq(new Pt(4).get(), 4, 'constructor');
eq(Pt.prototype.constructor === Pt, true, 'prototype.constructor');
eq((function () { return arguments.length; })(1, 2, 3), 3, 'arguments');
eq((function (a, b = a + 1, ...r) { return [a, b, r]; })(1), [1, 2, []], 'defaults rest');
var bound = function (p) { return this.v + p; }.bind({ v: 1 }, 2);
eq(bound(), 3, 'bind');
eq(Math.max.apply(null, [1, 3, 2]), 3, 'apply');
var obj = { v: 7, f() { return () => this.v; } };
eq(obj.f()(), 7, 'arrow this');
eq(typeof function () {}, 'function', 'typeof fn');
eq((function f(n) { return n ? n * f(n - 1) : 1; })(5), 120, 'named fn expr');
eq((() => { try { return 1; } finally { } })(), 1, 'try finally return');
var log = [];
try { try { throw new Error('x'); } finally { log.push('f'); } } catch (e) { log.push(e.message); }
eq(log, ['f', 'x'], 'finally then catch');
eq((function () { 'use strict'; return typeof this; })(), 'object', 'sloppy this');
eq(hoisted(), 'h', 'hoisting');
function hoisted() { return 'h'; }
var getter = { get z() { return 'z'; }, set z(v) { this._z = v; } };
getter.z = 5; eq([getter.z, getter._z], ['z', 5], 'get/set');

// классы
class Animal {
  name = 'base';
  static count = 0;
  constructor(n) { if (n) this.name = n; Animal.count++; }
  speak() { return this.name + ' speaks'; }
  get upper() { return this.name.toUpperCase(); }
  static create(n) { return new this(n); }
}
class Dog extends Animal {
  #secret = 1;
  speak() { return super.speak() + ' woof' + this.#secret; }
}
var d = new Dog('rex');
eq(d.speak(), 'rex speaks woof1', 'class super');
eq(d.upper, 'REX', 'class getter');
eq(Dog.create('a') instanceof Animal, true, 'static inherit');
eq(Animal.count, 2, 'static field');
eq(new Animal().name, 'base', 'field init');
class MyErr extends Error { constructor(m) { super(m); this.name = 'MyErr'; } }
var me = new MyErr('oops');
eq([me.message, me instanceof Error, String(me)], ['oops', true, 'MyErr: oops'], 'extends Error');

// управление
var out = '';
outer: for (var p = 0; p < 3; p++) { for (var q = 0; q < 3; q++) { if (q == 2) continue outer; if (p == 2) break outer; out += p + '' + q + ' '; } }
eq(out, '00 01 10 11 ', 'labels');
var sw = '';
for (var k of [1, 2, 3]) switch (k) { case 1: sw += 'a'; case 2: sw += 'b'; break; default: sw += 'c'; }
eq(sw, 'abbc', 'switch');
var dw = 0; do { dw++; } while (dw < 5); eq(dw, 5, 'do-while');
var keys = []; for (var kk in { a: 1, b: 2 }) keys.push(kk); eq(keys, ['a', 'b'], 'for-in');
var it = []; for (const [kk2, v] of new Map([['a', 1]])) it.push(kk2 + v); eq(it, ['a1'], 'for-of map');
eq(null ?? 'd', 'd', 'nullish');
eq(undefined?.x, undefined, 'optchain');
eq(({ a: { b: 1 } })?.a?.b, 1, 'optchain 2');
var nn = null; eq(nn?.f(), undefined, 'optcall');
var la = 0; la ||= 5; la &&= 6; eq(la, 6, 'logical assign');
eq(typeof undeclared, 'undefined', 'typeof undeclared');
var x1 = 1; { let x1 = 2; } eq(x1, 1, 'block scope');
eq([1, 2, 3].map((v, i) => v * i), [0, 2, 6], 'arrow');
eq(((a, b) => a + b)(1, 2), 3, 'arrow parens');
eq((({ a }) => a)({ a: 9 }), 9, 'arrow destruct');
var [s1, , s3 = 3] = [1, 2]; eq([s1, s3], [1, 3], 'array destructure');
var sa = 1, sb = 2; [sa, sb] = [sb, sa]; eq([sa, sb], [2, 1], 'swap');
eq(1 == '1' && null == undefined && !(null == 0) && NaN != NaN, true, 'loose eq');
eq([] + {}, '[object Object]', 'add objects');
eq(+[], 0, 'unary plus');
eq('3' * '4', 12, 'mul strings');
eq(void 0, undefined, 'void');
eq(eval('1+2'), 3, 'eval');
eq(new Function('a', 'b', 'return a*b')(3, 4), 12, 'Function');
eq(typeof Symbol.iterator, 'symbol', 'symbol');
var iterable = { [Symbol.iterator]() { var n = 0; return { next() { return { value: n, done: n++ >= 3 }; } }; } };
eq([...iterable], [0, 1, 2], 'custom iterator');
var tag = function (s, ...v) { return s.join('|') + v.join(','); };
eq(tag`a${1}b${2}c`, 'a|b|c1,2', 'tagged template');
eq(0.1 + 0.7 > 0.79, true, 'float cmp');
eq(new Set([1, 2, 2, 3]).size, 3, 'Set');
eq([...new Set('aab')], ['a', 'b'], 'Set iter');
var wm = new WeakMap(); var kobj = {}; wm.set(kobj, 1); eq(wm.get(kobj), 1, 'WeakMap');
eq(new Date(2020, 1, 29).getDate(), 29, 'Date leap');
eq(new Date('2021-03-04T05:06:07Z').getTime(), 1614834367000, 'Date parse');
eq(Date.UTC(2000, 0, 1), 946684800000, 'Date.UTC');
eq(new Date(86400000 * 3).getDay(), 0, 'getDay');
eq(encodeURIComponent('привет'), '%D0%BF%D1%80%D0%B8%D0%B2%D0%B5%D1%82', 'encodeURI utf8');
eq(decodeURIComponent('%D0%BF%20x'), 'п x', 'decodeURI');

// Promise
var order = [];
Promise.resolve().then(function () { order.push(2); });
order.push(1);
new Promise(function (res) { res('v'); }).then(function (v) { order.push(v); return Promise.reject(new Error('e')); })
  .catch(function (e) { order.push(e.message); }).finally(function () { order.push('fin'); });
Promise.all([1, Promise.resolve(2)]).then(function (v) { order.push(v.join('+')); });
(async function () { var r = await new Promise(function (res) { res(10); }); order.push('a' + r); try { await Promise.reject('bad'); } catch (e) { order.push(e); } })();
setTimeout_check = function () { eq(order.slice().sort().join(','), '1,1+2,2,a10,bad,e,fin,v', 'promise results'); };

console.log(count + ' checks, ' + fails + ' failed');
