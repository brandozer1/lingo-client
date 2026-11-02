// maker - for the tests: makes and changes lingos with lingo.save, and reads one with lingo.code.
//
// 1. make(name, code)   -> [{name, does}]: its functions, saved and running
// 2. read(name)         -> {code, openapi}: its code, as its maker wrote it
// 3. here()             -> [{name, about, functions}]: the lingos running here

// 1. Saves a lingo and gives its functions.
fn make(name, code) {
  return lingo.save(name, code)
}

// 2. A lingo's code.
fn read(name) {
  return lingo.code(name)
}

// 3. The lingos running here, each with its functions.
fn here() {
  return lingo.list()
}
