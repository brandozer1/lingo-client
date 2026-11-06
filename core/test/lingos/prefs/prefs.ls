// prefs - for the tests: settings answered on this device, from the file kept for it, its openapi.json's defaults, and
// another lingo's.

// Its units, metric unless set.
fn units() {
  return setting("units")
}

// Its name, which has no default.
fn name() {
  return setting("name") ?? "no name"
}

// Its color, a setting only its code reads.
fn color() {
  return setting("color") ?? "blue"
}

// github's token, another lingo's setting.
fn token() {
  return setting("token", "github")
}

// settings.set of its own, then settings.get.
fn set_units() {
  settings.set("prefs", "units", "kelvin")
  return [units(), settings.get("prefs")]
}
