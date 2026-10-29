// writer.ls - Writes lingos for you. Say what a lingo should do, like a lingo that tells me the weather for a city, and
// it writes it, reviewed and fixed by a second look, ready to put in your computer's lingos folder. It changes a lingo
// you already have the same way. agent's AI writes them, on your own key: a strong coding model writes the best ones.
//
// 1. write(request, chat=nothing, model=nothing)  -> made: a new lingo that does what request says, chat the chat before it
// 2. revise(name, code, change, model=nothing)    -> made: the lingo name, its code, changed as asked
// made = {name, file, code, manifest, notes}: file is <name>.ls, manifest its openapi.json when it needs one, else
// nothing, and notes in plain words what it made, the settings to set and the lingos it calls. model is a model id of
// the service agent uses, else this lingo's model setting, else agent's. A model drafts it, a second pass reviews it
// against a checklist, and a third fixes what the review found, at most twice, so at most 5 asks. It isn't loaded to
// check it: the computer does that when the person puts it in its lingos folder, and the notes say so. Nothing is saved.

////////// 1-2. WRITING //////////

// 1. Writes a new lingo from a plain description of what it should do, like a lingo that tells me the weather for a
// city: its Lingo code, ready to put in the computer's lingos folder. Use it only when the person asks for a lingo to be
// written, made, created or built. Not for chatting, answering questions or doing the task itself. notes says what it
// made, the settings to set and the lingos it calls. request is what the lingo should do, in plain words. chat is the
// conversation before the request, oldest first: a list of {from, text}, from being you or onsay.
fn write(request, chat=nothing, model=nothing) {
  before = ""
  for t in chat ?? [] {before += "{t.from}: {t.text}\n"}
  if before != "" {before = "The conversation before it:\n" + before + "\n"}
  return made(before + "Write a new lingo that does this: {request}", model, nothing)
}

// 2. Changes a lingo the person already has, as they ask, like add a function for tomorrow's weather, and gives back its
// code changed and reviewed again. Use it only when they ask to change, edit, fix or add to a lingo and give its code.
// Not for chatting or doing the task itself. name is the lingo's name, like weather, code its whole .ls file, and
// change what to change, in plain words. Its name stays, and so does everything the change doesn't touch.
fn revise(name, code, change, model=nothing) {
  if text.trim(std.text(code ?? "")) == "" {fail "To change a lingo, give its code: the whole of its .ls file"}
  ask = "Change this lingo as asked, keeping its name, {name}, and everything the change doesn't touch. The change: {change}\n\n"
  return made(ask + shown({name: name, code: code, manifest: nothing}), model, name)
}

////////// HELPERS //////////

// The draft, then each round a review and a fix while the review or the checks find problems, at most 2 rounds. For
// writer's own use.
fn made(asked, model, name) {
  m = model ?? setting("model")
  if m == "" {m = nothing}
  start = "You write lingos for Onsay. " + form() + "\n\n" + guide() + "\n\n" + asked
  d = drafted(start, m, name)
  for turn in 1..2 {
    problems = checked(d) + reviewed(asked, d, m)
    if std.length(problems) == 0 {return result(d, [])}
    d = drafted(start + "\n\nYour lingo so far:\n" + form(d) + "\n\nA review found these problems:\n- " + text.join(problems, "\n- ") +
      "\nFix every one, change nothing else, and answer with the whole lingo again, the same way.", m, name)
  }
  return result(d, checked(d))
}

// The model's lingo, its name as a lingo's name: lower case words joined by _. For writer's own use.
fn drafted(prompt, model, name) {
  a = answered(agent.answer(prompt, model))
  if json.kind(a.code) != "text" {
    fail "The model's answer had no code in it, so nothing was written. Try again, or set a stronger model in writer's settings"
  }
  n = name ?? text.case(a.name ?? "", "snake")
  if n == "" {fail "The model's lingo had no name, so nothing was written. Try again, saying what it should be called"}
  return {name: n, code: a.code, manifest: json.kind(a.manifest) == "record" ? a.manifest : nothing, notes: std.text(a.notes ?? "")}
}

// The problems a second model finds, reading the lingo against the checklist. For writer's own use.
fn reviewed(asked, d, model) {
  a = answered(agent.answer("You review lingos written for Onsay, against the guide they were written by.\n\n" + guide() +
    "\n\nWhat was asked:\n" + asked + "\n\nThe lingo, as written:\n\n" + shown(d) + "\n\n" + checklist(), model))
  out = []
  for p in json.kind(a.problems) == "list" ? a.problems : [] {out += [std.text(p)]}
  return out
}

// What can be checked without a model: a comment above each fn, and no em dash. For writer's own use.
fn checked(d) {
  out = []
  if std.from_bytes([226, 128, 148]) in d.code + d.notes {
    out += ["It has an em dash in it: write a comma, a colon or a full stop instead"]
  }
  before = ""
  for line in text.lines(d.code) {
    l = text.trim(line)
    head = text.split(l, "\{")[0]
    if text.starts_with(l, "fn ") and not text.starts_with(before, "//") {
      out += ["{head} has no comment right above it saying what it does, when to use it and when not to"]
    }
    before = l
  }
  return out
}

// What the model said, read as JSON past any words or fence around it, with the code from its lingo block, so the code
// is never escaped inside JSON. For writer's own use.
fn answered(said) {
  t = ""
  code = nothing
  for i, p in text.split(std.text(said ?? ""), "```") {
    first = text.split(p, "\n")[0]
    if i % 2 == 1 and text.trim(first) == "lingo" {code = std.slice(p, std.length(first) + 1)} else {t += p}
  }
  t = std.slice(t, std.length(text.split(t, "\{")[0]))
  t = std.slice(t, 0, std.length(t) - std.length(text.split(t, "\}")[-1]))
  a = json.parse(t)
  if json.kind(a) != "record" {
    fail "The model didn't answer in JSON, so nothing was written. Try again, or set a stronger model in writer's settings"
  }
  if code != nothing {a.code = code}
  return a
}

// The lingo as the person gets it: its file and manifest, and notes on what to set, what it calls and what is still
// wrong. For writer's own use.
fn result(d, left) {
  notes = text.trim(d.notes) == "" ? [] : [text.trim(d.notes)]
  set = []
  for part in std.slice(text.split(d.code, "setting(\""), 1) {
    s = text.split(part, "\"")[0]
    rest = text.trim(std.slice(part, std.length(s) + 1))
    if not (s in set) and std.slice(rest, 0, 1) == ")" {set += [s]}
  }
  if std.length(set) > 0 {notes += ["Then set these in its settings, in the Onsay app's Lingos settings or with the command line's set: " + text.join(set, ", ") + "."]}
  calls = []
  for l in ["request", "json", "text", "list", "math", "url", "encode", "time", "agent"] {if l + "." in d.code {calls += [l]}}
  if std.length(calls) > 0 {notes += ["It calls " + text.join(calls, ", ") + ", so they go in the same lingos folder."]}
  if std.length(left) > 0 {notes += ["Still to fix before you use it: " + text.join(left, "; ") + "."]}
  if d.manifest == nothing {
    notes += ["To add it, make a folder {d.name} in your computer's lingos folder, with {d.name}.ls in it, then start it."]
  } else {
    notes += ["To add it, make a folder {d.name} in your computer's lingos folder, with {d.name}.ls and its openapi.json in it, then start it."]
  }
  notes += ["It hasn't been loaded to check it: once it is in the lingos folder, the lingo client loads it, or says why it can't."]
  return {name: d.name, file: "{d.name}.ls", code: d.code, manifest: d.manifest, notes: text.join(notes, "\n")}
}

// The lingo's files, as the models read them. For writer's own use.
fn shown(d) {
  out = "{d.name}.ls:\n" + d.code
  if d.manifest != nothing {out += "\n\nopenapi.json:\n" + std.to_json(d.manifest)}
  return out
}

// How the writing model answers, and its answer written that way. For writer's own use.
fn form(d=nothing) {
  if d != nothing {return "```json\n" + std.to_json({name: d.name, notes: d.notes, manifest: d.manifest}) + "\n```\n```lingo\n" + d.code + "\n```"}
  return "Answer with its name, notes and manifest as one JSON object, then its whole code in a lingo block, and nothing else:
```json
\{\"name\": \"weather\", \"notes\": \"what it does and how to use it, in one or two plain sentences\", \"manifest\": null\}
```
```lingo
the whole weather.ls file
```
manifest is its openapi.json as a JSON object when it needs one, and null when it doesn't."
}

// What the review checks, and how it answers. For writer's own use.
fn checklist() {
  return "Check it against each point, and list every real problem:
1. Its code is fn blocks only: no tool, no steps and no names set outside a function.
2. The comment lines right above each fn say what it gives or does, when to use it, and when not to, and what its values are, in words a search for the task would match. The first comment lines of the file, before a blank line, say what the lingo is for.
3. The code is Lingo as the guide says: \\\{ and \\\} for braces meant as themselves in text, no quotes inside \{\} in text, one comparison at a time, a record set before its parts, every phrase with its group, nothing called that doesn't exist.
4. A lingo of the library is called by its dotted name, like request.get(...), and anything somewhere else is reached with use(\"address\", values). No other dotted call.
5. Each key or token it needs is read with setting\(\"key\"), never written in the code or taken as a value, and a missing one fails in plain words saying where to set it. A setting that needs a title, a default or to be kept secret is in its manifest.
6. The web goes through request, or use(...) for a plain request, at real addresses of real services, with their real fields and answers. Nothing made up.
7. It checks an answer is ok before reading it, and its errors say in plain words what went wrong and what to do.
8. It does all of what was asked, and nothing more, small and readable.
9. Plain words everywhere, and no em dash anywhere.
Answer with one JSON object and nothing else: \{\"problems\": [\"each problem, short and exact, naming the function and what to change\"]\}, or \{\"problems\": []\} when it is right. Never list a matter of taste."
}

// What a model needs to write a lingo that loads the first time. For writer's own use.
fn guide() {
  return "A lingo is a small program in Lingo that runs on a person's computer, in its lingos folder: a folder <name> holding its code, <name>.ls, and its manifest, openapi.json, when it needs one. Every fn in it is a function others can call: at its route, as POST /<name>/<function> with its values as a JSON record by name, or by its dotted name, <name>.<function>(...), from another lingo in the same folder. The comments right above a fn say what it does, and the first comment lines of the file what the lingo is. Write lingos that load the first time, do exactly what was asked, and read plainly.

THE MANIFEST
openapi.json is a JSON object holding x-lingo, an object, and every part of that is optional. Its routes, what each takes and what each needs are made from the code, so never write them:
- settings: a list, each a name, or \{\"name\": \"key\", \"title\": \"API key\", \"about\": \"where to get it\", \"default\": ..., \"required\": true, \"secret\": true\}. A form is made from them, and the code reads each with setting(\"key\"). A setting the code reads that isn't listed is a setting too, by its name alone.
- route: where it answers on its computer, like \"/weather\". Without it, /<name>.
Like \{\"x-lingo\": \{\"settings\": [\{\"name\": \"key\", \"title\": \"API key\", \"secret\": true\}]\}\}. Write a manifest only when a setting needs a title, a default or to be kept secret, or the lingo needs a route.

SERVE
A lingo with fn serve(request) answers every request to its route itself, like a web page. request is \{method, path, query, headers, body, json\}: path the part after its route, starting with /, query a record, headers a record by lower case name, body text, and json the body read as JSON, or nothing. serve gives text, any other value, sent as JSON, or \{status, headers, body\} for a status of its own.

LINGO
- Whitespace never matters. // starts a comment. Names are letters, digits and _, not starting with a digit, and case matters.
- Keywords, never names: if else while for in break continue fn return tool try catch fail and or not true false nothing. null is nothing. After a dot they are fine: r.in.
- The file holds only fn blocks: no steps outside them and no names set outside a function. A value used in many places is a small fn that returns it.
- fn name(a, b=1, c=nothing) \{...\}. A function is called by its name with its values in order, from before or after where it is written. A default is worked out when its value is missing or nothing. Functions are not values: they can't be kept in a name, passed or returned, and there are no lambdas.
- Values: nothing, true, false, numbers (3, 0.5), text in double quotes, lists [1, \"a\"] counted from 0, and records \{name: \"Ann\", \"two words\": 1\}. Only nothing and false count as false: 0, \"\" and [] are true.
- In text, \\n is a new line, \\t a tab, \\\" a quote and \\\\ a backslash. \{expression\} puts a value in text: \"Hi \{name\}, \{n + 1\} new\". So a brace meant as itself is written \\\{ or \\\}, JSON in text too: \"\\\{\\\"a\\\": 1\\\}\". The expression inside \{\} can't hold quotes: put that value in a name first.
- Setting a name copies the value: a = [1]; b = a; b += [2] leaves a as [1]. Names belong to the function they are set in, blocks make no new ones, and a name never set reads as nothing.
- Statements: x = e, x.part = e, x[i] = e, x += e (and -= *= /=), list += [item] to add one, if c \{...\} else if c \{...\} else \{...\}, while c \{...\}, for x in e \{...\}, for k, v in e \{...\}, break, continue, return e, return, fail e, try \{...\} catch e \{...\}. Blocks always have braces, and conditions need no parentheses. A step on its own must be a call: x + 1 alone is an error.
- for goes through a list's items (k, v: the position and the item), a record's part names (k, v: the name and its value), text's characters, a..b's whole numbers from a to b, both included, and nothing not at all.
- Operators, loosest first: c ? a : b, then a ?? b (b when a is nothing), or, and, not, then == != < > <= >= in (one at a time: never a < b < c), a..b, + -, * / %, unary -, and last x.part, x[i] and calls. + adds numbers, joins lists, and joins as text when either side is text. a in b: an item of a list, part of a text, or a part name of a record. == compares lists and records by what is in them.
- Reading never fails: a missing part, an item out of range, or a part of nothing is nothing, so r.a.b is nothing when r has no a, and -1 is the last item. Setting a part needs a list or record there: r = \{\} first, then r.a = 1.
- Errors: fail \"plain words\" stops with that message. try \{...\} catch e \{...\} catches it, e being the message, and catches a phrase that failed or a value of the wrong kind too.
- What every computer has, each phrase written with its group:
  std: length(x), keys(r), slice(x, from, to=nothing) (list or text, to not included, below 0 counts from the end), number(x) (from text like \"$1,204.50\", or nothing), text(x), to_json(x), from_json(text) (fails on bad JSON), bytes(text), from_bytes(list).
  compute: now() (seconds since 1970, UTC), wait(seconds) (at most 60), log(x).
  store: get(key) (text, or nothing), put(key, value) (kept as text), keys(prefix=\"\"), delete(key): small things a lingo remembers.
  http: send(method, url, headers=nothing, body=nothing, timeout=30) -> \{status, headers, text\}. Use request instead.
- Lingos in the same folder, called by their dotted names: request for web requests with headers, a query, a timeout and tries again (request.get(address, options), request.post(address, body, options)), json, text (split, join, replace, trim, lower, upper and more), list (sort, reverse), math (round, min, max), url, encode, time (dates and times), and agent, an AI model on the person's own key: agent.answer(prompt) gives one answer as text. Call only functions you are sure of: a call to one that doesn't exist doesn't load.
- use(\"address\", values) reaches anything somewhere else and gives back its answer: a lingo on another computer, like use(\"laptop.brandon/text/slug\", \{text: \"Hi there\"\}), an MCP tool, like use(\"https://mcp.example.com/mcp#search\", \{q: \"x\"\}), or a web address, a GET with no values and a POST of the values as JSON with them, giving the answer read as JSON when it is JSON, else its text, and failing in plain words on any status but 2xx.
- setting(\"key\") is the person's own setting of that name for this lingo, or nothing when it isn't set. setting(\"token\", \"github\") reads another lingo's.
- lingo(\"what to do, in words\", \{city: \"Paris\"\}, \"what should come back\") has Onsay find a function that does it, run it, and give back its answer. Use it only when no lingo here does the job.

THE WEB
Reach it through request, or with use(...) for one plain request. Use only real services you know well, at addresses, with values and answers you are sure of, and those that need no key when one will do, like Open-Meteo for weather. When a service needs a key, read it with setting(\"key\"), and a missing one fails in plain words: fail \"Weather needs your API key: set key in weather's settings, in the Onsay app or with the command line's set. Get one at ...\". Never make up a service, an address or a field: when you aren't sure of one, say so in notes.

THE FILE
- name: lower case letters, digits and _, starting with a letter, like weather or stock_price. The file is <name>.ls.
- Its first lines are comments saying what the lingo is and does, in a sentence or two, in the words people would search with, starting with its file's name: // weather.ls - The weather now anywhere... Then an empty //, a numbered list of its functions, each with its values and what it gives, and a blank line.
- Right above each fn, its comment lines, starting with its number: what it gives or does, when to use it, and when not to, in plain words a search can match, short and concrete, and what its values are. A helper's comment ends: For weather's own use.
- A key or token is always a setting, never in the code and never a function's value. A default is applied in the code, setting(\"units\") ?? \"metric\", or in the manifest.

HOW IT IS WRITTEN
- Small and readable: the fewest functions that do the job well, each short. The functions it is for first, then its helpers.
- Comments only where the code can't say it, but always the one right above each fn.
- Each error says what went wrong and what to do next, in plain words: fail \"There's no city called \{city\}: check its spelling\". Check an answer is ok before reading it.
- Plain words everywhere: comments, notes and errors. Never an em dash: use a comma, a colon or a full stop."
}
