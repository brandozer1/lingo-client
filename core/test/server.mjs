// server.mjs - a stand-in for Onsay on this computer, proving the lingo client end to end, over its connection and its port.
//
//   node test/server.mjs          then, in another terminal:   bin/test/onsay-core.exe test/onsay.json
// The test core's home is test/data, made fresh each run: its lingos folder holds the test lingos (test/lingos) as
// folders, memo packed into memo.lingo, every .lingo file the build made (bin/lingos), and a deflated one and a broken
// one made here. Each are running, as running.json says, under the policy.json written here. Each case is a request,
// sent down the connection as Onsay sends one for this computer's own account, or to the port, or a command, and what
// the answer must be. A case about a lingo the core hasn't got, like windows, is skipped.
//
// 1. WebSocket   just enough of it, with no packages
// 2. Zips        .lingo files read and made, stored or deflated
// 3. Cases       requests and what each must answer
// 4. Onsay       answers to lingo(...) and use(...), an AI that answers the way OpenAI does, signing in, and pages for
//                http.send
// 5. Session     hello, the cases in order, the catalog, then a drop the core must come back from

import http from 'node:http'
import net from 'node:net'
import os from 'node:os'
import crypto from 'node:crypto'
import zlib from 'node:zlib'
import { rmSync, cpSync, readdirSync, existsSync, mkdirSync, writeFileSync, readFileSync } from 'node:fs'
import { execFileSync, spawn } from 'node:child_process'
import { fileURLToPath } from 'node:url'

const PORT = 8787, TOKEN = 'test-token'   // bin/test/onsay-core.exe connects to ws://127.0.0.1:8787/live
const HTTP = `http://127.0.0.1:${PORT}/http`   // the stand-in's pages for http.send
const AI = `http://127.0.0.1:${PORT}/ai`        // and its AI, for agent
const WEB = 6970, BARE = 6971, CLOSED = 6973     // the test core's port, and the bare and the closed cores', below
const CORE = fileURLToPath(new URL('..', import.meta.url)), EXE = fileURLToPath(new URL('../bin/test/onsay-core.exe', import.meta.url))
const DATA = new URL('./data/', import.meta.url), SETTINGS = new URL('settings/', DATA), LINGOS = new URL('lingos/', DATA)
const LIBRARY = new URL('../../lingo-library/', import.meta.url), HOME = fileURLToPath(DATA).replace(/[\\/]$/, '')
// What the test core may do: everything but act on the screen, the ceiling, and grants for the lingos that read the
// screen, run commands and reach files outside its home. Anyone may reach page, and with no account policy, the
// defaults let 192.0.2.60 reach memo.
const POLICY = {
  ceiling: ['net', 'store', 'files.read', 'files.write', 'screen.read', 'run'],
  overrides: {
    access: [{ who: 'public', lingos: 'page' }],
    grants: {
      desk: ['screen.read', 'run', 'files.read ~/onsay-test', 'files.write ~/onsay-test'],
      windows: ['screen.read', 'files.read ~/no-such-file-here.txt', 'screen.act'],
      native: ['run'],
    },
  },
  defaults: { access: [{ who: 'ip 192.0.2.60', lingos: 'memo' }] },
}

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

////////// 2. ZIPS //////////

// Every file in a zip by its name, from its central directory, stored or deflated.
const unzip = (zip) => {
  const end = zip.lastIndexOf(Buffer.from([0x50, 0x4b, 5, 6])), out = {}
  for (let at = zip.readUInt32LE(end + 16), k = 0; k < zip.readUInt16LE(end + 10); k++) {
    const nl = zip.readUInt16LE(at + 28), local = zip.readUInt32LE(at + 42), size = zip.readUInt32LE(at + 20)
    const from = local + 30 + zip.readUInt16LE(local + 26) + zip.readUInt16LE(local + 28), data = zip.subarray(from, from + size)
    out[zip.toString('utf8', at + 46, at + 46 + nl)] = zip.readUInt16LE(at + 10) === 8 ? zlib.inflateRawSync(data) : data
    at += 46 + nl + zip.readUInt16LE(at + 30) + zip.readUInt16LE(at + 32)
  }
  return out
}

// A zip of files by name, each deflated, as most zip programs make them.
const deflated = (files) => {
  const parts = [], list = []
  let at = 0
  for (const [name, text] of Object.entries(files)) {
    const data = Buffer.from(text), packed = zlib.deflateRawSync(data), n = Buffer.from(name), head = Buffer.alloc(30), entry = Buffer.alloc(46)
    const sum = zlib.crc32(data)
    head.writeUInt32LE(0x04034b50, 0), head.writeUInt16LE(20, 4), head.writeUInt16LE(8, 8), head.writeUInt32LE(sum, 14)
    head.writeUInt32LE(packed.length, 18), head.writeUInt32LE(data.length, 22), head.writeUInt16LE(n.length, 26)
    entry.writeUInt32LE(0x02014b50, 0), entry.writeUInt16LE(20, 4), entry.writeUInt16LE(20, 6), entry.writeUInt16LE(8, 10), entry.writeUInt32LE(sum, 16)
    entry.writeUInt32LE(packed.length, 20), entry.writeUInt32LE(data.length, 24), entry.writeUInt16LE(n.length, 28), entry.writeUInt32LE(at, 42)
    parts.push(head, n, packed)
    list.push(entry, n)
    at += 30 + n.length + packed.length
  }
  const central = Buffer.concat(list), end = Buffer.alloc(22)
  end.writeUInt32LE(0x06054b50, 0), end.writeUInt16LE(list.length / 2, 8), end.writeUInt16LE(list.length / 2, 10)
  end.writeUInt32LE(central.length, 12), end.writeUInt32LE(at, 16)
  return Buffer.concat([...parts, central, end])
}

////////// 3. CASES //////////

const same = (a, b) => JSON.stringify(a) === JSON.stringify(b)
const gives = (want) => (r) => r.status === 200 && same(r.json, want)
const fails = (status, words) => (r) => r.status === status && words.test(r.json?.error)
const STOPPED = /Stopped by the person$/
const pause = (ms) => new Promise((done) => setTimeout(done, ms))
// A lingo's settings as this computer keeps them, a record by name in home/settings/<lingo>.json, as settings.set writes them.
const keep = (lingo, values) => {
  mkdirSync(SETTINGS, { recursive: true })
  writeFileSync(new URL(`${lingo}.json`, SETTINGS), JSON.stringify(values))
}
// The command line, on the test core's settings, whether or not the core runs: what it printed, as JSON when it is.
const command = (...args) => {
  const out = execFileSync(EXE, ['test/onsay.json', ...args], { cwd: CORE, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] })
  try { return JSON.parse(out) } catch { return out }
}
const policyFile = () => JSON.parse(readFileSync(new URL('policy.json', DATA), 'utf8'))
const decisions = () => readFileSync(new URL('decisions.log', DATA), 'utf8').trim().split('\n').map((l) => JSON.parse(l))

// Each is a request for path, a GET, or a POST of values as JSON, and what its answer must be. lingo: the device lingo it
// needs. stopAfter: a stop for it is sent that many ms after it.
const CASES = [
  // The language and the core's own phrases, from checks.ls.
  { path: '/checks/numbers', check: gives([7, 1, 2, 0.3333333333333333, 0.30000000000000004, -3, 1e21, 100000000000000000000, 123456789012, 0.000001, 1e-7, 0, 2.5, 2, 33.333333333333336]) },
  { path: '/checks/text_in_text', check: gives('3 and x and [1,"two",1.5] and  and true and {"k":null}') },
  { path: '/checks/lists', check: gives([[100, 4, 9, 16, 25, 6], 6, null, 6, [4, 9, 16, 25], false]) },
  { path: '/checks/copies', check: gives([[1, [2]], [1, [9]], { x: { y: 1 } }, { x: { y: 2 } }]) },
  { path: '/checks/records', check: gives([['b=2', 'a=1', 'c=3', 'b', 'a', 'c'], ['b', 'a', 'c'], true, null]) },
  { path: '/checks/texts', check: gives([12, 'wörld', 'll', 'rld', 'é', true, '1.5', '[1,"a"]']) },
  { path: '/checks/chars', check: gives(['0a', '1ñ', '2b']) },
  { path: '/checks/numbers_from_text', check: gives([1204.5, 12, null, -350]) },
  { path: '/checks/remainders', check: gives([1, 2, -2, 0.1, 0.2999999999999998, 0.9, 0, -2, 2, 0.75]) },
  { path: '/checks/jsons', check: gives([{ a: [1, 2.5, 'é\t'], b: null }, '{"a":[1,2.5,"é\\t"],"b":null}', [1, 'é', -2500, null], '"q\\"\\n"', '1.5']) },
  { path: '/checks/bytes_back', check: gives([[97, 195, 169, 226, 130, 172, 240, 159, 152, 128], 'aé€😀', '', "std.from_bytes: those bytes aren't UTF-8 text", 'std.from_bytes: a byte is a whole number from 0 to 255']) },
  { path: '/checks/functions', check: gives([6765, 11, 3]) },
  { path: '/checks/control', check: gives([11, 5, 'big', true, 2, 'b']) },
  { path: '/checks/greet', values: { who: 'Ana' }, check: gives(['hi Ana', 'hi Ana']) },
  { path: '/checks/try_and_fail', check: gives(['boom', null, "Can't use - with text", "std.from_json: that text isn't JSON"]) },
  { name: 'an error names its lingo and line', path: '/checks/error_at_a_line', check: fails(500, /^checks\.ls line \d+: Can't compare number with text$/) },
  { path: '/checks/divide_by_zero', check: fails(500, /Can't divide by zero/) },
  { path: '/checks/deep_loop', check: gives(5000050000) },
  { name: 'compute.log writes to the log, and the program goes on', path: '/checks/notes', check: gives(1) },
  { path: '/checks/waited', check: (r) => r.status === 200 && r.json >= 0.19 && r.json < 1 },
  { name: 'store: any key, kept as text, listed in order', path: '/checks/stored', check: gives(['milk', 'eggs', '[1]', null, ['notes/today'], null, ['Notes/Today', 'notes/today', 'x/../y']]) },
  { name: 'limit: too many steps', path: '/checks/forever', check: fails(500, /too many steps/), timed: true },
  { name: 'stop: between two steps', path: '/checks/forever', stopAfter: 300, check: fails(500, STOPPED) },
  { name: 'stop: in a wait', path: '/checks/wait_long', stopAfter: 300, check: fails(500, STOPPED) },
  { name: 'stop: a try lets it through', path: '/checks/forever_in_try', stopAfter: 300, check: fails(500, STOPPED) },
  { name: 'limit: calls too deep', path: '/checks/deeper', values: { n: 0 }, check: fails(500, /1000 deep/) },
  { name: 'limit: too much memory', path: '/checks/memory', check: fails(500, /too much memory/) },
  { name: 'a phrase takes its default', path: '/checks/sliced', check: gives('bc') },
  { name: 'a function a lingo has not is a 404', path: '/checks/nope', check: fails(404, /checks has no function checks\.nope/) },
  // memo, a Lingo lingo on the core's own storage, loaded from the .lingo file pack made of it, called by its routes.
  { path: '/memo/remember', values: { k: 'milk', v: '2 litres' }, check: gives(null) },
  { path: '/memo/remember', values: ['eggs', '6'], check: gives(null) },
  { path: '/memo/recall?k=milk', check: gives('2 litres') },
  { path: '/memo/all', check: gives(['eggs', 'milk']) },
  { name: '/name alone is its main', path: '/asker', check: gives("asker's main") },
  { name: 'a value it does not take is a 400', path: '/memo/recall', values: { key: 'milk' }, check: fails(400, /doesn't take a value called key/) },
  { name: 'a lingo with no main is a 404 at its route', path: '/memo', check: fails(404, /memo has no function memo\.main/) },
  { name: 'a .lingo file whose files are deflated', path: '/squeezed', check: gives('squeezed') },
  // setting(...): answered on this computer as the line runs, never asked of Onsay: the value kept in the test core's home,
  // else its openapi.json's default, else nothing.
  { name: 'setting(...) of another lingo, and nothing when unset', before: () => keep('github', { token: 'tok' }), path: '/checks/tokens', check: (r) => gives(['tok', 'metric'])(r) && !every.some((m) => 'key' in m) },
  { name: "setting(...): the default its openapi.json gives", path: '/prefs/units', check: gives('metric') },
  { name: 'setting(...): nothing when it has no default', path: '/prefs/name', check: gives('no name') },
  { name: 'setting(...): a value kept wins over the default', before: () => keep('prefs', { units: 'imperial', name: 'Ana' }), path: '/prefs/units', check: gives('imperial') },
  { name: "setting(...): another lingo's, from a lingo", path: '/prefs/token', check: gives('tok') },
  {
    name: "settings.set keeps one in its file, and settings.get gives each with what its openapi.json says and its value",
    path: '/checks/set_units',
    check: (r) => r.status === 200 && r.json[0] === 'kelvin' && same(r.json[1], [
      { name: 'units', title: 'Units', about: 'metric or imperial.', default: 'metric', required: true, value: 'kelvin' }, { name: 'name', value: 'Ana' }, { name: 'color', value: null }]) &&
      JSON.parse(readFileSync(new URL('prefs.json', SETTINGS), 'utf8')).units === 'kelvin',
  },
  { name: 'keep: a lingo that keeps its settings with its own functions', path: '/checks/vault_keep', check: (r) => gives(['0000', '1234', '"1234"', '1234'])(r) && !existsSync(new URL('vault.json', SETTINGS)) },
  {
    name: 'the command line: set changes one, as JSON or as text, and settings prints them all',
    before: () => {
      try {
        command('set', 'prefs', 'units', '"cli"')
        command('set', 'prefs', 'name', 'Bo')
        shown = command('settings', 'prefs')
      } catch (e) { shown = [e.message] }
    },
    path: '/prefs/name',
    check: (r) => gives('Bo')(r) && same(shown.map((s) => s.value), ['cli', 'Bo', null]) && shown[0].title === 'Units',
  },
  // agent: the library's AI, on a key from its settings on this computer, here the stand-in's.
  { name: 'agent.answer with no key says where to set one', before: () => keep('agent', {}), path: '/agent/answer', values: { prompt: 'hello' }, check: fails(500, /agent's settings/) },
  { name: 'agent.answer asks with the key, model and address from its settings', before: () => keep('agent', { openai_key: 'test-key', url: AI }), path: '/agent/answer', values: { prompt: 'hello' }, check: gives('gpt-5-mini says: hello') },
  { name: 'agent.answer with a model of its own', path: '/agent/answer', values: { prompt: 'hi there', model: 'qwen/qwen3' }, check: gives('qwen/qwen3 says: hi there') },
  { name: 'agent.answer with a key turned down', before: () => keep('agent', { openai_key: 'wrong', url: AI }), path: '/agent/answer', values: { prompt: 'hello' }, check: fails(500, /turned the key down/) },
  // http.send, the core's own web request, against the stand-in's own pages (PAGES below), and request on it.
  { name: 'http: a GET, its status, headers and text', path: '/checks/http_get', values: { base: HTTP }, check: gives([200, 'yes', 'hello ?x=1']) },
  { name: 'http: a POST with headers and a body', path: '/checks/http_post', values: { base: HTTP }, check: gives([201, { method: 'POST', name: 'Ada', type: 'text/plain', body: 'héllo' }]) },
  { name: 'http: an answer that is not 2xx is still an answer', path: '/checks/http_missing', values: { base: HTTP }, check: gives([404, 'nothing here']) },
  { name: 'http: a redirect is followed', path: '/checks/http_moved', values: { base: HTTP }, check: gives('hello ') },
  { name: 'http: a timeout', path: '/checks/http_slow', values: { base: HTTP }, check: fails(500, /http\.send: /) },
  { name: 'http: nothing at that address', path: '/checks/http_nowhere', check: (r) => r.status === 200 && /^http\.send: /.test(r.json) },
  { name: 'request: a GET with a query, tried again after a 503', path: '/checks/request_busy', values: { base: HTTP }, check: gives([200, true, { tries: 3 }]) },
  // text, list, math and time, from ../lingo-library, written in Lingo on the core's floor.
  { path: '/checks/texts_library', check: gives(['HÉLLO, WÖRLD', 'héllo, wörld', ['Héllo', 'wörld'], 'a-b', 'HéLLo, wörLd', true, 'cba', 'x', ['a', 'b', 'c'], ['a', 'b'], '1x', 'x y']) },
  { path: '/checks/cases', check: gives(['ÇA VA, ØRE, ŁÓDŹ, Ÿ Ÿ, ΑΛΦΑ, ПРИВЕТ, ЁЁ', 'ça va, øre, łódź, ÿ ÿ, αλφα, привет, ёё', ['a', 'b', ''], 'bbbbbb', '', '[1];{"a":2}']) },
  { path: '/checks/sorts', check: gives([[1, 2, 3], ['a', 'b', 'c'], [{ n: 1, k: 'y' }, { n: 2, k: 'x' }, { n: 2, k: 'z' }], [], ['B', 'C', 'a'], [3, 2, 1], [], 'bña']) },
  { path: '/checks/rounds', check: gives([3, 1, -2, -2, 2, 2, 'b', 1200, 1.3, -1, 2, 0, 0]) },
  { path: '/checks/days', check: (r) => r.status === 200 && same(r.json.slice(0, 6), [59, 0.4166666666666667, 0.10416087962962962, 2.5, null, 1792325100]) && /nope isn't a time/.test(r.json[6]) },
  // native: an OS function called by its layout, values in order, run granted.
  { lingo: 'native', name: 'native: an OS function with no values, a whole number back', path: '/native/native/call', values: ['kernel32.dll', 'GetTickCount', 'u32', []], check: (r) => r.status === 200 && r.json > 0 },
  { lingo: 'native', name: 'native: a text value passed in', path: '/native/call', values: ['kernel32.dll', 'lstrlenA', 'i32', [{ type: 'text', value: 'hello' }]], check: gives(5) },
  { lingo: 'native', name: 'native: a function that is not there', path: '/native/native/call', values: ['kernel32.dll', 'NoSuchFunction', 'i32', []], check: fails(500, /was not found/) },
  // windows, for real, on this computer, from desk.ls and by its routes, under the grants in POLICY. The ceiling leaves
  // out screen.act, so nothing is clicked or typed.
  { lingo: 'windows', path: '/desk/windows_open', check: gives([true, true, true]) },
  { lingo: 'windows', path: '/desk/controls_read', check: gives([true, true]) },
  { lingo: 'windows', name: 'grants: files outside the home, in the folder granted', path: '/desk/files_used', check: gives(['héllo there', false, true, 2, 'c.txt', 12, false]) },
  { lingo: 'windows', name: 'windows: a missing file, its path granted', path: '/windows/files/read', values: { path: '~/no-such-file-here.txt' }, check: fails(500, /doesn't exist/) },
  { lingo: 'windows', name: 'windows: the clipboard', path: '/windows/clipboard/read', check: (r) => r.status === 200 && typeof r.json === 'string' },
  { lingo: 'windows', path: '/desk/commands_run', check: gives([0, '5', true]) },
  { lingo: 'windows', name: 'windows: a command stopped', path: '/desk/command_slow', stopAfter: 1500, check: fails(500, STOPPED) },
  { lingo: 'windows', path: '/desk/screen_read', check: gives([true, true, true, true]) },
  { lingo: 'windows', name: 'the ceiling: typing is refused, granted or not', path: '/windows/input/type', values: { text: 'x' }, check: fails(403, /never allows/) },
  { lingo: 'windows', name: 'input: wait for words that never show', path: '/windows/input/wait_for_text', values: { words: 'zq-never-on-screen-qz', seconds: 1 }, check: gives(null) },
  { lingo: 'windows', name: 'the ceiling: a click is refused', path: '/desk/click_a_word', check: (r) => r.status === 403 },
  { lingo: 'windows', name: 'look: open windows', path: '/windows/look/open_windows', check: (r) => r.status === 200 && r.json.length > 0 && r.json.every((w) => w.app !== undefined && w.title) },
  { lingo: 'windows', name: 'look: a window in brief', path: '/desk/overview', check: (r) => r.status === 200 && typeof r.json.title === 'string' && Array.isArray(r.json.pressable) && Array.isArray(r.json.fields) },
  { lingo: 'windows', name: 'look: lines, where one is, and what is near it', path: '/desk/lines_near', check: gives([true, true, true]) },
  { lingo: 'windows', name: 'input: the pointer mode', path: '/desk/cursor_modes', check: gives(['virtual', 'real', 'virtual']) },
  { lingo: 'windows', name: 'input: a mode it has not', path: '/windows/input/cursor', values: { how: 'ghost' }, check: fails(500, /virtual or real/) },
  { lingo: 'windows', name: 'input: point at words on screen', path: '/desk/point_at_words', check: gives([true, true, true]) },
  { lingo: 'windows', name: 'input: point at a box, then stop', path: '/windows/input/point_at', values: [10, 10, 60, 20, 'here', 1], check: (r) => gives(null)(r) },
  { lingo: 'windows', path: '/windows/input/stop_pointing', check: gives(null) },
  { lingo: 'windows', name: 'input: places in a window, and on the main screen', path: '/desk/places', check: gives([true, true, true, true, true, true, true, true]) },
  { lingo: 'windows', name: 'the ceiling: a click in a window is refused', path: '/desk/click_in_window', check: (r) => r.status === 403 },
  { lingo: 'windows', name: 'look: elements, numbered and placed', path: '/desk/elements', check: gives(true) },
  // lingo(...): asked of Onsay as the program reaches it, here the stand-in (LINGO below), and the program goes on with
  // the answer. Requests Onsay sends meanwhile are answered inside the waiting one.
  {
    name: 'lingo(...): asked as it runs, with what was around it and the function asking',
    path: '/checks/capital', values: { country: 'France' },
    check: (r) => gives('it is Paris')(r) && asked.ask === 'the capital of a country' && same(asked.values, { country: 'France' }) && asked.gives === 'text' &&
      typeof asked.id === 'number' && asked.asking === 'checks.capital' && /^(windows|linux)$/.test(asked.context?.os) &&
      /^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(Z|[+-]\d\d:\d\d)$/.test(asked.context.time),
  },
  { name: 'lingo(...) with words worked out as it runs', path: '/checks/capital_of_japan', check: (r) => gives('Tokyo')(r) && asked.gives === '' },
  { name: "lingo(...): Onsay reaches this computer's lingos while it waits", path: '/checks/only_here', check: (r) => gives('heard: 2 litres')(r) && device?.status === 200 },
  { name: "a lingo's own lingo(...)", path: '/asker/kept', values: { k: 'eggs' }, check: gives('kept: eggs') },
  {
    name: 'a lingo reaching one elsewhere with use(...), asked of Onsay at its address',
    path: '/asker/forecast', values: { city: 'Rome' },
    check: (r) => gives('Rome: sunny for 1 days')(r) && asked.address === 'weather.example.com/lingos/weather.lingo/forecast' && same(asked.values, ['Rome', 1]),
  },
  { name: 'a lingo calling one in the same lingos folder by its dotted name', path: '/asker/noted', values: { k: 'milk' }, check: gives('2 litres') },
  { name: 'lingo(...) nothing can do', path: '/checks/impossible', check: fails(500, /book a flight/) },
  { name: 'lingo(...): stopped while Onsay works', path: '/checks/unanswered', stopAfter: 300, check: fails(500, STOPPED) },
  { name: 'the core works after a stop in lingo(...)', path: '/checks/capital_of_japan', check: gives('Tokyo') },
  // use(address, values): lingo.use, asked of Onsay by its address as the line runs, values by name or in order.
  {
    name: 'use(...): another computer by its address, Onsay reaching this one while it works',
    path: '/checks/use_by_name',
    check: (r) => gives('it says heard: 2 litres')(r) && device?.status === 200 && asked.address === 'notes.laptop/check' && same(asked.values, { key: 'milk' }) && !('ask' in asked),
  },
  { name: 'use(...) with its values in a list', path: '/checks/use_in_order', check: (r) => gives('Rome: sunny for 2 days')(r) && same(asked.values, ['Rome', 2]) },
  // agent.ask: the model, given its tools, reaches a lingo by its address and one by its words, Onsay reaching this
  // computer while the agent waits, and replies with what they gave.
  {
    name: 'agent.ask: its model uses its tools, and replies with what they gave, its lingo(...) saying it asks',
    before: () => keep('agent', { openai_key: 'test-key', url: AI }),
    path: '/agent/ask', values: { prompt: 'what is the weather in Rome?' },
    check: (r) => gives('gpt-5-mini heard: Rome: sunny for 2 days and heard: 2 litres')(r) && device?.status === 200 &&
      every.some((m) => m.ask === 'something only this device knows' && m.asking === 'agent.ask'),
  },
]

// Requests to the test core's own port (test/onsay.json), as any program on this computer sends them, its control
// routes as an app uses them, and down its connection as the proxy passes them on, from an address.
const web = (path, { method = 'GET', body, headers = {}, port = WEB, host = '127.0.0.1' } = {}) => new Promise((done) => {
  const q = http.request({ host, port, path, method, headers, agent: false }, (s) => {
    let text = ''
    s.setEncoding('utf8')
    s.on('data', (d) => { text += d })
    s.on('end', () => { let json; try { json = JSON.parse(text) } catch {} done({ status: s.statusCode, headers: s.headers, text, json }) })
  })
  q.on('error', (e) => done({ status: 0, text: e.message }))
  q.end(body === undefined ? undefined : typeof body === 'string' ? body : JSON.stringify(body))
})
const control = (verb, values) => web(`/.lingo/${verb}`, values === undefined ? {} : { method: 'POST', body: values })
const post = (path, body) => web(path, { method: 'POST', body })
// Bytes written straight to the port, for what no program would send, and the first line of the answer.
const raw = (text) => new Promise((done) => {
  let got = ''
  const s = net.connect(WEB, '127.0.0.1', () => s.end(text))
  s.on('data', (d) => { got += d })
  s.on('close', () => done(got.split('\r\n')[0]))
  s.on('error', () => done(''))
})
// Polls until check passes, or seconds pass, for what the core sees at its next look at its lingos folder.
const soon = async (check, seconds = 8) => {
  for (const until = Date.now() + seconds * 1000; Date.now() < until; await pause(250)) if (await check()) return true
  return false
}
// An app: asks for the questions waiting, as the Onsay app does, and answers each in turn from answers.
const app = (answers) => {
  const seen = []
  let going = true
  const poll = async () => {
    while (going) {
      const r = await control('asks')
      for (const ask of r.json ?? []) if (!seen.some((s) => s.id === ask.id)) { seen.push(ask); await control('answer', { id: ask.id, answer: answers.shift() }) }
      await pause(150)
    }
  }
  poll()
  return { seen, stop: () => { going = false } }
}
const OUTSIDE = `${os.tmpdir()}/onsay-test-outside.txt`, INSIDE = `${HOME}/inside.txt`   // a file outside the test core's home, and one inside

// Each is [name, send => whether it passed, lingo it needs]. send(path, values, more) is a request down the connection.
const WEB_CASES = [
  ['web: GET / lists every lingo for this computer, each at its route, and whether it runs', async () => {
    const r = await web('/')
    return r.status === 200 && r.json.lingos.some((l) => l.name === 'page' && l.route === '/hello' && l.running === true) && r.json.lingos.some((l) => l.name === 'memo' && l.route === '/memo') &&
      r.json.lingos.some((l) => l.name === 'broken' && /broken\.ls line 3/.test(l.error))
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
    const r = await post('/memo/recall', { k: 'milk' })
    return r.status === 200 && r.headers['content-type'] === 'application/json' && r.json === '2 litres'
  }],
  ['web: GET /name/function, its values in the query', async () => { const r = await web('/memo/recall?k=milk'); return r.status === 200 && r.json === '2 litres' }],
  ['web: a function it has not, and a lingo not here, are 404', async () => {
    const [a, b] = [await web('/memo/nope'), await web('/nothing/here')]
    return a.status === 404 && /memo has no function memo.nope/.test(a.json?.error) && b.status === 404
  }],
  ['web: a page elsewhere in a browser is not this computer, and reaches only what is shared with anyone', async () => {
    const [a, b] = [await web('/memo/recall?k=milk', { headers: { origin: 'http://elsewhere.example' } }), await web('/hello/text', { headers: { origin: 'http://elsewhere.example' } })]
    return a.status === 404 && b.status === 200
  }],
  ['web: what is not a web request is a 400', async () => (await raw('HELLO THERE\r\n\r\n')).startsWith('HTTP/1.1 400')],
  ['web: a body in chunks is a 411, and one over 4 MB a 413', async () =>
    (await raw('POST /memo/recall HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n')).startsWith('HTTP/1.1 411') &&
    (await raw('POST /memo/recall HTTP/1.1\r\nContent-Length: 5000000\r\n\r\n')).startsWith('HTTP/1.1 413')],
  ['web: a need above the ceiling is a 403', async () => (await post('/windows/input/type', ['x'])).status === 403, 'windows'],
  ['proxied: a request through Onsay is answered as the port answers it', async (send) => {
    const r = await send('/hello/text?name=Bo', undefined, { owner: undefined })
    return r.status === 200 && r.body === 'hi Bo' && r.base64 === false && r.headers['content-type'].startsWith('text/plain')
  }],
  ['proxied: a body in base64 is a 400 that says why', async (send) => {
    const r = await send('/hello/json', undefined, { owner: undefined, method: 'POST', body: Buffer.from([0xff, 0xfe]).toString('base64'), base64: true })
    return r.status === 400 && /isn't text/.test(r.json?.error)
  }],
  ['access: a lingo not shared is not there, and / lists only what is shared', async (send) => {
    const [a, b] = [await send('/memo/recall?k=milk', undefined, { owner: undefined }), await send('/', undefined, { owner: undefined })]
    return a.status === 404 && /There's nothing at \/memo\/recall/.test(a.json?.error) && same(b.json.lingos.map((l) => l.name), ['page'])
  }],
  ["owner: Onsay's own request counts as this computer: every running lingo, natives too", async (send) => {
    const [a, b] = [await send('/memo/recall?k=milk'), await send('/')]
    return a.status === 200 && a.json === '2 litres' && b.json.lingos.some((l) => l.name === 'memo') && (!has('windows') || b.json.lingos.some((l) => l.name === 'windows'))
  }],
  ['access: shared with one address, by the control route, and only that address reaches it', async (send) => {
    const shared = await control('share', { lingo: 'memo', who: 'ip 203.0.113.7' })
    const [a, b] = [await send('/memo/recall?k=milk', undefined, from('203.0.113.7')), await send('/memo/recall?k=milk', undefined, from('203.0.113.8'))]
    return shared.status === 200 && shared.json.access.some((r) => r.who === 'ip 203.0.113.7' && r.lingos === 'memo') && a.status === 200 && a.json === '2 litres' && b.status === 404
  }],
  ['access: shared with a range, by the command line, IPv4 and IPv6', async (send) => {
    command('share', 'asker', 'ip', '198.51.100.0/24')
    command('share', 'asker', 'ip 2001:db8::/32')
    const r = await Promise.all(['198.51.100.9', '198.51.101.1', '2001:db8::5', '2001:db9::5'].map((ip) => send('/asker', undefined, from(ip))))
    return same(r.map((x) => x.status), [200, 404, 200, 404]) && r[0].json === "asker's main"
  }],
  ['access: shared on one route only', async (send) => {
    command('share', 'checks', 'ip 192.0.2.1', '--route', '/numbers')
    const [a, b] = [await send('/checks/numbers', undefined, from('192.0.2.1')), await send('/checks/lists', undefined, from('192.0.2.1'))]
    return a.status === 200 && b.status === 404
  }],
  ['access: / lists for an address only what it may reach', async (send) => {
    const r = await send('/', undefined, from('203.0.113.7'))
    return same(r.json.lingos.map((l) => l.name).sort(), ['memo', 'page'])
  }],
  ['access: with no account policy, the defaults; with one, the account policy, the overrides still on top', async (send) => {
    const before = await send('/memo/all', undefined, from('192.0.2.60'))
    writeFileSync(new URL('policy.account.json', DATA), JSON.stringify({ version: 1, access: [{ who: 'ip 192.0.2.70', lingos: 'memo' }] }))
    const [a, b, c] = [await send('/memo/all', undefined, from('192.0.2.60')), await send('/memo/all', undefined, from('192.0.2.70')), await send('/hello/text', undefined, from('192.0.2.70'))]
    rmSync(new URL('policy.account.json', DATA))
    return before.status === 200 && a.status === 404 && b.status === 200 && c.status === 200
  }],
  ['access: a blocked address reaches nothing, shared or public', async (send) => {
    const p = policyFile()
    p.overrides.block = ['203.0.113.7']
    writeFileSync(new URL('policy.json', DATA), JSON.stringify(p))
    const [a, b, c] = [await send('/memo/recall?k=milk', undefined, from('203.0.113.7')), await send('/hello/text', undefined, from('203.0.113.7')), await send('/hello/text', undefined, from('203.0.113.9'))]
    return a.status === 404 && b.status === 404 && c.status === 200
  }],
  ['access: a request to its port from this computer\'s own network address is not this computer, and is judged by address', async () => {
    const lan = Object.values(os.networkInterfaces()).flat().find((i) => i.family === 'IPv4' && !i.internal)?.address
    if (!lan) return true
    const [a, b] = [await web('/memo/recall?k=milk', { host: lan, headers: { host: `${lan}:${WEB}` } }), await web('/hello/text', { host: lan, headers: { host: `${lan}:${WEB}` } })]
    return a.status === 404 && b.status === 200 && decisions().some((d) => d.caller === `ip ${lan}` && d.lingo === 'memo' && !d.allowed)
  }],
  ['the log: each access decided, with the caller, the lingo, the route and why', async () => {
    const d = decisions()
    return d.some((l) => l.caller === 'ip 203.0.113.8' && l.lingo === 'memo' && l.route === '/memo/recall' && l.need === null && l.allowed === false && l.why === 'no rule lets them in') &&
      d.some((l) => l.caller === 'ip 203.0.113.7' && l.allowed === false && l.why === 'blocked: 203.0.113.7') &&
      d.some((l) => l.caller === 'ip 203.0.113.7' && l.allowed === true && l.why === 'rule: ip 203.0.113.7') && d.every((l) => /^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$/.test(l.at))
  }],
  ['control: only this computer, never through Onsay, not even for its own account', async (send) => {
    const [a, b, c] = [await send('/.lingo/policy'), await send('/.lingo/policy', undefined, from('203.0.113.9')), await web('/.lingo/policy', { headers: { origin: 'http://elsewhere.example' } })]
    return a.status === 404 && b.status === 404 && c.status === 404 && (await control('policy')).status === 200
  }],
  ['control: the policy, the ceiling clamping its grants', async () => {
    const r = await control('policy')
    return same(r.json.ceiling, POLICY.ceiling) && same(r.json.grants.windows, ['screen.read', 'files.read ~/no-such-file-here.txt']) && r.json.local.overrides.grants.windows.includes('screen.act') &&
      r.json.account === null
  }],
  ['control: settings and set', async () => {
    const set = await control('set', { lingo: 'prefs', key: 'name', value: 'Cy' }), got = await web('/.lingo/settings?lingo=prefs')
    return set.status === 200 && got.status === 200 && got.json.find((s) => s.name === 'name').value === 'Cy'
  }],
  ['control: grant and ceiling, and what is wrong with one said plainly', async () => {
    const [a, b, c, d] = [await control('grant', { lingo: 'memo', need: 'files.read ~/Pictures' }), await control('grant', { lingo: 'memo', need: 'teleport' }),
      await control('ceiling', { needs: ['net', 'flying'] }), await control('nope', {})]
    return a.status === 200 && policyFile().overrides.grants.memo.includes('files.read ~/Pictures') && b.status === 400 && /A grant is a need/.test(b.json.error) &&
      c.status === 400 && /The ceiling is a list/.test(c.json.error) && d.status === 400 && /There's nothing called nope/.test(d.json.error)
  }],
  ['the ceiling: lowered, a need above it is refused for every lingo, and raised again', async () => {
    await control('ceiling', { needs: ['store'] })
    const r = await post('/checks/http_get', { base: HTTP })
    command('ceiling', ...POLICY.ceiling)
    const again = await post('/checks/http_get', { base: HTTP })
    return r.status === 403 && /checks needs net, which this computer never allows/.test(r.json.error) && again.status === 200 && same(policyFile().ceiling, POLICY.ceiling)
  }],
  ['running: stopped by the control route, it is not there, and leaves the catalog; started, back', async () => {
    const s = await control('stop', { lingo: 'memo' })
    const [a, list] = [await web('/memo/recall?k=milk'), await web('/')]
    const left = await soon(() => !catalog.some((l) => l.name === 'memo'))
    await control('start', { lingo: 'memo' })
    const b = await web('/memo/recall?k=milk')
    return same(s.json, { lingo: 'memo', running: false }) && a.status === 404 && list.json.lingos.some((l) => l.name === 'memo' && l.running === false) && left &&
      b.status === 200 && JSON.parse(readFileSync(new URL('running.json', DATA), 'utf8')).includes('memo')
  }],
  ['running: stopped and started by the command line, seen at the next look', async () => {
    command('stop', 'asker')
    const gone = await soon(async () => (await web('/asker')).status === 404)
    command('start', 'asker')
    return gone && await soon(async () => (await web('/asker')).status === 200)
  }],
  ['running: a lingo newly put in the lingos folder is stopped until started', async () => {
    mkdirSync(new URL('late/', LINGOS), { recursive: true })
    writeFileSync(new URL('late/late.ls', LINGOS), '// late - for the tests: put in the lingos folder while the core runs.\n\n// Says it came late.\nfn main() {return "late"}\n')
    const seen = await soon(async () => (await web('/')).json?.lingos.some((l) => l.name === 'late' && l.running === false))
    const before = await web('/late')
    await control('start', { lingo: 'late' })
    return seen && before.status === 404 && gives('late')(await web('/late'))
  }],
  ['pack: a folder made into its .lingo file, its paths made from its code, with what each needs', async () => {
    command('pack', 'test/lingos/memo', 'test/data/packed')
    const files = unzip(readFileSync(new URL('packed/memo.lingo', DATA))), m = JSON.parse(files['openapi.json']), recall = m.paths['/memo/recall']?.post
    return Object.keys(files).join() === 'openapi.json,memo.ls' && files['memo.ls'].equals(readFileSync(new URL('./lingos/memo/memo.ls', import.meta.url))) && m.openapi === '3.1.0' &&
      m.info.title === 'memo' && m.info.description === "things to remember, kept in Onsay's own storage on this device." && same(m['x-lingo'], { name: 'memo', program: 'memo.ls', route: '/memo' }) &&
      recall['x-lingo-function'] === 'memo.recall' && same(recall['x-lingo-needs'], ['store']) && recall.summary === 'What was kept under that name.' &&
      same(recall.requestBody.content['application/json'].schema, { type: 'object', properties: { k: {} }, required: ['k'] })
  }],
  ['pack: windows, a native lingo, its paths made from its phrases', async () => {
    const m = JSON.parse(unzip(readFileSync(fileURLToPath(new URL('../bin/lingos/windows.lingo', import.meta.url))))['openapi.json'])
    const type = m.paths['/windows/input/type']?.post
    return same(type?.['x-lingo-needs'], ['screen.act']) && type['x-lingo-function'] === 'input.type' && m['x-lingo'].program === 'windows.dll'
  }, 'windows'],
  ['pack: a serve lingo shares serve alone, at its route', async () => {
    command('pack', 'test/lingos/page', 'test/data/packed')
    const m = JSON.parse(unzip(readFileSync(new URL('packed/page.lingo', DATA)))['openapi.json'])
    return same(Object.keys(m.paths), ['/hello']) && m.paths['/hello'].get['x-lingo-function'] === 'page.serve'
  }],
  ['the command line: policy, grant written into policy.json, and what it does not do refused', async () => {
    const before = command('policy')
    let refused = false
    command('grant', 'memo', 'files.write', '~/Notes')
    try { command('--settings', 'prefs') } catch { refused = true }
    return before.ceiling.length === POLICY.ceiling.length && policyFile().overrides.grants.memo.includes('files.write ~/Notes') && refused
  }],
  ['the root: a file inside this lingo client\'s home is routine, no grant or app needed', async () => {
    writeFileSync(INSIDE, 'in here')
    return gives('in here')(await post('/chores/read', { path: INSIDE }))
  }, 'windows'],
  ['asks: with no app open, a need that takes a yes is refused, saying how to grant it', async () => {
    writeFileSync(OUTSIDE, 'out there')
    const [a, b] = [await post('/chores/run_it'), await post('/chores/read', { path: OUTSIDE })]
    return fails(403, /chores needs run here, which takes a yes from the person, and no app is open to ask them. To allow it, grant it: grant chores "run"/)(a) &&
      fails(403, /chores needs files\.read .*outside\.txt here/)(b) && decisions().some((d) => d.lingo === 'chores' && d.need === 'run' && d.why === 'no app to ask')
  }, 'windows'],
  ['asks: an app answers once, then no, then always, kept as a grant', async () => {
    const a = app(['once', 'no', 'always'])
    await pause(400)
    const r = []
    for (let i = 0; i < 4; i++) r.push(await post('/chores/run_it'))
    a.stop()
    const d = decisions()
    return same(r.map((x) => x.status), [200, 403, 200, 200]) && r[0].json === '7' && /The person said no to chores using run/.test(r[1].json.error) && a.seen.length === 3 &&
      same(Object.keys(a.seen[0]), ['id', 'lingo', 'need', 'path', 'caller']) && a.seen[0].lingo === 'chores' && a.seen[0].need === 'run' && a.seen[0].caller === 'this computer' &&
      policyFile().overrides.grants.chores.includes('run') && ['asked: once', 'asked: no', 'asked: always', 'granted: run'].every((why) => d.some((l) => l.lingo === 'chores' && l.why === why))
  }, 'windows'],
  ['asks: a file outside the home asked with its path', async () => {
    const a = app(['once'])
    await pause(400)
    const r = await post('/chores/read', { path: OUTSIDE })
    a.stop()
    rmSync(OUTSIDE)
    return gives('out there')(r) && a.seen.length === 1 && a.seen[0].need === 'files.read' && /outside\.txt$/.test(a.seen[0].path)
  }, 'windows'],
  ['load: a lingo whose openapi.json says less than its code needs is set aside with why', async () => {
    const r = await web('/')
    return r.json.lingos.some((l) => l.name === 'liar' && /liar\.ls: liar\.fetch needs net, which its openapi\.json doesn't say/.test(l.error))
  }],
  ['the command line: sign-in writes the token Onsay gives into the settings', async () => {
    writeFileSync(new URL('signin.json', DATA), JSON.stringify({ home: 'test/data/signin', port: 6972 }))
    const done = await new Promise((end) => spawn(EXE, ['test/data/signin.json', 'sign-in'], { cwd: CORE, stdio: 'ignore' }).on('exit', end))
    const s = JSON.parse(readFileSync(new URL('signin.json', DATA), 'utf8'))
    return done === 0 && s.token === 'signed-in-token' && /^[0-9a-f]{32}$/.test(s.install)
  }],
  ['bare: with no token it runs on its own, answers its port, its own limits hold, and lingo(...) says it is not connected', async () => {
    writeFileSync(new URL('bare.json', DATA), JSON.stringify({ home: 'test/data', lingos: 'test/data/lingos', port: BARE, steps: 1000000000, ms: 300 }))
    const core = spawn(EXE, ['test/data/bare.json'], { cwd: CORE, stdio: 'ignore' })
    let r = { status: 0 }
    for (let i = 0; i < 50 && !r.status; i++) { await pause(200); r = await web('/', { port: BARE }) }
    const [asked, long] = [await web('/asker/kept', { method: 'POST', body: { k: 'eggs' }, port: BARE }), await web('/checks/forever', { method: 'POST', port: BARE })]
    core.kill()
    return r.status === 200 && r.json.lingos.some((l) => l.name === 'memo') && asked.status === 500 && /isn't connected to Onsay/.test(asked.json?.error) &&
      long.status === 500 && /ran too long/.test(long.json?.error)
  }],
  ['the port: with nothing shared it is open to this computer alone, and to others within a look of a rule for an address', async () => {
    const lan = Object.values(os.networkInterfaces()).flat().find((i) => i.family === 'IPv4' && !i.internal)?.address
    if (!lan) return true
    writeFileSync(new URL('closed.json', DATA), JSON.stringify({ home: 'test/data/closed', lingos: 'test/data/lingos', port: CLOSED }))
    const core = spawn(EXE, ['test/data/closed.json'], { cwd: CORE, stdio: 'ignore' })
    const there = { port: CLOSED, host: lan, headers: { host: `${lan}:${CLOSED}` } }
    let r = { status: 0 }
    for (let i = 0; i < 50 && !r.status; i++) { await pause(200); r = await web('/', { port: CLOSED }) }
    const before = await web('/', there)
    execFileSync(EXE, ['test/data/closed.json', 'share', 'memo', 'ip', lan], { cwd: CORE, stdio: 'ignore' })
    const opened = await soon(async () => (await web('/', there)).status === 200, 10)
    core.kill()
    return r.status === 200 && before.status === 0 && opened
  }],
]
// What a request through Onsay's proxy carries, from an address.
const from = (ip) => ({ owner: undefined, headers: { host: 'laptop.example.onsay.dev', 'x-forwarded-for': `${ip}, 10.0.0.1` } })

////////// 4. ONSAY //////////

/* Canned answers to lingo(...) and use(...): each ask, by its words or its address, answered as the program runs, or
   failed with why. One reaches this computer on the way with a request, memo's recall, run while the program waits.
   asked is the last ask, every each one, device the last request's answer, shown what the command line printed. */
let asked = {}, device = null, every = [], shown = null
const LINGO = [
  [/capital of a country/, ({ values }) => ({ France: 'Paris', Japan: 'Tokyo' })[values.country]],
  [/kept under a name/, ({ values }) => `kept: ${values.name}`],
  [/^weather\/forecast$|\/weather\.lingo\/forecast$/, ({ values: [city, days] }) => `${city}: sunny for ${days} days`],
  [/only this device knows|^notes(\.laptop)?\/check$/, async (m, send) => {
    device = await send('/memo/recall?k=milk')
    if (device.status !== 200) throw new Error(device.json?.error)
    return `heard: ${device.json}`
  }],
  [/never answered/, () => new Promise(() => {})],
]
const answer = async (m, link, send) => {
  asked = m
  every.push(m)
  const found = LINGO.find(([match]) => match.test(m.ask ?? m.address))
  try {
    if (!found) throw new Error(m.address ? `There's no lingo called ${m.address.split(/[./]/)[0]}` : `Nothing can do "${m.ask}" yet`)
    link.send({ type: 'lingo', id: m.id, value: (await found[1](m, send)) ?? null })
  } catch (e) { link.send({ type: 'lingo', id: m.id, error: e.message, code: 'phrase' }) }
}

// An AI that answers the way OpenAI does, to the test key only, saying which model was asked and the last thing said.
// Given tools, as agent.ask gives them, it first reaches weather/forecast by its address and asks for one in words, then
// says what they gave.
const ai = (q, s) => {
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

// Signing in, as Onsay does it: a code to show, then the token once the person pressed Connect, here at once.
const SIGN_IN = {
  '/connect': (s) => s.writeHead(201, { 'content-type': 'application/json' }).end(JSON.stringify({ url: 'http://127.0.0.1:8787/connect', code: 'TEST-CODE', poll: 'p1' })),
  '/connect/poll': (s) => s.writeHead(200, { 'content-type': 'application/json' }).end(JSON.stringify({ status: 'done', token: 'signed-in-token', account: { handle: 'tester', email: 'tester@example.com' } })),
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

////////// 5. SESSION //////////

let catalog = [], hello = {}, hellos = 0, failed = 0, passed = 0, skipped = 0
const has = (name) => catalog.some((l) => l.name === name && l.status === 'ok')
const report = (name, ok, more = '', ms) => {
  ok ? passed++ : failed++
  console.log(`${ok ? '  ok  ' : '  FAIL'} ${name}${ms === undefined ? '' : ` (${ms} ms)`}${more}`)
}

const session = (link) => {
  let next = 0
  const waiting = new Map()
  // A request down the connection, Onsay's own for this computer's account unless more says otherwise. Its response, its
  // body read as JSON too.
  const send = (path, values, more = {}) => new Promise((done) => {
    const id = `r${++next}`
    waiting.set(id, (m) => { try { m.json = JSON.parse(m.body) } catch {} done(m) })
    link.send({ type: 'request', id, method: values === undefined ? 'GET' : 'POST', path, body: values === undefined ? '' : JSON.stringify(values), base64: false, owner: true,
      ...more, headers: { host: 'api.onsay.dev', 'content-type': 'application/json', ...more.headers } })
  })

  const go = async () => {
    for (const t of CASES) {
      const name = t.name ?? t.path.slice(1).replace('/', ': ').replace(/_/g, ' ')
      if (t.lingo && !has(t.lingo)) { skipped++; console.log(`  skip  ${name}: this core has no ${t.lingo}`); continue }
      t.before?.()
      const started = Date.now(), answered = send(t.path, t.values)
      if (t.stopAfter) setTimeout(() => link.send({ type: 'stop', id: `r${next}` }), t.stopAfter)
      const r = await answered, ok = t.check(r)
      report(name, ok, ok ? '' : `\n    got: ${r.status} ${r.body}`, Date.now() - started)
    }
    // The catalog: every running lingo and what it shares, made from its code for a Lingo lingo, never the code.
    const entry = (name) => catalog.find((l) => l.name === name), phrase = (lingo, id) => entry(lingo)?.phrases?.find((p) => p.id === id)
    const recall = phrase('memo', 'memo.recall')
    report('catalog: a Lingo lingo from a .lingo file, its listing made from its code, never the code', entry('memo')?.status === 'ok' &&
      entry('memo').about === "things to remember, kept in Onsay's own storage on this device." && entry('memo').phrases.length === 4 &&
      same(Object.keys(recall ?? {}), ['id', 'does', 'inputSchema', 'lingos']) && recall.does === 'What was kept under that name.' && recall.lingos === 0 &&
      'k' in (recall.inputSchema?.properties ?? {}) && !JSON.stringify(entry('memo')).includes('store.get'))
    report('catalog: each function counts its lingo(...) calls', phrase('asker', 'asker.kept')?.lingos === 1 && phrase('asker', 'asker.forecast')?.lingos === 0)
    report('catalog: core, every computer\'s floor, is never listed', !entry('core'))
    if (has('windows')) {
      const click = phrase('windows', 'input.click_text'), ids = entry('windows').phrases.map((p) => p.id)
      report('catalog: windows, a native lingo from its .lingo file, with what it is', entry('windows').native === true && entry('windows').about?.length > 0)
      report('windows: each phrase says what it gives and does, with its values', click?.does.startsWith('-> {x,y,text}: rings and clicks') &&
        same(click.inputSchema, { type: 'object', properties: { words: {}, window: { default: null }, nth: { default: 0 } }, required: ['words'] }))
      report('windows: one layer, input and look, the low level groups gone', ['look.screenshot', 'input.type', 'input.point_at', 'input.cursor'].every((id) => ids.includes(id)) &&
        !ids.some((id) => /^(mouse|keyboard|screen|pointer)\.|^look\.see$|^input\.move_and_click$/.test(id)))
    } else report('catalog: windows set aside where it can\'t load', entry('windows')?.status === 'broken')
    report('catalog: settings are names alone, its openapi.json\'s first, then any other its code reads', same(entry('prefs')?.settings, ['units', 'name', 'color']) && same(entry('memo')?.settings, []))
    report('catalog: agent is the one AI lingo, ready on every core', entry('agent')?.status === 'ok' && ['agent.ask', 'agent.answer'].every((id) => phrase('agent', id)) && !entry('openrouter'))
    if (has('native')) report('catalog: native is ready, one phrase that calls into Windows', phrase('native', 'native.call') !== undefined)
    report('catalog: a native without its library', entry('nolibrary')?.status === 'broken' && /nolibrary\.dll/.test(entry('nolibrary').error))
    report('catalog: a broken one says why', entry('broken')?.status === 'broken' && /^broken\.ls line 3: There's no lingo or group called nope here.*use\(/.test(entry('broken').error))
    report('catalog: a phrase spelled wrong says what there is', entry('typo')?.status === 'broken' && /There's no phrase std\.lenn/.test(entry('typo').error))
    report('catalog: a Lingo lingo can\'t hide a group of windows', entry('input')?.status === 'broken' &&
      (has('windows') ? /already a part of windows/ : /no lingo or group called pointer/).test(entry('input').error))
    report('catalog: a folder with no openapi.json and no program is not a lingo', !entry('notalingo'))
    report('catalog: a .lingo file that is not a zip says so', entry('notazip')?.status === 'broken' && /isn't a \.lingo file/.test(entry('notazip').error))
    const library = readdirSync(LIBRARY).filter((f) => existsSync(new URL(`${f}/${f}.ls`, LIBRARY)))
    const silent = library.flatMap((l) => (entry(l)?.phrases ?? []).filter((p) => !p.does).map((p) => p.id))
    report('library: every lingo loads from the .lingo file the build packed, says what it is, and every function says what it does', library.length >= 9 &&
      library.every((l) => entry(l)?.status === 'ok' && entry(l).about) && !silent.length, silent.length ? `\n    silent: ${silent.join(', ')}` : '')
    report('hello: the lingo client, its version and its catalog, and nothing else', same(Object.keys(hello), ['type', 'token', 'client', 'version', 'catalog']) &&
      hello.client === 'lingo-client' && hello.version === '0.6.0' && Array.isArray(hello.catalog))
    for (const [name, test, lingo] of WEB_CASES) {
      if (lingo && !has(lingo)) { skipped++; console.log(`  skip  ${name}: this core has no ${lingo}`); continue }
      const started = Date.now()
      report(name, await test(send).catch((e) => { console.log(`    ${e.message}`); return false }), '', Date.now() - started)
    }
    // Then the connection drops, and the core must come back by itself.
    console.log('  dropping the connection...')
    link.drop()
  }

  return (text) => {
    const m = JSON.parse(text)
    if (m.type === 'hello') {
      hellos++
      if (m.token !== TOKEN) { link.drop(); return }
      hello = m
      catalog = m.catalog
      console.log(`hello ${hellos} from ${m.client} ${m.version}: ${catalog.map((l) => `${l.name} ${l.status}`).join(', ')}`)
      link.send({ type: 'ready', device: 'test-device' })
      if (hellos === 1) go()
      else { report('reconnects by itself', true); console.log(`\n${passed} passed, ${failed} failed${skipped ? `, ${skipped} skipped` : ''}`); process.exit(failed ? 1 : 0) }
    } else if (m.type === 'response') waiting.get(m.id)?.(m)
    else if (m.type === 'lingo' && 'id' in m) answer(m, link, send)
    else if (m.type === 'catalog') catalog = m.catalog
    else if (m.type === 'ping') link.send({ type: 'pong' })
    else console.log('  core said', m)
  }
}

// Every run starts from a fresh home: no store, no settings, the policy above, and the lingos folder made again, as a
// person puts lingos in a computer's lingos folder, every one of them running.
rmSync(DATA, { recursive: true, force: true })
mkdirSync(LINGOS, { recursive: true })
for (const from of [new URL('./lingos/', import.meta.url), new URL('../bin/lingos/', import.meta.url)])
  for (const name of readdirSync(from)) if (name !== 'memo') cpSync(new URL(name, from), new URL(name, LINGOS), { recursive: true })
writeFileSync(new URL('squeezed.lingo', LINGOS), deflated({
  'openapi.json': JSON.stringify({ 'x-lingo': { program: 'squeezed.ls' } }),
  'squeezed.ls': '// squeezed - for the tests: a .lingo file whose files are deflated, as most zip programs make them.\n\n// Says its name.\nfn main() {return "squeezed"}\n',
}))
writeFileSync(new URL('notazip.lingo', LINGOS), 'just text, not a zip')
writeFileSync(new URL('policy.json', DATA), JSON.stringify(POLICY))
execFileSync(EXE, ['test/onsay.json', 'pack', 'test/lingos/memo', 'test/data/lingos'], { cwd: CORE, stdio: 'ignore' })
writeFileSync(new URL('running.json', DATA), JSON.stringify(readdirSync(LINGOS).map((n) => n.replace(/\.lingo$/, ''))))

const server = http.createServer((q, s) => {
  if (q.method === 'POST' && q.url === '/ai/chat/completions') return ai(q, s)
  if (q.method === 'POST' && SIGN_IN[q.url]) return q.resume().on('end', () => SIGN_IN[q.url](s))
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
setTimeout(() => { console.log('timed out'); process.exit(1) }, 300000)
