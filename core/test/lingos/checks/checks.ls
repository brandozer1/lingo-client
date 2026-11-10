"for the tests: the language, the core's own phrases and the library, each fn a case test/server.mjs calls."

fn numbers() "Numbers, written the way JavaScript writes them." {
  big = 1000000 * 1000000 * 1000000
  return [1 + 2 * 3, 10 % 3, -7 % 3, 1 / 3, 0.1 + 0.2, 2 - 5, big * 1000, big * 100, 123456789012, 0.000001, 1 / 10000000, -0, 5 / 2, 2 / 3 * 3, 100 / 3]
}

fn text_in_text() "Values inside text." {
  a = 3
  b = "x"
  l = [1, "two", 1.5]
  r = {k: nothing}
  return "{a} and {b} and {l} and {nothing} and {true} and {r}"
}

fn lists() "Lists grown, set and read, from the end too." {
  l = []
  for i in 1..5 {l += [i * i]}
  l[0] = 100
  l[5] = 6
  return [l, l[-1], l[9], length(l), slice(l, 1, -1), 3 in l]
}

fn copies() "Setting a name copies the value." {
  a = [1, [2]]
  b = a
  b[1][0] = 9
  r = {x: {y: 1}}
  s = r
  s.x.y = 2
  return [a, b, r, s]
}

fn records() "Records keep their parts in the order they were added." {
  r = {b: 2, a: 1}
  r.c = 3
  out = []
  for k, v in r {out += ["{k}={v}"]}
  for k in r {out += [k]}
  return [out, std.keys(r), "a" in r, r.zz]
}

fn texts() "Text, counted by characters." {
  s = "Héllo, wörld"
  return [length(s), slice(s, -5), slice(s, 2, 4), slice(s, 9, 99), s[1], "wö" in s, text(1.5), text([1, "a"])]
}

fn chars() "Each character of text, with its place." {
  out = []
  for i, c in "añb" {out += ["{i}{c}"]}
  return out
}

fn numbers_from_text() "Numbers read from text." {
  return [number("$1,204.50"), number("12%"), number("abc"), number(" -3.5e2x")]
}

fn remainders() "% has the divisor's sign and is exact, so n - n % 1 is the whole number below n, which math.round stands on." {
  return [7 % 3, -7 % 3, 7 % -3, 0.1 % 1, 2.3 % 1, -0.1 % 1, 0.1 - 0.1 % 1, -1.5 - (-1.5 % 1), 2.5 - 2.5 % 1, -7.25 % 2]
}

fn jsons() "JSON there and back." {
  x = from_json(to_json({a: [1, 2.5, "é\t"], b: nothing}))
  return [x, to_json(x), from_json("[1,\"\\u00e9\",-2.5e3,null]"), to_json("q\"\n"), std.text(1.5)]
}

fn bytes_back() "Text as its bytes and back, and bytes that aren't text." {
  b = bytes("aé€😀")
  try {from_bytes([255])} catch e {x = e}
  try {from_bytes([1.5])} catch e {y = e}
  return [b, from_bytes(b), from_bytes([]), x, y]
}

fn bytes_like_a_list() "Bytes read as a list of numbers: indexed, counted, sliced, added to, found in, gone through and compared." {
  b = bytes("GET / HTTP/1.1\r\nHost: x\r\n\r\n")
  end = std.find(b, "\r\n\r\n")
  line = slice(b, 0, std.find(b, "\r\n"))
  more = bytes("ab") + [99]
  sum = 0
  for n in more {sum += n}
  return [b[0], b[-1], b[999], length(b), end, std.find(b, "nope"), from_bytes(line), from_bytes(more), sum, 97 in more, "bc" in more, more == [97, 98, 99], bytes([1, 2]) == bytes([1, 2])]
}

fn fib(n) "A function calling itself." {
  if n < 2 {return n}
  return fib(n - 1) + fib(n - 2)
}

fn add(a, b=10) "A value with a default." {return a + b}

fn functions() "Functions with their values and defaults." {return [fib(20), add(1), add(1, 2)]}

fn control() "Loops, breaks and the operators that stop early." {
  n = 0
  while true {
    n += 1
    if n > 10 {break}
    if n % 2 == 0 {continue}
  }
  x = nothing
  return [n, x ?? 5, n > 5 ? "big" : "small", not false, 1 and 2, nothing or "b"]
}

fn greet(who, times=2) "Values given by name, and a default." {
  out = []
  for i in 1..times {out += ["hi {who}"]}
  return out
}

fn try_and_fail() "Errors caught, a call to a group this computer hasn't got among them, which a try lets it load." {
  try {fail "boom"} catch e {a = e}
  try {x = 1 + "a" + []} catch e {b = e}
  try {y = 1 - "a"} catch e {c = e}
  try {from_json("[")} catch e {d = e}
  try {nowhere.thing(1)} catch e {f = e}
  return [a, b, c, d, f]
}

fn error_at_a_line() "An error, with its line." {
  x = 1
  z = x < "a"
  return z
}

fn divide_by_zero() "Dividing by zero." {return 1 / 0}

fn deep_loop() "A long loop." {
  n = 0
  for i in 1..100000 {n += i}
  return n
}

fn notes() "A line in the lingo client's log." {
  log("first")
  log([1, 2])
  return 1
}

fn waited() "How long a wait takes." {
  a = now()
  compute.wait(0.2)
  return now() - a
}

fn stored() "Onsay's own storage, its keys any text after its own name." {
  store.put("checks/Notes/Today", "milk")
  store.put("checks/notes/today", "eggs")
  store.put("checks/a.b", [1])
  store.put("checks/x/../y", "safe")
  out = [store.get("checks/Notes/Today"), store.get("checks/notes/today"), store.get("checks/a.b"), store.get("checks/nope"), store.keys("checks/notes")]
  store.delete("checks/a.b")
  return out + [store.get("checks/a.b"), store.keys()]
}

fn others() "Another lingo's keys and secrets are its own, even after an error from its code is caught." {
  out = []
  try {store.get("vault/pin")} catch e {out += [e]}
  try {setting("pin", "vault")} catch e {out += [e]}
  try {settings.set("prefs", "units", "kelvin")} catch e {out += [e]}
  try {vault.read("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx")} catch e {}
  try {store.keys("vault/")} catch e {out += [e]}
  return out + [settings.get("vault")[0].value, setting("units", "prefs")]
}

fn forever() "Never ends, until a limit or a stop." {while true {}}

fn forever_in_try() "A stop gets through a try." {while true {try {while true {}} catch e {}}}

fn wait_long() "A wait a stop ends." {compute.wait(30)}

fn deeper(n) "Deeper and deeper." {return deeper(n + 1)}

fn memory() "More and more memory." {
  l = ["xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"]
  while true {l += l}
}

fn sliced() "A phrase called with a default." {return std.slice("abc", 1)}

fn through() "use(...) of another computer, from a lingo the loop of a to-do called here." {return use("desk.tester/memo/recall", {k: "milk"})}

fn tokens() "setting(...) of another lingo, and nothing when it is unset." {return [setting("token", "github"), setting("nope", "github") ?? "metric"]}


fn http_get(base) "http.send: a GET, its status, headers and text." {
  r = http.send("GET", base + "/hello?x=1")
  return [r.status, r.headers["x-test"], r.text]
}

fn http_post(base) "http.send: a POST with headers and a body." {
  r = http.send("post", base + "/echo", {"X-Name": "Ada", "Content-Type": "text/plain"}, "héllo")
  return [r.status, from_json(r.text)]
}

fn http_secret(base) "http.send with a key in a header and in the address, which never reach Onsay in a call." {
  r = http.send("GET", base + "/hello?api_key=sekrit&x=1", {"Authorization": "Bearer sekrit"})
  return r.status
}

fn http_missing(base) "http.send: an answer that is not 2xx is still an answer." {
  r = http.send("GET", base + "/missing")
  return [r.status, r.text]
}

fn http_moved(base) "http.send follows a redirect." {return http.send("GET", base + "/moved").text}

fn http_slow(base) "http.send ends on time." {http.send("GET", base + "/slow", nothing, nothing, 1)}

fn http_nowhere(address="http://127.0.0.1:1/") "http.send where nothing answers, or to a name with nowhere to send it." {
  try {http.send("GET", address)} catch e {return e}
}

fn socket_echo(base) "socket: one opened with a header, a message sent and its echo back, nothing when none comes in time, and closed, after which it is gone." {
  s = socket.open(base + "/echo", {"X-Name": "Ada"})
  hi = socket.receive(s)
  socket.send(s, "héllo")
  back = socket.receive(s, 5)
  none = socket.receive(s, 1)
  socket.close(s)
  try {socket.send(s, "again")} catch e {return [hi, back, none, e]}
}

fn socket_left(base) "socket: one left open, which the end of the run closes." {return socket.open(base + "/echo") > 0}

fn connection_round(port) "connection: a listener on this computer, a tcp connection to it, bytes each way, nothing when none comes in time, the other side closing, and addresses it turns down." {
  l = connection.listen("tcp://127.0.0.1:{port}")
  c = connection.open("tcp://localhost:{port}")
  s = connection.accept(l, 5)
  connection.send(c, "ping")
  got = connection.receive(s, 5)
  connection.send(s, std.bytes([0, 255]))
  back = connection.receive(c, 5)
  none = connection.receive(c, 0.2)
  connection.close(c)
  try {connection.receive(s, 5)} catch e {closed = e}
  wrong = []
  for address in ["example.com:80", "tcp://example.com", "carrier://x:1"] {
    try {connection.open(address)} catch e {wrong += [e]}
  }
  try {connection.listen("tcp://0.0.0.0:{port}")} catch e {wrong += [e]}
  try {connection.accept(s)} catch e {wrong += [e]}
  return [std.from_bytes(got), back, none, closed, wrong]
}

fn socket_names(names) "socket.open at Onsay names: each one's greeting and echo, or why it couldn't open." {
  out = []
  for name in names {
    try {
      s = socket.open(name + "/echo", {"X-Name": "Ada"})
      socket.send(s, "héllo")
      out += [[socket.receive(s, 5), socket.receive(s, 5)]]
      socket.close(s)
    } catch e {out += [e]}
  }
  return out
}

fn socket_nowhere() "socket.open where nothing answers, and to an address that isn't a WebSocket's." {
  out = []
  try {socket.open("ws://127.0.0.1:1/")} catch e {out += [e]}
  try {socket.open("http://127.0.0.1:1/")} catch e {out += [e]}
  return out
}

fn request_busy(base) "request tries again after a 503." {
  r = request.get(base + "/busy", {query: {id: "r1", n: 2}, retries: 2})
  return [r.status, r.ok, r.json]
}

fn texts_library() "text: split, join, replace, trim, lower and upper." {
  s = "Héllo, wörld"
  return [text.upper(s), text.lower(s), text.split(s, ", "), text.join(["a", "b"], "-"), text.replace(s, "l", "L"), "wö" in s, list.reverse("abc"),
    text.trim("  x  "), text.split("abc", ""), text.split("a,b"), text.join([1, nothing, "x"]), text.trim(" \t\nx y\n ")]
}

fn cases() "text: lower and upper in Latin with its accents, Greek and Cyrillic." {
  s = "Ça va, Øre, Łódź, Ÿ ÿ, Αλφα, Привет, ёЁ"
  return [text.upper(s), text.lower(s), text.split("a--b--", "--"), text.replace("aaa", "a", "bb"), text.join([]), text.join([[1], {a: 2}], ";")]
}

fn sorts() "list: sort, records by a part, equal ones kept in order, and reverse." {
  return [list.sort([3, 1, 2]), list.sort(["b", "a", "c"]), list.sort([{n: 2, k: "x"}, {n: 1, k: "y"}, {n: 2, k: "z"}], "n"), list.sort([]),
    list.sort(["B", "a", "C"]), list.reverse([1, 2, 3]), list.reverse([]), list.reverse("añb")]
}

fn rounds() "math: round to the nearest, up and down, to places, and min and max." {
  return [math.round(2.5), math.round(1.005, 2), math.round(-2.5), math.round(-1.5, 0, "down"), math.round(1.1, 0, "up"), math.min(3, 2),
    math.max("a", "b"), math.round(1234.5678, -2), math.round(1.25, 1), math.round(-1.5, 0, "up"), math.round(2, 0, "down"),
    math.round(0.1, 0, "down"), math.round(-0.1, 0, "up")]
}

fn days() "time: the days between ISO times, and one that is not a time." {
  try {time.difference("nope", "2026-01-01", "days")} catch e {x = e}
  return [time.difference("2026-01-01", "2026-03-01", "days"), time.difference("2026-10-12T14:00:00Z", "2026-10-13T02:00:00+02:00", "days"),
    time.difference("2026-10-12T14:00:00.5Z", "2026-10-12T15:30:00-01:00", "days"), time.difference("2024-02-28", "2024-03-01T12:00", "days"),
    time.parse("2026-13-01"), time.parse("2026-10-18T14:05:00+02:00"), x]
}

fn capital(country) "lingo(...), asked as it runs, the program going on with the answer." {
  x = lingo("the capital of a country", {country: country}, "text")
  return "it is " + x
}

fn capital_of_japan() "lingo(...) with its words worked out as it runs." {
  goal = "the capital of a country"
  return lingo(goal, {country: "Japan"})
}

fn only_here() "lingo(...) that only this computer can answer, so Onsay asks it while it waits." {return lingo("something only this device knows", {})}

fn impossible() "lingo(...) nothing can do." {return lingo("book a flight to Paris", {})}

fn unanswered() "lingo(...) never answered." {return lingo("a question never answered", {})}

fn use_by_name() "use(...) of another computer's lingo, values by name." {
  x = use("notes.laptop/check", {key: "milk"})
  return "it says " + x
}

fn use_in_order() "use(...) with its values in a list." {return use("weather/forecast", ["Rome", 2])}

fn list_basics() "The array and number basics, and a web address with an Onsay name taken apart." {
  place = url.parse("https://laptop.brandon/weather/now")
  return [list.sum([1, 2, 3]), list.sum([]), list.unique([1, 2, 1, 3]), list.flatten([1, [2, 3], [4]]), list.index_of(["a", "b"], "b"), list.index_of([], "b"), math.abs(-3), [place.host, place.path]]
}

fn who() "Who called, and the data of the badge it came with." {return badge.read()}
