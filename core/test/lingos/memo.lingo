// memo - things to remember, kept in Onsay's own storage on this device.
//
// 1. remember(k, v)    keeps v under the name k
// 2. recall(k)         -> text?: what was kept under k
// 3. all(prefix="")    -> [text]: every name kept
// 4. key(k)            -> text: where k is kept in the store

// 1. Keeps it under that name.
fn remember(k, v) {
  store.put(key(k), v)
}

// 2. What was kept under that name.
fn recall(k) {
  return store.get(key(k))
}

// 3. Every name kept, only those that start with prefix when it is given.
fn all(prefix="") {
  out = []
  for k in store.keys(key(prefix)) {out += [std.slice(k, 5)]}
  return out
}

// 4. Where a name is kept in Onsay's own storage.
fn key(k) {return "memo/" + k}
