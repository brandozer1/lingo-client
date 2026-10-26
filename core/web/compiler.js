// compiler.js - the Lingo compiler on its own, as WebAssembly, for the tests: the same C the core compiles with
// (src/compile.c). ../bin/lingo.wasm is built by build.ps1.
//
// 1. lingo(request)   -> one call to the compiler, JSON in and out: see compile.c section 7 for every op

import { readFileSync } from 'node:fs'
import { WASI } from 'node:wasi'

const wasi = new WASI({ version: 'preview1' })
const { instance } = await WebAssembly.instantiate(readFileSync(new URL('../bin/lingo.wasm', import.meta.url)), wasi.getImportObject())
wasi.initialize(instance)
const { memory, lingo_alloc, lingo_free, lingo_call, lingo_length } = instance.exports

////////// FUNCTIONS //////////

// 1.
export const lingo = (request) => {
  const bytes = new TextEncoder().encode(JSON.stringify(request))
  const at = lingo_alloc(bytes.length)
  new Uint8Array(memory.buffer, at, bytes.length).set(bytes)
  const out = lingo_call(at, bytes.length)
  lingo_free(at)
  return JSON.parse(new TextDecoder().decode(new Uint8Array(memory.buffer, out, lingo_length())))
}
