// std.mjs - how the core lingo's std group does its phrases, as src/std.c does them: reaching inside values and converting
// them, what Lingo can't write for itself. What they are called, what they take and what they give is in the core's own
// manifest, which is the authority. Every one works on values alone, never the device, so all of them are pure.

import { Stop, kindOf, text } from './run.mjs'

const list = (l) => { if (!Array.isArray(l)) throw new Stop('type', `Expected a list, but this is ${kindOf(l)}`); return l }
const at = (x, i) => (i == null ? undefined : i < 0 ? Math.max(0, x.length + i) : i)

export const STD = {
  'std.length': ({ x }) => (x == null ? 0 : Array.isArray(x) || typeof x === 'string' ? x.length : kindOf(x) === 'record' ? Object.keys(x).length : 0),
  'std.keys': ({ r }) => (kindOf(r) === 'record' ? Object.keys(r) : []),
  'std.slice': ({ x, from, to }) => (typeof x === 'string' ? x : list(x)).slice(at(x, from), at(x, to)),
  // "$1,204.50" is 1204.5: money signs, commas, spaces and percent signs are ignored.
  'std.number': ({ x }) => {
    if (typeof x === 'number') return x
    const n = parseFloat(text(x).replace(/[$€£,%\s]/g, ''))
    return Number.isFinite(n) ? n : null
  },
  'std.text': ({ x }) => text(x),
  'std.to_json': ({ x }) => JSON.stringify(x),
  'std.from_json': ({ text: t }) => {
    try { return JSON.parse(text(t), (_, v) => (v && typeof v === 'object' && !Array.isArray(v) ? Object.assign(Object.create(null), v) : v)) }
    catch { throw new Stop('phrase', "std.from_json: that text isn't JSON") }
  },
  'std.bytes': ({ text: t }) => [...new TextEncoder().encode(text(t))],
  'std.from_bytes': ({ list: l }) => {
    if (list(l).some((n) => typeof n !== 'number' || n < 0 || n > 255 || n !== Math.floor(n))) throw new Stop('phrase', 'std.from_bytes: a byte is a whole number from 0 to 255')
    try { return new TextDecoder('utf-8', { fatal: true }).decode(new Uint8Array(l)) } catch { throw new Stop('phrase', "std.from_bytes: those bytes aren't UTF-8 text") }
  },
}
