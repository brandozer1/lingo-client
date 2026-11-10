"for the tests: its settings kept by its own functions, in Onsay's own storage on this device, never in the file the core keeps."

fn pin() "Its pin." {
  return setting("pin")
}

fn read(key) "One of its settings, as it keeps them. For the core." {
  kept = store.get("vault/" + key)
  return kept == nothing ? nothing : from_json(kept)
}

fn write(key, value) "Keeps one of its settings. For the core." {
  store.put("vault/" + key, to_json(value))
}

fn change() "Its pin, changed with settings.set, then read every way." {
  a = pin()
  settings.set("vault", "pin", "1234")
  return [a, pin(), store.get("vault/pin"), settings.get("vault")[0].value]
}
