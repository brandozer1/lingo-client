// browser - A web browser for lingos and agents: opens pages, reads them, clicks, types, runs scripts and takes
// screenshots, in LibreWolf, a fully open source Firefox with no telemetry, out of sight unless its show setting is on.
//
// 1. open(url, tab=nothing)               -> {tab, title, url}: a page opened, in the open tab, the tab given, or a new
//                                            one for "new"
// 2. read(tab=nothing)                    -> {title, url, text, links, fields}: what the page shows, its links and its
//                                            form's fields by their labels
// 3. click(words, tab=nothing)            -> {clicked, title, url}: the link or button showing those words clicked
// 4. type(text, into=nothing, tab=nothing) -> {field, title, url}: text typed into the field with that label, else the
//                                            one with the focus. A new line presses Enter
// 5. run(script, tab=nothing)             -> any: JavaScript run in the page, and its value
// 6. screenshot(tab=nothing)              -> {path}: a picture of the page, saved as a PNG file
// 7. tabs()                               -> [{tab, title, url}]: every tab open
// 8. close(tab=nothing)                   -> nothing: a tab closed, the open one when none is given
// tab is a tab's number, from 1 in the browser's order, as open and tabs give it, and the open one, the last one used,
// when left out.
//
// On first use it fetches LibreWolf for this computer, Windows or Linux, from LibreWolf's own releases
// (dl.librewolf.net), one version pinned in release() and checked against its SHA-256, into a folder of its own with its
// own profile, so it never touches the person's own browser. It drives it over WebDriver BiDi, the W3C standard built
// into Firefox, through the core's socket phrases: each call starts it when it isn't running, opens a socket to it,
// begins a session, does its work and ends the session, so later calls find the same browser and its tabs. As each call
// ends, the open tab is drawn on a screen, Browser, beside the to-do it works for, so the person can watch. Its settings,
// in openapi.json beside it: show, to see its window, and folder, where it keeps LibreWolf.

////////// 1. THE BROWSER //////////

// 1. Opens a web page in a browser and gives its title and address once it has loaded: to look something up, visit a
// site, or start a task on a web page before reading, clicking or typing there. url is any web address, https:// left
// off when it likes. tab is the number of the tab to open it in, "new" for a new one, else the open one.
fn open(url, tab=nothing) {
  b = begin()
  try {
    t = tabbed(b, tab)
    to = std.text(url)
    if not ("://" in to) and not text.starts_with(to, "about:") {to = "https://" + to}
    asked(b.socket, "browsingContext.navigate", {context: t.id, url: to, wait: "complete"})
    now = page(b, t.id, "return JSON.stringify(\{title: document.title, url: location.href\});")
    out = {tab: t.number, title: now.title, url: now.url}
  } catch e {
    finish(b)
    fail e
  }
  finish(b)
  return out
}

// 2. What the web page open in the browser shows: its title, address and words, its links as {text, url}, and its form's
// fields as {label, type, value}, a password never given. Use it to read a page that open, click or type reached, before
// deciding what to do next on it.
fn read(tab=nothing) {
  b = begin()
  try {
    t = tabbed(b, tab)
    out = page(b, t.id, text.join([
      "const value = (f) => f.type == 'checkbox' || f.type == 'radio' ? f.checked : f.type == 'password' ? '' : f.value;",
      "return JSON.stringify(\{",
      "  title: document.title, url: location.href, text: document.body ? document.body.innerText : '',",
      "  links: [...document.links].filter(seen).map((a) => (\{text: said(a), url: a.href\})).filter((l) => l.text),",
      "  fields: [...document.querySelectorAll('input:not([type=hidden]), textarea, select')].filter(seen).map((f) => (\{label: named(f), type: f.type, value: value(f)\}))",
      "\});"
    ], "\n"))
  } catch e {
    finish(b)
    fail e
  }
  finish(b)
  return out
}

// 3. Clicks the link or button on the web page that shows those words, like Sign in or Next, the way a person would, and
// gives what it clicked and the page's title and address after. Fails when nothing on the page shows them.
fn click(words, tab=nothing) {
  b = begin()
  try {
    t = tabbed(b, tab)
    spot = page(b, t.id, text.join([
      "const want = " + std.to_json(text.lower(text.trim(std.text(words)))) + ";",
      "const all = [...document.querySelectorAll('a, button, input[type=submit], input[type=button], input[type=checkbox], input[type=radio], [role=button], [role=link], [role=tab], [role=menuitem], [role=checkbox], summary, label, [onclick]')].filter((e) => seen(e) && said(e).toLowerCase().includes(want));",
      "all.sort((x, y) => said(x).length - said(y).length);",
      "const e = all[0];",
      "if (!e) return JSON.stringify(null);",
      "e.scrollIntoView(\{block: 'center', inline: 'center'\});",
      "const r = e.getBoundingClientRect();",
      "return JSON.stringify(\{x: Math.round(r.left + r.width / 2), y: Math.round(r.top + r.height / 2), words: said(e)\});"
    ], "\n"))
    if spot == nothing {fail "Nothing on the page shows the words {words}"}
    mouse = [{type: "pointerMove", x: spot.x, y: spot.y}, {type: "pointerDown", button: 0}, {type: "pointerUp", button: 0}]
    asked(b.socket, "input.performActions", {context: t.id, actions: [{type: "pointer", id: "mouse", parameters: {pointerType: "mouse"}, actions: mouse}]})
    now = settled(b, t.id)
    out = {clicked: spot.words, title: now.title, url: now.url}
  } catch e {
    finish(b)
    fail e
  }
  finish(b)
  return out
}

// 4. Types text into a field on the web page the way a person would, key by key: the field whose label, placeholder or
// name is into, its old text replaced, else the field that has the focus. A new line in text presses Enter, to send a
// search or a form. Gives the field's label and the page's title and address after.
fn type(text, into=nothing, tab=nothing) {
  b = begin()
  try {
    t = tabbed(b, tab)
    found = "const f = document.activeElement;\nreturn JSON.stringify(f && f != document.body ? named(f) || f.tagName.toLowerCase() : null);"
    if into != nothing {
      found = text.join([
        "const want = " + std.to_json(text.lower(text.trim(std.text(into)))) + ";",
        "const words = (f) => [named(f), f.placeholder, f.name, f.getAttribute('aria-label')].filter(Boolean).map((w) => w.toLowerCase().trim());",
        "const all = [...document.querySelectorAll('input:not([type=hidden]), textarea, select, [contenteditable]')].filter(seen);",
        "const f = all.find((x) => words(x).includes(want)) || all.find((x) => words(x).some((w) => w.includes(want)));",
        "if (!f) return JSON.stringify(null);",
        "f.scrollIntoView(\{block: 'center'\});",
        "f.focus();",
        "if (f.select) f.select();",
        "return JSON.stringify(named(f) || f.type);"
      ], "\n")
    }
    field = page(b, t.id, found)
    if field == nothing {fail into == nothing ? "No field has the focus. Say which, as into" : "There's no field called {into} on the page"}
    enter = std.from_bytes([238, 128, 135])
    keys = []
    for c in std.text(text) {
      k = c == "\n" ? enter : c
      keys += [{type: "keyDown", value: k}, {type: "keyUp", value: k}]
    }
    asked(b.socket, "input.performActions", {context: t.id, actions: [{type: "key", id: "keys", actions: keys}]})
    now = settled(b, t.id)
    out = {field: field, title: now.title, url: now.url}
  } catch e {
    finish(b)
    fail e
  }
  finish(b)
  return out
}

// 5. Runs JavaScript in the web page and gives its value, a promise's once it settles: for what reading, clicking and
// typing can't do, like pulling a table out of a page or scrolling it. script is run as a script, its last statement
// its value.
fn run(script, tab=nothing) {
  b = begin()
  try {
    t = tabbed(b, tab)
    got = asked(b.socket, "script.evaluate", {expression: std.text(script), target: {context: t.id}, awaitPromise: true})
    if got.type == "exception" {fail "The script failed: {got.exceptionDetails.text}"}
    out = plain(got.result)
  } catch e {
    finish(b)
    fail e
  }
  finish(b)
  return out
}

// 6. A picture of the web page as the browser shows it, saved as a PNG file, and its path: to see a page's layout, or
// to show it to an AI that takes pictures.
fn screenshot(tab=nothing) {
  b = begin()
  try {
    t = tabbed(b, tab)
    data = asked(b.socket, "browsingContext.captureScreenshot", {context: t.id}).data
  } catch e {
    finish(b)
    fail e
  }
  finish(b)
  p = b.place
  kept = p.folder + "/screenshot.txt"
  path = p.folder + "/screenshots/screenshot-" + std.text(math.round(compute.now() * 1000)) + ".png"
  files.write(kept, data)
  if p.windows {
    done = commands.run("[IO.File]::WriteAllBytes(" + quoted(path, true) + ", [Convert]::FromBase64String([IO.File]::ReadAllText(" + quoted(kept, true) + ")))")
  } else {
    done = commands.run("base64 -d " + quoted(kept, false) + " > " + quoted(path, false))
  }
  if done.code != 0 {fail "The screenshot couldn't be saved: {done.out}"}
  return {path: path}
}

// 7. Every tab open in the browser, by its number, with its title and address, to pick one for the other functions. A
// page the browser keeps to itself, like about:home, has no title here.
fn tabs() {
  b = begin()
  try {
    out = []
    for i, c in asked(b.socket, "browsingContext.getTree", {maxDepth: 0}).contexts {
      title = ""
      try {title = page(b, c.context, "return JSON.stringify(document.title);")} catch e {}
      out += [{tab: i + 1, title: title, url: c.url}]
    }
  } catch e {
    finish(b)
    fail e
  }
  finish(b)
  return out
}

// 8. Closes a tab in the browser, the open one when none is given, and the first becomes the open one. The last tab left
// is emptied instead.
fn close(tab=nothing) {
  b = begin()
  try {
    t = tabbed(b, tab)
    if std.length(asked(b.socket, "browsingContext.getTree", {maxDepth: 0}).contexts) > 1 {
      asked(b.socket, "browsingContext.close", {context: t.id})
    } else {
      asked(b.socket, "browsingContext.navigate", {context: t.id, url: "about:blank", wait: "complete"})
    }
    store.delete("browser/tab")
  } catch e {
    finish(b)
    fail e
  }
  finish(b)
}

////////// HELPERS //////////

// For browser's own use: the LibreWolf it fetches, by its version, where each computer's is, and its SHA-256, from
// LibreWolf's releases (codeberg.org/librewolf/bsys6/releases). A new version is a new entry here.
fn release() {
  return {
    version: "159.0.1-1",
    from: "https://dl.librewolf.net/librewolf/159.0.1-1/librewolf-159.0.1-1-",
    files: {
      "windows-x86_64": {file: "windows-x86_64-package.zip", sha256: "6b3f1570007649d8b194a06323c60c1036aa86e212b1ac33b22a3148e213ea2d"},
      "windows-arm64": {file: "windows-arm64-package.zip", sha256: "96374c2f36fc25c5390e77cfc61f994055a52d05d51ac57334ce989c29902d31"},
      "linux-x86_64": {file: "linux-x86_64-package.tar.xz", sha256: "692524efc43922eaa3f1ed39a91e6483ad9d794996787ebbb1be3c9c9ca46f87"},
      "linux-arm64": {file: "linux-arm64-package.tar.xz", sha256: "7a6937f08aa6bc7f7a8fbcdc682b89906cce392aaab2193660c4414e4fc84e98"}
    }
  }
}

// For browser's own use: where LibreWolf is on this computer, {version, windows, folder, exe, profile}, kept in store,
// fetched and checked first when it isn't there, is another version, or the folder setting moved it.
fn installed() {
  r = release()
  want = text.replace(std.text(setting("folder") ?? ""), "\\", "/")
  kept = store.get("browser/place")
  p = kept == nothing ? nothing : std.from_json(kept)
  if p != nothing and p.version == r.version and files.exists(p.exe) and (want == "" or want == p.folder) {return p}
  windows = files.exists("C:/Windows")
  if windows {
    said = commands.run("[Console]::Out.Write($env:PROCESSOR_ARCHITECTURE + '|' + (Join-Path $env:LOCALAPPDATA 'lingo-browser'))").out
  } else {
    said = commands.run("printf '%s|%s' \"$(uname -sm)\" \"$HOME/.local/share/lingo-browser\"").out
  }
  parts = text.split(text.trim(said), "|")
  if not windows and not text.starts_with(parts[0], "Linux") {fail "LibreWolf can be fetched for Windows and Linux so far, not {parts[0]}"}
  arch = "ARM64" in parts[0] or "aarch64" in parts[0] ? "arm64" : "x86_64"
  one = r.files[(windows ? "windows-" : "linux-") + arch]
  folder = want == "" ? text.replace(parts[1], "\\", "/") : want
  dir = folder + "/" + r.version
  p = {version: r.version, windows: windows, folder: folder, exe: dir + "/librewolf/librewolf" + (windows ? ".exe" : ""), profile: folder + "/profile"}
  if windows {
    done = commands.run(text.join([
      "$ErrorActionPreference='Stop'",
      "New-Item -ItemType Directory -Force " + quoted(dir, true) + ", " + quoted(p.profile, true) + ", " + quoted(folder + "/screenshots", true) + " | Out-Null",
      "$zip=" + quoted(dir + "/librewolf.zip", true),
      "Invoke-WebRequest -UseBasicParsing -Uri " + quoted(r.from + one.file, true) + " -OutFile $zip",
      "if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne '" + one.sha256 + "') \{ Remove-Item $zip; throw 'The download did not match its checksum' \}",
      "& (Join-Path $env:SystemRoot 'System32/tar.exe') -xf $zip -C " + quoted(dir, true),
      "Remove-Item $zip"
    ], "\n"))
  } else {
    done = commands.run(text.join([
      "set -e",
      "mkdir -p " + quoted(dir, false) + " " + quoted(p.profile, false) + " " + quoted(folder + "/screenshots", false),
      "f=" + quoted(dir + "/librewolf.tar.xz", false),
      "curl -fsSL -o \"$f\" " + quoted(r.from + one.file, false),
      "if [ \"$(sha256sum \"$f\" | cut -d' ' -f1)\" != '" + one.sha256 + "' ]; then rm -f \"$f\"; echo 'The download did not match its checksum'; exit 1; fi",
      "tar -xJf \"$f\" -C " + quoted(dir, false),
      "rm -f \"$f\""
    ], "\n"))
  }
  if done.code != 0 {fail "LibreWolf couldn't be fetched: {done.out}"}
  store.put("browser/place", std.to_json(p))
  return p
}

// For browser's own use: a socket to the running browser with a session begun on it, as {socket, place}, LibreWolf
// fetched and started first when it isn't running.
fn begin() {
  p = installed()
  s = reached(p)
  if s != nothing {return {socket: s, place: p}}
  shown = setting("show")
  flags = "--no-remote --remote-debugging-port 0 --profile " + (p.windows ? "\"" + p.profile + "\"" : quoted(p.profile, false))
  if not (shown == true or text.lower(std.text(shown ?? "")) in ["true", "on", "yes", "1"]) {flags = "--headless " + flags}
  ready = p.profile + "/WebDriverBiDiServer.json"
  if p.windows {
    done = commands.run("Remove-Item -LiteralPath " + quoted(ready, true) + " -ErrorAction SilentlyContinue\n[Console]::Out.Write((Start-Process -FilePath " + quoted(p.exe, true) + " -ArgumentList " + quoted(flags, true) + " -PassThru).Id)")
  } else {
    done = commands.run("rm -f " + quoted(ready, false) + "\nnohup " + quoted(p.exe, false) + " " + flags + " >/dev/null 2>&1 & echo $!")
  }
  if done.code != 0 {fail "LibreWolf couldn't start: {done.out}"}
  store.put("browser/pid", text.trim(done.out))
  for turn in 1..60 {
    compute.wait(0.5)
    s = reached(p)
    if s != nothing {return {socket: s, place: p}}
  }
  fail "LibreWolf didn't start in 30 seconds. Try again, or delete {p.folder} so it's fetched again"
}

// For browser's own use: a socket to the browser with a session begun, or nothing when it isn't running. One with a
// session left from a call that was stopped can't begin another, so it is ended, to be started again.
fn reached(p) {
  ready = p.profile + "/WebDriverBiDiServer.json"
  if not files.exists(ready) {return nothing}
  try {
    at = std.from_json(files.read(ready))
    s = socket.open("ws://{at.ws_host}:{at.ws_port}/session")
  } catch e {
    return nothing
  }
  try {
    asked(s, "session.new", {capabilities: {}})
  } catch e {
    socket.close(s)
    if not ("Maximum number of active sessions" in e) {fail e}
    pid = std.number(store.get("browser/pid") ?? "")
    if pid != nothing {
      try {commands.stop(pid)} catch e {}
    }
    compute.wait(1)
    return nothing
  }
  return s
}

// For browser's own use: the open tab drawn on the Browser screen, then the session ended, so the next call can begin
// one, and the socket closed. A computer without screens draws nothing.
fn finish(b) {
  try {
    tabs = asked(b.socket, "browsingContext.getTree", {maxDepth: 0}).contexts
    n = std.number(store.get("browser/tab") ?? 1) ?? 1
    if n < 1 or n > std.length(tabs) {n = 1}
    picture = asked(b.socket, "browsingContext.captureScreenshot", {context: tabs[n - 1].context}).data
    screen.show(screen.open("Browser"), picture)
  } catch e {}
  try {asked(b.socket, "session.end")} catch e {}
  socket.close(b.socket)
}

// For browser's own use: one WebDriver BiDi command and its result, or its error as a failure.
fn asked(s, method, params={}) {
  socket.send(s, std.to_json({id: 1, method: method, params: params}))
  while true {
    got = socket.receive(s, 60)
    if got == nothing {fail "LibreWolf didn't answer {method} within a minute"}
    m = std.from_json(got)
    if m.type == "success" {return m.result}
    if m.type == "error" {fail "{method}: {m.message}"}
  }
}

// For browser's own use: the tab to use, {id, number}, which becomes the open one: number tab, a new one for "new", else
// the open one, else the first, else a new one. Tabs are numbered from 1 in the browser's order, since the browser gives
// them new ids with every session.
fn tabbed(b, tab) {
  made = tab == "new" ? asked(b.socket, "browsingContext.create", {type: "tab"}).context : nothing
  ids = []
  for c in asked(b.socket, "browsingContext.getTree", {maxDepth: 0}).contexts {ids += [c.context]}
  if std.length(ids) == 0 {
    made = asked(b.socket, "browsingContext.create", {type: "tab"}).context
    ids = [made]
  }
  n = made != nothing ? list.index_of(ids, made) + 1 : std.number(tab ?? store.get("browser/tab") ?? 1)
  if n == nothing or n < 1 or n > std.length(ids) {
    if tab != nothing {fail "There's no tab {tab}. tabs gives the tabs open"}
    n = 1
  }
  store.put("browser/tab", n)
  return {id: ids[n - 1], number: n}
}

// For browser's own use: JavaScript run in a tab and its answer, body the inside of a function that gives
// JSON.stringify of it, with seen(element), whether it shows, said(element), the words it shows, and named(field), its
// label, there to use.
fn page(b, t, body) {
  js = text.join([
    "(() => \{",
    "const seen = (e) => \{ const r = e.getBoundingClientRect(); return r.width > 0 && r.height > 0; \};",
    "const said = (e) => (e.innerText || e.value || e.getAttribute('aria-label') || e.title || '').trim().replace(/\\s+/g, ' ');",
    "const named = (f) => ((f.labels && f.labels[0] && f.labels[0].innerText) || f.getAttribute('aria-label') || f.placeholder || f.name || f.id || '').trim();",
    body,
    "\})()"
  ], "\n")
  got = asked(b.socket, "script.evaluate", {expression: js, target: {context: t}, awaitPromise: true})
  if got.type == "exception" {fail "The page's script failed: {got.exceptionDetails.text}"}
  return std.from_json(got.result.value)
}

// For browser's own use: the tab's title and address once its page has loaded, after a click or a key that may have
// begun loading another, waiting at most 10 seconds.
fn settled(b, t) {
  compute.wait(0.5)
  for turn in 1..20 {
    try {now = page(b, t, "return JSON.stringify(\{state: document.readyState, title: document.title, url: location.href\});")} catch e {}
    if now != nothing and now.state == "complete" {break}
    compute.wait(0.5)
  }
  return {title: now.title, url: now.url}
}

// For browser's own use: a value from the page, as WebDriver BiDi writes it, as a plain value.
fn plain(v) {
  if v.type == "array" or v.type == "set" or v.type == "nodelist" or v.type == "htmlcollection" {
    out = []
    for x in v.value ?? [] {out += [plain(x)]}
    return out
  }
  if v.type == "object" or v.type == "map" {
    out = {}
    for pair in v.value ?? [] {out[json.kind(pair[0]) == "text" ? pair[0] : std.to_json(plain(pair[0]))] = plain(pair[1])}
    return out
  }
  return v.value
}

// For browser's own use: text as one word of this computer's shell, in single quotes, PowerShell's on Windows, else sh's.
fn quoted(words, windows) {
  if windows {return "'" + text.replace(std.text(words), "'", "''") + "'"}
  return "'" + text.replace(std.text(words), "'", "'\\''") + "'"
}
