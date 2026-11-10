"for the tests: its functions call lingo(...), reach a lingo elsewhere with use(...), and call one in this same lingos folder by its dotted name, all as they run."

fn started() "What was kept under a name, asked through lingo(...)." {
  id = commands.start("echo started-ok").id
  for i in 1..40 {
    got = commands.output(id)
    if not got.running {return got}
    compute.wait(0.5)
  }
  return got
}

fn ways(task) {
  return lingo.ways(task, {name: "x"})
}

fn kept(k) {
  return lingo("what was kept under a name", {name: k}, "text")
}

fn forecast(city) "A city's forecast, from a lingo on a web server, reached with use(...)." {
  return use("weather.example.com/weather/forecast", [city, 1])
}

fn noted(k) "What memo, a lingo in this same lingos folder, keeps under a name, called by its dotted name." {
  return memo.recall(k)
}

fn main() "What it says when called with no function named." {
  return "asker's main"
}
