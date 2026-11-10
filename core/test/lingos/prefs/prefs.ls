"for the tests: settings answered on this device, from the file kept for it, its openapi.json's defaults, and another lingo's."

fn units() "Its units, metric unless set." {
  return setting("units")
}

fn name() "Its name, which has no default." {
  return setting("name") ?? "no name"
}

fn color() "Its color, a setting only its code reads." {
  return setting("color") ?? "blue"
}

fn token() "github's token, another lingo's setting." {
  return setting("token", "github")
}

fn set_units() "settings.set of its own, then settings.get." {
  settings.set("prefs", "units", "kelvin")
  return [units(), settings.get("prefs")]
}
