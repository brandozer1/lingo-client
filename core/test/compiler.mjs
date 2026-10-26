// compiler.mjs - proves the C compiler (as WebAssembly) answers every case with the same JSON as reference/code.mjs.
//
//   ./build.ps1, then: node test/compiler.mjs
//
// 1. The JavaScript side   code.mjs, with load, face and prepare for any native lingos
// 2. Cases                 manifests, programs, faces and preparing, on this core's own native lingos
// 3. Compare
// 4. Lingo lingos          the C alone: a lingo's listing made from its code, calls between lingos of one folder, and
//                          every lingo in the repo read and compiled, the library's against core and windows

import { execFileSync } from 'node:child_process'
import { readFileSync, readdirSync, existsSync } from 'node:fs'
import { parse, manifest, link, gives } from './reference/code.mjs'
import { lingo } from '../web/compiler.js'

const here = (p) => new URL(p, import.meta.url)
// The core's native lingos, core and windows, as their manifests: --manifest prints each, a line of --- between them.
// Run from core/, since test/onsay.json names its folders from there, wherever this test is started.
const CORES = execFileSync(here('../bin/onsay-core.exe').pathname.slice(1), [here('onsay.json').pathname.slice(1), '--manifest'], { encoding: 'utf8', cwd: here('..').pathname.slice(1) }).split(/^---\r?\n/m)

////////// 1. THE JAVASCRIPT SIDE //////////

// The native lingos are given, so any device's can be used. Folder lingos are tested on the core itself, see server.mjs.
const load = (natives) => {
  const nat = natives.map((t) => {
    try { return { ...manifest(t), native: true } } catch (err) { throw new Error(`the device's own lingo: ${err.message}`) }
  })
  return { lingos: new Map(nat.map((l) => [l.name, l])), stack: { phrases: new Map(), fns: {}, lines: {} }, natives: nat }
}

const chosen = (inst, names) => {
  const out = []
  const add = (l, groups) => {
    const had = out.find((x) => x.lingo === l)
    if (had) for (const g of groups) { if (!had.groups.includes(g)) had.groups.push(g) }
    else out.push({ lingo: l, groups: [...groups] })
  }
  for (const n of names) {
    const l = inst.lingos.get(n)
    const native = inst.natives.find((x) => x.groups.some((g) => g.name === n))
    if (l) add(l, l.groups.map((g) => g.name))
    else if (native) add(native, [n])
    else throw new Error(`There's no lingo or group called ${n}. There are: ${[...inst.lingos.keys(), ...inst.natives.flatMap((x) => x.groups.map((g) => g.name)).filter((g) => !inst.lingos.has(g))].join(', ')}`)
  }
  return out
}

const face = (inst, names) => chosen(inst, names).map(({ lingo: l, groups }) => {
  const phrases = l.phrases.filter((p) => groups.includes(p.group))
  const mentioned = Object.entries(l.shapes).filter(([s]) => phrases.some((p) => new RegExp(`\\b${s}\\b`).test(`${p.gives} ${p.does}`)))
  return [
    `lingo ${l.name} ${l.version}`,
    ...mentioned.map(([s, shape]) => `${s} = ${shape}`),
    ...l.groups.filter((g) => groups.includes(g.name)).flatMap((g) => {
      const own = phrases.filter((p) => p.group === g.name), groupNeeds = g.needs.join(', ')
      return [`${g.name}: ${groupNeeds}${g.pure ? ', pure' : ''}`, ...own.map((p) => {
        const needs = p.needs.join(', ') || 'none'
        return `${p.head}${p.gives === 'nothing' ? '' : ` -> ${p.gives}`}${needs !== groupNeeds ? ` [${needs}]` : ''}${p.does ? `: ${p.does}` : ''}`
      })]
    }),
  ].join('\n')
}).join('\n\n')

const prepare = (inst, code, names) => {
  const allowed = chosen(inst, names).flatMap(({ lingo: l, groups }) => l.phrases.filter((p) => groups.includes(p.group)))
  const parsed = parse(code, allowed)
  if (parsed.error) return parsed
  return { program: link(parsed.program, inst.stack) }
}

// What the JavaScript side answers to the same request the C side gets.
const js = (q) => {
  try {
    if (q.op === 'manifest') return { manifest: manifest(q.text) }
    if (q.op === 'parse') return parse(q.code, q.phrases)
    const inst = load(q.natives)
    return q.op === 'face' ? { face: face(inst, q.names) } : prepare(inst, q.code, q.names)
  } catch (err) { return { error: { message: err.message } } }
}

////////// 2. CASES //////////

const head = 'lingo t 1.0, Lingo 1.0\n'
const MANIFESTS = [
  ...CORES, '', '// only a comment\n\n',
  'hello', 'lingo x, Lingo 1.0', 'lingo x 1.0 Lingo 1.0', 'lingo x 1.0, Lingo 1.0, uses y', 'lingo x 1.0, Lingo 1.0, uses y 1, z 2.1 extra',
  'lingo x 1.0, Lingo 1.0, uses ', 'lingo  x 1.0, Lingo 1.0', 'lingo x 1.0, Lingo 1 .0',
  `${head}g: fly`, `${head}g: pure`, `${head}g: none,pure`, `${head}g: none, pure\nf()`, `${head}f()`, `${head}g: none\nfoo bar`,
  `${head}g: none\nf(a=1+2)`, `${head}g: none\nf(a=-2, b="x", c=nothing, d=true, e=1.5)`, `${head}g: none\nf(a b)`, `${head}g: none\nf(a,) -> x`,
  `${head}g: none\nf() -> a: b: c`, `${head}g: none\nf() [screen.read]`, `${head}g: none\nf(): x [net]`, `${head}g: none\nf() -> [x] [files.read,net]: y`,
  `${head}g: none\nf() -> list|text [screen.read, screen.act]: does it`, `${head}g: none\nf() [fly]: x`, `${head}g: none\nf() -> x {`,
  `${head}shape = {a,b}\ng: none, pure\nf() -> shape`,
  'lingo t 1.0, Lingo 1.0\r\ng: none\r\nf() -> x: y\r\n', `${head}g: none\n  f(a="(", b=")")  -> x  `, `${head}g: none\n f() -> x: y　`,
  `${head}g: screen.read, net\nf()\nh() [none]`, `${head}g: none\nnote(x): a line, with: colons -> and arrows`,
]

const PROGRAMS = [
  'return 1', 'x=1;y=2;return x+y*3', 'big=1000000*1000000;return [1.5, -2, 0.25, big]', 'return "a{1+2}b{"q"}"', 'return "{x"',
  'return "{"a"}"', 'return "never', 'return @', 'x', 'x.y', '1+2', 'x=', 'if {}', 'else{}', 'if x {fn f(){}}',
  'tool a(){}tool b(){}', 'tool a(){}x=1', 'fn f(){}fn f(){}', 'foo()', 'wait(1)', 'read("a")', 'list()', 'std.length()', 'slice()', 'slice(1,2,3,4)',
  'fn f(a,b=1){return a}f()', 'fn f(a,b=1){return a}f(1,2,3)', 'fn f(a){return a}return f(1)', 'window.nope()', 'zzz.nope()', '"a"=1',
  'for(i, x in [1,2]){notify.say(x)}', 'for x in l {break;continue}', 'for k,v in {a:1} {}', 'for i in 1..10 {}', 'return', 'if true {return;}',
  'r={"two words":1, in:2, a:{b:[1,2]}};return r', 'return "{ {a:1}.a }"', '// a comment\nx=1 // more\nreturn x', 'x=1\r\n\ty=2 ;return [x,y]',
  'return 1..5', 'return a?b:c', 'return a??b??c', 'return not a', 'return !a&&b||c and d or e', 'x+=[1];x.k=1;x[0]+=2;x-=1;x*=2;x/=3',
  'return x.in.if', 'return "\\{" + "\\\\" + "a\\nb\\tc\\q"', 'return "multi\nline {x}\n"', 'if a {x=1} else if b {x=2} else {x=3}',
  'while x<10 {x+=1}', 'try {fail "no"} catch e {notify.say(e)}', 'try {x=1}', 'try {} catch {}', 'return -x - -1', 'return x[1][-1].k',
  'n=0;out=[];for f in files.list("downloads","*.pdf"){n+=1;out+=["{f.name}: {f.size/1048576} MB"]}return n',
  'w=window.open("notepad");controls.wait_for(w,{name:"Text editor"});keyboard.type("hi")', 'return std.slice("a,b",1)', 'return slice("a,b",1,2,3)',
  'tool t(a, b=a+1){return [a,b]}', 'fn f(){return g()}fn g(){return 1}return f()', 'x=[1,2,];y={a:1,};return [x,y]', 'return [,]', 'return {,}',
  'return (1+2)*3', 'return 1 < 2 == true', 'return "é\u{1F600}" + "x"', 'return 007', 'return 1.5.x', 'x=1;;;y=2', 'return {a:1}{b:2}',
  'return "{}"', 'return "{1}{2}"', 'if (x>1) {y=1}', 'while {}', 'for x {}', 'for x in {}', 'fail', 'return f(', 'return [1', 'tool ()', 'fn (){}',
  // lingo(...): always a call of lingo.run as the program runs, whatever its words are.
  'return lingo("the weather in a city", {city: "Paris"}, "text")', 'x="a";return lingo(x)', 'return lingo("x {1}", nothing, x)',
  'lingo("a note")', 'return length(lingo("a name", {}))', 'lingo()', 'lingo("a",{},"b",1)', 'lingo("")', 'fn f(){return lingo("q")}return f()',
  // use(address, values): always a call of lingo.use, its values a record by name, a list in order, or none.
  'return use("laptop.brandon/lingos/weather.lingo/now", {place: "Paris"})', 'return use("weather/now", ["Paris", 3])', 'use("https://api.example.com/v1/search?q=x")',
  'a="https://mcp.example.com/mcp#search";return use(a, {k: 1})', 'use()', 'use("")', 'use("a", {}, 1)', 'fn f(){return use("q", [length([1])])}return f()',
  'return use("laptop.brandon/lingos/check.lingo@desktop.brandon/lingos/show.lingo/print")', 'call("x")',
  // setting(...): always a call of lingo.setting. A program is no lingo, so it says whose.
  'return setting("token", "github") ?? "none"', 'r="openrouter";return setting("key", r)', 'return setting("key")', 'setting()', 'setting("a", "b.c", 1)',
  // A dotted call is local: a phrase it can see, checked, or else an error saying to use use("address"). A place is never in code.
  'return weather.forecast("Paris", 3)', 'return windows.window.list()', 'x=layout.snap()', 'return [203.0.113.5:7400].layout.snap("chrome", "left")',
  'return [1.5].x', 'return [1,2.5].y', 'return [1.2.3].x', '[1.2.3.4]', 'files.nope.x()', 'fn f(){return zz.q(length([1]))}return f()', 'return zz.q(1)+zz.q(2)',
]

const PREPARES = [
  [CORES, ['files', 'std'], 'return length(files.list("downloads"))'],
  [CORES, ['windows', 'std'], 'out=[];for w in window.list(){out+=[length(w.title)]};return out'],
  [CORES, ['input', 'look'], 'p=input.position_in(look.focused(),0.5,0.5);return look.near("x",nothing,p.x)'],
  [CORES, ['files'], 'store.get("x")'],
  [CORES, ['nope'], 'return 1'],
  [CORES, ['windows'], 'return std.length([1])'],
  [CORES, ['files'], 'files.read()'],
  [['lingo broken'], ['x'], 'f()'],
  [CORES, ['files'], 'return lingo("what is in a folder", {folder: "downloads"}, "[file]")'],
  [CORES, ['files'], 'return [layout.snap("notepad", "left"), window.list(), files.exists("a")]'],
  [CORES, ['files'], 'return files.reed("a")'],
  [CORES, ['files'], 'return files.exists()'],
]
const FACES = [
  [CORES, ['files', 'std', 'files', 'window']], [CORES, ['windows']], [CORES, ['input', 'look', 'store']], [CORES, ['nope']],
]

const phrases = CORES.flatMap((t) => manifest(t).phrases)
const requests = [
  ...MANIFESTS.map((text) => ({ op: 'manifest', text })),
  ...PROGRAMS.map((code) => ({ op: 'parse', code, phrases })),
  ...PREPARES.map(([natives, names, code]) => ({ op: 'prepare', natives, stacked: [], names, code })),
  ...FACES.map(([natives, names]) => ({ op: 'face', natives, stacked: [], names })),
  { op: 'nope' },
]

////////// 3. COMPARE //////////

let passed = 0, failed = 0
const label = (q) => `${q.op} ${JSON.stringify(q.text ?? q.code ?? q.names).slice(0, 70)}`
for (const q of requests) {
  const want = q.op === 'nope' ? { error: { message: 'the request needs an op: manifest, parse, face, prepare or read, with what that op takes' } } : js(q)
  const got = lingo(q)
  if (JSON.stringify(got) === JSON.stringify(want)) { passed++; continue }
  failed++
  console.log(`FAIL ${label(q)}\n  C:  ${JSON.stringify(got).slice(0, 600)}\n  JS: ${JSON.stringify(want).slice(0, 600)}`)
}
console.log(`\n${passed} same, ${failed} different, of ${requests.length}`)

////////// 4. LINGO LINGOS //////////

const repo = (path) => new URL(`../../${path}`, import.meta.url)
const manifestOf = (path) => JSON.parse(readFileSync(repo(`${path}/lingo.json`), 'utf8'))
const files = (dir) => readdirSync(repo(dir)).filter((f) => f.endsWith('.lingo')).map((file) => ({ file, code: readFileSync(repo(`${dir}/${file}`), 'utf8') }))
const NOTES = { file: 'notes.lingo', code: `// notes - Notes, for the tests: kept and found again.
// A second line of what it is.
//
// 1. add(text)
// 2. find(words, most=10)

// 1. Keeps a note. Use it to remember something.
fn add(text) {
  units = setting("units") ?? "metric"
  token = setting("token", "github")
  return {id: key(text), text: text, units: units, key: setting("key")}
}

// 2. Finds notes by their words,
//    the newest first.
fn find(words, most=10, o={a: 1}, back=-2) {return [words, most]}

fn key(k) {return "notes/" + k}
` }
const library = files('lingo-library')
const compile = (stacked, names, code = 'return 1') => lingo({ op: 'prepare', natives: CORES, stacked, names, code })
const notes = (change) => ({ ...NOTES, code: change(NOTES.code) })
const line = (p) => `${p.head}${p.gives === 'nothing' ? '' : ` -> ${p.gives}`}${p.needs.join(', ') === 'screen.read' ? '' : ` [${p.needs.join(', ')}]`}${p.does ? `: ${p.does}` : ''}`

const CHECKS = [
  ['a listing made from the code: its about, what each fn does from the comments above it, its values and defaults, and its own settings', () => {
    const l = lingo({ op: 'read', ...NOTES }).lingo, [add, find, key] = l?.phrases ?? []
    const ok = l?.name === 'notes' && l.about === 'Notes, for the tests: kept and found again. A second line of what it is.' && !('version' in l) &&
      add?.id === 'notes.add' && add.head === 'add(text)' && add.does === 'Keeps a note. Use it to remember something.' &&
      find?.head === 'find(words, most=10, o=nothing, back=-2)' && find.does === 'Finds notes by their words, the newest first.' &&
      JSON.stringify(find.inputSchema) === '{"type":"object","properties":{"words":{},"most":{"default":10},"o":{"default":null},"back":{"default":-2}},"required":["words"]}' &&
      key?.does === '' && JSON.stringify(l.settings) === '["units","key"]'
    return ok || JSON.stringify(l)
  }],
  ['the face Onsay is shown: every fn, with what it does', () => {
    const f = lingo({ op: 'face', natives: CORES, stacked: [NOTES], names: ['notes'] }).face
    return f === 'lingo notes\n// Notes, for the tests: kept and found again. A second line of what it is.\nnotes: none\nadd(text): Keeps a note. Use it to remember something.\nfind(words, most=10, o=nothing, back=-2): Finds notes by their words, the newest first.\nkey(k)' || f
  }],
  ['a top comment that runs into a fn is the about, never that fn\'s too', () => {
    const l = lingo({ op: 'read', file: 'one.lingo', code: '// one.lingo - says one\nfn one() {return 1}\n' }).lingo
    return (l?.about === 'says one' && l.phrases[0].does === '') || JSON.stringify(l)
  }],
  ['every fn is called at its address, a value left out taking its default', () => {
    const r = compile([NOTES], ['notes'], 'return [notes.find("milk"), notes.key("x")]')
    return !!(r.program?.fns?.['notes.find'] && r.program.fns['notes.key']) || JSON.stringify(r)
  }],
  ['a lingo in the same folder is called by its dotted name, linked to its own code, with no uses', () => {
    const geo = { file: 'geo.lingo', code: '// geo - places.\n\n// Where a place is.\nfn find(place) {return place}\n' }
    const r = compile([geo, notes((c) => c.replace('return [words, most]', 'return geo.find(words)'))], ['notes'], 'return notes.find("x")')
    return !!(/"geo\.find"/.test(JSON.stringify(r.program?.fns?.['notes.find'])) && r.program.fns['geo.find']) || JSON.stringify(r)
  }],
  ['a dotted call to a lingo that isn\'t here is a load error that says to use use(...)', () => {
    const r = compile([notes((c) => c.replace('return [words, most]', 'return kit.find.near(words)'))], ['notes'])
    return r.error?.message === 'notes.lingo line 16: There\'s no lingo or group called kit here. To reach one somewhere else, write use("its address"), like use("laptop.brandon/lingos/kit.lingo/find/near")' || JSON.stringify(r)
  }],
  ['use(...) in a lingo is a call of lingo.use, asked as it runs', () => {
    const r = compile([notes((c) => c.replace('return [words, most]', 'return use("bob.example.com/lingos/stockfish.lingo/move", [words])'))], ['notes'], 'return notes.find("x")')
    return JSON.stringify(r.program?.fns?.['notes.find']).includes('["say","lingo.use",{"address":"bob.example.com/lingos/stockfish.lingo/move"') || JSON.stringify(r)
  }],
  ['setting("key") in a lingo reads its own, by its name', () => {
    const r = compile([NOTES], ['notes'], 'return notes.add("x")')
    return JSON.stringify(r.program?.fns?.['notes.add']).includes('"reference":"notes"') || JSON.stringify(r)
  }],
  ['a lingo\'s file is its name: one that isn\'t a name, steps outside a fn, and no fn at all are refused', () => {
    const bad = [['Notes.lingo', 'fn a() {}'], ['notes.lingo', 'x = 1\nfn a() {}'], ['notes.lingo', '// nothing']].map(([file, code]) => lingo({ op: 'read', file, code }).error?.message)
    return JSON.stringify(bad) === JSON.stringify(['Notes.lingo: a lingo is a file named for it, like notes.lingo, its name lower case letters, digits, _ and -',
      'notes.lingo: only fn blocks belong in it, with no tool and no steps outside them', 'notes.lingo: a lingo shares its fn blocks, and it has none']) || JSON.stringify(bad)
  }],
  ['a lingo.json is only for a native or host lingo: one of a Lingo lingo says it is one file now', () => {
    const r = lingo({ op: 'read', manifest: { name: 'memo', version: '1.0.0', main: 'memo.lingo', functions: [] } })
    return /^a lingo written in Lingo is one file, memo\.lingo, in the lingos folder itself/.test(r.error?.message) || JSON.stringify(r)
  }],
  ['a native lingo\'s functions are described in the same JSON shape, and read back the same', () => {
    const all = CORES.flatMap((t) => manifest(t).phrases), bad = all.filter((p) => !p.inputSchema || p.gives !== gives(p.outputSchema, Object.assign({}, ...CORES.map((t) => manifest(t).shapes)))).map((p) => p.id)
    const click = CORES.flatMap((t) => lingo({ op: 'manifest', text: t }).manifest?.phrases ?? []).find((p) => p.id === 'input.click_text')
    return (all.length > 80 && !bad.length && JSON.stringify(click.inputSchema) === '{"type":"object","properties":{"words":{},"window":{"default":null},"nth":{"default":0}},"required":["words"]}') || JSON.stringify(bad)
  }],
  ['host lingos: a host lingo\'s functions read back as the lines they were', () => {
    const r = lingo({ op: 'read', manifest: manifestOf('core/test/lingos/hosted') }).lingo, lines = r?.phrases.map(line) || []
    const want = ['echo(x) -> any [none]: Gives back what it was given.', 'slow(seconds=1) -> text [none]: Waits, then says woke.', 'press() -> text [screen.act]: Needs a permission the tests don\'t give.']
    return (want.every((w) => lines.includes(w)) && r.groups.map((g) => g.name).join() === 'hosted') || JSON.stringify(lines)
  }],
  ['every lingo in the repo reads, and every library function says what it does', () => {
    const natives = ['lingos/windows', 'core/test/lingos/hosted'].map((d) => [d, lingo({ op: 'read', manifest: manifestOf(d) }).error?.message])
    const coded = [...files('core/test/lingos'), ...library].map((l) => [l.file, lingo({ op: 'read', ...l })])
    const silent = library.flatMap((l) => lingo({ op: 'read', ...l }).lingo?.phrases.filter((p) => !p.does).map((p) => p.id) ?? [])
    const bad = [...natives.filter(([, e]) => e), ...coded.filter(([, r]) => r.error).map(([f, r]) => [f, r.error.message])]
    return (!bad.length && !silent.length && library.every((l) => lingo({ op: 'read', ...l }).lingo.about)) || JSON.stringify([bad, silent])
  }],
  ['every library lingo compiles against core and windows, and calls the others by their dotted names', () => {
    const r = compile(library, library.map((l) => l.file.slice(0, -6)), 'return agent.ask("hi")')
    return !!(library.length >= 9 && !r.error && r.program.fns['openrouter.reply'] && r.program.fns['request.send']) || JSON.stringify(r.error)
  }],
]
let good = 0
for (const [name, check] of CHECKS) {
  let r
  try { r = check() } catch (err) { r = err.stack }
  if (r === true) good++
  else console.log(`FAIL ${name}\n  ${String(r).slice(0, 800)}`)
}
console.log(`${good} of ${CHECKS.length} Lingo lingo checks passed`)
process.exit(failed || good < CHECKS.length ? 1 : 0)
