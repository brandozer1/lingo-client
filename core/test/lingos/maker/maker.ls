"for the tests: makes and changes lingos with lingo.save, and reads one with lingo.code."
// 1. make(name, code)   -> [{name, does}]: its functions, saved and running
// 2. read(name)         -> {code, openapi}: its code, as its maker wrote it
// 3. here()             -> [{name, about, functions}]: the lingos running here

fn make(name, code) "Saves a lingo and gives its functions." {
  return lingo.save(name, code)
}

fn read(name) "A lingo's code." {
  return lingo.code(name)
}

fn here() "The lingos running here, each with its functions." {
  return lingo.list()
}
