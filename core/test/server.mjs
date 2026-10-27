// server.mjs - a stand-in for Onsay on this computer, proving the lingo client end to end, over its connection and its port.
//
//   node test/server.mjs          then, in another terminal:   bin/test/onsay-core.exe test/onsay.json
// The test core's lingos folder is test/data/lingos, made fresh each run from the test lingos (test/lingos), the library
// (../lingo-library) and the device lingos as built (bin/lingos). Each case is a request, sent down the connection as
// Onsay sends one for this computer's own account, or to the port, and what the answer must be. A case about a lingo
// the core hasn't got, like windows, is skipped.
//
// 1. WebSocket   just enough of it, with no packages
// 2. Cases       requests and what each must answer
// 3. Onsay       answers to lingo(...) and use(...), an AI that answers the way OpenAI does, and pages for http.send
// 4. Session     hello, the cases in order, the catalog, then a drop the core must come back from

import http from 'node:http'
import net from 'node:net'
import crypto from 'node:crypto'
import { rmSync, cpSync, readdirSync, existsSync, mkdirSync, writeFileSync, readFileSync } from 'node:fs'
import { execFileSync, spawn } from 'node:child_process'
import { fileURLToPath } from 'node:url'

const PORT = 8787, TOKEN = 'test-token'   // bin/test/onsay-core.exe connects to ws://127.0.0.1:8787/live
const HTTP = `http://127.0.0.1:${PORT}/http`   // the stand-in's pages for http.send
const AI = `http://127.0.0.1:${PORT}/ai`        // and its AI, for agent
const WEB = 6970, BARE = 6971                   // the test core's port, and the bare core's, below
const CORE = fileURLToPath(new URL('..', import.meta.url)), EXE = fileURLToPath(new URL('../bin/test/onsay-core.exe', import.meta.url))
const DATA = new URL('./data/', import.meta.url), SETTINGS = new URL('settings/', DATA), LINGOS = new URL('lingos/', DATA)
const LIBRARY = new URL('../../lingo-library/', import.meta.url)

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

////////// 2. CASES //////////

const same = (a, b) => JSON.stringify(a) === JSON.stringify(b)
const gives = (want) => (r) => r.status === 200 && same(r.json, want)
const fails = (status, words) => (r) => r.status === status && words.test(r.json?.error)
const STOPPED = /Stopped by the person$/
// A lingo's settings as this computer keeps them, a record by name in home/settings/<lingo>.json, as settings.set writes them.
const keep = (lingo, values) => {
  mkdirSync(SETTINGS, { recursive: true })
  writeFileSync(new URL(`${lingo}.json`, SETTINGS), JSON.stringify(values))
}

// Each is a request for path, a GET, or a POST of values as JSON, and what its answer must be. lingo: the device lingo it
// needs. stopAfter: a stop for it is sent that many ms after it.
const CASES = [
  // The language and the core's own phrases, from checks.lingo.
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
  { name: 'an error names its lingo and line', path: '/checks/error_at_a_line', check: fails(500, /^checks\.lingo line \d+: Can't compare number with text$/) },
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
  // memo, a Lingo lingo on the core's own storage, called by its routes.
  { path: '/memo/remember', values: { k: 'milk', v: '2 litres' }, check: gives(null) },
  { path: '/memo/remember', values: ['eggs', '6'], check: gives(null) },
  { path: '/memo/recall?k=milk', check: gives('2 litres') },
  { path: '/memo/all', check: gives(['eggs', 'milk']) },
  { name: '/name alone is its main', path: '/asker', check: gives("asker's main") },
  { name: 'a value it does not take is a 400', path: '/memo/recall', values: { key: 'milk' }, check: fails(400, /doesn't take a value called key/) },
  { name: 'a lingo with no main is a 404 at its route', path: '/memo', check: fails(404, /memo has no function memo\.main/) },
  // setting(...): answered on this computer as the line runs, never asked of Onsay: the value kept in the test core's home,
  // else its lingo.json's default, else nothing.
  { name: 'setting(...) of another lingo, and nothing when unset', before: () => keep('github', { token: 'tok' }), path: '/checks/tokens', check: (r) => gives(['tok', 'metric'])(r) && !every.some((m) => 'key' in m) },
  { name: 'setting(...): the default its lingo.json gives', path: '/prefs/units', check: gives('metric') },
  { name: 'setting(...): nothing when it has no default', path: '/prefs/name', check: gives('no name') },
  { name: 'setting(...): a value kept wins over the default', before: () => keep('prefs', { units: 'imperial', name: 'Ana' }), path: '/prefs/units', check: gives('imperial') },
  { name: "setting(...): another lingo's, from a lingo", path: '/prefs/token', check: gives('tok') },
  {
    name: 'settings.set keeps one in its file, and settings.get gives each with what its lingo.json says and its value',
    path: '/checks/set_units',
    check: (r) => r.status === 200 && r.json[0] === 'kelvin' && same(r.json[1], [
      { name: 'units', title: 'Units', about: 'metric or imperial.', default: 'metric', required: true, value: 'kelvin' }, { name: 'name', value: 'Ana' }, { name: 'color', value: null }]) &&
      JSON.parse(readFileSync(new URL('prefs.json', SETTINGS), 'utf8')).units === 'kelvin',
  },
  { name: 'keep: a lingo that keeps its settings with its own functions', path: '/checks/vault_keep', check: (r) => gives(['0000', '1234', '"1234"', '1234'])(r) && !existsSync(new URL('vault.json', SETTINGS)) },
  {
    name: 'the command line: --set changes one, as JSON or as text, and --settings prints them all',
    before: () => {
      const core = (...args) => execFileSync(EXE, ['test/onsay.json', ...args], { cwd: CORE, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] })
      try {
        core('--set', 'prefs', 'units', '"cli"')
        core('--set', 'prefs', 'name', 'Bo')
        shown = JSON.parse(core('--settings', 'prefs'))
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
  // native: an OS function called by its layout, values in order.
  { lingo: 'native', name: 'native: an OS function with no values, a whole number back', path: '/native/native/call', values: ['kernel32.dll', 'GetTickCount', 'u32', []], check: (r) => r.status === 200 && r.json > 0 },
  { lingo: 'native', name: 'native: a text value passed in', path: '/native/call', values: ['kernel32.dll', 'lstrlenA', 'i32', [{ type: 'text', value: 'hello' }]], check: gives(5) },
  { lingo: 'native', name: 'native: a function that is not there', path: '/native/native/call', values: ['kernel32.dll', 'NoSuchFunction', 'i32', []], check: fails(500, /was not found/) },
  // windows, for real, on this computer, from desk.lingo and by its routes. The test core may read the screen but never
  // act on it, so nothing is clicked or typed.
  { lingo: 'windows', path: '/desk/windows_open', check: gives([true, true, true]) },
  { lingo: 'windows', path: '/desk/controls_read', check: gives([true, true]) },
  { lingo: 'windows', path: '/desk/files_used', check: gives(['héllo there', false, true, 2, 'c.txt', 12, false]) },
  { lingo: 'windows', name: 'windows: a missing file', path: '/windows/files/read', values: { path: '~/no-such-file-here.txt' }, check: fails(500, /doesn't exist/) },
  { lingo: 'windows', name: 'windows: the clipboard', path: '/windows/clipboard/read', check: (r) => r.status === 200 && typeof r.json === 'string' },
  { lingo: 'windows', path: '/desk/commands_run', check: gives([0, '5', true]) },
  { lingo: 'windows', name: 'windows: a command stopped', path: '/desk/command_slow', stopAfter: 1500, check: fails(500, STOPPED) },
  { lingo: 'windows', path: '/desk/screen_read', check: gives([true, true, true, true]) },
  { lingo: 'windows', name: 'windows: typing is refused without screen.act', path: '/windows/input/type', values: { text: 'x' }, check: (r) => r.status === 403 },
  { lingo: 'windows', name: 'input: wait for words that never show', path: '/windows/input/wait_for_text', values: { words: 'zq-never-on-screen-qz', seconds: 1 }, check: gives(null) },
  { lingo: 'windows', name: 'input: a click is refused without screen.act', path: '/desk/click_a_word', check: (r) => r.status === 403 },
  { lingo: 'windows', name: 'look: open windows', path: '/windows/look/open_windows', check: (r) => r.status === 200 && r.json.length > 0 && r.json.every((w) => w.app !== undefined && w.title) },
  { lingo: 'windows', name: 'look: a window in brief', path: '/desk/overview', check: (r) => r.status === 200 && typeof r.json.title === 'string' && Array.isArray(r.json.pressable) && Array.isArray(r.json.fields) },
  { lingo: 'windows', name: 'look: lines, where one is, and what is near it', path: '/desk/lines_near', check: gives([true, true, true]) },
  { lingo: 'windows', name: 'input: the pointer mode', path: '/desk/cursor_modes', check: gives(['virtual', 'real', 'virtual']) },
  { lingo: 'windows', name: 'input: a mode it has not', path: '/windows/input/cursor', values: { how: 'ghost' }, check: fails(500, /virtual or real/) },
  { lingo: 'windows', name: 'input: point at words on screen', path: '/desk/point_at_words', check: gives([true, true, true]) },
  { lingo: 'windows', name: 'input: point at a box, then stop', path: '/windows/input/point_at', values: [10, 10, 60, 20, 'here', 1], check: (r) => gives(null)(r) },
  { lingo: 'windows', path: '/windows/input/stop_pointing', check: gives(null) },
  { lingo: 'windows', name: 'input: places in a window, and on the main screen', path: '/desk/places', check: gives([true, true, true, true, true, true, true, true]) },
  { lingo: 'windows', name: 'input: a click in a window is refused without screen.act', path: '/desk/click_in_window', check: (r) => r.status === 403 },
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

// Requests to the test core's own port (test/onsay.json), as any program on this computer sends them, and down its
// connection without the owner's mark, as the proxy passes them. Each is [name, send => whether it passed, lingo].
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
const WEB_CASES = [
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
  ['web: a permission not given is a 403', async () => (await web('/windows/input/type', { method: 'POST', body: ['x'] })).status === 403, 'windows'],
  ['proxied: a request through Onsay is answered as the port answers it', async (send) => {
    const r = await send('/hello/text?name=Bo', undefined, { owner: undefined })
    return r.status === 200 && r.body === 'hi Bo' && r.base64 === false && r.headers['content-type'].startsWith('text/plain')
  }],
  ['proxied: a body in base64 is a 400 that says why', async (send) => {
    const r = await send('/hello/json', undefined, { owner: undefined, method: 'POST', body: Buffer.from([0xff, 0xfe]).toString('base64'), base64: true })
    return r.status === 400 && /isn't text/.test(r.json?.error)
  }],
  ['proxied: a lingo not open is not there, and / lists the open ones only', async (send) => {
    const [a, b] = [await send('/memo/recall?k=milk', undefined, { owner: undefined }), await send('/', undefined, { owner: undefined })]
    return a.status === 404 && same(b.json.lingos.map((l) => l.name), ['page'])
  }],
  ["owner: Onsay's own request reaches every lingo, natives too, and / lists them all", async (send) => {
    const [a, b] = [await send('/memo/recall?k=milk'), await send('/')]
    return a.status === 200 && a.json === '2 litres' && b.json.lingos.some((l) => l.name === 'memo') && (!has('windows') || b.json.lingos.some((l) => l.name === 'windows'))
  }],
  ['bare: with no token it runs on its own, answers its port, its own limits hold, and lingo(...) says it is not connected', async () => {
    writeFileSync(new URL('bare.json', DATA), JSON.stringify({ home: 'test/data', lingos: 'test/data/lingos', port: BARE, allow: ['none'], steps: 1000000000, ms: 300 }))
    const core = spawn(EXE, ['test/data/bare.json'], { cwd: CORE, stdio: 'ignore' })
    let r = { status: 0 }
    for (let i = 0; i < 50 && !r.status; i++) { await new Promise((d) => setTimeout(d, 200)); r = await web('/', { port: BARE }) }
    const [asked, long] = [await web('/asker/kept', { method: 'POST', body: { k: 'eggs' }, port: BARE }), await web('/checks/forever', { method: 'POST', port: BARE })]
    core.kill()
    return r.status === 200 && r.json.lingos.some((l) => l.name === 'memo') && asked.status === 500 && /isn't connected to Onsay/.test(asked.json?.error) &&
      long.status === 500 && /ran too long/.test(long.json?.error)
  }],
]

////////// 3. ONSAY //////////

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

////////// 4. SESSION //////////

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
    link.send({ type: 'request', id, method: values === undefined ? 'GET' : 'POST', path, headers: { host: 'api.onsay.dev', 'content-type': 'application/json' },
      body: values === undefined ? '' : JSON.stringify(values), base64: false, owner: true, ...more })
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
    // The catalog: what each lingo shares, made from its code for a Lingo lingo, never its code.
    const entry = (name) => catalog.find((l) => l.name === name), phrase = (lingo, id) => entry(lingo)?.phrases?.find((p) => p.id === id)
    const recall = phrase('memo', 'memo.recall')
    report('catalog: a Lingo lingo, its listing made from its code, never the code', entry('memo')?.status === 'ok' &&
      entry('memo').about === "things to remember, kept in Onsay's own storage on this device." && entry('memo').phrases.length === 4 &&
      same(Object.keys(recall ?? {}), ['id', 'does', 'inputSchema', 'lingos']) && recall.does === 'What was kept under that name.' && recall.lingos === 0 &&
      'k' in (recall.inputSchema?.properties ?? {}) && !JSON.stringify(entry('memo')).includes('store.get'))
    report('catalog: each function counts its lingo(...) calls', phrase('asker', 'asker.kept')?.lingos === 1 && phrase('asker', 'asker.forecast')?.lingos === 0)
    report('catalog: core, every computer\'s floor, is never listed', !entry('core'))
    if (has('windows')) {
      const click = phrase('windows', 'input.click_text'), ids = entry('windows').phrases.map((p) => p.id)
      report('catalog: windows, a native lingo, with what it is', entry('windows').native === true && entry('windows').about?.length > 0)
      report('windows: each phrase says what it gives and does, with its values', click?.does.startsWith('-> {x,y,text}: rings and clicks') &&
        same(click.inputSchema, { type: 'object', properties: { words: {}, window: { default: null }, nth: { default: 0 } }, required: ['words'] }))
      report('windows: one layer, input and look, the low level groups gone', ['look.screenshot', 'input.type', 'input.point_at', 'input.cursor'].every((id) => ids.includes(id)) &&
        !ids.some((id) => /^(mouse|keyboard|screen|pointer)\.|^look\.see$|^input\.move_and_click$/.test(id)))
    } else report('catalog: windows set aside where it can\'t load', entry('windows')?.status === 'broken')
    report('catalog: settings are names alone, its lingo.json\'s first, then any other its code reads', same(entry('prefs')?.settings, ['units', 'name', 'color']) && same(entry('memo')?.settings, []))
    report('catalog: agent is the one AI lingo, ready on every core', entry('agent')?.status === 'ok' && ['agent.ask', 'agent.answer'].every((id) => phrase('agent', id)) && !entry('openrouter'))
    if (has('native')) report('catalog: native is ready, one phrase that calls into Windows', phrase('native', 'native.call') !== undefined)
    report('catalog: a native without its library', entry('nolibrary')?.status === 'broken' && /nolibrary\.dll/.test(entry('nolibrary').error))
    report('catalog: a broken one says why', entry('broken')?.status === 'broken' && /^broken\.lingo line 3: There's no lingo or group called nope here.*use\(/.test(entry('broken').error))
    report('catalog: a phrase spelled wrong says what there is', entry('typo')?.status === 'broken' && /There's no phrase std\.lenn/.test(entry('typo').error))
    report('catalog: a Lingo lingo can\'t hide a group of windows', entry('input')?.status === 'broken' &&
      (has('windows') ? /already a part of windows/ : /no lingo or group called pointer/).test(entry('input').error))
    report('catalog: a folder without a lingo.json is not a lingo', !entry('notalingo'))
    const library = readdirSync(LIBRARY).filter((f) => f.endsWith('.lingo') || existsSync(new URL(`${f}/lingo.json`, LIBRARY))).map((f) => f.replace(/\.lingo$/, ''))
    const silent = library.flatMap((l) => (entry(l)?.phrases ?? []).filter((p) => !p.does).map((p) => p.id))
    report('library: every lingo loads, says what it is, and every function says what it does', library.length >= 9 &&
      library.every((l) => entry(l)?.status === 'ok' && entry(l).about) && !silent.length, silent.length ? `\n    silent: ${silent.join(', ')}` : '')
    report('hello: the lingo client, its version and its catalog, and nothing else', same(Object.keys(hello), ['type', 'token', 'client', 'version', 'catalog']) &&
      hello.client === 'lingo-client' && hello.version === '0.5.0' && Array.isArray(hello.catalog))
    for (const [name, test, lingo] of WEB_CASES) {
      if (lingo && !has(lingo)) { skipped++; console.log(`  skip  ${name}: this core has no ${lingo}`); continue }
      const started = Date.now()
      report(name, await test(send).catch(() => false), '', Date.now() - started)
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

// Every run starts from a fresh home: no store, no settings, and the lingos folder made again, as a person puts lingos
// in a computer's lingos folder.
rmSync(DATA, { recursive: true, force: true })
mkdirSync(LINGOS, { recursive: true })
for (const from of [new URL('./lingos/', import.meta.url), LIBRARY, new URL('../bin/lingos/', import.meta.url)])
  for (const name of readdirSync(from)) if (name !== 'README.md') cpSync(new URL(name, from), new URL(name, LINGOS), { recursive: true })

const server = http.createServer((q, s) => {
  if (q.method === 'POST' && q.url === '/ai/chat/completions') return ai(q, s)
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
setTimeout(() => { console.log('timed out'); process.exit(1) }, 180000)
