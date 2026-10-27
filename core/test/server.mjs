// server.mjs - a local stand-in for the other end of the core's connection, proving the core end to end, pure programs matching run.mjs exactly.
//
//   node test/server.mjs          then, in another terminal:   bin/test/onsay-core.exe test/onsay.json
// Tests of a lingo the core hasn't got (windows and those written on it, or hosted) are skipped, and its catalog must say why.
// input and look are groups of windows, so their tests need windows.
//
// 1. WebSocket   just enough of it, with no packages
// 2. Tests       programs and what they must give, and web requests to the core's port and down its connection
// 3. Session     hello, the tests in order, ping, then a drop the core must come back from

import http from 'node:http'
import net from 'node:net'
import crypto from 'node:crypto'
import { rmSync, cpSync, readdirSync, existsSync, mkdirSync, writeFileSync, readFileSync } from 'node:fs'
import { execFileSync, spawn } from 'node:child_process'
import { fileURLToPath } from 'node:url'
import { lingo as compiler } from '../web/compiler.js'
import { run } from './reference/run.mjs'
import { STD } from './reference/std.mjs'

const PORT = 8787, TOKEN = 'test-token'   // bin/test/onsay-core.exe connects to ws://127.0.0.1:8787/live
const HTTP = `http://127.0.0.1:${PORT}/http`   // the stand-in's pages for http.send

////////// 1. WEBSOCKET //////////

const frame = (op, data) => {
  const body = Buffer.from(data)
  const head = body.length < 126 ? Buffer.from([0x80 | op, body.length])
    : body.length < 65536 ? Buffer.from([0x80 | op, 126, body.length >> 8, body.length & 255])
    : Buffer.concat([Buffer.from([0x80 | op, 127]), (() => { const b = Buffer.alloc(8); b.writeBigUInt64BE(BigInt(body.length)); return b })()])
  return Buffer.concat([head, body])
}

// Calls on(text) for each whole message. Pings are answered here, the way any WebSocket server does.
const socketOf = (socket, on, closed) => {
  let held = Buffer.alloc(0), parts = []
  socket.on('data', (chunk) => {
    held = Buffer.concat([held, chunk])
    for (;;) {
      if (held.length < 2) return
      const fin = held[0] & 0x80, op = held[0] & 15, masked = held[1] & 0x80
      let len = held[1] & 127, at = 2
      if (len === 126) { if (held.length < 4) return; len = held.readUInt16BE(2); at = 4 }
      else if (len === 127) { if (held.length < 10) return; len = Number(held.readBigUInt64BE(2)); at = 10 }
      const mask = masked ? held.subarray(at, at + 4) : null
      if (masked) at += 4
      if (held.length < at + len) return
      const body = Buffer.from(held.subarray(at, at + len))
      if (mask) for (let i = 0; i < body.length; i++) body[i] ^= mask[i & 3]
      held = held.subarray(at + len)
      if (op === 8) { socket.end(frame(8, '')); return }
      if (op === 9) { socket.write(frame(10, body)); continue }
      if (op === 10) continue
      parts.push(body)
      if (fin) { on(Buffer.concat(parts).toString('utf8')); parts = [] }
    }
  })
  socket.on('close', closed)
  socket.on('error', () => {})
  return { send: (x) => socket.write(frame(1, JSON.stringify(x))), drop: () => socket.destroy() }
}

////////// 2. TESTS //////////

// Each runs on the core. pure: also on run.mjs, and both must agree. check: what the core's result must be.
const TESTS = [
  { name: 'numbers', pure: true, code: 'big=1000000*1000000*1000000;return [1+2*3, 10%3, -7%3, 1/3, 0.1+0.2, 2-5, big*1000, big*100, 123456789012, 0.000001, 1/10000000, -0, 5/2, 2/3*3, 100/3]' },
  { name: 'text in text', pure: true, code: 'a=3;b="x";l=[1,"two",1.5];r={k:nothing};return "{a} and {b} and {l} and {nothing} and {true} and {r}"' },
  { name: 'lists', pure: true, code: 'l=[];for i in 1..5{l+=[i*i]};l[0]=100;l[5]=6;return [l,l[-1],l[9],length(l),slice(l,1,-1),3 in l]' },
  { name: 'copies', pure: true, code: 'a=[1,[2]];b=a;b[1][0]=9;r={x:{y:1}};s=r;s.x.y=2;return [a,b,r,s]' },
  { name: 'records', pure: true, code: 'r={b:2,a:1};r.c=3;out=[];for k,v in r{out+=["{k}={v}"]};for k in r{out+=[k]};return [out,std.keys(r),"a" in r,r.zz]' },
  { name: 'text', pure: true, code: 's="Héllo, wörld";return [length(s),slice(s,-5),slice(s,2,4),slice(s,9,99),s[1],"wö" in s,text(1.5),text([1,"a"])]' },
  { name: 'chars', pure: true, code: 'out=[];for i,c in "añb"{out+=["{i}{c}"]};return out' },
  { name: 'numbers from text', pure: true, code: 'return [number("$1,204.50"),number("12%"),number("abc"),number(" -3.5e2x")]' },
  // % has the divisor's sign and is exact, so n - n % 1 is the whole number below n, which math.round stands on.
  { name: 'remainders', pure: true, code: 'return [7%3,-7%3,7%-3,0.1%1,2.3%1,-0.1%1,0.1-0.1%1,-1.5-(-1.5%1),2.5-2.5%1,-7.25%2]' },
  { name: 'json', pure: true, code: 'x=from_json(to_json({a:[1,2.5,"é\\t"],b:nothing}));return [x,to_json(x),from_json("[1,\\"\\\\u00e9\\",-2.5e3,null]"),to_json("q\\"\\n"),std.text(1.5)]' },
  { name: 'bytes', pure: true, code: 'b=bytes("aé€😀");try{from_bytes([255])}catch e{x=e};try{from_bytes([1.5])}catch e{y=e};return [b,from_bytes(b),from_bytes([]),x,y]' },
  { name: 'functions', pure: true, code: 'fn fib(n){if n<2{return n};return fib(n-1)+fib(n-2)}fn add(a,b=10){return a+b}return [fib(20),add(1),add(1,2)]' },
  { name: 'control', pure: true, code: 'n=0;while true{n+=1;if n>10{break};if n%2==0{continue}};x=nothing;return [n,x??5,n>5?"big":"small",not false,1 and 2,nothing or "b"]' },
  { name: 'tool input', pure: true, code: 'tool greet(who, times=2){out=[];for i in 1..times{out+=["hi {who}"]};return out}', input: { who: 'Ana' } },
  { name: 'try and fail', pure: true, code: 'try{fail "boom"}catch e{a=e};try{x=1+"a"+[]}catch e{b=e};try{y=1-"a"}catch e{c=e};try{from_json("[")}catch e{d=e};return [a,b,c,d]' },
  { name: 'error at a line', pure: true, code: 'x=1\ny=2\nz=x<"a"\nreturn z' },
  { name: 'divide by zero', pure: true, code: 'return 1/0' },
  { name: 'deep loop', pure: true, code: 'n=0;for i in 1..100000{n+=i};return n' },
  { name: 'notes', code: 'log("first");log([1,2]);return 1', check: (r) => r.ok && r.value === 1 && r.log.join('|') === 'first|[1,2]' },
  { name: 'time', code: 'a=now();compute.wait(0.2);return now()-a', check: (r) => r.ok && r.value >= 0.19 && r.value < 1 },
  {
    name: 'store',
    code: 'store.put("Notes/Today","milk");store.put("notes/today","eggs");store.put("a.b",[1]);store.put("x/../y","safe");' +
      'out=[store.get("Notes/Today"),store.get("notes/today"),store.get("a.b"),store.get("nope"),store.keys("notes")];store.delete("a.b");out+=[store.get("a.b"),store.keys()];return out',
    check: (r) => r.ok && JSON.stringify(r.value) === JSON.stringify(['milk', 'eggs', '[1]', null, ['notes/today'], null, ['Notes/Today', 'notes/today', 'x/../y']]),
  },
  { name: 'steps limit', code: 'while true{}', limits: { steps: 10000 }, check: (r) => !r.ok && r.error.code === 'limit' },
  { name: 'time limit', code: 'while true{}', limits: { ms: 300 }, check: (r) => !r.ok && r.error.code === 'limit' && /too long/.test(r.error.message) },
  { name: 'stop', code: 'while true{}', stopAfter: 300, check: (r) => !r.ok && r.error.code === 'stopped' },
  { name: 'stop in a wait', code: 'compute.wait(30)', stopAfter: 300, check: (r) => !r.ok && r.error.code === 'stopped' },
  { name: 'try lets a stop through', code: 'while true{try{while true{}}catch e{}}', stopAfter: 300, check: (r) => !r.ok && r.error.code === 'stopped' },
  { name: 'deep recursion', code: 'fn f(n){return f(n+1)}return f(0)', check: (r) => !r.ok && r.error.code === 'limit' },
  { name: 'memory', code: 'l=["xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"];while true{l+=l}', check: (r) => !r.ok && r.error.code === 'limit' },
  { name: 'call', call: { phrase: 'std.length', values: ['hi there'] }, check: (r) => r.ok && r.value === 8 },
  { name: 'call with a default', call: { phrase: 'std.slice', values: ['abc', 1] }, check: (r) => r.ok && r.value === 'bc' },
  { name: 'unknown phrase', call: { phrase: 'zzz.nope', values: [] }, check: (r) => !r.ok && r.error.code === 'unknown' },
  { name: 'not a program', raw: { program: { nope: 1 } }, check: (r) => !r.ok && r.error.code === 'type' },
  // Code compiled on the device, through its Lingo lingo memo, down to its own store phrases.
  {
    name: 'compiled on the device',
    raw: { code: 'remember("milk","2 litres");remember("eggs","6");return [recall("milk"),all()]', names: ['memo'] },
    check: (r) => r.ok && JSON.stringify(r.value) === '["2 litres",["eggs","milk"]]',
  },
  { name: 'every lingo when none are named', raw: { code: 'return upper(recall("milk"))' }, check: (r) => r.ok && r.value === '2 LITRES' },
  { name: 'a compile error on the device', raw: { code: 'x=1\nremember("x")', names: ['memo'] }, check: (r) => !r.ok && r.error.code === 'compile' && r.error.line === 2 },
  // hosted, a host lingo: its phrases run in the program around the core, the way a browser extension's do in JavaScript.
  { name: 'hosted: values there and back', raw: { code: 'return hosted.echo({a:[1,"é",nothing,true],b:2.5})', names: ['hosted'] }, check: (r) => r.ok && JSON.stringify(r.value) === '{"a":[1,"é",null,true],"b":2.5}' },
  { name: 'hosted: its error', raw: { code: 'hosted.boom("no")', names: ['hosted'] }, check: (r) => !r.ok && r.error.code === 'phrase' && /boom: no/.test(r.error.message) },
  { name: 'hosted: try catches its error', raw: { code: 'try{hosted.boom("x")}catch e{return "caught "+e}', names: ['hosted'] }, check: (r) => r.ok && /^caught .*boom: x/.test(r.value) },
  { name: 'hosted: a default', raw: { code: 'return hosted.slow(0)', names: ['hosted'] }, check: (r) => r.ok && r.value === 'woke' },
  { name: 'hosted: stopped while the host works', raw: { code: 'hosted.slow(30)', names: ['hosted'] }, stopAfter: 300, check: (r) => !r.ok && r.error.code === 'stopped' },
  { name: 'hosted: a permission not given', raw: { code: 'hosted.press()', names: ['hosted'] }, check: (r) => !r.ok && r.error.code === 'denied' },
  // The Windows lingo, for real, on this computer. The test core may not act on the screen, so nothing is clicked or typed.
  { name: 'windows: list and current', code: 'ws=window.list();f=window.current();return [std.length(ws)>0,f==nothing or f.id>0,ws[0].title!=""]', check: (r) => r.ok && JSON.stringify(r.value) === '[true,true,true]' },
  { name: 'windows: controls', code: 'w=window.list()[0];c=controls.list(w,5);return [std.length(c)<=5,std.length(controls.read(w))>=0]', check: (r) => r.ok && JSON.stringify(r.value) === '[true,true]' },
  {
    name: 'windows: files',
    code: 'd="~/onsay-test";files.make_folder(d);files.write(d+"/a.txt","héllo");files.add_to(d+"/a.txt"," there");files.copy(d+"/a.txt",d+"/b.txt");files.move(d+"/b.txt",d+"/c.txt");' +
      'l=files.list(d,"*.TXT");f=files.find("~/onsay-test","c*");out=[files.read(d+"/a.txt"),files.exists(d+"/b.txt"),files.exists(d+"/c.txt"),std.length(l),f[0].name,l[0].size];' +
      'commands.run("Remove-Item -Recurse -Force (Join-Path $HOME onsay-test)");return out+[files.exists(d)]',
    check: (r) => r.ok && JSON.stringify(r.value) === '["héllo there",false,true,2,"c.txt",12,false]',
  },
  { name: 'windows: a missing file', code: 'return files.read("~/no-such-file-here.txt")', check: (r) => !r.ok && r.error.code === 'phrase' && /doesn't exist/.test(r.error.message) },
  { name: 'windows: clipboard', code: 'return std.length(clipboard.read())>=0', check: (r) => r.ok && r.value === true },
  { name: 'windows: commands', code: 'r=commands.run("Write-Output (2+3)");p=commands.running();return [r.code,r.out,std.length(p)>10]', check: (r) => r.ok && JSON.stringify(r.value) === '[0,"5",true]' },
  { name: 'windows: a command stopped', code: 'commands.run("Start-Sleep 30")', stopAfter: 1500, check: (r) => !r.ok && r.error.code === 'stopped' },
  {
    name: 'windows: screen and OCR',
    code: 's=screen.screenshot();t=screen.read_text();commands.run("Get-ChildItem (Join-Path $HOME Onsay/shots) | ? LastWriteTime -gt (Get-Date).AddMinutes(-2) | Remove-Item");' +
      'return [s.w>0,s.h>0,std.length(t)>0,".png" in s.path]',
    check: (r) => r.ok && JSON.stringify(r.value) === '[true,true,true,true]',
  },
  { name: 'windows: not allowed', code: 'keyboard.type("x")', check: (r) => !r.ok && r.error.code === 'denied' },
  // input, the windows lingo's mouse and keyboard the way a person uses them. The test core may read the screen but never act on it.
  { name: 'input: wait for words that never show', raw: { code: 'return input.wait_for_text("zq-never-on-screen-qz", nothing, 1)', names: ['input'] }, check: (r) => r.ok && r.value === null },
  {
    name: 'input: a click is refused without screen.act',
    raw: { code: 'input.click_text(text.split(text.trim(look.lines()[0])," ")[0])', names: ['input', 'look', 'core', 'text'] },
    check: (r) => !r.ok && r.error.code === 'denied',
  },
  // look, Onsay's eyes, in the windows lingo. It only reads, so it runs for real on this screen.
  { name: 'look: open windows', raw: { code: 'return look.open_windows()', names: ['look'] }, check: (r) => r.ok && r.value.length > 0 && r.value.every((w) => w.app !== undefined && w.title) },
  {
    name: 'look: a window in brief',
    raw: { code: 'return look.overview(window.list()[0])', names: ['look', 'windows'] },
    check: (r) => r.ok && typeof r.value.title === 'string' && Array.isArray(r.value.pressable) && Array.isArray(r.value.fields),
  },
  {
    name: 'look: lines, then where one is and what is near it',
    raw: { code: 'l=look.lines();w=text.split(text.trim(l[0])," ")[0];return [std.length(l)>0,std.length(look.find_text(w))>0,std.length(look.near(w,nothing,400))>=0]', names: ['look', 'core', 'text'] },
    check: (r) => r.ok && JSON.stringify(r.value) === '[true,true,true]',
  },
  // Onsay's own pointer: its mode, and the ring, which the app draws from lines on the core's output.
  { name: 'windows: the pointer mode', code: 'a=mouse.cursor();b=mouse.cursor("real");c=mouse.cursor("virtual");return [a,b,c]', check: (r) => r.ok && JSON.stringify(r.value) === '["virtual","real","virtual"]' },
  { name: 'windows: a mode it has not', code: 'mouse.cursor("ghost")', check: (r) => !r.ok && /virtual or real/.test(r.error.message) },
  {
    name: 'input: point at words on screen',
    raw: { code: 'w=text.split(text.trim(look.lines()[0])," ")[0];b=input.point_text(w,"this one",nothing,1);input.stop_pointing();return [b.w>0,b.h>0,w in b.text]', names: ['input', 'look', 'core', 'text'] },
    check: (r) => r.ok && JSON.stringify(r.value) === '[true,true,true]',
  },
  // Places in a window, wherever it is: pixels from its corner, shares of its size, and back from its far edges.
  {
    name: 'input: places in a window',
    raw: {
      code: 'w=window.list()[0];a=input.position_in(w,0.5,0.5);b=input.position_in(w,-20,-0.25);c=input.position_in(w,10,0);d=input.position_in({title:w.title},10,0);' +
        'return [a.x==w.x+math.round(w.w*0.5),a.y==w.y+math.round(w.h*0.5),b.x==w.x+w.w-20,b.y==w.y+math.round(w.h*0.75),c.x==w.x+10,c.y==w.y,d==c]',
      names: ['input', 'windows', 'core', 'math'],
    },
    check: (r) => r.ok && JSON.stringify(r.value) === '[true,true,true,true,true,true,true]',
  },
  { name: 'input: a click in a window is refused without screen.act', raw: { code: 'input.click_in(window.list()[0],0.5,0.5)', names: ['input', 'windows'] }, check: (r) => !r.ok && r.error.code === 'denied' },
  // Pictures he sees go with the result, as JPEGs, placed in the window's own pixels.
  {
    name: 'look: a picture of a window goes with the result',
    raw: { code: 'w=window.list()[0];p=look.see(w,"what is this?");return [p.picture,p.w==w.w,p.h==w.h]', names: ['look', 'windows'] },
    check: (r) => r.ok && JSON.stringify(r.value) === '[1,true,true]' && r.pictures?.length === 1 && r.pictures[0].data.startsWith('/9j/') &&
      r.pictures[0].mime === 'image/jpeg' && r.pictures[0].ask === 'what is this?' && r.pictures[0].inside === true && JSON.stringify(r.pictures[0].at) === '[0,0]',
  },
  {
    name: 'look: a closer look at part of a window',
    raw: { code: 'w=window.list()[0];look.see(w,"",{x:10,y:20,w:120,h:60});return look.see(nothing,"and the screen?").picture', names: ['look', 'windows'] },
    check: (r) => r.ok && r.value === 2 && r.pictures?.length === 2 && JSON.stringify(r.pictures[0].at) === '[10,20]' && r.pictures[0].w === 120 && !r.pictures[0].marks && r.pictures[1].marks?.length > 0 &&
      r.pictures[0].h === 60 && r.pictures[1].inside === false && r.pictures[1].of === 'the whole screen',
  },
  // Everything on screen numbered, by accessibility and OCR, inside the window, and drawn on a picture when asked.
  {
    name: 'look: elements, numbered and placed',
    raw: {
      code: 'w=window.list()[0];e=look.elements(w);ok=std.length(e)>0;for i,x in e{if x.id!=i+1 or x.x<0 or x.y<0 or x.x+x.w>w.w or x.y+x.h>w.h or x.kind==nothing{ok=false}}return ok',
      names: ['look', 'windows', 'core'],
    },
    check: (r) => r.ok && r.value === true,
  },
  {
    name: 'look: a picture with its marks, and one without',
    raw: { code: 'w=window.list()[0];look.see(w,"what?");look.see(w,"what?",nothing,false);return 1', names: ['look', 'windows'] },
    check: (r) => r.ok && r.pictures?.length === 2 && r.pictures[0].marks?.length > 0 && r.pictures[0].marks[0].id === 1 && !r.pictures[1].marks,
  },
  // setting(...): a call of lingo.setting, answered on the device as the line runs, never asked of Onsay: the value kept
  // in the test core's home (keep, below), else its lingo.json's default, else nothing. A Lingo lingo's own is under its
  // name, and a program says whose.
  {
    name: 'setting(...) of a reference, answered on the device, and nothing when unset',
    before: () => keep('github', { token: 'tok' }),
    raw: { code: 'return [setting("token", "github"), setting("nope", "github") ?? "metric"]', names: ['core'] },
    check: (r) => r.ok && same(r.value, ['tok', 'metric']) && !every.some((m) => 'key' in m),
  },
  { name: 'setting(...) outside a lingo says whose', raw: { code: 'return setting("token")', names: ['core'] }, check: (r) => !r.ok && r.error.code === 'compile' && /says whose setting/.test(r.error.message) },
  { name: 'setting(...): the default its lingo.json gives, else nothing', raw: { code: 'return [prefs.units(), prefs.name()]', names: ['prefs'] }, check: (r) => r.ok && same(r.value, ['metric', 'no name']) },
  {
    name: 'setting(...): a value kept on the device wins over the default',
    before: () => keep('prefs', { units: 'imperial', name: 'Ana' }),
    raw: { code: 'return [prefs.units(), prefs.name()]', names: ['prefs'] },
    check: (r) => r.ok && same(r.value, ['imperial', 'Ana']),
  },
  { name: 'setting(...): another lingo\'s, by its reference, from a lingo', raw: { code: 'return prefs.token()', names: ['prefs'] }, check: (r) => r.ok && r.value === 'tok' },
  {
    name: 'settings.set keeps one in its file, and settings.get gives every setting with what its lingo.json says and its value',
    raw: { code: 'settings.set("prefs", "units", "kelvin");return [prefs.units(), settings.get("prefs")]', names: ['core', 'prefs'] },
    check: (r) => r.ok && r.value[0] === 'kelvin' && same(r.value[1], [
      { name: 'units', title: 'Units', about: 'metric or imperial.', default: 'metric', required: true, value: 'kelvin' }, { name: 'name', value: 'Ana' }, { name: 'color', value: null }]) &&
      JSON.parse(readFileSync(new URL('prefs.json', SETTINGS), 'utf8')).units === 'kelvin',
  },
  {
    name: 'keep: a lingo whose lingo.json says keep reads and writes its settings with its own functions',
    raw: { code: 'a=vault.pin();settings.set("vault", "pin", "1234");return [a, vault.pin(), store.get("vault/pin"), settings.get("vault")[0].value]', names: ['core', 'vault'] },
    check: (r) => r.ok && same(r.value, ['0000', '1234', '"1234"', '1234']) && !existsSync(new URL('vault.json', SETTINGS)),
  },
  {
    name: 'the command line: --set changes one, as JSON or as text, and --settings prints them all',
    before: () => {
      const core = (...args) => execFileSync(fileURLToPath(new URL('../bin/test/onsay-core.exe', import.meta.url)), ['test/onsay.json', ...args],
        { cwd: fileURLToPath(new URL('..', import.meta.url)), encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] })
      try {
        core('--set', 'prefs', 'units', '"cli"')
        core('--set', 'prefs', 'name', 'Bo')
        shown = JSON.parse(core('--settings', 'prefs'))
      } catch (e) { shown = [e.message] }
    },
    raw: { code: 'return [prefs.units(), prefs.name()]', names: ['prefs'] },
    check: (r) => r.ok && same(r.value, ['cli', 'Bo']) && same(shown.map((s) => s.value), ['cli', 'Bo', null]) && shown[0].title === 'Units',
  },
  // openrouter: AI models with the person's own key, from its settings on this device.
  {
    name: 'openrouter: with no key, it says where to add one',
    before: () => keep('openrouter', {}),
    raw: { code: 'return openrouter.ask("hello")', names: ['core', 'openrouter'] },
    check: (r) => !r.ok && /your settings for openrouter/.test(r.error.message),
  },
  {
    name: 'openrouter: asks with the key, model and address from its settings',
    before: () => keep('openrouter', { key: 'test-key', url: 'http://127.0.0.1:8787/openrouter' }),
    raw: { code: 'return openrouter.ask("hello")', names: ['core', 'openrouter'] },
    check: (r) => r.ok && r.value === 'openai/gpt-4o-mini says: hello',
  },
  {
    name: 'openrouter: a model of its own, and a conversation',
    raw: { code: 'return openrouter.chat([{role: "system", content: "be brief"}, {role: "user", content: "hi there"}], "qwen/qwen3")', names: ['core', 'openrouter'] },
    check: (r) => r.ok && r.value === 'qwen/qwen3 says: hi there',
  },
  {
    name: 'openrouter: a key it turns down',
    before: () => keep('openrouter', { key: 'wrong', url: 'http://127.0.0.1:8787/openrouter' }),
    raw: { code: 'return openrouter.ask("hello")', names: ['core', 'openrouter'] },
    check: (r) => !r.ok && /turned the API key down/.test(r.error.message),
  },
  // http.send, the core's own web request, made by the exe itself, against the stand-in's own pages (PAGES below).
  { name: 'http: a GET, its status, headers and text', code: `r=http.send("GET","${HTTP}/hello?x=1");return [r.status,r.headers["x-test"],r.text]`, check: (r) => r.ok && same(r.value, [200, 'yes', 'hello ?x=1']) },
  {
    name: 'http: a POST with headers and a body',
    code: `r=http.send("post","${HTTP}/echo",{"X-Name":"Ada","Content-Type":"text/plain"},"héllo");return [r.status,from_json(r.text)]`,
    check: (r) => r.ok && same(r.value, [201, { method: 'POST', name: 'Ada', type: 'text/plain', body: 'héllo' }]),
  },
  { name: 'http: an answer that is not 2xx is still an answer', code: `r=http.send("GET","${HTTP}/missing");return [r.status,r.text]`, check: (r) => r.ok && same(r.value, [404, 'nothing here']) },
  { name: 'http: a redirect is followed', code: `return http.send("GET","${HTTP}/moved").text`, check: (r) => r.ok && r.value === 'hello ' },
  { name: 'http: a timeout', code: `http.send("GET","${HTTP}/slow",nothing,nothing,1)`, check: (r) => !r.ok && r.error.code === 'phrase' && /^http\.send: /.test(r.error.message) },
  { name: 'http: nothing at that address', code: 'try{http.send("GET","http://127.0.0.1:1/")}catch e{return e}', check: (r) => r.ok && /^http\.send: /.test(r.value) },
  // native: calling an OS function directly by its layout (libffi), on Windows here. Skipped where there is no native.
  { name: 'native: an OS function with no arguments, a whole number back', raw: { code: 'return native.call("kernel32.dll","GetTickCount","u32",[])', names: ['native'] }, check: (r) => r.ok && typeof r.value === 'number' && r.value > 0 },
  { name: 'native: a text argument passed in, the function reads it', raw: { code: 'return native.call("kernel32.dll","lstrlenA","i32",[{type:"text",value:"hello"}])', names: ['native'] }, check: (r) => r.ok && r.value === 5 },
  { name: 'native: a function that is not there fails in plain words', raw: { code: 'try{native.call("kernel32.dll","NoSuchFunction","i32",[])}catch e{return e}', names: ['native'] }, check: (r) => r.ok && /was not found/.test(r.value) },
  // text, list, math and time, from ../../lingo-library: what the core once did itself, written in Lingo, giving what it gave.
  {
    name: 'text: split, join, replace, trim, lower and upper',
    raw: { code: 's="Héllo, wörld";return [text.upper(s),text.lower(s),text.split(s,", "),text.join(["a","b"],"-"),text.replace(s,"l","L"),"wö" in s,list.reverse("abc"),text.trim("  x  "),text.split("abc",""),text.split("a,b"),text.join([1,nothing,"x"]),text.trim(" \t\nx y\n ")]', names: ['text', 'list'] },
    check: (r) => r.ok && same(r.value, ['HÉLLO, WÖRLD', 'héllo, wörld', ['Héllo', 'wörld'], 'a-b', 'HéLLo, wörLd', true, 'cba', 'x', ['a', 'b', 'c'], ['a', 'b'], '1x', 'x y']),
  },
  {
    name: 'text: lower and upper in Latin with its accents, Greek and Cyrillic',
    raw: { code: 's="Ça va, Øre, Łódź, Ÿ ÿ, Αλφα, Привет, ёЁ";return [text.upper(s),text.lower(s),text.split("a--b--","--"),text.replace("aaa","a","bb"),text.join([]),text.join([[1],{a:2}],";")]', names: ['text'] },
    check: (r) => r.ok && same(r.value, ['ÇA VA, ØRE, ŁÓDŹ, Ÿ Ÿ, ΑΛΦΑ, ПРИВЕТ, ЁЁ', 'ça va, øre, łódź, ÿ ÿ, αλφα, привет, ёё', ['a', 'b', ''], 'bbbbbb', '', '[1];{"a":2}']),
  },
  {
    name: 'list: sort, records by a part, equal ones kept in order, and reverse',
    raw: { code: 'return [list.sort([3,1,2]),list.sort(["b","a","c"]),list.sort([{n:2,k:"x"},{n:1,k:"y"},{n:2,k:"z"}],"n"),list.sort([]),list.sort(["B","a","C"]),list.reverse([1,2,3]),list.reverse([]),list.reverse("añb")]', names: ['list'] },
    check: (r) => r.ok && same(r.value, [[1, 2, 3], ['a', 'b', 'c'], [{ n: 1, k: 'y' }, { n: 2, k: 'x' }, { n: 2, k: 'z' }], [], ['B', 'C', 'a'], [3, 2, 1], [], 'bña']),
  },
  {
    name: 'math: round to the nearest, up and down, to places, and min and max',
    raw: { code: 'return [math.round(2.5),math.round(1.005,2),math.round(-2.5),math.round(-1.5,0,"down"),math.round(1.1,0,"up"),math.min(3,2),math.max("a","b"),math.round(1234.5678,-2),math.round(1.25,1),math.round(-1.5,0,"up"),math.round(2,0,"down"),math.round(0.1,0,"down"),math.round(-0.1,0,"up")]', names: ['math'] },
    check: (r) => r.ok && same(r.value, [3, 1, -2, -2, 2, 2, 'b', 1200, 1.3, -1, 2, 0, 0]),
  },
  {
    name: 'time: the days between ISO times, and one that is not a time',
    raw: { code: 'try{time.difference("nope","2026-01-01","days")}catch e{x=e};return [time.difference("2026-01-01","2026-03-01","days"),time.difference("2026-10-12T14:00:00Z","2026-10-13T02:00:00+02:00","days"),time.difference("2026-10-12T14:00:00.5Z","2026-10-12T15:30:00-01:00","days"),time.difference("2024-02-28","2024-03-01T12:00","days"),time.parse("2026-13-01"),time.parse("2026-10-18T14:05:00+02:00"),x]', names: ['time'] },
    check: (r) => r.ok && same(r.value.slice(0, 6), [59, 0.4166666666666667, 0.10416087962962962, 2.5, null, 1792325100]) && /nope isn't a time/.test(r.value[6]),
  },
  // request, from ../../lingo-library, here since openrouter is built on it: a query from a record, and tries again after a 503.
  {
    name: 'request: a GET with a query, tried again after a 503',
    raw: { code: `r=request.get("${HTTP}/busy",{query:{id:"r1",n:2},retries:2});return [r.status,r.ok,r.json]`, names: ['request'] },
    check: (r) => r.ok && same(r.value, [200, true, { tries: 3 }]),
  },
  { name: 'input: a typo is a compile error', raw: { code: 'input.click_txt("File")', names: ['input'] }, check: (r) => !r.ok && r.error.code === 'compile' },
  // lingo(...): a call of lingo.run as the program reaches it. The core asks, here the stand-in (LINGO below), waits,
  // and goes on with the answer.
  {
    name: 'lingo(...): asked as it runs, and the program goes on with the answer',
    raw: { code: 'x = lingo("the capital of a country", {country: "France"}, "text")\nreturn "it is " + x', names: ['core'] },
    check: (r) => r.ok && r.value === 'it is Paris' && asked.ask === 'the capital of a country' && same(asked.values, { country: 'France' }) &&
      asked.gives === 'text' && typeof asked.id === 'number' && /^(windows|linux)$/.test(asked.context?.os) && /^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(Z|[+-]\d\d:\d\d)$/.test(asked.context.time),
  },
  {
    name: 'lingo(...) with words worked out as it runs',
    raw: { code: 'goal = "the capital of a country"\nreturn lingo(goal, {country: "Japan"})', names: ['core'] },
    check: (r) => r.ok && r.value === 'Tokyo' && asked.gives === '',
  },
  {
    // The web host serves a call itself while the core waits, so it can only be a host phrase there.
    name: 'lingo(...): Onsay uses this device while it works, and answers after',
    raw: { code: 'return lingo("something only this device knows", {})', names: ['core'] },
    check: (r) => r.ok && r.value === 'heard: 2 litres' && device?.ok === true,
  },
  { name: 'a lingo whose own code calls lingo(...)', raw: { code: 'return asker.kept("eggs")', names: ['asker'] }, check: (r) => r.ok && r.value === 'kept: eggs' },
  {
    name: 'a lingo reaching one elsewhere with use(...): asked of Onsay at its address',
    raw: { code: 'return asker.forecast("Rome")', names: ['asker'] },
    check: (r) => r.ok && r.value === 'Rome: sunny for 1 days' && asked.address === 'weather.example.com/lingos/weather.lingo/forecast' && same(asked.values, ['Rome', 1]),
  },
  { name: 'a lingo calling one in the same lingos folder by its dotted name, run right here', raw: { code: 'return asker.noted("milk")', names: ['asker'] }, check: (r) => r.ok && r.value === '2 litres' },
  { name: 'lingo(...) nothing can do', raw: { code: 'return lingo("book a flight to Paris", {})', names: ['core'] }, check: (r) => !r.ok && r.error.code === 'phrase' && /book a flight/.test(r.error.message) },
  { name: 'lingo(...): stopped while Onsay works', raw: { code: 'return lingo("a question never answered", {})', names: ['core'] }, stopAfter: 300, check: (r) => !r.ok && r.error.code === 'stopped' },
  { name: 'the core works after a stop in lingo(...)', raw: { code: 'return lingo("the capital of a country", {country: "Japan"})', names: ['core'] }, check: (r) => r.ok && r.value === 'Tokyo' },
  // use(address, values): lingo.use, asked of Onsay by its address as the line runs, values by name or in order. A dotted
  // call is always local: one to a lingo that isn't here doesn't compile, and says to use use(...).
  {
    name: 'use(...): another machine by its address, values by name, Onsay using this device while it works',
    raw: { code: 'x = use("notes.laptop/check", {key: "milk"})\nreturn "it says " + x', names: ['core'] },
    check: (r) => r.ok && r.value === 'it says heard: 2 litres' && device?.ok === true && asked.address === 'notes.laptop/check' && same(asked.values, { key: 'milk' }) &&
      !('ask' in asked) && typeof asked.id === 'number',
  },
  {
    name: 'use(...) with its values in a list',
    raw: { code: 'return use("weather/forecast", ["Rome", 2])', names: ['core'] },
    check: (r) => r.ok && r.value === 'Rome: sunny for 2 days' && same(asked.values, ['Rome', 2]),
  },
  {
    name: 'a dotted call to a lingo that isn\'t here is a compile error that says to use use(...)',
    raw: { code: 'x = 1\nreturn forcast.today(x)', names: ['core'] },
    check: (r) => !r.ok && r.error.code === 'compile' && r.error.line === 2 && /no lingo or group called forcast here.*use\("laptop\.brandon\/lingos\/forcast\.lingo\/today"\)/.test(r.error.message),
  },
  { name: 'a name the program can see is still checked first', raw: { code: 'return std.lenn([1])', names: ['core'] }, check: (r) => !r.ok && r.error.code === 'compile' },
  // call runs any phrase the device has, one written in Lingo too, as a program of its own.
  { name: 'call a phrase written in Lingo', call: { phrase: 'memo.recall', values: ['milk'] }, check: (r) => r.ok && r.value === '2 litres' },
  { name: 'call a phrase written in Lingo, with its default', call: { phrase: 'memo.all', values: [] }, check: (r) => r.ok && same(r.value, ['eggs', 'milk']) },
  { name: 'call: only a plain phrase id', call: { phrase: 'memo.recall("x")+memo.all', values: [] }, check: (r) => !r.ok && r.error.code === 'unknown' },
  // A call by where the lingo is in the lingos folder, as an address names it: its file, then its function, main when none.
  { name: 'call by file and function, its values by name', call: { lingo: 'memo.lingo/recall', values: { k: 'milk' } }, check: (r) => r.ok && r.value === '2 litres' },
  { name: 'call by file alone runs its main', call: { lingo: 'asker.lingo', values: [] }, check: (r) => r.ok && r.value === "asker's main" },
  { name: 'call by file and function: a value it does not take', call: { lingo: 'memo.lingo/recall', values: { key: 'milk' } }, check: (r) => !r.ok && r.error.code === 'type' && /doesn't take a value called key/.test(r.error.message) },
  { name: 'call by file: one with no main', call: { lingo: 'memo.lingo', values: [] }, check: (r) => !r.ok && /no phrase memo.main/.test(r.error.message) },
  { name: 'call by file: it names no .lingo file', call: { lingo: 'memo/recall', values: [] }, check: (r) => !r.ok && r.error.code === 'unknown' },
  // agent, the chat agent from the library: the model, given its tools, reaches a lingo by its address and one by its words,
  // Onsay using this device while the agent waits, and replies with what they gave.
  {
    name: 'agent, called by its file: the model reaches a lingo by its address and one in words, and replies with what they gave, its lingo(...) saying it asks',
    before: () => keep('openrouter', { key: 'test-key', url: 'http://127.0.0.1:8787/openrouter' }),
    call: { lingo: 'agent.lingo/ask', values: { prompt: 'what is the weather in Rome?' } },
    check: (r) => r.ok && r.value === 'openai/gpt-5-mini heard: Rome: sunny for 2 days and heard: 2 litres' && device?.ok === true && every.some((m) => m.ask === 'something only this device knows' && m.asking === 'agent.ask'),
  },
]

// Web requests, to the test core's own port (test/onsay.json) as any program on this computer sends them, and down its
// connection as Onsay sends them. Each is [name, ask => whether it passed], ask sending a message to the core.
const WEB = 6970, BARE = 6971   // the test core's port, and the bare core's, below
const web = (path, { method = 'GET', body, headers = {}, port = WEB } = {}) => new Promise((done) => {
  const q = http.request({ host: '127.0.0.1', port, path, method, headers, agent: false }, (s) => {
    let text = ''
    s.setEncoding('utf8')
    s.on('data', (d) => { text += d })
    s.on('end', () => { let json; try { json = JSON.parse(text) } catch {} done({ status: s.statusCode, headers: s.headers, text, json }) })
  })
  q.on('error', (e) => done({ status: 0, text: e.message }))
  q.end(body === undefined ? undefined : typeof body === 'string' ? body : JSON.stringify(body))
})
// Bytes written straight to the port, for what no program would send, and the first line of the answer.
const raw = (text) => new Promise((done) => {
  let got = ''
  const s = net.connect(WEB, '127.0.0.1', () => s.end(text))
  s.on('data', (d) => { got += d })
  s.on('close', () => done(got.split('\r\n')[0]))
  s.on('error', () => done(''))
})
const request = (ask, path, more = {}) => ask({ type: 'request', id: `w${path}`, method: 'GET', path, headers: { host: 'api.onsay.dev' }, body: '', base64: false, ...more })
const CORE = fileURLToPath(new URL('..', import.meta.url)), EXE = fileURLToPath(new URL('../bin/test/onsay-core.exe', import.meta.url))
const WEB_TESTS = [
  ['web: GET / lists every lingo for this computer, each at its route', async () => {
    const r = await web('/')
    return r.status === 200 && r.json.lingos.some((l) => l.name === 'page' && l.route === '/hello') && r.json.lingos.some((l) => l.name === 'memo' && l.route === '/memo')
  }],
  ['web: serve answers text as text', async () => {
    const r = await web('/hello/text?name=Ana+B%C3%A9')
    return r.status === 200 && r.text === 'hi Ana Bé' && r.headers['content-type'].startsWith('text/plain')
  }],
  ['web: serve gets the method, headers and the body read as JSON, and answers JSON', async () => {
    const r = await web('/hello/json', { method: 'POST', body: { a: 1 }, headers: { 'Content-Type': 'application/json' } })
    return r.status === 200 && r.headers['content-type'] === 'application/json' && same(r.json, { got: { a: 1 }, method: 'POST', type: 'application/json' })
  }],
  ['web: serve gives its own status, headers and body', async () => {
    const r = await web('/hello/status')
    return r.status === 201 && r.headers['x-made'] === 'page' && r.text === 'made'
  }],
  ['web: serve failing is a 500 that says why', async () => { const r = await web('/hello/fail'); return r.status === 500 && /it broke/.test(r.json?.error) }],
  ['web: POST /name/function, its values by name, answered as JSON', async () => {
    const r = await web('/memo/recall', { method: 'POST', body: { k: 'milk' } })
    return r.status === 200 && r.headers['content-type'] === 'application/json' && r.json === '2 litres'
  }],
  ['web: GET /name/function, its values in the query', async () => { const r = await web('/memo/recall?k=milk'); return r.status === 200 && r.json === '2 litres' }],
  ['web: a value it does not take is a 400', async () => {
    const r = await web('/memo/recall', { method: 'POST', body: { key: 'milk' } })
    return r.status === 400 && /doesn't take a value called key/.test(r.json?.error)
  }],
  ['web: a function it has not, and a lingo not here, are 404', async () => {
    const [a, b] = [await web('/memo/nope'), await web('/nothing/here')]
    return a.status === 404 && /memo has no function memo.nope/.test(a.json?.error) && b.status === 404
  }],
  ['web: a page elsewhere in a browser reaches only open lingos', async () => {
    const [a, b] = [await web('/memo/recall?k=milk', { headers: { origin: 'http://elsewhere.example' } }), await web('/hello/text', { headers: { origin: 'http://elsewhere.example' } })]
    return a.status === 404 && b.status === 200
  }],
  ['web: what is not a web request is a 400', async () => (await raw('HELLO THERE\r\n\r\n')).startsWith('HTTP/1.1 400')],
  ['web: a body in chunks is a 411, and one over 4 MB a 413', async () =>
    (await raw('POST /memo/recall HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n')).startsWith('HTTP/1.1 411') &&
    (await raw('POST /memo/recall HTTP/1.1\r\nContent-Length: 5000000\r\n\r\n')).startsWith('HTTP/1.1 413')],
  ['web: a permission not given is a 403', async () => { const r = await web('/windows/keyboard/type', { method: 'POST', body: ['x'] }); return r.status === 403 }, 'windows'],
  ['onsay: a request down the connection is answered as the port answers it', async (ask) => {
    const r = await request(ask, '/hello/text?name=Bo')
    return r.status === 200 && r.body === 'hi Bo' && r.base64 === false && r.headers['content-type'].startsWith('text/plain')
  }],
  ['onsay: a body in base64', async (ask) => {
    const r = await request(ask, '/hello/json', { method: 'POST', body: Buffer.from('{"b":2}').toString('base64'), base64: true })
    return r.status === 200 && same(JSON.parse(r.body).got, { b: 2 })
  }],
  ['onsay: a lingo not open is not there, and / lists the open ones only', async (ask) => {
    const [a, b] = [await request(ask, '/memo/recall?k=milk'), await request(ask, '/')]
    return a.status === 404 && same(JSON.parse(b.body).lingos.map((l) => l.name), ['page'])
  }],
  ['bare: with no token it runs on its own, answers its port, and lingo(...) says it is not connected', async () => {
    writeFileSync(new URL('./data/bare.json', import.meta.url), JSON.stringify({ name: 'bare core', home: 'test/data', lingos: 'test/lingos', port: BARE, allow: ['none'] }))
    const core = spawn(EXE, ['test/data/bare.json'], { cwd: CORE, stdio: 'ignore' })
    let r = { status: 0 }
    for (let i = 0; i < 50 && !r.status; i++) { await new Promise((d) => setTimeout(d, 200)); r = await web('/', { port: BARE }) }
    const asked = await web('/asker/kept', { method: 'POST', body: { k: 'eggs' }, port: BARE })
    core.kill()
    return r.status === 200 && r.json.lingos.some((l) => l.name === 'memo') && asked.status === 500 && /isn't connected to Onsay/.test(asked.json?.error)
  }],
]

////////// 3. SESSION //////////

let lingo = null   // run.mjs's view of std, made from the core's own manifest
let catalog = []   // the core's table of contents of its lingos
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b)
let hellos = 0, failed = 0, passed = 0, skipped = 0, version = ''
const has = (name) => catalog.some((l) => l.name === name && l.status === 'ok')
const needs = (t) => /^(windows|hosted|native):/.exec(t.name.replace(/^(input|look):/, 'windows:'))?.[1]   // the lingo a test is about, beyond core

const session = (link) => {
  let phrases = null, next = 0
  const waiting = new Map()
  const ask = (msg) => new Promise((done) => { waiting.set(msg.id, done); link.send(msg) })

  const go = async () => {
    for (const t of TESTS) {
      if (needs(t) && !has(needs(t))) { skipped++; console.log(`  skip  ${t.name}: this core has no ${needs(t)}`); continue }
      const id = `t${++next}`
      t.before?.()
      let program = null
      if (!t.call && !t.raw) {
        const parsed = compiler({ op: 'parse', code: t.code, phrases })
        if (parsed.error) { report(t.name, false, `doesn't compile: ${JSON.stringify(parsed.error)}`); continue }
        program = parsed.program
      }
      const asked = t.call ? ask({ type: 'call', id, ...t.call }) : ask({ type: 'run', id, ...(t.raw ?? { program }), input: t.input ?? {}, limits: t.limits ?? {} })
      if (t.stopAfter) setTimeout(() => link.send({ type: 'stop', id }), t.stopAfter)
      const started = Date.now()
      const got = await asked
      t.after?.()
      const took = Date.now() - started
      delete got.type
      delete got.id
      if (t.pure) {
        const want = await run(program, t.input ?? {}, lingo, {})
        report(t.name, same(got, want), same(got, want) ? '' : `\n    C:  ${JSON.stringify(got)}\n    JS: ${JSON.stringify(want)}`, took)
      } else report(t.name, t.check(got), t.check(got) ? '' : `\n    got: ${JSON.stringify(got)}`, took)
    }
    // The table of contents: memo ready with its listing made from its code, broken set aside with its reason, and a folder
    // without a lingo.json not a lingo.
    const entry = (name) => catalog.find((l) => l.name === name || l.folder === name)
    const recall = entry('memo')?.phrases?.find((p) => p.id === 'memo.recall'), kept = entry('asker')?.phrases?.find((p) => p.id === 'asker.kept')
    report('catalog: a Lingo lingo, one file, and its listing made from its code, never the code', entry('memo')?.status === 'ok' && entry('memo').file === 'memo.lingo' &&
      entry('memo').about === "things to remember, kept in Onsay's own storage on this device." && entry('memo').phrases.length === 4 &&
      recall?.head === 'recall(k)' && !('gives' in recall) && recall.does === 'What was kept under that name.' && recall.lingos === 0 && 'k' in (recall.inputSchema?.properties ?? {}) &&
      !JSON.stringify(entry('memo')).includes('store.get'))
    report('catalog: each function counts its lingo(...) calls', kept?.lingos === 1 && entry('asker').phrases.find((p) => p.id === 'asker.forecast')?.lingos === 0)
    if (has('windows')) {
      const phrase = (id) => phrases.find((p) => p.id === id)
      report('catalog: natives', entry('windows')?.native === true && entry('core')?.native === true)
      report('catalog: windows is a folder lingo', entry('windows')?.folder === 'windows' && entry('windows').about?.length > 0 && !entry('core').folder)
      report('windows: input and look are its own groups, each phrase with what it needs', phrase('input.click_text')?.head === 'click_text(words, window=nothing, nth=0)' &&
        phrase('input.click_text').gives === '{x,y,text}' && phrase('input.click_text').needs.join() === 'screen.act,screen.read' &&
        phrase('input.wait_for_text')?.gives === '{x,y,text}?' && phrase('input.cursor')?.needs.join() === 'none' && phrase('look.see')?.needs.join() === 'screen.read')
    } else {
      report('catalog: core without windows', entry('core')?.native === true && !entry('core').folder && entry('windows')?.status === 'broken')
    }
    report('catalog: openrouter is ready on every core, a folder whose lingo.json says its settings', entry('openrouter')?.status === 'ok' && entry('openrouter').file === 'openrouter.lingo' && same(entry('openrouter').settings, ['key', 'url', 'model']))
    report('catalog: settings are names alone, its lingo.json\'s first, then any other its code reads', same(entry('prefs')?.settings, ['units', 'name', 'color']) && same(entry('memo')?.settings, []) && !('settings' in entry('core')))
    report('catalog: agent is ready on every core, on its own', entry('agent')?.status === 'ok' && entry('agent').phrases.some((p) => p.id === 'agent.ask'))
    if (has('native')) report('catalog: native is ready, one phrase that calls into the OS', entry('native')?.status === 'ok' && entry('native').phrases.some((p) => p.id === 'native.call'))
    report('catalog: a native without its library', entry('nolibrary')?.status === 'broken' && /nolibrary\.dll/.test(entry('nolibrary').error))
    if (!has('hosted')) report('catalog: a host lingo where there is no host', entry('hosted')?.status === 'broken' && /has none/.test(entry('hosted').error))
    report('catalog: a broken one says why', entry('broken')?.status === 'broken' && entry('broken').file === 'broken.lingo' && /no lingo or group called nope here.*use\(/.test(entry('broken').error))
    report('catalog: a Lingo lingo can\'t hide a group of windows', entry('input')?.status === 'broken' &&
      (has('windows') ? /already a part of windows/ : /no lingo or group called pointer/).test(entry('input').error))
    report('catalog: not a lingo', !entry('notalingo'))
    for (const [name, test, lingo] of WEB_TESTS) {
      if (lingo && !has(lingo)) { skipped++; console.log(`  skip  ${name}: this core has no ${lingo}`); continue }
      const started = Date.now()
      report(name, await test(ask).catch(() => false), '', Date.now() - started)
    }
    // A ping from the server gets a pong.
    const ponged = await new Promise((done) => { link.onPong = () => done(true); link.send({ type: 'ping' }); setTimeout(() => done(false), 2000) })
    report('ping', ponged)
    report('hello carries the core version', version === '0.4.0')
    // Then the connection drops, and the core must come back by itself.
    console.log('  dropping the connection...')
    link.drop()
  }

  return (text) => {
    const m = JSON.parse(text)
    if (m.type === 'hello') {
      hellos++
      if (m.token !== TOKEN) { link.send({ type: 'error', error: 'That token is not signed in' }); link.drop(); return }
      const read = m.manifests.map((text) => compiler({ op: 'manifest', text }).manifest)
      phrases = read.flatMap((r) => r.phrases)
      catalog = m.catalog
      version = m.version
      lingo = { phrases: Object.fromEntries(phrases.filter((p) => STD[p.id]).map((p) => [p.id, { ...p, run: STD[p.id] }])) }
      const has = catalog.map((l) => `${l.name} ${l.status}`).join(', ')
      console.log(`hello ${hellos} from ${m.client} ${m.version} "${m.name}": ${read.map((r) => `${r.name} ${r.version}`).join(', ')}, ${phrases.length} phrases, allows ${m.allow.join(', ')}, has ${has}`)
      link.send({ type: 'ready', device: 'test-device' })
      if (hellos === 1) go()
      else { report('reconnects by itself', true); console.log(`\n${passed} passed, ${failed} failed${skipped ? `, ${skipped} skipped` : ''}`); process.exit(failed ? 1 : 0) }
    } else if (m.type === 'result' || m.type === 'response') waiting.get(m.id)?.(m)
    else if (m.type === 'lingo' && 'id' in m) answer(m, link, (phrase, values) => ask({ type: 'call', id: `l${++next}`, phrase, values }))
    else if (m.type === 'catalog') catalog = m.catalog
    else if (m.type === 'pong') link.onPong?.()
    else if (m.type !== 'ping') console.log('  core said', m)
  }
}

const report = (name, ok, more = '', ms) => {
  ok ? passed++ : failed++
  console.log(`${ok ? '  ok  ' : '  FAIL'} ${name}${ms === undefined ? '' : ` (${ms} ms)`}${more}`)
}

// Every run starts with an empty store and no settings, so what the last run kept, like memo's, can't change what this
// one sees.
const SETTINGS = new URL('./data/settings/', import.meta.url)   // the test core's home is test/data (test/onsay.json)
rmSync(new URL('./data/store', import.meta.url), { recursive: true, force: true })
rmSync(SETTINGS, { recursive: true, force: true })
// A lingo's settings as this device keeps them, a record by name in home/settings/<lingo>.json, as settings.set writes them.
const keep = (lingo, values) => {
  mkdirSync(SETTINGS, { recursive: true })
  writeFileSync(new URL(`${lingo}.json`, SETTINGS), JSON.stringify(values))
}
// Every library lingo, a file or a folder, copied into the test core's folder from ../../lingo-library, as a person puts
// them in a computer's lingos folder, the other shape of each taken away first. The app ships none of them.
const LIBRARY = new URL('../../lingo-library/', import.meta.url)
for (const name of readdirSync(LIBRARY).filter((f) => f.endsWith('.lingo') || existsSync(new URL(`${f}/lingo.json`, LIBRARY)))) {
  const lingo = name.replace(/\.lingo$/, '')
  for (const was of [lingo, `${lingo}.lingo`]) rmSync(new URL(`./lingos/${was}`, import.meta.url), { recursive: true, force: true })
  cpSync(new URL(name, LIBRARY), new URL(`./lingos/${name}`, import.meta.url), { recursive: true })
}

/* Canned answers to lingo.run and lingo.use: each ask, by its words or its address, answered as the program runs, or
   failed with why. One uses the device on the way with a call: memo.recall on the exe, run while the program waits, and
   on the web host, which serves calls itself while the core waits, one of its host phrases. asked is the last request,
   device the last call's result, shown what the command line printed, for the tests to look at. */
let asked = {}, device = null, every = [], shown = null   // every: each ask, in order
const LINGO = [
  [/capital of a country/, ({ values }) => ({ France: 'Paris', Japan: 'Tokyo' })[values.country]],
  [/kept under a name/, ({ values }) => `kept: ${values.name}`],
  [/^weather\/forecast$|\/weather\.lingo\/forecast$/, ({ values: [city, days] }) => `${city}: sunny for ${days} days`],
  [/only this device knows|^notes(\.laptop)?\/check$/, async (m, call) => {
    device = await (has('hosted') ? call('hosted.echo', ['2 litres']) : call('memo.recall', ['milk']))
    if (!device.ok) throw new Error(device.error.message)
    return `heard: ${device.value}`
  }],
  [/never answered/, () => new Promise(() => {})],
]
const answer = async (m, link, call) => {
  asked = m
  every.push(m)
  const found = LINGO.find(([match]) => match.test(m.ask ?? m.address))
  try {
    if (!found) throw new Error(m.address ? `There's no lingo called ${m.address.split(/[./]/)[0]}` : `Nothing can do "${m.ask}" yet`)
    link.send({ type: 'lingo', id: m.id, value: (await found[1](m, call)) ?? null })
  } catch (e) { link.send({ type: 'lingo', id: m.id, error: e.message, code: 'phrase' }) }
}

// OpenRouter, standing in: only the test key, and an answer that says which model was asked and the last thing said. Given
// tools, as the agent gives them, it first reaches weather/forecast by its address and asks for one in words, then says what they gave.
const openrouter = (q, s) => {
  let body = ''
  q.on('data', (d) => { body += d })
  q.on('end', () => {
    if (q.headers.authorization !== 'Bearer test-key') { s.writeHead(401); return s.end('{"error":{"message":"No auth credentials found"}}') }
    const ask = JSON.parse(body || '{}')
    s.writeHead(200, { 'content-type': 'application/json' })
    const told = (ask.messages || []).filter((m) => m.role === 'tool').map((m) => m.content), tool = (name, args, i) => ({ id: `c${i}`, type: 'function', function: { name, arguments: JSON.stringify(args) } })
    const message = !ask.tools ? { content: `${ask.model} says: ${ask.messages?.at(-1)?.content}` } : told.length ? { content: `${ask.model} heard: ${told.join(' and ')}` }
      : { content: null, tool_calls: [tool('use', { address: 'weather/forecast', values: ['Rome', 2] }, 0), tool('lingo', { task: 'something only this device knows' }, 1)] }
    s.end(JSON.stringify({ choices: [{ message: { role: 'assistant', ...message } }] }))
  })
}

// Pages for http.send and request, by their path under /http.
const tries = {}
const PAGES = {
  '/hello': (q, s) => s.writeHead(200, { 'x-test': 'yes' }).end(`hello ${new URL(q.url, HTTP).search}`),
  '/echo': (q, s, body) => s.writeHead(201, { 'content-type': 'application/json' }).end(JSON.stringify({ method: q.method, name: q.headers['x-name'], type: q.headers['content-type'], body })),
  '/missing': (q, s) => s.writeHead(404).end('nothing here'),
  '/moved': (q, s) => s.writeHead(302, { location: '/http/hello' }).end(),
  '/slow': (q, s) => setTimeout(() => s.writeHead(200).end('late'), 3000),
  '/busy': (q, s) => {
    const p = new URL(q.url, HTTP).searchParams, n = tries[p.get('id')] = (tries[p.get('id')] ?? 0) + 1
    if (n <= Number(p.get('n'))) return s.writeHead(503, { 'retry-after': '0' }).end('busy')
    s.writeHead(200, { 'content-type': 'application/json' }).end(JSON.stringify({ tries: n }))
  },
}

const server = http.createServer((q, s) => {
  if (q.method === 'POST' && q.url === '/openrouter/chat/completions') return openrouter(q, s)
  const page = PAGES[q.url.replace(/^\/http/, '').split('?')[0]]
  if (q.url.startsWith('/http/') && page) {
    let body = ''
    q.on('data', (d) => { body += d })
    return q.on('end', () => page(q, s, body))
  }
  s.writeHead(426)
  s.end('This is a WebSocket')
})
server.on('upgrade', (req, socket) => {
  const accept = crypto.createHash('sha1').update(req.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64')
  socket.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`)
  const link = socketOf(socket, (t) => on(t), () => {})
  const on = session(link)
})
server.listen(PORT, '127.0.0.1', () => console.log(`test server on ws://127.0.0.1:${PORT}/live`))
setTimeout(() => { console.log('timed out'); process.exit(1) }, 120000)
