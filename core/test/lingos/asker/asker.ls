// asker - for the tests: its functions call lingo(...), reach a lingo elsewhere with use(...), and call one in this same
// lingos folder by its dotted name, all as they run.

// What was kept under a name, asked through lingo(...).
fn kept(k) {
  return lingo("what was kept under a name", {name: k}, "text")
}

// A city's forecast, from a lingo on a web server, reached with use(...).
fn forecast(city) {
  return use("weather.example.com/weather/forecast", [city, 1])
}

// What memo, a lingo in this same lingos folder, keeps under a name, called by its dotted name.
fn noted(k) {
  return memo.recall(k)
}

// What it says when called with no function named.
fn main() {
  return "asker's main"
}
