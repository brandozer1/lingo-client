// vault - for the tests: its settings kept by its own functions, in Onsay's own storage on this device, never in the
// file the core keeps.

// Its pin.
fn pin() {
  return setting("pin")
}

// One of its settings, as it keeps them. For the core.
fn read(key) {
  kept = store.get("vault/" + key)
  return kept == nothing ? nothing : from_json(kept)
}

// Keeps one of its settings. For the core.
fn write(key, value) {
  store.put("vault/" + key, to_json(value))
}

// Its pin, changed with settings.set, then read every way.
fn change() {
  a = pin()
  settings.set("vault", "pin", "1234")
  return [a, pin(), store.get("vault/pin"), settings.get("vault")[0].value]
}
