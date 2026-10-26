// run.mjs - runs a Lingo tree from code.mjs. The language itself has no functions: everything beyond values, names,
// operators and control is a phrase, run by the lingo that has it, after checking the permission its manifest declares.
// Steps and time are counted, so a runaway program is stopped.
//
// 1. run(program, input, lingo, options)  -> { ok, value, log } or { ok: false, error: { code, message, at, line }, log }
// 2. exec(body, frame, at, state)         -> statements in order, tracking where it is for errors
// 3. step(s, frame, at, state)            -> one statement
// 4. evaluate(e, frame, state)            -> one expression's value
// 5. values                               -> what they are, and how they meet

////////// ERRORS AND JUMPS //////////

export class Stop extends Error {
  constructor(code, message) { super(message); this.code = code }
}
const BREAK = Symbol('break'), NEXT = Symbol('next')
class Back { constructor(value) { this.value = value } }

////////// 1. RUN //////////

/* lingo is { phrases: { id: { needs, params: [{ name, default? }], run(args, ctx) } } }. options: steps and ms are the
   limits, allow the permissions given, ctx is handed to every phrase (note, stop signal, anything the lingo needs). */
export const run = async (program, input = {}, lingo, { steps = 5_000_000, ms = 600_000, allow = [], ctx = {} } = {}) => {
  const log = []
  const state = {
    program, lingo, allow: new Set(['none', ...allow]), left: steps, until: Date.now() + ms, depth: 0, at: 'tool',
    ctx: { ...ctx, note: (t) => { log.push(t); ctx.note?.(t) } },
  }
  try {
    const frame = Object.create(null)
    await bind(program.tool.params, input, frame, state, true)
    const done = await exec(program.tool.body, frame, 'tool', state)
    return { ok: true, value: done instanceof Back ? done.value : null, log }
  } catch (err) {
    const code = err instanceof Stop ? err.code : 'type'
    const message = err instanceof Stop ? err.message : `Something went wrong inside the lingo: ${err.message}`
    return { ok: false, error: { code, message, at: state.at, line: program.lines?.[state.at] ?? null }, log }
  }
}

// What a tool or function was given, by name or by position, and its defaults for the rest.
const bind = async (params, given, frame, state, named) => {
  for (let i = 0; i < params.length; i++) {
    const [name, fallback] = params[i]
    const v = named ? given[name] : given[i]
    frame[name] = copy(v !== undefined && v !== null ? v : fallback === undefined ? null : await evaluate(fallback, frame, state))
  }
}

////////// 2-3. STATEMENTS //////////

const exec = async (body, frame, at, state) => {
  for (let i = 0; i < body.length; i++) {
    const here = `${at}/${i}`
    state.at = here
    const out = await step(body[i], frame, here, state)
    if (out) return out
  }
  return null
}

const step = async (s, frame, at, state) => {
  tick(state)
  switch (s[0]) {
    case 'set': {
      // list+=[x] grows the list where it is, instead of copying it every time.
      const [, name, value] = s
      if (value?.[0] === 'op' && value[1] === '+' && value[2]?.[0] === 'var' && value[2][1] === name && Array.isArray(frame[name]) && value[3]?.[0] === 'list') {
        for (const x of value[3].slice(1)) frame[name].push(copy(await evaluate(x, frame, state)))
      } else frame[name] = copy(await evaluate(value, frame, state))
      return null
    }
    case 'put': {
      const into = await evaluate(s[1], frame, state), key = await evaluate(s[2], frame, state), v = copy(await evaluate(s[3], frame, state))
      if (Array.isArray(into) && Number.isInteger(key)) {
        const i = key < 0 ? into.length + key : key
        if (i < 0 || i > into.length) throw new Stop('type', `There is no item ${key} in a list of ${into.length}`)
        into[i] = v
      } else if (kindOf(into) === 'record') into[text(key)] = v
      else throw new Stop('type', `Can't set ${text(key)} of ${kindOf(into)}`)
      return null
    }
    case 'if': {
      for (let j = 0; j < s[1].length; j++) {
        if (truthy(await evaluate(s[1][j][0], frame, state))) return exec(s[1][j][1], frame, `${at}/${j}`, state)
      }
      return exec(s[2], frame, `${at}/${s[1].length}`, state)
    }
    case 'while': {
      while (truthy(await evaluate(s[1], frame, state))) {
        const out = await exec(s[2], frame, `${at}/0`, state)
        if (out === BREAK) break
        if (out instanceof Back) return out
        tick(state)
      }
      return null
    }
    case 'each': {
      // ['each', item, source, body, key?]: a list gives position and item, a record part and value, a range each count.
      const [, item, source, body, key] = s
      const go = async (k, v) => {
        frame[item] = copy(v)
        if (key) frame[key] = k
        const out = await exec(body, frame, `${at}/0`, state)
        tick(state)
        return out
      }
      if (source?.[0] === 'range') {
        const from = num(await evaluate(source[1], frame, state)), to = num(await evaluate(source[2], frame, state))
        for (let i = from, n = 0; i <= to; i++, n++) { const out = await go(n, i); if (out === BREAK) break; if (out instanceof Back) return out }
        return null
      }
      const from = await evaluate(source, frame, state)
      const pairs = Array.isArray(from) ? from.map((v, i) => [i, v]) : kindOf(from) === 'record' ? Object.entries(from).map(([k, v]) => (key ? [k, v] : [null, k]))
        : typeof from === 'string' ? [...from].map((c, i) => [i, c]) : from == null ? [] : null
      if (!pairs) throw new Stop('type', `Can't go through each item of ${kindOf(from)}`)
      for (const [k, v] of pairs) { const out = await go(k, v); if (out === BREAK) break; if (out instanceof Back) return out }
      return null
    }
    case 'break': return BREAK
    case 'next': return NEXT
    case 'return': return new Back(await evaluate(s[1], frame, state))
    case 'fail': throw new Stop('fail', text(await evaluate(s[1], frame, state)))
    case 'try': {
      try {
        return await exec(s[1], frame, `${at}/0`, state)
      } catch (err) {
        if (!(err instanceof Stop) || err.code === 'limit' || err.code === 'denied' || err.code === 'stopped') throw err
        frame[s[2]] = err.message
        return exec(s[3], frame, `${at}/1`, state)
      }
    }
    case 'do': await evaluate(s[1], frame, state); return null
    default: throw new Stop('type', `This lingo doesn't know the step "${s[0]}"`)
  }
}

const tick = (state) => {
  if (state.ctx.signal?.aborted) throw new Stop('stopped', 'Stopped by the person')
  if (--state.left < 0) throw new Stop('limit', 'It took too many steps, so it was stopped. Is a loop never ending?')
  if (Date.now() > state.until) throw new Stop('limit', 'It ran too long, so it was stopped')
}

////////// 4. EXPRESSIONS //////////

const evaluate = async (e, frame, state) => {
  if (!Array.isArray(e)) return e   // nothing, true, false, a number or text
  tick(state)
  switch (e[0]) {
    case 'var': return e[1] in frame ? frame[e[1]] : null
    case 'list': { const out = []; for (const x of e.slice(1)) out.push(await evaluate(x, frame, state)); return out }
    case 'map': { const out = Object.create(null); for (const [k, v] of Object.entries(e[1])) out[k] = await evaluate(v, frame, state); return out }
    case 'get': return part(await evaluate(e[1], frame, state), await evaluate(e[2], frame, state))
    case 'op': return op(e[1], await evaluate(e[2], frame, state), await evaluate(e[3], frame, state))
    case 'and': return truthy(await evaluate(e[1], frame, state)) ? evaluate(e[2], frame, state) : false
    case 'or': { const a = await evaluate(e[1], frame, state); return truthy(a) ? a : evaluate(e[2], frame, state) }
    case 'not': return !truthy(await evaluate(e[1], frame, state))
    case 'neg': return -num(await evaluate(e[1], frame, state))
    case 'pick': return evaluate(truthy(await evaluate(e[1], frame, state)) ? e[2] : e[3], frame, state)
    case 'else': { const a = await evaluate(e[1], frame, state); return a ?? evaluate(e[2], frame, state) }
    case 'range': {
      const out = [], to = num(await evaluate(e[2], frame, state))
      for (let i = num(await evaluate(e[1], frame, state)); i <= to; i++) out.push(i)
      return out
    }
    case 'call': {
      const fn = state.program.fns[e[1]]
      if (!fn) throw new Stop('type', `There is no function ${e[1]}`)
      const args = []
      for (const a of e[2]) args.push(await evaluate(a, frame, state))
      if (++state.depth > 1000) throw new Stop('limit', 'A function called itself more than 1000 deep, so it was stopped')
      const inner = Object.create(null), back = state.at
      try {
        await bind(fn.params, args, inner, state, false)
        const out = await exec(fn.body, inner, `fn:${e[1]}`, state)
        state.at = back
        return out instanceof Back ? out.value : null
      } finally { state.depth-- }
    }
    case 'say': {
      const p = state.lingo.phrases[e[1]]
      if (!p) throw new Stop('unknown', `This device doesn't have the phrase ${e[1]}`)
      const refused = [].concat(p.needs).find((n) => !state.allow.has(n))
      if (refused) throw new Stop('denied', `${e[1]} needs permission for ${refused}, which wasn't given`)
      const args = {}
      for (const x of p.params) args[x.name] = e[2][x.name] !== undefined ? await evaluate(e[2][x.name], frame, state) : x.default ?? null
      try { return (await p.run(args, state.ctx)) ?? null }
      catch (err) { throw err instanceof Stop ? err : new Stop('phrase', `${e[1]}: ${err.message}`) }
    }
    default: throw new Stop('type', `This lingo doesn't know the expression "${e[0]}"`)
  }
}

const op = (o, a, b) => {
  switch (o) {
    case '+':
      if (typeof a === 'number' && typeof b === 'number') return a + b
      if (Array.isArray(a) && Array.isArray(b)) return [...a, ...b]
      if (typeof a === 'string' || typeof b === 'string') return text(a) + text(b)
      throw new Stop('type', `Can't add ${kindOf(a)} and ${kindOf(b)}`)
    case '-': return num(a, o) - num(b, o)
    case '*': return num(a, o) * num(b, o)
    case '/': if (num(b, o) === 0) throw new Stop('type', "Can't divide by zero"); return num(a, o) / b
    case '%': if (num(b, o) === 0) throw new Stop('type', "Can't divide by zero"); { const m = num(a, o) % b; return m !== 0 && (m < 0) !== (b < 0) ? m + b : m }
    case '==': return same(a, b)
    case '!=': return !same(a, b)
    case '<': return compare(a, b) < 0
    case '>': return compare(a, b) > 0
    case '<=': return compare(a, b) <= 0
    case '>=': return compare(a, b) >= 0
    case 'in': return Array.isArray(b) ? b.some((x) => same(x, a)) : typeof b === 'string' ? b.includes(text(a)) : kindOf(b) === 'record' ? Object.hasOwn(b, text(a)) : false
    default: throw new Stop('type', `There is no operator ${o}`)
  }
}

// Reading something missing gives nothing, never an error. Negative positions count from the end.
const part = (from, key) => {
  if (Array.isArray(from) || typeof from === 'string') {
    if (!Number.isInteger(key)) return null
    return from[key < 0 ? from.length + key : key] ?? null
  }
  if (kindOf(from) === 'record') return Object.hasOwn(from, text(key)) ? from[text(key)] : null
  return null
}

////////// 5. VALUES //////////

export const kindOf = (x) => (x == null ? 'nothing' : typeof x === 'boolean' ? 'true/false' : typeof x === 'number' ? 'number' : typeof x === 'string' ? 'text' : Array.isArray(x) ? 'list' : 'record')
export const truthy = (x) => x != null && x !== false
const num = (x, o) => { if (typeof x !== 'number' || Number.isNaN(x)) throw new Stop('type', `${o ? `Can't use ${o} with` : 'Expected a number, but this is'} ${kindOf(x)}`); return x }

const compare = (a, b) => {
  if (typeof a === 'number' && typeof b === 'number') return a - b
  if (typeof a === 'string' && typeof b === 'string') return a < b ? -1 : a > b ? 1 : 0
  throw new Stop('type', `Can't compare ${kindOf(a)} with ${kindOf(b)}`)
}

export const same = (a, b) => {
  if (a === b) return true
  if (kindOf(a) !== kindOf(b) || a == null || typeof a !== 'object') return false
  if (Array.isArray(a)) return a.length === b.length && a.every((x, i) => same(x, b[i]))
  const ka = Object.keys(a)
  return ka.length === Object.keys(b).length && ka.every((k) => same(a[k], b[k]))
}

// Setting a name copies, so two names never share one list.
export const copy = (x) => (Array.isArray(x) ? x.map(copy) : x && typeof x === 'object' ? Object.assign(Object.create(null), ...Object.entries(x).map(([k, v]) => ({ [k]: copy(v) }))) : x)

// How a value reads as text: nothing is empty, lists and records as JSON.
export const text = (x) => (x == null ? '' : typeof x === 'boolean' ? String(x) : typeof x === 'number' ? String(x) : typeof x === 'object' ? JSON.stringify(x) : x)
