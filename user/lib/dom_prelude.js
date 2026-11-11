// dom_prelude.js — часть окружения браузера, которую проще написать на самом JS.
// Выполняется перед скриптами каждой страницы. __myos — встроенные помощники из domjs.c.
(function (w) {
  var my = w.__myos;
  class Blob {
    constructor(parts, options) {
      var chunks = [], size = 0;
      Array.from(parts || []).forEach(function (part) {
        var bytes;
        if (part instanceof Blob) bytes = part._bytes;
        else if (part instanceof ArrayBuffer) bytes = new Uint8Array(part);
        else if (part instanceof Uint8Array) bytes = part;
        else if (ArrayBuffer.isView(part)) bytes = new Uint8Array(part.buffer).subarray(part.byteOffset,part.byteOffset+part.byteLength);
        else {
          bytes=[]; var str=String(part);
          for(var k=0;k<str.length;k++) { var cp=str.charCodeAt(k);
            if(cp>=128) k += cp<2048 ? 1 : cp<65536 ? 2 : 3;
            if(cp>=55296 && cp<=56319 && k+1<str.length) {var low=str.charCodeAt(k+1);if(low>=56320 && low<=57343){cp=65536+((cp-55296)<<10)+low-56320;k++;}}
            if(cp>=55296 && cp<=57343)cp=65533;
            if(cp<128)bytes.push(cp);else if(cp<2048)bytes.push(192|(cp>>6),128|(cp&63));else if(cp<65536)bytes.push(224|(cp>>12),128|((cp>>6)&63),128|(cp&63));else bytes.push(240|(cp>>18),128|((cp>>12)&63),128|((cp>>6)&63),128|(cp&63));
          }
        }
        bytes = new Uint8Array(bytes); chunks.push(bytes); size += bytes.length;
      });
      this._bytes = new Uint8Array(size);
      var offset = 0, self = this;
      chunks.forEach(function (b) { self._bytes.set(b, offset); offset += b.length; });
      var type = String(options && options.type || '');
      this._type = /[^\x20-\x7e]/.test(type) ? '' : type.toLowerCase();
    }
    get size() { return this._bytes.length; }
    get type() { return this._type; }
    text() {
      var text = '', b = this._bytes;
      for (var i = 0; i < b.length;) {
        var c = b[i++], count = c < 128 ? 0 : c >= 194 && c < 224 ? 1 : c < 240 && c >= 224 ? 2 : c >= 240 && c <= 244 ? 3 : -1;
        if(count < 0) { text += '\ufffd'; continue; }
        var cp = count ? c & (127 >> count) : c, valid = true;
        for(var j=0;j<count;j++) { if(i>=b.length || (b[i]&192)!==128) { valid=false; break; } cp=(cp<<6)|(b[i++]&63); }
        if(!valid || (count===1 && cp<128) || (count===2 && cp<2048) || (count===3 && cp<65536) || cp>1114111 || (cp>=55296 && cp<=57343)) text+='\ufffd';
        else if(cp<65536) text+=String.fromCharCode(cp);
        else {cp-=65536;text+=String.fromCharCode(55296+(cp>>10),56320+(cp&1023));}
      }
      return Promise.resolve(text);
    }
    arrayBuffer() { var buffer=new ArrayBuffer(this.size); buffer._b=Array.from(this._bytes); return Promise.resolve(buffer); }
    bytes() { return Promise.resolve(new Uint8Array(this._bytes)); }
    slice(start, end, type) {
      var size = this.size;
      function bound(n, fallback) { n = n === undefined ? fallback : Math.trunc(Number(n)) || 0; return n < 0 ? Math.max(size + n, 0) : Math.min(n, size); }
      start = bound(start, 0); end = Math.max(start, bound(end, size));
      return new Blob([this._bytes.subarray(start, end)], {type:type});
    }
  }
  w.Blob = Blob;
  function def(o, name, v) { Object.defineProperty(o, name, { value: v, writable: true, configurable: true, enumerable: false }); }

  // ---------------------------------------------------------------- события
  class Event {
    constructor(type, init) {
      init = init || {};
      this.type = String(type);
      this.bubbles = !!init.bubbles;
      this.cancelable = !!init.cancelable;
      this.composed = !!init.composed;
      this.defaultPrevented = false;
      this.target = null;
      this.currentTarget = null;
      this.srcElement = null;
      this.eventPhase = 0;
      this.isTrusted = false;
      this.timeStamp = Date.now();
      this.cancelBubble = false;
      this.returnValue = true;
    }
    preventDefault() { if (this.cancelable) { this.defaultPrevented = true; this.returnValue = false; } }
    stopPropagation() { this.cancelBubble = true; }
    stopImmediatePropagation() { this.cancelBubble = true; this.__stopNow = true; }
    composedPath() { var p = [], n = this.target; while (n) { p.push(n); n = n.parentNode; } p.push(w); return p; }
    initEvent(type, bubbles, cancelable) { this.type = type; this.bubbles = !!bubbles; this.cancelable = !!cancelable; }
  }
  Event.NONE = 0; Event.CAPTURING_PHASE = 1; Event.AT_TARGET = 2; Event.BUBBLING_PHASE = 3;
  class CustomEvent extends Event {
    constructor(t, i) { super(t, i); this.detail = i && i.detail !== undefined ? i.detail : null; }
    initCustomEvent(t, b, c, d) { this.initEvent(t, b, c); this.detail = d; }
  }
  class UIEvent extends Event { constructor(t, i) { super(t, i); this.view = w; this.detail = (i && i.detail) || 0; } }
  class MouseEvent extends UIEvent {
    constructor(t, i) {
      super(t, i); i = i || {};
      this.button = i.button || 0; this.buttons = i.buttons || 0;
      this.clientX = i.clientX || 0; this.clientY = i.clientY || 0; this.pageX = this.clientX; this.pageY = this.clientY;
      this.screenX = 0; this.screenY = 0; this.offsetX = 0; this.offsetY = 0;
      this.ctrlKey = !!i.ctrlKey; this.shiftKey = !!i.shiftKey; this.altKey = !!i.altKey; this.metaKey = !!i.metaKey;
      this.relatedTarget = i.relatedTarget || null; this.which = this.button + 1;
    }
  }
  class KeyboardEvent extends UIEvent {
    constructor(t, i) {
      super(t, i); i = i || {};
      this.key = i.key || ''; this.code = i.code || ''; this.keyCode = i.keyCode || 0; this.charCode = i.charCode || 0;
      this.which = this.keyCode; this.ctrlKey = !!i.ctrlKey; this.shiftKey = !!i.shiftKey; this.altKey = !!i.altKey;
      this.metaKey = !!i.metaKey; this.repeat = false; this.location = 0; this.isComposing = false;
    }
    getModifierState() { return false; }
  }
  class FocusEvent extends UIEvent { constructor(t, i) { super(t, i); this.relatedTarget = null; } }
  class InputEvent extends UIEvent { constructor(t, i) { super(t, i); i = i || {}; this.data = i.data === undefined ? null : i.data; this.inputType = i.inputType || ''; } }
  class PopStateEvent extends Event { constructor(t, i) { super(t, i); this.state = i ? i.state : null; } }
  class HashChangeEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.oldURL = i.oldURL || ''; this.newURL = i.newURL || ''; } }
  class ErrorEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.message = i.message || ''; this.error = i.error || null; } }
  class ProgressEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.loaded = i.loaded || 0; this.total = i.total || 0; this.lengthComputable = !!i.total; } }
  class MessageEvent extends Event { constructor(t, i) { super(t, i); this.data = i ? i.data : undefined; this.origin = ''; } }
  w.Event = Event; w.CustomEvent = CustomEvent; w.UIEvent = UIEvent; w.MouseEvent = MouseEvent;
  w.PointerEvent = MouseEvent; w.KeyboardEvent = KeyboardEvent; w.FocusEvent = FocusEvent; w.InputEvent = InputEvent;
  w.PopStateEvent = PopStateEvent; w.HashChangeEvent = HashChangeEvent; w.ErrorEvent = ErrorEvent;
  w.ProgressEvent = ProgressEvent; w.MessageEvent = MessageEvent; w.TouchEvent = UIEvent; w.WheelEvent = MouseEvent;
  w.AnimationEvent = Event; w.TransitionEvent = Event; w.SubmitEvent = Event; w.PageTransitionEvent = Event;
  document.createEvent = function (k) { var e = new Event(''); e.initCustomEvent = CustomEvent.prototype.initCustomEvent; return e; };
  class DOMException extends Error { constructor(message, name) { super(message || ''); this.name=name || 'Error'; } }
  class EventTarget {
    constructor() { this._listeners=[]; }
    addEventListener(type, callback, options) { if(callback && !this._listeners.some(p=>p.type===type && p.callback===callback)) this._listeners.push({type:String(type),callback:callback,once:!!(options && options.once)}); }
    removeEventListener(type, callback) { this._listeners=this._listeners.filter(p=>p.type!==type || p.callback!==callback); }
    dispatchEvent(event) {
      event.target=event.currentTarget=this;
      this._listeners.slice().forEach(p=>{if(p.type!==event.type)return;if(p.once)this.removeEventListener(p.type,p.callback);if(typeof p.callback==='function')p.callback.call(this,event);else p.callback.handleEvent(event);});
      const handler=this['on'+event.type];if(typeof handler==='function')handler.call(this,event);
      return !event.defaultPrevented;
    }
  }
  class AbortSignal extends EventTarget {
    constructor(){super();this.aborted=false;this.reason=undefined;this.onabort=null;}
    throwIfAborted(){if(this.aborted)throw this.reason;}
    _abort(reason){if(this.aborted)return;this.aborted=true;this.reason=reason===undefined?new DOMException('The operation was aborted','AbortError'):reason;this.dispatchEvent(new Event('abort'));}
    static abort(reason){const s=new AbortSignal();s._abort(reason);return s;}
    static timeout(ms){const s=new AbortSignal();setTimeout(()=>s._abort(new DOMException('The operation timed out','TimeoutError')),ms);return s;}
    static any(signals){const out=new AbortSignal();for(const s of signals){if(s.aborted){out._abort(s.reason);break;}s.addEventListener('abort',()=>out._abort(s.reason),{once:true});}return out;}
  }
  class AbortController {constructor(){this.signal=new AbortSignal();}abort(reason){this.signal._abort(reason);}}
  w.DOMException=DOMException;w.EventTarget=EventTarget;w.AbortSignal=AbortSignal;w.AbortController=AbortController;
  w.NodeFilter={FILTER_ACCEPT:1,FILTER_REJECT:2,FILTER_SKIP:3,SHOW_ALL:4294967295,SHOW_ELEMENT:1,SHOW_TEXT:4,SHOW_COMMENT:128,SHOW_DOCUMENT:256,SHOW_DOCUMENT_FRAGMENT:1024};
  document.createTreeWalker=function(root,whatToShow,filter){
    const walker={root:root,currentNode:root,whatToShow:whatToShow===undefined?4294967295:whatToShow,filter:filter||null};
    function accept(n){if(!(walker.whatToShow & (1<<(n.nodeType-1))))return 3;return !filter?1:typeof filter==='function'?filter(n):filter.acceptNode(n);}
    function nodes(){const out=[root],stack=[];for(let n=root.lastChild;n;n=n.previousSibling)stack.push(n);while(stack.length){const n=stack.pop(),result=accept(n);if(result===1)out.push(n);if(result!==2)for(let c=n.lastChild;c;c=c.previousSibling)stack.push(c);}return out;}
    walker.nextNode=function(){const all=nodes(),i=all.indexOf(this.currentNode);if(i>=0 && i+1<all.length)return this.currentNode=all[i+1];return null;};
    walker.previousNode=function(){const all=nodes(),i=all.indexOf(this.currentNode);if(i>0)return this.currentNode=all[i-1];return null;};
    walker.parentNode=function(){let n=this.currentNode;while(n && n!==root){n=n.parentNode;if(n && (n===root||accept(n)===1))return this.currentNode=n;}return null;};
    walker.firstChild=function(){const parent=this.currentNode,all=nodes();for(const n of all){if(n!==parent && n.parentNode===parent)return this.currentNode=n;}return null;};
    walker.lastChild=function(){const parent=this.currentNode,all=nodes();for(let i=all.length-1;i>=0;i--)if(all[i].parentNode===parent)return this.currentNode=all[i];return null;};
    walker.nextSibling=function(){if(this.currentNode===root)return null;for(let n=this.currentNode.nextSibling;n;n=n.nextSibling)if(accept(n)===1)return this.currentNode=n;return null;};
    walker.previousSibling=function(){if(this.currentNode===root)return null;for(let n=this.currentNode.previousSibling;n;n=n.previousSibling)if(accept(n)===1)return this.currentNode=n;return null;};
    return walker;
  };
  document.createNodeIterator=function(root,show,filter){const walker=document.createTreeWalker(root,show,filter);let started=false;return {root:root,whatToShow:walker.whatToShow,filter:filter||null,nextNode:function(){if(!started){started=true;if((walker.whatToShow&(1<<(root.nodeType-1))) && (!filter || (typeof filter==='function'?filter(root):filter.acceptNode(root))===1))return root;}return walker.nextNode();},previousNode:function(){return walker.previousNode();},detach:function(){}};};
  if(w.Element) w.Element.prototype.animate=function(frames,options){
    options=typeof options==='number'?{duration:options}:options||{};
    const animation=new EventTarget(), target=this;
    animation.startTime=Date.now();animation.currentTime=0;animation.playState='running';animation.playbackRate=1;
    animation.effect={target:target,getTiming:()=>options,getComputedTiming:()=>Object.assign({progress:animation.playState==='finished'?1:0,endTime:Number(options.duration)||0},options)};
    animation.ready=Promise.resolve(animation);
    let resolveFinished;
    animation.finished=new Promise(resolve=>{resolveFinished=resolve;});
    const finish=()=>{if(animation.playState==='finished')return;animation.playState='finished';animation.currentTime=Number(options.duration)||0;clearTimeout(animation._timer);resolveFinished(animation);animation.dispatchEvent(new Event('finish'));};
    animation.finish=finish;animation.cancel=()=>{clearTimeout(animation._timer);animation.playState='idle';animation.dispatchEvent(new Event('cancel'));};
    animation.pause=()=>{clearTimeout(animation._timer);animation.playState='paused';};
    animation.play=()=>{animation.playState='running';animation._timer=setTimeout(finish,Math.max(0,Number(options.duration)||0));};
    animation.reverse=()=>{animation.playbackRate=-animation.playbackRate;animation.play();};animation.commitStyles=()=>{};
    animation.play();return animation;
  };

  // ---------------------------------------------------------------- адреса
  function encodeQ(s) { return encodeURIComponent(s).replace(/%20/g, '+'); }
  function decodeQ(s) { return decodeURIComponent(String(s).replace(/\+/g, ' ')); }
  class URLSearchParams {
    constructor(init) {
      this._l = [];
      if (init instanceof URLSearchParams) { this._l = init._l.map(function (p) { return p.slice(); }); return; }
      if (init && typeof init === 'object') {
        if (Array.isArray(init)) for (var p of init) this._l.push([String(p[0]), String(p[1])]);
        else for (var k in init) this._l.push([k, String(init[k])]);
        return;
      }
      var s = init === undefined || init === null ? '' : String(init);
      if (s[0] === '?') s = s.slice(1);
      for (var part of s.split('&')) {
        if (!part) continue;
        var i = part.indexOf('=');
        this._l.push(i < 0 ? [decodeQ(part), ''] : [decodeQ(part.slice(0, i)), decodeQ(part.slice(i + 1))]);
      }
    }
    get(n) { for (var p of this._l) if (p[0] === n) return p[1]; return null; }
    getAll(n) { return this._l.filter(function (p) { return p[0] === n; }).map(function (p) { return p[1]; }); }
    has(n) { return this._l.some(function (p) { return p[0] === n; }); }
    set(n, v) { var i = this._l.findIndex(function (p) { return p[0] === n; }); if (i < 0) this._l.push([n, String(v)]); else { this._l[i][1] = String(v); this._l = this._l.filter(function (p, j) { return j <= i || p[0] !== n; }); } this._upd(); }
    append(n, v) { this._l.push([String(n), String(v)]); this._upd(); }
    delete(n) { this._l = this._l.filter(function (p) { return p[0] !== n; }); this._upd(); }
    forEach(f, t) { for (var p of this._l) f.call(t, p[1], p[0], this); }
    keys() { return this._l.map(function (p) { return p[0]; })[Symbol.iterator](); }
    values() { return this._l.map(function (p) { return p[1]; })[Symbol.iterator](); }
    entries() { return this._l.map(function (p) { return [p[0], p[1]]; })[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
    sort() { this._l.sort(function (a, b) { return a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0; }); this._upd(); }
    toString() { return this._l.map(function (p) { return encodeQ(p[0]) + '=' + encodeQ(p[1]); }).join('&'); }
    get size() { return this._l.length; }
    _upd() { if (this._url) { var q = this.toString(); this._url._search = q ? '?' + q : ''; } }
  }
  class URL {
    constructor(u, base) {
      var abs = my.resolve(String(u), base === undefined ? undefined : String(base));
      if (!abs) throw new TypeError("Failed to construct 'URL': Invalid URL");
      this._set(abs);
    }
    _set(abs) {
      var m = /^([a-zA-Z][\w+.-]*:)(\/\/([^\/?#]*))?([^?#]*)(\?[^#]*)?(#.*)?$/.exec(abs) || [];
      this._protocol = (m[1] || '').toLowerCase();
      var host = m[3] || '';
      var at = host.lastIndexOf('@');
      this.username = ''; this.password = '';
      if (at >= 0) { var cred = host.slice(0, at).split(':'); this.username = cred[0]; this.password = cred[1] || ''; host = host.slice(at + 1); }
      var c = host.lastIndexOf(':');
      this._hostname = (c >= 0 && host.indexOf(']') < c ? host.slice(0, c) : host).toLowerCase();
      this._port = c >= 0 && host.indexOf(']') < c ? host.slice(c + 1) : '';
      if ((this._protocol === 'http:' && this._port === '80') || (this._protocol === 'https:' && this._port === '443')) this._port = '';
      this._pathname = m[4] || (m[2] ? '/' : '');
      this._search = m[5] && m[5] !== '?' ? m[5] : '';
      this._hash = m[6] && m[6] !== '#' ? m[6] : '';
      this._sp = null;
    }
    get protocol() { return this._protocol; } set protocol(v) { this._protocol = String(v).replace(/:?$/, ':'); }
    get hostname() { return this._hostname; } set hostname(v) { this._hostname = String(v); }
    get port() { return this._port; } set port(v) { this._port = String(v); }
    get host() { return this._hostname + (this._port ? ':' + this._port : ''); }
    set host(v) { var c = String(v).split(':'); this._hostname = c[0]; this._port = c[1] || ''; }
    get pathname() { return this._pathname; } set pathname(v) { v = String(v); this._pathname = v[0] === '/' ? v : '/' + v; }
    get search() { return this._search; }
    set search(v) { v = String(v); this._search = v && v !== '?' ? (v[0] === '?' ? v : '?' + v) : ''; if (this._sp) { this._sp._l = new URLSearchParams(this._search)._l; } }
    get hash() { return this._hash; } set hash(v) { v = String(v); this._hash = v && v !== '#' ? (v[0] === '#' ? v : '#' + v) : ''; }
    get origin() { return this._protocol + '//' + this.host; }
    get searchParams() { if (!this._sp) { this._sp = new URLSearchParams(this._search); this._sp._url = this; } return this._sp; }
    get href() {
      var auth = this.username ? this.username + (this.password ? ':' + this.password : '') + '@' : '';
      var hier = (this._protocol === 'http:' || this._protocol === 'https:' || this._protocol === 'file:' || this._hostname) ? '//' + auth + this.host : '';
      return this._protocol + hier + this._pathname + this._search + this._hash;
    }
    set href(v) { this._set(my.resolve(String(v))); }
    toString() { return this.href; }
    toJSON() { return this.href; }
    static createObjectURL() { return 'blob:myos'; }
    static revokeObjectURL() {}
    static canParse(u, b) { try { new URL(u, b); return true; } catch (e) { return false; } }
  }
  w.URL = URL; w.URLSearchParams = URLSearchParams; w.webkitURL = URL;

  // ---------------------------------------------------------------- хранилище (пока открыт браузер)
  function makeStorage(tag) {
    var st = Object.create(Storage.prototype);
    def(st, '__tag', tag);
    return st;
  }
  function Storage() { throw new TypeError('Illegal constructor'); }
  Storage.prototype.getItem = function (k) { k = String(k); return Object.prototype.hasOwnProperty.call(this, k) ? this[k] : null; };
  Storage.prototype.setItem = function (k, v) { this[String(k)] = String(v); my.store(this.__tag, JSON.stringify(Object.assign({}, this))); };
  Storage.prototype.removeItem = function (k) { delete this[String(k)]; my.store(this.__tag, JSON.stringify(Object.assign({}, this))); };
  Storage.prototype.clear = function () { for (var k of Object.keys(this)) delete this[k]; my.store(this.__tag, '{}'); };
  Storage.prototype.key = function (i) { return Object.keys(this)[i] || null; };
  Object.defineProperty(Storage.prototype, 'length', { get: function () { return Object.keys(this).length; } });
  w.Storage = Storage;
  var ls = makeStorage('local'), ss = makeStorage('session');
  try { Object.assign(ls, JSON.parse(my.load('local') || '{}')); } catch (e) {}
  try { Object.assign(ss, JSON.parse(my.load('session') || '{}')); } catch (e) {}
  w.localStorage = ls; w.sessionStorage = ss;

  // ---------------------------------------------------------------- сеть
  class Headers {
    constructor(init) {
      this._h = {};
      if (init instanceof Headers) init.forEach(function (v, k) { this.append(k, v); }, this);
      else if (Array.isArray(init)) for (var p of init) this.append(p[0], p[1]);
      else if (init) for (var k in init) this.append(k, init[k]);
    }
    append(k, v) { k = String(k).toLowerCase(); this._h[k] = this._h[k] ? this._h[k] + ', ' + v : String(v); }
    set(k, v) { this._h[String(k).toLowerCase()] = String(v); }
    get(k) { var v = this._h[String(k).toLowerCase()]; return v === undefined ? null : v; }
    has(k) { return String(k).toLowerCase() in this._h; }
    delete(k) { delete this._h[String(k).toLowerCase()]; }
    forEach(f, t) { for (var k in this._h) f.call(t, this._h[k], k, this); }
    entries() { var a = []; for (var k in this._h) a.push([k, this._h[k]]); return a[Symbol.iterator](); }
    keys() { return Object.keys(this._h)[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
  }
  class Response {
    constructor(body, init) {
      init = init || {};
      this._body = body === undefined || body === null ? '' : body;
      this._blob = body instanceof Blob ? body : new Blob([this._body]);
      this.status = init.status === undefined ? 200 : init.status;
      this.statusText = init.statusText || '';
      this.ok = this.status >= 200 && this.status < 300;
      this.headers = init.headers instanceof Headers ? init.headers : new Headers(init.headers);
      this.url = init.url || '';
      this.redirected = false;
      this.type = 'basic';
      this.bodyUsed = false;
    }
    _consume(read) { if(this.bodyUsed)return Promise.reject(new TypeError('Response body has already been consumed'));this.bodyUsed=true;return read(); }
    text() { return this._consume(()=>this._blob.text()); }
    json() { return this.text().then(text=>JSON.parse(text)); }
    blob() { return this._consume(()=>Promise.resolve(new Blob([this._blob],{type:this.headers.get('content-type')||''}))); }
    arrayBuffer() { return this._consume(()=>this._blob.arrayBuffer()); }
    clone() { if(this.bodyUsed)throw new TypeError('Response body has already been consumed');return new Response(this._body, this); }
    static json(d, i) { return new Response(JSON.stringify(d), i); }
    static error() { return new Response('', { status: 0 }); }
  }
  class Request {
    constructor(u, init) {
      init = init || {};
      if (u instanceof Request) { init = Object.assign({ method: u.method, body: u._body, headers: u.headers, signal:u.signal }, init); u = u.url; }
      this.url = my.resolve(String(u));
      this.method = (init.method || 'GET').toUpperCase();
      this.headers = new Headers(init.headers);
      this._body = init.body;
      this.credentials = init.credentials || 'same-origin';
      this.mode = init.mode || 'cors';
      this.signal = init.signal || new AbortController().signal;
    }
    clone() { return new Request(this); }
  }
  function bodyText(b) {
    if (b === undefined || b === null) return null;
    if (b instanceof URLSearchParams) return b.toString();
    if (b instanceof FormData) return b.toString();
    return String(b);
  }
  w.fetch = function (u, init) {
    var req = new Request(u, init);
    return new Promise(function (ok, fail) {
      if(req.signal.aborted){fail(req.signal.reason);return;}
      var settled=false;
      var abort=function(){if(settled)return;settled=true;clearTimeout(timer);fail(req.signal.reason);};
      req.signal.addEventListener('abort',abort,{once:true});
      var timer=setTimeout(function () {
        if(settled)return;
        var r = my.fetch(req.url, req.method, bodyText(req._body));
        req.signal.removeEventListener('abort',abort);
        if(settled)return;settled=true;
        if (!r) { fail(new TypeError('Failed to fetch')); return; }
        var h = new Headers(); if (r.type) h.set('content-type', r.type);
        var resp = new Response(r.text, { status: r.status, headers: h, url: req.url });
        ok(resp);
      }, 0);
    });
  };
  w.Headers = Headers; w.Response = Response; w.Request = Request;

  class XMLHttpRequest {
    constructor() {
      this.readyState = 0; this.status = 0; this.statusText = ''; this.responseText = ''; this.response = '';
      this.responseType = ''; this.responseURL = ''; this.timeout = 0; this.withCredentials = false;
      this._h = {}; this._l = {}; this.upload = { addEventListener: function () {} };
    }
    open(m, u, async) { this._m = String(m).toUpperCase(); this._u = my.resolve(String(u)); this._async = async !== false; this.readyState = 1; }
    setRequestHeader(k, v) { this._h[k] = v; }
    getResponseHeader(k) { return String(k).toLowerCase() === 'content-type' ? (this._type || null) : null; }
    getAllResponseHeaders() { return this._type ? 'content-type: ' + this._type + '\r\n' : ''; }
    overrideMimeType() {}
    abort() { this._aborted = true; }
    addEventListener(t, f) { (this._l[t] = this._l[t] || []).push(f); }
    removeEventListener(t, f) { var l = this._l[t]; if (l) this._l[t] = l.filter(function (x) { return x !== f; }); }
    _fire(t) {
      var ev = new ProgressEvent(t); ev.target = this; ev.currentTarget = this;
      var h = this['on' + t]; if (typeof h === 'function') h.call(this, ev);
      for (var f of (this._l[t] || []).slice()) f.call(this, ev);
    }
    send(body) {
      var x = this;
      var go = function () {
        if (x._aborted) return;
        var r = my.fetch(x._u, x._m, bodyText(body));
        x.readyState = 4;
        x.responseURL = x._u;
        if (!r) { x.status = 0; x._fire('readystatechange'); x._fire('error'); x._fire('loadend'); return; }
        x.status = r.status; x.statusText = r.status === 200 ? 'OK' : ''; x._type = r.type;
        x.responseText = r.text;
        if (x.responseType === 'json') { try { x.response = JSON.parse(r.text); } catch (e) { x.response = null; } }
        else x.response = r.text;
        x._fire('readystatechange'); x._fire('load'); x._fire('loadend');
      };
      if (this._async) setTimeout(go, 0); else go();
    }
  }
  XMLHttpRequest.UNSENT = 0; XMLHttpRequest.OPENED = 1; XMLHttpRequest.HEADERS_RECEIVED = 2; XMLHttpRequest.LOADING = 3; XMLHttpRequest.DONE = 4;
  w.XMLHttpRequest = XMLHttpRequest;

  class FormData {
    constructor(form) {
      this._l = [];
      if (form && form.elements) for (var el of form.elements) if (el.name && !el.disabled && el.type !== 'submit' && el.type !== 'button') this._l.push([el.name, el.value]);
    }
    append(k, v) { this._l.push([String(k), String(v)]); }
    set(k, v) { this.delete(k); this.append(k, v); }
    get(k) { for (var p of this._l) if (p[0] === k) return p[1]; return null; }
    getAll(k) { return this._l.filter(function (p) { return p[0] === k; }).map(function (p) { return p[1]; }); }
    has(k) { return this._l.some(function (p) { return p[0] === k; }); }
    delete(k) { this._l = this._l.filter(function (p) { return p[0] !== k; }); }
    forEach(f, t) { for (var p of this._l) f.call(t, p[1], p[0], this); }
    entries() { return this._l.map(function (p) { return [p[0], p[1]]; })[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
    toString() { return new URLSearchParams(this._l).toString(); }
  }
  w.FormData = FormData;
  w.WebSocket = function () { throw new Error('WebSocket is not supported'); };
  w.EventSource = function () { throw new Error('EventSource is not supported'); };

  // ---------------------------------------------------------------- наблюдатели (заглушки)
  class MutationObserver { constructor(f) { this._f = f; } observe() {} disconnect() {} takeRecords() { return []; } }
  class ResizeObserver { constructor(f) { this._f = f; } observe() {} unobserve() {} disconnect() {} }
  class IntersectionObserver {
    constructor(f, o) { this._f = f; this.root = null; this.rootMargin = ''; this.thresholds = [0]; }
    observe(el) {   // всё «видно» сразу — так ленивые картинки и подгрузки срабатывают
      var self = this;
      setTimeout(function () { self._f([{ target: el, isIntersecting: true, intersectionRatio: 1, boundingClientRect: {}, intersectionRect: {}, rootBounds: null, time: Date.now() }], self); }, 0);
    }
    unobserve() {} disconnect() {} takeRecords() { return []; }
  }
  w.MutationObserver = MutationObserver; w.WebKitMutationObserver = MutationObserver;
  w.ResizeObserver = ResizeObserver; w.IntersectionObserver = IntersectionObserver;
  w.PerformanceObserver = class { observe() {} disconnect() {} };

  // ---------------------------------------------------------------- окно
  var t0 = Date.now();
  w.performance = {
    now: function () { return Date.now() - t0; }, timeOrigin: t0,
    timing: { navigationStart: t0, loadEventEnd: t0, domContentLoadedEventEnd: t0, responseStart: t0 },
    mark: function () {}, measure: function () {}, getEntriesByType: function () { return []; },
    getEntriesByName: function () { return []; }, getEntries: function () { return []; }, clearMarks: function () {},
    navigation: { type: 0 }
  };
  w.requestAnimationFrame = function (f) { return setTimeout(function () { f(w.performance.now()); }, 16); };
  w.cancelAnimationFrame = function (id) { clearTimeout(id); };
  w.requestIdleCallback = function (f) { return setTimeout(function () { f({ didTimeout: false, timeRemaining: function () { return 50; } }); }, 1); };
  w.cancelIdleCallback = function (id) { clearTimeout(id); };
  var mediaLists = [];
  w.matchMedia = function (query) {
    var q = String(query), listeners = [], last = my.media(q);
    var list = {
      get matches() { return my.media(q); }, media: q, onchange: null,
      addEventListener: function (type, fn, options) {
        if (type !== 'change' || !fn) return;
        for (var i = 0; i < listeners.length; i++) if (listeners[i].fn === fn) return;
        listeners.push({fn: fn, once: !!(options && options.once)});
      },
      removeEventListener: function (type, fn) {
        if (type !== 'change') return;
        for (var i = listeners.length - 1; i >= 0; i--) if (listeners[i].fn === fn) listeners.splice(i, 1);
      },
      addListener: function (fn) { list.addEventListener('change', fn); },
      removeListener: function (fn) { list.removeEventListener('change', fn); },
      dispatchEvent: function (event) {
        if (event.type !== 'change') return true;
        var snapshot = listeners.slice();
        for (var i = 0; i < snapshot.length; i++) {
          var entry = snapshot[i];
          if (entry.once) list.removeEventListener('change', entry.fn);
          if (typeof entry.fn === 'function') entry.fn.call(list, event);
          else if (entry.fn && typeof entry.fn.handleEvent === 'function') entry.fn.handleEvent(event);
        }
        if (typeof list.onchange === 'function') list.onchange.call(list, event);
        return !event.defaultPrevented;
      }
    };
    mediaLists.push(function () {
      var matches = my.media(q);
      if (matches === last) return;
      last = matches;
      var event = new Event('change');
      event.matches = matches; event.media = q;
      event.target = event.currentTarget = list;
      list.dispatchEvent(event);
    });
    return list;
  };
  w.addEventListener('resize', function () {
    var snapshot = mediaLists.slice();
    for (var i = 0; i < snapshot.length; i++) snapshot[i]();
  });
  w.getComputedStyle = function (el) {
    var st = el && el.style ? el.style : {};
    return new Proxy_(st, el);
  };
  function Proxy_(st, el) {
    var o = Object.create(st);
    o.getPropertyValue = function (p) { return my.computed(el, String(p)); };
    ['display', 'visibility', 'color', 'backgroundColor', 'fontSize', 'fontWeight', 'fontStyle', 'textAlign',
     'opacity', 'position', 'marginTop', 'marginBottom', 'marginLeft', 'marginRight', 'paddingTop', 'paddingBottom',
     'paddingLeft', 'paddingRight'].forEach(function (k) { o[k] = my.computed(el, k); });
    return o;
  }
  w.scrollTo = w.scroll = w.scrollBy = function () {};
  w.focus = w.blur = w.print = w.stop = function () {};
  w.postMessage = function (data) { var ev = new MessageEvent('message', { data: data }); setTimeout(function () { w.dispatchEvent(ev); }, 0); };
  w.open = function (u) { if (u) location.href = u; return null; };
  w.close = function () {};
  w.history = {
    length: 1, state: null, scrollRestoration: 'auto',
    back: function () { my.go(-1); }, forward: function () { my.go(1); }, go: function (n) { if (n) my.go(n); },
    pushState: function (s, t, u) { this.state = s; if (u !== undefined && u !== null) my.setUrl(my.resolve(String(u))); this.length++; },
    replaceState: function (s, t, u) { this.state = s; if (u !== undefined && u !== null) my.setUrl(my.resolve(String(u))); }
  };
  w.crypto = {
    getRandomValues: function (a) { for (var i = 0; i < a.length; i++) a[i] = Math.floor(Math.random() * 256); return a; },
    randomUUID: function () { return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, function (c) { var r = Math.random() * 16 | 0; return (c === 'x' ? r : (r & 3 | 8)).toString(16); }); }
  };
  w.TextEncoder = class { encode(s) { var a = []; s = unescape(encodeURIComponent(String(s))); for (var i = 0; i < s.length; i++) a.push(s.charCodeAt(i)); return a; } };
  w.TextDecoder = class { decode(a) { var s = ''; if (a) for (var i = 0; i < a.length; i++) s += String.fromCharCode(a[i]); return s; } };
  w.DOMParser = class { parseFromString(s) { var d = document.createElement('html'); d.innerHTML = s; d.body = d; d.documentElement = d; return d; } };
  w.Image = function (wd, ht) { var i = document.createElement('img'); if (wd) i.width = wd; if (ht) i.height = ht; return i; };
  w.Option = function (t, v) { var o = document.createElement('option'); if (t) o.textContent = t; if (v !== undefined) o.value = v; return o; };
  w.CSS = { supports: function () { return false; }, escape: function (s) { return String(s).replace(/[^\w-]/g, '\\$&'); } };
  w.customElements = { define: function () {}, get: function () {}, whenDefined: function () { return Promise.resolve(); } };
  w.visualViewport = { width: w.innerWidth, height: w.innerHeight, scale: 1, addEventListener: function () {} };
  w.Intl = w.Intl || {
    DateTimeFormat: function () { return { format: function (d) { return new Date(d === undefined ? Date.now() : d).toLocaleString(); }, resolvedOptions: function () { return { timeZone: 'UTC', locale: 'ru' }; } }; },
    NumberFormat: function () { return { format: function (n) { return Number(n).toLocaleString(); } }; },
    Collator: function () { return { compare: function (a, b) { return a < b ? -1 : a > b ? 1 : 0; } }; },
    PluralRules: function () { return { select: function () { return 'other'; } }; },
    RelativeTimeFormat: function () { return { format: function (v, u) { return v + ' ' + u; } }; }
  };
  navigator.sendBeacon = function () { return true; };
  navigator.clipboard = { writeText: function () { return Promise.resolve(); }, readText: function () { return Promise.resolve(''); } };
  if (!document.fonts) document.fonts = { ready: Promise.resolve(), load: function () { return Promise.resolve([]); }, check: function () { return true; }, addEventListener: function () {} };
  // Module records are retained by this realm and discarded on navigation.
  var modules = Object.create(null), moduleRecords = [], moduleBytes = 0;
  function moduleURL(base, spec) {
    spec = String(spec);
    if (!/^(\.{1,2}\/|\/|https?:|file:)/.test(spec)) throw new TypeError('Bare module specifier: ' + spec);
    var url = my.resolve(spec, base);
    if (!/^(https?:|file:)/.test(url)) throw new TypeError('Unsupported module URL: ' + url);
    return url;
  }
  function moduleGetter(object, name, get) {
    if (Object.prototype.hasOwnProperty.call(object, name)) throw new SyntaxError('Duplicate module binding: ' + name);
    Object.defineProperty(object, name, { enumerable: true, get: get,
      set: function () { throw new TypeError('Module binding is read-only: ' + name); } });
  }
  function prepareModule(url, source) {
    if (modules[url]) { if (modules[url].error) throw modules[url].error; return modules[url]; }
    if (moduleRecords.length >= 128) throw new RangeError('Too many modules');
    var m = { url: url, ns: Object.create(null), imports: Object.create(null), deps: [], state: 0, error: null };
    modules[url] = m; moduleRecords.push(m);
    try {
      if (source === undefined) source = my.moduleSource(url);
      moduleBytes += source.length;
      if (moduleBytes > 6 * 1024 * 1024) throw new RangeError('Module source limit exceeded');
      var desc = my.moduleCompile(source), prefix = '';
      desc.exports.forEach(function (p) {
        prefix += '__module_export(' + JSON.stringify(p[0]) + ',function(){return ' + p[1] + ';});\n';
      });
      var factory = new Function('return function*(__module_export,__module_import,__module_meta,__module_bindings){with(__module_bindings){' + prefix +
        '\nyield;\n' + desc.code + '\n}}')();
      m.iterator = factory(function (name, get) { moduleGetter(m.ns, name, get); },
        function (spec) { return Promise.resolve().then(function () { return runModule(moduleURL(url, spec)); }); },
        { url: url, resolve: function (spec) { return moduleURL(url, spec); } }, m.imports);
      m.iterator.next();
      desc.deps.forEach(function (d) {
        var dep = prepareModule(moduleURL(url, d.url));
        m.deps.push(dep);
        d.names.forEach(function (p) {
          var target = d.reexport ? m.ns : m.imports;
          if (p[0] === '*' && p[1] === '*' && d.reexport) {
            Object.keys(dep.ns).forEach(function (name) {
              if (name !== 'default' && !Object.prototype.hasOwnProperty.call(target, name))
                moduleGetter(target, name, function () { return dep.ns[name]; });
            });
          } else {
            moduleGetter(target, p[1], function () {
              if (p[0] === '*') return dep.ns;
              if (!Object.prototype.hasOwnProperty.call(dep.ns, p[0])) throw new SyntaxError('Missing module export: ' + p[0]);
              return dep.ns[p[0]];
            });
            m.deps.push({ check: function () {
              if (p[0] !== '*' && !Object.prototype.hasOwnProperty.call(dep.ns, p[0])) throw new SyntaxError('Missing module export: ' + p[0]);
            } });
          }
        });
      });
      return m;
    } catch (e) { m.error = e; throw e; }
  }
  function evaluateModule(m) {
    if (m.error) throw m.error;
    if (m.state) return;
    m.state = 1;
    try {
      m.deps.forEach(function (dep) { if (!dep.check) evaluateModule(dep); });
      m.iterator.next(); m.state = 2;
    } catch (e) { m.error = e; throw e; }
  }
  function runModule(url, source) {
    var m = prepareModule(url, source);
    // Link errors must occur before any module body has side effects.
    var visited = [];
    function validate(record) {
      if (visited.indexOf(record) >= 0) return;
      visited.push(record);
      if (record.error) throw record.error;
      record.deps.forEach(function (d) { if (d.check) d.check(); else validate(d); });
    }
    validate(m);
    evaluateModule(m); return m.ns;
  }
  my.runModule = runModule;
  w.__import = function (spec) { return Promise.resolve().then(function () { return runModule(moduleURL(location.href, spec)); }); };
})(this);
