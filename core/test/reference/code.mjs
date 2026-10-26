// code.mjs - turns Lingo code into the tree a lingo runs. Whitespace never matters: a program means the same on one line
// as indented. The lingos a program runs on decide what it can call: every phrase is checked against their manifests,
// by name and by how many values it takes, so a program that parses can only ask a device for what it really does.
//
// 1. parse(text, manifest)  -> { program } or { error: { line, message } }
// 2. lex(text)              -> tokens, with text that holds {values} split into its parts
// 3. Parser                 -> statements and expressions, loosest first
// 4. paths(program)         -> each statement's place in the tree, mapped to its line, for errors at run time
// 5. signature(text)        -> { name, params } from one phrase line's signature, like list(folder, pattern="*")
// 6. manifest(text)         -> { name, version, language, uses, shapes, groups, phrases } from a native lingo's manifest,
//                              each phrase with its inputSchema and outputSchema, as a host lingo's lingo.json has them
//    schema(gives, shapes)  -> what a phrase gives, like [window]?, as JSON Schema, and gives(schema, shapes) the way back
// 7. rewrite(hooks)         -> walks every expression by its shape, for link and finding what code reaches
//    link(program, stack)   -> stacked phrases swapped for their code, only what is reached kept, needs worked out again

const KEYWORDS = new Set(['if', 'else', 'while', 'for', 'in', 'break', 'continue', 'fn', 'return', 'tool', 'try', 'catch', 'fail',
  'and', 'or', 'not', 'true', 'false', 'nothing', 'null'])

class Problem extends Error {
  constructor(line, message) { super(message); this.line = line }
}

////////// 1. PARSE //////////

/* manifest is every phrase the program may use: [{ id, params: [{ name, default? }] }], from every lingo it runs on. A
   param with a default may be left out. Returns the program ready to send, or the first problem, in words. */
export const parse = (text, manifest = []) => {
  try {
    const phrases = new Map(manifest.map((p) => [p.id, p]))
    const p = new Parser(lex(text), { phrases, used: new Set(), calls: [], lines: new WeakMap() })
    const program = p.program()
    /* Functions can be used before they are written, so bare calls are settled once every one is known. A function of the
       program comes first. Otherwise a phrase may be called by its bare name when only one group has it: length(x) is
       std.length(x), and the tree holds the full name, so a device never knows the difference. */
    const byName = new Map()
    for (const id of phrases.keys()) {
      const short = id.slice(id.lastIndexOf('.') + 1)
      byName.set(short, [...(byName.get(short) ?? []), id])
    }
    for (const c of p.ctx.calls) {
      const fn = program.fns[c.name]
      if (!fn) {
        const ids = byName.get(c.name) ?? []
        if (ids.length > 1) throw new Problem(c.line, `${c.name} is in ${ids.map((id) => id.slice(0, id.lastIndexOf('.'))).join(' and ')}: write ${ids.join(' or ')}`)
        if (!ids.length) throw new Problem(c.line, `There's no function or phrase called ${c.name}. Write it with fn ${c.name}(...){...}`)
        c.node.splice(0, 3, ...c.parser.phrase(ids[0], c.node[2], c.line))
        continue
      }
      const required = fn.params.filter(([, d]) => d === undefined).length
      if (c.count < required || c.count > fn.params.length) throw new Problem(c.line, `${c.name} takes ${required === fn.params.length ? required : `${required} to ${fn.params.length}`} values, not ${c.count}`)
    }
    program.needs = [...p.ctx.used]
    program.lines = paths(program, p.ctx.lines)
    return { program }
  } catch (err) {
    if (err instanceof Problem) return { error: { line: err.line, message: err.message } }
    throw err
  }
}

////////// 2. LEX //////////

const SYMBOLS = ['..', '+=', '-=', '*=', '/=', '==', '!=', '<=', '>=', '??', '&&', '||', '=', '+', '-', '*', '/', '%', '<', '>', '!', '?', ':',
  '(', ')', '[', ']', '{', '}', ',', '.', ';']

const lex = (text) => {
  const out = []
  let line = 1
  for (let i = 0; i < text.length;) {
    const c = text[i]
    if (c === '\n') { line++; i++; continue }
    if (/\s/.test(c)) { i++; continue }
    if (text.startsWith('//', i)) { while (i < text.length && text[i] !== '\n') i++; continue }
    if (/\d/.test(c)) {
      const m = /^\d+(\.\d+)?/.exec(text.slice(i))
      out.push({ t: 'num', v: Number(m[0]), line })
      i += m[0].length
      continue
    }
    if (/[A-Za-z_]/.test(c)) {
      const m = /^[A-Za-z_][A-Za-z0-9_]*/.exec(text.slice(i))
      out.push({ t: KEYWORDS.has(m[0]) ? 'kw' : 'id', v: m[0], line })
      i += m[0].length
      continue
    }
    if (c === '"') {
      // Text: \ escapes, and {expression} puts a value inside. \{ is a plain brace.
      const start = line, parts = []
      let cur = ''
      for (i++; ; i++) {
        if (i >= text.length) throw new Problem(start, 'This text never ends: a " is missing')
        const ch = text[i]
        if (ch === '"') { i++; break }
        if (ch === '\n') line++
        if (ch === '\\') { const n = text[++i]; cur += n === 'n' ? '\n' : n === 't' ? '\t' : n; continue }
        if (ch === '{') {
          let depth = 1, j = i + 1
          for (; j < text.length && depth; j++) {
            if (text[j] === '{') depth++
            else if (text[j] === '}') depth--
            else if (text[j] === '"') throw new Problem(line, 'Text inside {} inside text is not allowed. Put it in a name first')
          }
          if (depth) throw new Problem(line, 'A { inside text is never closed')
          if (cur) parts.push(cur)
          cur = ''
          parts.push({ src: text.slice(i + 1, j - 1), line })
          i = j - 1
          continue
        }
        cur += ch
      }
      if (cur || !parts.length) parts.push(cur)
      out.push({ t: 'str', v: parts, line: start })
      continue
    }
    const sym = SYMBOLS.find((s) => text.startsWith(s, i))
    if (!sym) throw new Problem(line, `I can't read "${text.slice(i, i + 10)}"`)
    out.push({ t: 'sym', v: sym, line })
    i += sym.length
  }
  return out
}

const show = (t) => (!t ? 'the end' : t.t === 'str' ? 'some text' : `"${t.v}"`)

////////// 3. PARSER //////////

class Parser {
  constructor(tokens, ctx) { Object.assign(this, { tokens, ctx, i: 0 }) }

  peek(n = 0) { return this.tokens[this.i + n] }
  end() { return this.i >= this.tokens.length }
  line() { return (this.peek() ?? this.tokens.at(-1))?.line ?? 1 }
  is(v, n = 0) { const t = this.peek(n); return !!t && (t.t === 'sym' || t.t === 'kw') && t.v === v }
  accept(v) { if (!this.is(v)) return false; this.i++; return true }
  expect(v, what = `"${v}"`) { if (!this.accept(v)) this.fail(what) }
  fail(what) { throw new Problem(this.line(), `Expected ${what}, but found ${show(this.peek())}`) }
  name(what = 'a name') {
    const t = this.peek()
    if (t?.t !== 'id') this.fail(what)
    this.i++
    return t.v
  }

  word(what) {
    const t = this.peek()
    if (t?.t !== 'id' && t?.t !== 'kw') this.fail(what)
    this.i++
    return t.v
  }

  // tool and fn may come in any order. Steps with no tool around them are a one-off tool of their own.
  program() {
    const fns = {}, loose = []
    let tool = null
    while (!this.end()) {
      if (this.accept(';')) continue
      if (this.is('tool')) {
        const line = this.line()
        this.i++
        if (tool) throw new Problem(line, 'A program has one tool. Make the other one a fn')
        tool = { name: this.name('the name of the tool'), params: this.params(), body: this.block() }
      } else if (this.is('fn')) {
        this.i++
        const name = this.name('the name of the function')
        if (fns[name]) throw new Problem(this.line(), `There are two functions called ${name}`)
        fns[name] = { params: this.params(), body: this.block() }
      } else loose.push(this.statement())
    }
    if (tool && loose.length) throw new Problem(this.ctx.lines.get(loose[0]) ?? 1, 'Steps outside the tool. Put them inside it, or inside a fn')
    return { lingo: 1, tool: tool ?? { name: 'steps', params: [], body: loose }, fns }
  }

  // (a, b=10). A default is any expression, worked out when the value is missing.
  params() {
    this.expect('(')
    const out = []
    if (this.accept(')')) return out
    do {
      const name = this.name('the name of a value it takes')
      out.push(this.accept('=') ? [name, this.expr()] : [name])
    } while (this.accept(','))
    this.expect(')')
    return out
  }

  block() {
    this.expect('{', 'a { to start the block')
    const out = []
    while (!this.accept('}')) {
      if (this.end()) this.fail('a } to end the block')
      if (this.accept(';')) continue
      out.push(this.statement())
    }
    return out
  }

  /* A condition needs no parentheses: if x>1 {...}. It ends at the block's {, which can't be mistaken for a record,
     because a record only starts where a value is expected, and once the condition is a whole value nothing can
     continue it with {. Parentheses still work, as plain grouping: if (x>1) {...}. */
  cond() {
    if (this.is('{')) this.fail('a condition before the {')
    return this.expr()
  }

  statement() {
    const line = this.line()
    const s = this.bare()
    this.ctx.lines.set(s, line)
    return s
  }

  bare() {
    if (this.accept('if')) {
      const branches = [[this.cond(), this.block()]]
      while (this.is('else') && this.is('if', 1)) { this.i += 2; branches.push([this.cond(), this.block()]) }
      return ['if', branches, this.accept('else') ? this.block() : []]
    }
    if (this.accept('while')) return ['while', this.cond(), this.block()]
    if (this.accept('for')) {
      // for x in e {...}, or the older for(x in e){...}, told apart by what follows the (.
      const wrapped = this.is('(') && this.peek(1)?.t === 'id' && (this.is('in', 2) || (this.is(',', 2) && this.peek(3)?.t === 'id' && this.is('in', 4)))
      if (wrapped) this.i++
      const a = this.name('a name for each item'), b = this.accept(',') ? this.name('a second name') : null
      this.expect('in', '"in"')
      const from = this.expr()
      if (wrapped) this.expect(')')
      // for(x in list) names the item. for(i, x in list) names the position and the item, or a record's part and value.
      return b ? ['each', b, from, this.block(), a] : ['each', a, from, this.block()]
    }
    if (this.accept('break')) return ['break']
    if (this.accept('continue')) return ['next']
    if (this.accept('return')) return ['return', this.end() || this.is('}') || this.is(';') ? null : this.expr()]
    if (this.accept('fail')) return ['fail', this.expr()]
    if (this.accept('try')) {
      const body = this.block()
      this.expect('catch', '"catch" after the try block')
      return ['try', body, this.name('a name for the error, like catch e'), this.block()]
    }
    if (this.is('tool') || this.is('fn')) throw new Problem(this.line(), `"${this.peek().v}" goes at the top level, not inside a block`)
    if (this.is('else')) throw new Problem(this.line(), '"else" needs an if block just before it')

    const e = this.expr()
    for (const op of ['=', '+=', '-=', '*=', '/=']) {
      if (!this.accept(op)) continue
      let value = this.expr()
      if (op !== '=') value = ['op', op[0], e, value]
      if (e?.[0] === 'var') return ['set', e[1], value]
      if (e?.[0] === 'get') return ['put', e[1], e[2], value]
      throw new Problem(this.line(), 'Only a name, a part or an item can be set')
    }
    if (e?.[0] !== 'call' && e?.[0] !== 'say') {
      throw new Problem(this.line(), e?.[0] === 'var' ? `"${e[1]}" on its own does nothing. Did you mean ${e[1]}=... or ${e[1]}(...)?` : 'This line works something out and then throws it away. Set a name to it, or return it')
    }
    return ['do', e]
  }

  // Loosest first: pick, ??, or, and, not, comparing, ranges, adding, multiplying, minus, then calls, parts and items.
  expr() {
    const c = this.coalesce()
    if (!this.accept('?')) return c
    const a = this.expr()
    this.expect(':', 'a : between the two choices')
    return ['pick', c, a, this.expr()]
  }
  coalesce() { let a = this.or(); while (this.accept('??')) a = ['else', a, this.or()]; return a }
  or() { let a = this.and(); while (this.accept('or') || this.accept('||')) a = ['or', a, this.and()]; return a }
  and() { let a = this.not(); while (this.accept('and') || this.accept('&&')) a = ['and', a, this.not()]; return a }
  not() { return this.accept('not') || this.accept('!') ? ['not', this.not()] : this.compare() }
  compare() {
    const a = this.range()
    const op = ['==', '!=', '<=', '>=', '<', '>', 'in'].find((o) => this.accept(o))
    return op ? ['op', op, a, this.range()] : a
  }
  range() { const a = this.sum(); return this.accept('..') ? ['range', a, this.sum()] : a }
  sum() {
    let a = this.product()
    for (let op; (op = ['+', '-'].find((o) => this.accept(o)));) a = ['op', op, a, this.product()]
    return a
  }
  product() {
    let a = this.unary()
    for (let op; (op = ['*', '/', '%'].find((o) => this.accept(o)));) a = ['op', op, a, this.unary()]
    return a
  }
  unary() { return this.accept('-') ? ['neg', this.unary()] : this.postfix() }

  postfix() {
    let e = this.primary()
    for (;;) {
      if (this.accept('.')) e = ['get', e, this.word('the name of a part after .')]
      else if (this.accept('[')) { e = ['get', e, this.expr()]; this.expect(']') }
      else return e
    }
  }

  primary() {
    const t = this.peek()
    if (!t) this.fail('a value')
    if (t.t === 'num') { this.i++; return t.v }
    if (t.t === 'str') { this.i++; return this.text(t) }
    if (this.accept('true')) return true
    if (this.accept('false')) return false
    if (this.accept('nothing') || this.accept('null')) return null
    if (this.accept('(')) { const e = this.expr(); this.expect(')'); return e }
    if (this.accept('[')) return ['list', ...this.items(']')]
    if (this.accept('{')) return this.record()
    if (t.t !== 'id') this.fail('a value')

    // a.b.c(...) is a phrase, f(...) is a function, and anything else is a name, with its parts read by postfix.
    // After a dot any word is a name, keywords too, so a phrase can be called dev.fail or a part in.
    let j = this.i + 1
    while (this.tokens[j]?.v === '.' && this.tokens[j]?.t === 'sym' && ['id', 'kw'].includes(this.tokens[j + 1]?.t)) j += 2
    const dotted = j > this.i + 1
    if (this.tokens[j]?.v === '(' && this.tokens[j]?.t === 'sym') {
      const line = t.line
      const name = this.tokens.slice(this.i, j).filter((x, k) => k % 2 === 0).map((x) => x.v).join('.')
      this.i = j + 1
      const args = this.items(')')
      if (dotted) return this.phrase(name, args, line)
      if (name === 'lingo') return this.lingo(args, line)
      if (name === 'use') return this.use(args, line)
      if (name === 'setting') return this.setting(args, line)
      const node = ['call', name, args]
      this.ctx.calls.push({ name, count: args.length, line, node, parser: this })
      return node
    }
    this.i++
    return ['var', t.v]
  }

  // Values in the order the manifest lists them, checked against it, then named for the lingo.
  phrase(id, args, line) {
    const p = this.ctx.phrases.get(id), keys = [...this.ctx.phrases.keys()]
    if (!p) {
      const first = id.split('.')[0]
      if (!keys.some((k) => k.startsWith(`${first}.`))) throw new Problem(line, `There's no lingo or group called ${first} here. To reach one somewhere else, write use("its address"), like use("laptop.brandon/lingos/${first}.lingo/${id.slice(first.length + 1).replaceAll('.', '/')}")`)
      const group = id.slice(0, id.lastIndexOf('.'))
      const near = keys.filter((k) => k.startsWith(`${group}.`))
      throw new Problem(line, `There's no phrase ${id}.${near.length ? ` ${group} has: ${near.map((k) => k.slice(group.length + 1)).join(', ')}` : ` The groups are: ${[...new Set(keys.map((k) => k.slice(0, k.lastIndexOf('.'))))].join(', ')}`}`)
    }
    const required = p.params.filter((x) => !('default' in x)).length
    if (args.length < required || args.length > p.params.length) {
      throw new Problem(line, `${id}(${p.params.map((x) => ('default' in x ? `${x.name}=${JSON.stringify(x.default)}` : x.name)).join(', ')}) takes ${required === p.params.length ? required : `${required} to ${p.params.length}`} values, not ${args.length}`)
    }
    this.ctx.used.add(id)
    return ['say', id, Object.fromEntries(args.map((a, k) => [p.params[k].name, a]))]
  }

  /* use(address, values) is a call of lingo.use, made as the program reaches it: Onsay reaches what the address names, a
     lingo at an address, an MCP tool or a plain web address, and gives back its answer. Local things need no use: a lingo
     in the same lingos folder is a dotted call. */
  use(args, line) {
    const [address, values = null] = args
    if (!args.length) throw new Problem(line, 'use(...) says what it reaches first, like use("laptop.brandon/lingos/weather.lingo/now", {place: "Paris"})')
    if (args.length > 2) throw new Problem(line, 'use(...) takes an address and its values, and no more')
    if (address === '') throw new Problem(line, 'use(...) names what it reaches, like use("laptop.brandon/lingos/weather.lingo/now", {place: "Paris"})')
    this.ctx.used.add('lingo.use')
    return ['say', 'lingo.use', { address, values }]
  }

  /* setting("key", "github.brandon") is a call of lingo.setting, made as the program reaches it. A program here is never
     a lingo, so it always says whose setting it reads. */
  setting(args, line) {
    const [key, reference] = args
    if (!args.length) throw new Problem(line, 'setting(...) names the setting first, like setting("key")')
    if (args.length > 2) throw new Problem(line, "setting(...) takes a setting's name and whose it is, and no more")
    if (args.length < 2) throw new Problem(line, 'setting(...) outside a lingo says whose setting it is too, like setting("key", "openrouter")')
    this.ctx.used.add('lingo.setting')
    return ['say', 'lingo.setting', { key, reference }]
  }

  /* lingo("what it does", {name: value}, "what it gives") is a call of lingo.run, made as the program reaches it: Onsay
     is asked then, with the values, and only the answer comes back. */
  lingo(args, line) {
    const [ask, values = ['map', {}], gives = ''] = args
    if (!args.length) throw new Problem(line, 'lingo(...) says what it needs first, like lingo("the weather in a city", {city: "Paris"})')
    if (args.length > 3) throw new Problem(line, 'lingo(...) takes what it needs, its values and what it gives back, and no more')
    if (ask === '') throw new Problem(line, 'lingo(...) says what it needs in words, like lingo("the weather in a city", {city: "Paris"})')
    this.ctx.used.add('lingo.run')
    return ['say', 'lingo.run', { ask, values, gives }]
  }

  items(close) {
    const out = []
    if (this.accept(close)) return out
    do { if (this.is(close)) break; out.push(this.expr()) } while (this.accept(','))
    this.expect(close)
    return out
  }

  // {name:"Rex", "two words":1}
  record() {
    const parts = {}
    if (this.accept('}')) return ['map', parts]
    do {
      if (this.is('}')) break
      const t = this.peek()
      const key = t?.t === 'id' || t?.t === 'kw' ? t.v : t?.t === 'str' && t.v.length === 1 && typeof t.v[0] === 'string' ? t.v[0] : this.fail('the name of a part')
      this.i++
      this.expect(':')
      parts[key] = this.expr()
    } while (this.accept(','))
    this.expect('}')
    return ['map', parts]
  }

  // "Hi {name}" becomes "Hi " + name. Text first, so + always joins as text.
  text(t) {
    if (t.v.length === 1 && typeof t.v[0] === 'string') return t.v[0]
    return t.v.map((part) => {
      if (typeof part === 'string') return part
      const inner = new Parser(lex(part.src).map((x) => ({ ...x, line: part.line })), this.ctx)
      const e = inner.expr()
      if (!inner.end()) inner.fail('a } after the value inside the text')
      return e
    }).reduce((a, b) => ['op', '+', a, b], '')
  }
}

////////// 4. PATHS //////////

const paths = (program, lines) => {
  const map = {}
  const walk = (body, at) => body.forEach((s, i) => {
    const here = `${at}/${i}`
    if (lines.has(s)) map[here] = lines.get(s)
    if (s[0] === 'if') { s[1].forEach(([, b], j) => walk(b, `${here}/${j}`)); walk(s[2], `${here}/${s[1].length}`) }
    if (s[0] === 'while') walk(s[2], `${here}/0`)
    if (s[0] === 'each') walk(s[3], `${here}/0`)
    if (s[0] === 'try') { walk(s[1], `${here}/0`); walk(s[3], `${here}/1`) }
  })
  walk(program.tool.body, 'tool')
  for (const [n, f] of Object.entries(program.fns)) walk(f.body, `fn:${n}`)
  return map
}

////////// 5. SIGNATURE //////////

// One phrase line's signature, read with this same parser, so a manifest is written in Lingo's own syntax. Defaults must
// be plain values, because a manifest is data, not code.
const signature = (text) => {
  const p = new Parser(lex(text), { phrases: new Map(), used: new Set(), calls: [], lines: new WeakMap() })
  const name = p.word('the name of the phrase')
  const params = p.params().map(([n, d]) => {
    if (d === undefined) return { name: n }
    const v = Array.isArray(d) && d[0] === 'neg' && typeof d[1] === 'number' ? -d[1] : d
    if (Array.isArray(v)) throw new Error(`the default for ${n} must be a plain value, like 10, "*" or nothing`)
    return { name: n, default: v }
  })
  if (!p.end()) p.fail('nothing after the )')
  return { name, params }
}

////////// 6. MANIFEST //////////

/* A native lingo's manifest, one line each, laid out the way the phrases are called:
     lingo windows 1.0, Lingo 1.0, uses core 0.1    its name, its version, the Lingo it speaks, and the lingos it is built on
     file = {name,path,size,folder,changed}         a record shape, named once and used in what phrases give
     files: files.read                              a group, and the permission its phrases need. , pure marks them pure
     write(path, text) [files.write]: replaces it   a phrase: its signature, -> what it gives, a [permission] of its own,
                                                    and : what it does. Only the signature is required
   // starts a comment line. Blank lines are ignored. */
const PERMISSIONS = ['none', 'screen.read', 'screen.act', 'files.read', 'files.write', 'run', 'net']
const PERM = PERMISSIONS.join('|').replace(/\./g, '\\.')
const TAIL = new RegExp(`^(?: -> (.+?))?(?: \\[((?:${PERM})(?:, ?(?:${PERM}))*)\\])?(?:: (.+))?$`)

// Where the ( that starts at col closes, wherever text holds one.
const closeParen = (s, col) => {
  for (let j = col, depth = 0, quoted = false; j < s.length; j++) {
    const c = s[j]
    if (quoted) { if (c === '\\') j++; else if (c === '"') quoted = false }
    else if (c === '"') quoted = true
    else if (c === '(') depth++
    else if (c === ')' && --depth === 0) return j + 1
  }
  return -1
}

export const manifest = (text) => {
  const out = { name: '', version: '', language: '', uses: [], shapes: {}, groups: [], phrases: [] }
  const lines = text.split(/\r?\n/)
  let group = null
  for (let i = 0; i < lines.length; i++) {
    const raw = lines[i], line = raw.trim(), at = `line ${i + 1}`
    if (!line || line.startsWith('//')) continue
    let m
    if (!out.name) {
      if (!(m = /^lingo (\S+) (\S+), Lingo (\S+)(?:, uses (.+))?$/.exec(line))) throw new Error(`${at}: the first line must be like: lingo notes 1.0, Lingo 1.0, uses windows 1.0`)
      ;[, out.name, out.version, out.language] = m
      out.uses = (m[4] ?? '').split(',').map((u) => u.trim()).filter(Boolean).map((u) => {
        const [name, version] = u.split(/\s+/)
        if (!version) throw new Error(`${at}: each lingo it uses needs a version, like uses windows 1.0`)
        return { name, version }
      })
    } else if ((m = /^([A-Za-z_]+) = (\{.*\})$/.exec(line))) out.shapes[m[1]] = m[2]
    else if ((m = /^([A-Za-z_]+): ([a-z.]+(?:, [a-z.]+)*)$/.exec(line))) {
      // files: files.read, or several: calculator: screen.act, screen.read. pure, last, marks the group pure.
      const words = m[2].split(', '), pure = words.at(-1) === 'pure'
      const needs = pure ? words.slice(0, -1) : words
      const wrong = needs.find((n) => !PERMISSIONS.includes(n))
      if (wrong || !needs.length) throw new Error(`${at}: ${wrong ?? 'pure'} is not a permission. They are ${PERMISSIONS.join(', ')}`)
      group = { name: m[1], needs, pure }
      out.groups.push(group)
    } else {
      if (!group) throw new Error(`${at}: a phrase needs a group line above it, like files: files.read`)
      const end = closeParen(line, line.indexOf('('))
      const tail = end > 0 && TAIL.exec(line.slice(end))
      if (!tail) throw new Error(`${at}: a phrase is like: name(a, b=1) -> what it gives [permission]: what it does`)
      let sig
      try { sig = signature(line.slice(0, end)) } catch (err) { throw new Error(`${at}: ${err.message}`) }
      out.phrases.push({
        id: `${group.name}.${sig.name}`, group: group.name, params: sig.params, gives: tail[1] ?? 'nothing',
        needs: tail[2] ? tail[2].split(/, ?/) : [...group.needs], pure: group.pure, does: tail[3] ?? '', head: line.slice(0, end),
        inputSchema: inputOf(sig.params), ...(tail[1] && { outputSchema: schema(tail[1], out.shapes) }),
      })
    }
  }
  return out
}

// A phrase's values as an inputSchema: those with no default required.
const inputOf = (params) => ({
  type: 'object', properties: Object.fromEntries(params.map((p) => [p.name, 'default' in p ? { default: p.default } : {}])),
  required: params.filter((p) => !('default' in p)).map((p) => p.name),
})

/* 7. Text, number, bool, nothing, list, record and any are its types, and a shape is an object titled with its name. What
   doesn't read as one is any. */
const TYPES = { text: 'string', number: 'number', bool: 'boolean', nothing: 'null', list: 'array', record: 'object' }
const schema = (text, shapes = {}) => {
  let i = 0
  const at = (c) => { while (text[i] === ' ') i++; return text[i] === c && ++i > 0 }
  const word = () => { while (text[i] === ' ') i++; const w = /^\w+/.exec(text.slice(i))?.[0]; if (!w) throw new Error(); i += w.length; return w }
  const base = () => {
    if (at('[')) { const items = alt(); if (!at(']')) throw new Error(); return { type: 'array', items } }
    if (at('{')) {
      const properties = {}
      if (at('}')) return { type: 'object', properties }
      do { const k = word(); properties[k] = at(':') ? alt() : {} } while (at(','))
      if (!at('}')) throw new Error()
      return { type: 'object', properties }
    }
    const w = word()
    if (Object.hasOwn(TYPES, w)) return { type: TYPES[w] }
    if (w === 'any') return {}
    return Object.hasOwn(shapes, w) ? { type: 'object', title: w, properties: schema(shapes[w]).properties } : { title: w }
  }
  const opt = () => { const b = base(); return at('?') ? { anyOf: [b, { type: 'null' }] } : b }
  const alt = () => { const x = opt(); if (!at('|')) return x; const all = [x]; do all.push(opt()); while (at('|')); return { anyOf: all } }
  try { const o = alt(); while (text[i] === ' ') i++; return i === text.length ? o : {} } catch { return {} }
}

// 7. An object titled with one of the shapes is that shape, and a part whose type is plain is written as its name alone.
export const gives = (s, shapes = {}) => {
  if (s === undefined || s === null) return 'nothing'
  const of = (s) => {
    const r = s && typeof s === 'object' && !Array.isArray(s) ? s : {}, any = r.anyOf ?? r.oneOf
    if (Array.isArray(any) && any.length) {
      const maybe = any.length === 2 && any[1]?.type === 'null'
      return any.slice(0, maybe ? 1 : any.length).map(of).join('|') + (maybe ? '?' : '')
    }
    if (Array.isArray(r.type)) {
      const some = r.type.filter((t) => t !== 'null').map((t) => ({ type: t })), all = r.type.includes('null') ? [...some, { type: 'null' }] : some
      return all.length > 1 ? of({ anyOf: all }) : all.length ? of(all[0]) : 'any'
    }
    const t = typeof r.type === 'string' ? r.type : ''
    if (typeof r.title === 'string' && (!t || Object.hasOwn(shapes, r.title))) return r.title
    if (t === 'string') return 'text'
    if (t === 'number' || t === 'integer') return 'number'
    if (t === 'boolean') return 'bool'
    if (t === 'null') return 'nothing'
    if (t === 'array') return r.items && typeof r.items === 'object' ? `[${of(r.items)}]` : 'list'
    if (t === 'object' && r.properties && typeof r.properties === 'object') {
      return `{${Object.entries(r.properties).map(([k, v]) => { const g = of(v); return ['text', 'number', 'bool', 'any', 'nothing'].includes(g) ? k : `${k}:${g}` }).join(',')}}`
    }
    return t === 'object' ? 'record' : 'any'
  }
  return of(s)
}

////////// 7. LINK //////////

/* Rewrites every expression in a function or a body, knowing each node's shape, so a value that happens to read "call"
   is never taken for a call. hooks.say(id, args) and hooks.call(name, args) may give a node back to use instead. */
export const rewrite = (hooks) => {
  const ex = (e) => {
    if (!Array.isArray(e)) return e
    switch (e[0]) {
      case 'var': return e
      case 'map': return ['map', Object.fromEntries(Object.entries(e[1]).map(([k, v]) => [k, ex(v)]))]
      case 'op': return ['op', e[1], ex(e[2]), ex(e[3])]
      case 'call': { const args = e[2].map(ex); return hooks.call?.(e[1], args) ?? ['call', e[1], args] }
      case 'say': { const args = Object.fromEntries(Object.entries(e[2]).map(([k, v]) => [k, ex(v)])); return hooks.say?.(e[1], args) ?? ['say', e[1], args] }
      default: return [e[0], ...e.slice(1).map(ex)]   // list, get, and, or, not, neg, pick, else, range: all values
    }
  }
  const body = (b) => b.map((s) => {
    switch (s[0]) {
      case 'set': return ['set', s[1], ex(s[2])]
      case 'put': return ['put', ex(s[1]), ex(s[2]), ex(s[3])]
      case 'if': return ['if', s[1].map(([c, b2]) => [ex(c), body(b2)]), body(s[2])]
      case 'while': return ['while', ex(s[1]), body(s[2])]
      case 'each': return ['each', s[1], ex(s[2]), body(s[3]), ...s.slice(4)]
      case 'try': return ['try', body(s[1]), s[2], body(s[3])]
      case 'return': case 'fail': case 'do': return [s[0], ex(s[1])]
      default: return s   // break, continue
    }
  })
  const fn = (f) => ({ params: f.params.map(([n, d]) => (d === undefined ? [n] : [n, ex(d)])), body: body(f.body) })
  return { ex, body, fn }
}

/* stack is every stacked lingo the instance has: { phrases: Map(id -> params), fns, lines }. Each
   call to a stacked phrase becomes a call to its body, with its values in order and nothing for any left out, so the
   body's defaults apply. Then only the functions something actually reaches are pulled in, so a program that uses one
   phrase of a big lingo carries only what that phrase needs, and what is left runs entirely on native phrases. */
export const link = (program, stack) => {
  const fns = { ...program.fns }, lines = { ...program.lines }, want = []
  const r = rewrite({
    say: (id, args) => (stack.phrases.has(id) ? (want.push(id), ['call', id, stack.phrases.get(id).map((p) => args[p.name] ?? null)]) : null),
    call: (n) => { if (n in stack.fns) want.push(n) },
  })
  const tool = { ...program.tool, ...r.fn(program.tool) }
  for (const [n, f] of Object.entries(program.fns)) fns[n] = r.fn(f)
  while (want.length) {
    const k = want.pop()
    if (fns[k]) continue
    fns[k] = { params: [], body: [] }   // held first, so a body that reaches itself doesn't loop here
    fns[k] = r.fn(stack.fns[k])
    for (const [path, line] of Object.entries(stack.lines)) if (path.startsWith(`fn:${k}/`)) lines[path] = line
  }
  const needs = new Set()
  const find = rewrite({ say: (id) => { needs.add(id) } })
  find.fn(tool)
  for (const f of Object.values(fns)) find.fn(f)
  return { ...program, tool, fns, needs: [...needs], lines }
}
