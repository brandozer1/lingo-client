// floor.mjs - the floor's tests: a stand-in for Onsay on 8788 (its resolve endpoint, its log box, an ask and pages that
// echo), the floor started against it on 6975, and every case run on it, real requests, signed as the owner, as james,
// as anyone, and with a badge; then the floor built with the lingos a Windows computer carries, on 6976.
//
//   node test/floor.mjs        after build.ps1, which makes bin/test/floor.exe and bin/test/windows/floor.exe
//   node test/floor.mjs x86    the same with the 32-bit builds, bin/test/x86/

import http from 'node:http'
import crypto from 'node:crypto'
import { spawn, execFile } from 'node:child_process'
import { promisify } from 'node:util'
import { mkdirSync, rmSync, writeFileSync, readFileSync, existsSync } from 'node:fs'
import { fileURLToPath } from 'node:url'

const ONSAY_PORT = 8788, FLOOR = 6975, WINDOWS = 6976
const CORE = fileURLToPath(new URL('..', import.meta.url)), BUILD = process.argv[2] === 'x86' ? 'x86/' : ''
const EXE = fileURLToPath(new URL(`../bin/test/${BUILD}floor.exe`, import.meta.url))
const WINDOWS_EXE = fileURLToPath(new URL(`../bin/test/${BUILD}windows/floor.exe`, import.meta.url))
const HOME = fileURLToPath(new URL('./floor-data', import.meta.url)), SETTINGS = fileURLToPath(new URL('./floor.json', import.meta.url))
const WINDOWS_HOME = `${HOME}/windows`, WINDOWS_SETTINGS = fileURLToPath(new URL('./floor-windows.json', import.meta.url))

////////// 1. KEYS //////////

const PKCS8 = Buffer.from('302e020100300506032b657004220420', 'hex')
const keyOf = (seed) => crypto.createPrivateKey({ key: Buffer.concat([PKCS8, seed]), format: 'der', type: 'pkcs8' })
const publicOf = (k) => `ed25519:${crypto.createPublicKey(k).export({ format: 'der', type: 'spki' }).subarray(-32).toString('base64')}`
const ONSAY_SEED = Buffer.alloc(32, 7), ONSAY = keyOf(ONSAY_SEED)
const OWNER = keyOf(Buffer.alloc(32, 11)), JAMES = keyOf(Buffer.alloc(32, 12)), STRANGER = keyOf(Buffer.alloc(32, 13))
const canonical = (v) => Array.isArray(v) ? `[${v.map(canonical).join(',')}]`
  : v && typeof v === 'object' ? `{${Object.keys(v).filter((k) => v[k] !== undefined).sort().map((k) => `${JSON.stringify(k)}:${canonical(v[k])}`).join(',')}}`
    : JSON.stringify(v ?? null)
const digest = (body) => `sha-256=:${crypto.createHash('sha256').update(body).digest('base64')}:`

// The headers of a request signed by key (RFC 9421), keyid its name or its key itself.
const signed = (key, keyid, method, url, body = '') => {
  const params = `("@method" "@target-uri" "content-digest");created=${Math.floor(Date.now() / 1000)};nonce="${crypto.randomBytes(8).toString('hex')}";keyid="${keyid}"`
  const base = `"@method": ${method}\n"@target-uri": ${url}\n"content-digest": ${digest(body)}\n"@signature-params": ${params}`
  return { 'content-digest': digest(body), 'signature-input': `sig=${params}`, signature: `sig=:${crypto.sign(null, Buffer.from(base), key).toString('base64')}:` }
}

// Whether a request the floor sent checks out: its signature by the key its keyid names, over what it covers.
const checks = (req, body) => {
  const input = req.headers['signature-input'] ?? '', sig = (req.headers.signature ?? '').match(/sig=:(.*):/)?.[1]
  const params = input.slice(4), keyid = params.match(/keyid="([^"]*)"/)?.[1] ?? ''
  if (!sig || !keyid.startsWith('ed25519:')) return false
  const base = `"@method": ${req.method}\n"@target-uri": http://${req.headers.host}${req.url}\n"content-digest": ${req.headers['content-digest']}\n"@signature-params": ${params}`
  const key = crypto.createPublicKey({ key: Buffer.concat([Buffer.from('302a300506032b6570032100', 'hex'), Buffer.from(keyid.slice(8), 'base64')]), format: 'der', type: 'spki' })
  return req.headers['content-digest'] === digest(body) && !!sig && crypto.verify(null, Buffer.from(base), key, Buffer.from(sig, 'base64'))
}

// A badge, its grants handed on: staple(given, grant, key) adds grant to given, signed over the sig before it and the grant.
const staple = (given, grant, key) => {
  const list = given ? JSON.parse(Buffer.from(given, 'base64').toString()) : []
  const sig = crypto.sign(null, Buffer.from((list.at(-1)?.sig ?? '') + canonical(grant)), key).toString('base64')
  return Buffer.from(JSON.stringify([...list, { ...grant, sig }])).toString('base64')
}

// A sealed log opened as Onsay opens it: X25519 with Onsay's key turned to it, the secret hashed with both keys, and
// ChaCha20-Poly1305 (RFC 8439); and its signature checked by the key it came with.
const opened = (m) => {
  const scalar = crypto.createHash('sha512').update(ONSAY_SEED).digest().subarray(0, 32)
  const mine = crypto.createPrivateKey({ key: Buffer.concat([Buffer.from('302e020100300506032b656e04220420', 'hex'), scalar]), format: 'der', type: 'pkcs8' })
  const ours = crypto.createPublicKey(mine).export({ format: 'der', type: 'spki' }).subarray(-32)
  const eph = Buffer.from(m.eph, 'base64'), nonce = Buffer.from(m.nonce, 'base64'), box = Buffer.from(m.box, 'base64')
  const theirs = crypto.createPublicKey({ key: Buffer.concat([Buffer.from('302a300506032b656e032100', 'hex'), eph]), format: 'der', type: 'spki' })
  const shared = crypto.diffieHellman({ privateKey: mine, publicKey: theirs })
  const key = crypto.createHash('sha256').update(Buffer.concat([shared, eph, ours])).digest()
  const d = crypto.createDecipheriv('chacha20-poly1305', key, nonce, { authTagLength: 16 })
  d.setAuthTag(box.subarray(-16))
  const text = Buffer.concat([d.update(box.subarray(0, -16)), d.final()]).toString()
  const signer = crypto.createPublicKey({ key: Buffer.concat([Buffer.from('302a300506032b6570032100', 'hex'), Buffer.from(m.key.slice(8), 'base64')]), format: 'der', type: 'spki' })
  const ok = crypto.verify(null, Buffer.concat([eph, nonce, box]), signer, Buffer.from(m.sig, 'base64'))
  return { text, ok }
}

////////// 2. THE STAND-IN //////////

const logs = [], echoed = []
const relay = []   // calls for the floor, handed over in the answer to its next held post
// The newest release, signed as Onsay signs it unless forged, and its files by name.
const release = { version: '9.9.9', files: [] }, shipped = {}
let forged = false
const stand = http.createServer((req, res) => {
  let body = ''
  req.on('data', (d) => { body += d })
  req.on('end', () => {
    const json = (status, v) => { res.writeHead(status, { 'content-type': 'application/json' }); res.end(JSON.stringify(v)) }
    if (req.url === '/logs') {
      const m = JSON.parse(body)
      logs.push({ ...opened(m), when: Date.now() })
      return json(200, { kept: 1, next: 1, calls: m.hold ? relay.splice(0) : [] })
    }
    if (req.url === '/updates/release') {
      const sig = crypto.sign(null, Buffer.from(canonical(release)), ONSAY).toString('base64')
      return json(200, { release: forged ? { ...release, version: '6.6.6' } : release, signed: `onsay:${sig}` })
    }
    if (req.url.startsWith('/updates/')) {
      const file = shipped[decodeURIComponent(req.url.slice(9))]
      if (file === undefined) return json(404, { error: 'no such file' })
      res.writeHead(200, { 'content-type': 'application/octet-stream' })
      return res.end(file)
    }
    if (req.url === '/echo/slow') return setTimeout(() => json(200, 'slow'), 1500)
    if (req.url.startsWith('/echo')) { echoed.push({ path: req.url, body: JSON.parse(body || 'null'), signed: checks(req, body), run: req.headers['onsay-run'] }); return json(200, { path: req.url, got: JSON.parse(body || 'null') }) }
    if (req.url === '/broken') return json(500, { error: 'it broke over there' })
    if (req.url === '/weather') return json(200, `sunny in ${JSON.parse(body).city}`)
    // The lingo search: the ways it finds, best first, in list mode, or a call relayed to its address.
    if (req.url === '/search/ask') {
      const q = JSON.parse(body)
      const here = `http://127.0.0.1:${ONSAY_PORT}`
      if (q.address === 'weather.tester/now') return json(200, { value: 'raining' })
      if (q.mode !== 'list') return json(200, { error: 'the floor asks for ways', code: 'type' })
      if (q.ask.includes('refused')) return json(401, { error: 'Sign in to continue' })
      if (q.ask === 'the weather in a city') {
        return json(200, { value: [
          { address: `${here}/weather`, inputs: {}, needs: ['city'] },
          { address: `${here}/broken`, inputs: {} },
          { address: `${here}/weather`, inputs: { city: q.values.city } },
        ] })
      }
      if (q.ask === 'whats the weather in rome') return json(200, { value: [{ address: `${here}/weather`, inputs: { city: 'Rome' } }] })
      if (q.ask === 'the weather where I am') return json(200, { value: [{ address: `${here}/weather`, inputs: { city: q.values.messages?.[0]?.content } }] })
      return json(200, { value: [] })
    }
    json(404, { error: 'nothing here' })
  })
})

////////// 3. CASES //////////

let URL_ = `http://127.0.0.1:${FLOOR}`
const call = async (path, body, { key, keyid, badge, run, method = 'POST' } = {}) => {
  const text = typeof body === 'string' ? body : body === undefined ? '' : JSON.stringify(body)
  const headers = { ...(key ? signed(key, keyid ?? publicOf(key), method, URL_ + path, text) : {}), ...(badge ? { 'onsay-badge': badge } : {}), ...(run ? { 'onsay-run': run } : {}) }
  const r = await fetch(URL_ + path, { method, headers, body: method === 'GET' ? undefined : text })
  const got = await r.text()
  let parsed
  try { parsed = JSON.parse(got) } catch { parsed = got }
  return { status: r.status, json: parsed }
}
const owner = { key: OWNER }
// The echo lingo, offered as its signatures alone: its calls are answered where it answers, the stand-in's /echo.
const ECHO = '"Says what it was sent back."\nfn hello(a, b) "Says it back." {}\nfn again(x: text) "Says one back." {}\nfn hi() {}\nfn _quiet() {}'
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b)
const policy = (grants) => writeFileSync(`${HOME}/policy.json`, JSON.stringify({ grants }))
const lines = () => readFileSync(`${HOME}/requests.log`, 'utf8').trim().split('\n').map((l) => JSON.parse(l))
// Every line the floor posted to Onsay.
const posted = () => logs.flatMap((x) => x.text.split('\n')).filter(Boolean).map((l) => JSON.parse(l))
// A call sent through Onsay, as the stand-in hands it over, and its answer once the floor posts it.
let relays = 0
const relayed = async (path, body, key) => {
  const id = `req_${++relays}`
  const headers = { host: `127.0.0.1:${FLOOR}`, ...(key ? signed(key, publicOf(key), 'POST', URL_ + path, body) : {}) }
  relay.push({ id, method: 'POST', path, headers, body })
  for (let i = 0; i < 40; i++) {
    const answer = posted().find((l) => l.way === 'answer' && l.id === id)
    if (answer) return answer
    await pause(100)
  }
  return null
}
const pause = (ms) => new Promise((r) => setTimeout(r, ms))

const CASES = [
  ['GET / says what it is and its own key, for an owner to add to their record', async () => {
    const r = await call('/', undefined, { method: 'GET' })
    return r.status === 200 && r.json.floor && /^ed25519:/.test(r.json.key)
  }],
  ['a lingo offers itself, as the owner may, its functions read from its code, and anyone else may not', async () => {
    const offer = (key) => call('/lingo/offer', { name: 'echo', code: ECHO, address: `http://127.0.0.1:${ONSAY_PORT}/echo` }, key)
    const [theirs, mine] = [await offer(undefined), await offer(owner)]
    const table = JSON.parse(readFileSync(`${HOME}/offered.json`, 'utf8'))
    return theirs.status === 403 && mine.status === 200 && same(mine.json, [{ name: 'hello', does: 'Says it back.' }, { name: 'again', does: 'Says one back.' }, { name: 'hi', does: '' }]) &&
      table.echo.address === `http://127.0.0.1:${ONSAY_PORT}/echo` && table.echo.openapi.paths['/echo/hello'].post.summary === 'Says it back.' &&
      table.echo.openapi.paths['/echo/again'].post.requestBody.content['application/json'].schema.properties.x.type === 'string' &&
      table.echo.openapi.paths['/echo/_quiet'].post['x-lingo-internal'] === true
  }],
  ['a script is run, pure computing open to anyone, its value as JSON', async () => {
    const r = await call('/', 'x = [1, 2, 3]\nreturn {sum: x[0] + x[1] + x[2], words: text.split("a b", " ")}')
    return r.status === 200 && same(r.json, { sum: 6, words: ['a', 'b'] })
  }],
  ['a call of the computer from anyone with no grant is refused, 403, and logged', async () => {
    const r = await call('/', 'return use("echo/hi", {a: 1})')
    return r.status === 403 && /can't make lingo.use here/.test(r.json.error) && r.json.code === 'denied' &&
      lines().some((l) => l.way === 'call' && l.route === '/lingo/use' && !l.allowed && l.why === 'no grant lets them in')
  }],
  ['the owner, signed by their key, reaches everything: use() sends to where calls names, signed by this floor', async () => {
    const r = await call('/', 'return use("echo/hi", {a: 1})', owner)
    const sent = echoed.at(-1)
    return r.status === 200 && r.json.path === '/echo/hi' && same(r.json.got, { a: 1 }) && sent.signed &&
      lines().some((l) => l.way === 'call' && l.allowed && l.why === 'the owner')
  }],
  ['a call nothing here answers goes where calls names its group, its values in order', async () => {
    const r = await call('/', 'return echo.hello("x", 2)', owner)
    return r.status === 200 && r.json.path === '/echo/hello' && same(r.json.got, ['x', 2]) && echoed.at(-1).signed
  }],
  ['/group/name with values is the same call: a list in order', async () => {
    const r = await call('/echo/again', ['y'], owner)
    return r.status === 200 && r.json.path === '/echo/again' && same(r.json.got, ['y'])
  }],
  ['an offered function by name takes a record, matched to its values as its code names them', async () => {
    const r = await call('/echo/again', { x: 'z' }, owner)
    return r.status === 200 && same(r.json.got, ['z'])
  }],
  ['one of the computer\'s own calls by name takes a record, matched to its values', async () => {
    const r = await call('/std/find', { in: 'abc', what: 'c' }, owner)
    return r.status === 200 && r.json === 2
  }],
  ['a call sent somewhere that fails is the call\'s failure, said', async () => {
    const r = await call('/', `return use("http://127.0.0.1:${ONSAY_PORT}/broken", {})`, owner)
    return r.status === 500 && /it broke over there/.test(r.json.error) && lines().some((l) => l.way === 'out' && !l.allowed)
  }],
  ['lingo(...) asks Onsay\'s index for ways and calls them from here, best first, past one missing an input and one that fails', async () => {
    const r = await call('/', 'w = lingo("the weather in a city", {city: "Rome"}, "text")\nreturn "it is " + w', owner)
    const out = lines().filter((l) => l.way === 'out')
    return r.status === 200 && r.json === 'it is sunny in Rome' && out.some((l) => /broken/.test(l.route) && !l.allowed)
  }],
  ['use() of a name it can\'t reach goes through Onsay', async () => {
    const r = await call('/', 'return use("weather.tester/now", {})', owner)
    return r.status === 200 && r.json === 'raining'
  }],
  ['what doesn\'t compile is asked of the lingo search, and its answer comes back as if it had compiled', async () => {
    const r = await call('/', 'whats the weather in rome', owner)
    return r.status === 200 && r.json === 'sunny in Rome'
  }],
  ['text with fences runs only what is in its lingo fences, and a mistake in them names its line', async () => {
    const doc = 'This adds two numbers, if it rains or not.\n\n```lingo\n// the sum\nx = 2 + 3\n```\n\nThen it says it.\n\n```js\nnope(\n```\n\n```\nreturn "it is {x}"\n```\n'
    const bad = 'Some words.\n```lingo\nx = = 1\nreturn x\n```'
    const [r, b] = [await call('/', doc, owner), await call('/', bad, owner)]
    return r.status === 200 && r.json === 'it is 5' && b.json.code === 'compile' && /line 3/.test(b.json.error)
  }],
  ['a script sent with values by name, like a chat, gives the search those values when it isn\'t Lingo', async () => {
    const r = await call('/', { script: 'the weather where I am', values: { messages: [{ role: 'user', content: 'Oslo' }] } }, owner)
    return r.status === 200 && r.json === 'sunny in Oslo'
  }],
  ['what doesn\'t compile and finds nothing is the compile error it was', async () => {
    const r = await call('/', 'return 1 +', owner)
    return r.status >= 400 && r.json.code === 'compile'
  }],
  ['and so is one the search refuses, like a computer not signed in', async () => {
    const r = await call('/', 'refused = 1 +', owner)
    return r.status >= 400 && r.json.code === 'compile'
  }],
  ['a grant lets james, signed and known by key, make what it allows and nothing more', async () => {
    policy([{ to: 'james', allow: ['/lingo/use'] }])
    const keyid = 'laptop.james'
    const [a, b] = [await call('/', 'return use("echo/j", {})', { key: JAMES, keyid }), await call('/', 'return use("echo/j", {})', { key: STRANGER })]
    policy([])
    return a.status === 200 && b.status === 403 && lines().some((l) => l.way === 'call' && l.caller === 'laptop.james' && l.why === 'grant: james')
  }],
  ['a badge the owner signed lets anyone carrying it make what it allows', async () => {
    const badge = staple(null, { by: publicOf(OWNER), to: 'public', allow: ['/lingo/use'], until: Math.floor(Date.now() / 1000) + 600 }, OWNER)
    const [a, b] = [await call('/', 'return use("echo/b", {})', { badge }), await call('/', 'return echo.other(1)', { badge })]
    return a.status === 200 && b.status === 403
  }],
  ['a badge by anyone else, or changed, is a 401', async () => {
    const theirs = staple(null, { by: publicOf(STRANGER), to: 'public', allow: ['/'] }, STRANGER)
    const good = staple(null, { by: publicOf(OWNER), to: 'public', allow: ['/lingo'] }, OWNER)
    const changed = Buffer.from(Buffer.from(good, 'base64').toString().replace('/lingo', '/')).toString('base64')
    const [a, b] = [await call('/', 'return use("echo/x", {})', { badge: theirs }), await call('/', 'return use("echo/x", {})', { badge: changed })]
    return a.status === 403 && b.status === 401
  }],
  ['a signature that doesn\'t check out is a 401', async () => {
    const headers = signed(OWNER, publicOf(OWNER), 'POST', URL_ + '/', 'return 1')
    const r = await fetch(URL_ + '/', { method: 'POST', headers, body: 'return 2' })
    return r.status === 401 && /Content-Digest/.test((await r.json()).error)
  }],
  ['code that can\'t compile is a 400 that says why', async () => {
    const r = await call('/', 'return 1 +')
    return r.status === 400 && r.json.code === 'compile' && r.json.error.length > 0
  }],
  ['a call from a program on this computer is logged with that program\'s file, so the person can be asked about it by name', async () => {
    await call('/', 'return 1')
    const last = lines().filter((l) => l.way === 'in').at(-1)
    return process.platform !== 'win32' || /node(\.exe)?$/i.test(last?.program ?? '')
  }],
  ['a script past its steps is stopped', async () => {
    const r = await call('/', 'while true {}')
    return r.status === 500 && r.json.code === 'limit'
  }],
  ['the command line: key gives its key, and run runs a script as the owner', async () => {
    const run = promisify(execFile)
    const key = JSON.parse((await run(EXE, [SETTINGS, 'key'], { cwd: CORE })).stdout).key
    const ran = (await run(EXE, [SETTINGS, 'run', 'return use("echo/cli", {})'], { cwd: CORE })).stdout
    return /^ed25519:/.test(key) && JSON.parse(ran).path === '/echo/cli'
  }],
  ['requests.log has every call in, every call a script made and every one sent out', async () => {
    const l = lines()
    return ['in', 'call', 'out'].every((w) => l.some((x) => x.way === w)) && l.every((x) => /^\d{4}-\d\d-\d\dT/.test(x.at))
  }],
  ['the log reaches Onsay sealed, only Onsay can open it, and it is signed by this floor', async () => {
    for (let i = 0; i < 20 && !logs.length; i++) await pause(250)
    const all = logs.map((x) => x.text).join('')
    return logs.length > 0 && logs.every((x) => x.ok) && /"way":"in"/.test(all) && /"way":"call"/.test(all)
  }],
  ['what is offered reaches Onsay as its OpenAPI, never where it answers', async () => {
    const lines = posted().filter((l) => l.way === 'offered')
    const last = lines.at(-1)
    return !!last && last.lingos[0].name === 'echo' && !!last.lingos[0].openapi.paths['/echo/hello'] && !JSON.stringify(last).includes('127.0.0.1')
  }],
  ['it sends again when Onsay\'s answer says, sooner than its own wait', async () => {
    const had = logs.length, asked = Date.now()
    await call('/', 'return 1')
    for (let i = 0; i < 30 && logs.length === had; i++) await pause(100)
    return logs.length > had && logs.at(-1).when - asked < 1900
  }],
  ['a lingo offered with its code and no address runs right here, and one kept lingo uses another', async () => {
    const tally = await call('/lingo/offer', { name: 'tally', code: '"Counts."\nfn twice(x: number "a number") -> number "It doubled." {return x * 2}' }, owner)
    const sums = await call('/lingo/offer', { name: 'sums', code: 'fn four() -> number {return tally.twice(2)}' }, owner)
    const direct = await call('/tally/twice', [21], owner)
    const through = await call('/', 'return sums.four()', owner)
    const table = JSON.parse(readFileSync(`${HOME}/offered.json`, 'utf8'))
    return tally.status === 200 && sums.status === 200 && direct.json === 42 && through.json === 4 && !table.tally.address && !!table.tally.code
  }],
  ['use() of a lingo kept here runs it here, its values by name, and a stranger is refused it', async () => {
    const mine = await call('/', 'return use("tally/twice", {x: 5})', owner)
    const theirs = await call('/', 'return use("tally/twice", {x: 5})', { key: STRANGER })
    return mine.status === 200 && mine.json === 10 && theirs.status === 403
  }],
  ['use() of one of this computer\'s own calls runs it here, and lingo.list shows those calls with the lingos', async () => {
    const found = await call('/', 'return use("std/find", {in: "abc", what: "c"})', owner)
    const list = await call('/', 'return lingo.list()', owner)
    const compute = list.json?.find?.((l) => l.name === 'compute')
    return found.json === 2 && !!compute?.functions.some((f) => f.name === 'wait') && !list.json.some((l) => l.name === 'std')
  }],
  ['lingo.list gives every lingo offered with its functions, and lingo.code the code of one kept here', async () => {
    const list = await call('/', 'return lingo.list()', owner)
    const tally = list.json?.find?.((l) => l.name === 'tally')
    const code = await call('/', 'return lingo.code("tally")', owner)
    const elsewhere = await call('/', 'return lingo.code("echo")', owner)
    return tally?.about === 'Counts.' && tally.functions[0].name === 'twice' && /return x \* 2/.test(code.json) && elsewhere.json === null
  }],
  ['calls run side by side: a quick one is answered while a slow one still waits', async () => {
    const began = Date.now()
    const slow = call('/', 'return echo.slow()', owner)
    await pause(200)
    const quick = await call('/', 'return 1', owner)
    const quickAt = Date.now() - began
    const done = await slow
    return quick.json === 1 && quickAt < 1200 && done.json === 'slow'
  }],
  ['a call through Onsay is checked like one from the port, and its answer comes straight back', async () => {
    policy([])
    const mine = await relayed('/', 'return 6 * 7', OWNER)
    const theirs = await relayed('/', 'return echo.hi()')
    const l = lines()
    return mine?.status === 200 && mine.body === '42' && theirs?.status === 403 && l.some((x) => x.caller === 'through Onsay' && !x.allowed)
  }],
]

// The same floor with the lingos a Windows computer carries built in: its base calls and the web, each call of theirs a
// request judged by the policy like any other.
const ROOT = `${WINDOWS_HOME}/install`, sha = (b) => crypto.createHash('sha256').update(b).digest('hex')
const LIBRARY = fileURLToPath(new URL('../../lingo-library', import.meta.url))
const WINDOWS_CASES = [
  ['the lingos the app ships are kept here, offered with their code, and read by the compiler whole', async () => {
    const offered = []
    for (const name of ['ai', 'agent', 'coding', 'browser']) {
      offered.push(await call('/lingo/offer', { name, code: readFileSync(`${LIBRARY}/${name}/${name}.ls`, 'utf8') }, owner))
    }
    return offered.every((r) => r.status === 200) && offered[1].json.some((f) => f.name === 'ask')
  }],
  ['the update lingo puts the newest release Onsay signed beside the one running, each file checked, and applies it', async () => {
    rmSync(ROOT, { recursive: true, force: true })
    mkdirSync(`${ROOT}/app-1.0.0`, { recursive: true })
    mkdirSync(`${ROOT}/app-0.9.0/lingos`, { recursive: true })
    writeFileSync(`${ROOT}/app-0.9.0/lingos/old.lingo`, 'old')
    writeFileSync(`${ROOT}/version.txt`, '1.0.0')
    writeFileSync(`${ROOT}/app-1.0.0/same.dll`, 'unchanged')
    const exe = Buffer.from([0x4d, 0x5a, 0, 1, 2, 255])
    shipped['Onsay.exe'] = exe
    shipped['lingos.agent.lingo'] = 'fn ask() {}'
    release.files = [
      { path: 'Onsay.exe', name: 'Onsay.exe', sha256: sha(exe), size: exe.length },
      { path: 'same.dll', name: 'same.dll', sha256: sha('unchanged'), size: 9 },
      { path: 'lingos/agent.lingo', name: 'lingos.agent.lingo', sha256: sha('fn ask() {}'), size: 11 },
      { path: 'other.exe', name: 'other.exe', sha256: sha('x'), size: 1, arch: 'arm64' },
    ]
    const checked = await call('/update/check', [ROOT], owner)
    const into = `${ROOT}/app-9.9.9`
    const ready = readFileSync(`${into}/Onsay.exe`).equals(exe) && readFileSync(`${into}/same.dll`, 'utf8') === 'unchanged' &&
      readFileSync(`${into}/lingos/agent.lingo`, 'utf8') === 'fn ask() {}' && !existsSync(`${into}/other.exe`) && !existsSync(`${ROOT}/app-0.9.0`)
    const before = readFileSync(`${ROOT}/version.txt`, 'utf8')
    const applied = await call('/update/apply', [ROOT, '9.9.9'], owner)
    const again = await call('/update/check', [ROOT], owner)
    return checked.json?.version === '9.9.9' && ready && before === '1.0.0' && applied.json?.version === '9.9.9' &&
      readFileSync(`${ROOT}/version.txt`, 'utf8') === '9.9.9' && again.status === 200 && again.json === null
  }],
  ["and refuses a release Onsay didn't sign, changing nothing", async () => {
    forged = true
    const r = await call('/update/check', [ROOT], owner)
    forged = false
    return r.status === 500 && /signed/.test(r.json?.error) && !existsSync(`${ROOT}/app-6.6.6`)
  }],
  ['with the base calls built in, the owner reads a file', async () => {
    writeFileSync(`${WINDOWS_HOME}/note.txt`, 'héllo')
    const r = await call('/', `return files.read(${JSON.stringify(`${WINDOWS_HOME}/note.txt`)})`, owner)
    return r.status === 200 && r.json === 'héllo'
  }],
  ['and anyone else is refused it, by the same one check', async () => {
    const r = await call('/files/read', [`${WINDOWS_HOME}/note.txt`])
    return r.status === 403 && /can't make files.read here/.test(r.json.error)
  }],
  ['with the web built in, http.send reaches a page, and it is in the log as a request out', async () => {
    const r = await call('/', `return http.send("POST", "http://127.0.0.1:${ONSAY_PORT}/echo/web", {}, "[1]").status`, owner)
    const log = readFileSync(`${WINDOWS_HOME}/requests.log`, 'utf8')
    return r.status === 200 && r.json === 200 && log.includes('"way":"out"') && log.includes('/echo/web')
  }],
  ["the chat is the app's: offered by it like any lingo, its calls go to the app, signed by the floor, with the run that made them", async () => {
    const offered = await call('/lingo/offer', { name: 'chat', code: 'fn say(text: text) {}\nfn ask(questions: list|text) {}', address: `http://127.0.0.1:${ONSAY_PORT}/echo/chat` }, owner)
    const r = await call('/', 'return chat.say("hello")', { ...owner, run: 'todo-7' })
    const got = echoed.find((e) => e.path === '/echo/chat/say')
    return offered.status === 200 && r.status === 200 && got?.signed === true && got.run === 'todo-7' && got.body[0] === 'hello'
  }],
  ['one of this computer\'s own groups can\'t be offered', async () => {
    const r = await call('/lingo/offer', { name: 'files', code: 'fn read(path) {}', address: 'http://127.0.0.1:1' }, owner)
    return r.status === 403 && /one of this computer's own/.test(r.json.error)
  }],
  ['a command runs, as the owner', async () => {
    const r = await call('/', 'return commands.run("Write-Output 7").out', owner)
    return r.status === 200 && /7/.test(r.json)
  }],
]

////////// 4. RUN //////////

rmSync(HOME, { recursive: true, force: true })
mkdirSync(HOME, { recursive: true })
writeFileSync(SETTINGS, JSON.stringify({ home: HOME, port: FLOOR, owner: publicOf(OWNER), steps: 100000, ms: 5000,
  keys: { 'laptop.james': publicOf(JAMES) } }))
await new Promise((r) => stand.listen(ONSAY_PORT, '127.0.0.1', r))
const floor = spawn(EXE, [SETTINGS], { cwd: CORE, stdio: ['ignore', 'ignore', 'pipe'] })
let said = ''
floor.stderr.on('data', (d) => { said += d })
for (let i = 0; i < 40 && !/takes calls/.test(said); i++) await pause(100)
let passed = 0, failed = 0
const each = async (cases) => {
  for (const [name, test] of cases) {
    let ok = false, why = ''
    try { ok = await test() } catch (e) { why = String(e?.stack ?? e) }
    console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${name}${why ? `\n    ${why}` : ''}`)
    ok ? passed++ : failed++
  }
}
await each(CASES)
floor.kill()
mkdirSync(WINDOWS_HOME, { recursive: true })
writeFileSync(WINDOWS_SETTINGS, JSON.stringify({ home: WINDOWS_HOME, port: WINDOWS, owner: publicOf(OWNER), steps: 100000, ms: 10000 }))
const windows = spawn(WINDOWS_EXE, [WINDOWS_SETTINGS], { cwd: CORE, stdio: ['ignore', 'ignore', 'pipe'] })
let saidToo = ''
windows.stderr.on('data', (d) => { saidToo += d })
for (let i = 0; i < 40 && !/takes calls/.test(saidToo); i++) await pause(100)
URL_ = `http://127.0.0.1:${WINDOWS}`
await each(WINDOWS_CASES)
windows.kill()
console.log(`\n${passed} passed, ${failed} failed`)
stand.close()
process.exit(failed ? 1 : 0)
