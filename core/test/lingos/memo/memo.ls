"things to remember, kept in Onsay's own storage on this device."
// 1. remember(k, v)    keeps v under the name k
// 2. recall(k)         -> text?: what was kept under k
// 3. all(prefix="")    -> [text]: every name kept
// 4. key(k)            -> text: where k is kept in the store

fn remember(k: text "the name to keep it under", v: any "what to keep") -> nothing "Keeps it under that name." {
  store.put(key(k), v)
}

fn recall(k) "What was kept under that name." {
  return store.get(key(k))
}

fn all(prefix="") "Every name kept, only those that start with prefix when it is given." {
  out = []
  for k in store.keys(key(prefix)) {out += [std.slice(k, 5)]}
  return out
}

fn key(k) "Where a name is kept in Onsay's own storage." {return "memo/" + k}
