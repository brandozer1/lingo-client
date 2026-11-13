"A web browser for lingos and agents: opens pages, reads them, clicks, types, runs scripts and takes screenshots, in LibreWolf, a fully open source Firefox with no telemetry, its window shown while the person watches the work, else out of sight with its tabs drawn on Onsay's screens."
// 1. open(url, tab=nothing)               -> {tab, title, url}: a page opened, in the open tab, the tab given, or a new
//                                            one for "new"
// 2. read(tab=nothing)                    -> {title, url, text, links, fields}: what the page shows, its links and its
//                                            form's fields by their labels
// 3. click(words, tab=nothing)            -> {clicked, title, url}: the link or button showing those words clicked
// 4. type(text, into=nothing, tab=nothing) -> {field, title, url}: text typed into the field with that label, else the
//                                            one with the focus. A new line presses Enter
// 5. run(script, tab=nothing)             -> any: JavaScript run in the page, and its value, return allowed
// 6. screenshot(tab=nothing)              -> {path}: a picture of the page, saved as a PNG file
// 7. tabs()                               -> [{tab, title, url}]: every tab open
// 8. close(tab=nothing)                   -> nothing: a tab closed, the open one when none is given
// 9. render(html, tab=nothing)            -> {tab, title}: a page of HTML and CSS of the lingo's own laid out, in the
//                                            open tab, the tab given, or a new one for "new"
// 10. find(selector, tab=nothing)         -> [{tag, text, attributes, box}]: the page's elements matching a CSS selector
// 11. shown(on)                           -> nothing: what Onsay calls the moment the person turns Use desktop on or
//                                            off, so the browser switches then and there
// tab is a tab's number, from 1 in the browser's order, as open and tabs give it, and the open one, the last one used,
// when left out.
//
// On first use it fetches LibreWolf for this computer, Windows or Linux, from LibreWolf's own releases
// (dl.librewolf.net), one version pinned in _release() and checked against its SHA-256, into a folder of its own with its
// own profile, so it never touches the person's own browser. It drives it over WebDriver BiDi, the W3C standard built
// into Firefox, through the lingo client's socket calls: each call starts it when it isn't running, opens a socket to it,
// begins a session, does its work and ends the session, so later calls find the same browser and its tabs. Out of
// sight, as each call ends, every tab shows on a screen of its own, Browser tab 1 and on, beside the to-do it works for,
// so the person can watch: the picture LibreWolf itself drew of what is in view, exactly as a browser shows it. While the
// person watches the to-do it works for, Use desktop, which the app's chat lingo answers (chat.showing), or its show
// setting is on, its window comes in front instead, and its screens are closed. It is always one real LibreWolf window,
// moved off the edge of the screen while out of sight, so switching only moves it: the same tabs, pages and sign ins
// carry on, nothing reloaded. The app calls shown(on) the moment the person toggles, so it moves then, not on its next
// call. Its settings: show, on to always see its window, and
// folder, where it keeps LibreWolf, else lingo-browser in AppData\Local on Windows and ~/.local/share on Linux. A download that stops partway carries on from there next time.

////////// 1. THE BROWSER //////////

fn open(url: url "any web address, https:// may be left off", tab: whole|text? "the number of the tab to open it in, new for a new one, else the open one" = nothing) -> {tab: whole, title: text, url: url} "Opens a web page in a browser and gives its tab, title and address once it has loaded: to look something up, visit a site, or start a task on a web page before reading, clicking or typing there." {
  b = _begin()
  try {
    t = _tabbed(b, tab)
    to = std.text(url)
    if not ("://" in to) and not text.starts_with(to, "about:") {to = "https://" + to}
    _asked(b.socket, "browsingContext.navigate", {context: t.id, url: to, wait: "complete"})
    now = _page(b, t.id, "return JSON.stringify(\{title: document.title, url: location.href\});")
    out = {tab: t.number, title: now.title, url: now.url}
  } catch e {
    _finish(b)
    fail e
  }
  _finish(b)
  return out
}

fn read(tab: whole? "the tab's number, else the open one" = nothing) -> {title: text, url: url, text: text, links: [{text: text, url: url}], fields: [{label: text, type: text, value: any}]} "What the web page open in the browser shows: its title, address and words, its links, and its form's fields by their labels, a password never given. Use it to read a page that open, click or type reached, before deciding what to do next on it." {
  b = _begin()
  try {
    t = _tabbed(b, tab)
    out = _page(b, t.id, text.join([
      "const value = (f) => f.type == 'checkbox' || f.type == 'radio' ? f.checked : f.type == 'password' ? '' : f.value;",
      "return JSON.stringify(\{",
      "  title: document.title, url: location.href, text: document.body ? document.body.innerText : '',",
      "  links: [...document.links].filter(seen).map((a) => (\{text: said(a), url: a.href\})).filter((l) => l.text),",
      "  fields: [...document.querySelectorAll('input:not([type=hidden]), textarea, select')].filter(seen).map((f) => (\{label: named(f), type: f.type, value: value(f)\}))",
      "\});"
    ], "\n"))
  } catch e {
    _finish(b)
    fail e
  }
  _finish(b)
  return out
}

fn find(selector: text "a CSS selector, like .board .piece, button or [aria-label]", tab: whole? "the tab's number, else the open one" = nothing) -> [{tag: text, text: text, attributes: record, box: {x: number, y: number, w: number, h: number}}] "The elements of the web page that match a CSS selector, at most 200: each its tag, its words, its attributes, like class, id, href, aria-label and data- ones, and where it shows on the page. To read how a page is built when its words alone don't say it, like the pieces on a game board." {
  b = _begin()
  try {
    t = _tabbed(b, tab)
    out = _page(b, t.id, text.join([
      "const all = [...document.querySelectorAll(" + std.to_json(std.text(selector)) + ")].slice(0, 200);",
      "return JSON.stringify(all.map((e) => \{",
      "  const r = e.getBoundingClientRect();",
      "  const attributes = \{\};",
      "  for (const a of e.attributes) attributes[a.name] = a.value.slice(0, 300);",
      "  return \{tag: e.tagName.toLowerCase(), text: said(e).slice(0, 300), attributes, box: \{x: Math.round(r.x), y: Math.round(r.y), w: Math.round(r.width), h: Math.round(r.height)\}\};",
      "\}));"
    ], "\n"))
  } catch e {
    _finish(b)
    fail e
  }
  _finish(b)
  return out
}

fn click(words: text "the words the link or button shows, like Sign in or Next", tab: whole? "the tab's number, else the open one" = nothing) -> {clicked: text, title: text, url: url} "Clicks the link or button on the web page that shows those words, the way a person would, and gives what it clicked and the page's title and address after. Fails when nothing on the page shows them." {
  b = _begin()
  try {
    t = _tabbed(b, tab)
    spot = _page(b, t.id, text.join([
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
    _asked(b.socket, "input.performActions", {context: t.id, actions: [{type: "pointer", id: "mouse", parameters: {pointerType: "mouse"}, actions: mouse}]})
    now = _settled(b, t.id)
    out = {clicked: spot.words, title: now.title, url: now.url}
  } catch e {
    _finish(b)
    fail e
  }
  _finish(b)
  return out
}

fn type(text: text "what to type, a new line pressing Enter to send a search or a form", into: text? "the field's label, placeholder or name, else the field that has the focus" = nothing, tab: whole? "the tab's number, else the open one" = nothing) -> {field: text, title: text, url: url} "Types text into a field on the web page the way a person would, key by key, its old text replaced. Gives the field's label and the page's title and address after." {
  b = _begin()
  try {
    t = _tabbed(b, tab)
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
    field = _page(b, t.id, found)
    if field == nothing {fail into == nothing ? "No field has the focus. Say which, as into" : "There's no field called {into} on the page"}
    enter = std.from_bytes([238, 128, 135])
    keys = []
    for c in std.text(text) {
      k = c == "\n" ? enter : c
      keys += [{type: "keyDown", value: k}, {type: "keyUp", value: k}]
    }
    _asked(b.socket, "input.performActions", {context: t.id, actions: [{type: "key", id: "keys", actions: keys}]})
    now = _settled(b, t.id)
    out = {field: field, title: now.title, url: now.url}
  } catch e {
    _finish(b)
    fail e
  }
  _finish(b)
  return out
}

fn run(script: text "JavaScript run in the page: an expression, or statements with return for the value, await allowed", tab: whole? "the tab's number, else the open one" = nothing) -> any "Runs JavaScript in the web page and gives its value, a promise's once it settles: for what reading, clicking and typing can't do, like pulling data out of a page, clicking by selector or scrolling it." {
  b = _begin()
  try {
    t = _tabbed(b, tab)
    js = std.text(script)
    got = _asked(b.socket, "script.evaluate", {expression: js, target: {context: t.id}, awaitPromise: true})
    if got.type == "exception" and "return" in js {
      got = _asked(b.socket, "script.evaluate", {expression: "(async () => \{\n" + js + "\n\})()", target: {context: t.id}, awaitPromise: true})
    }
    if got.type == "exception" {fail "The script failed: {got.exceptionDetails.text}"}
    out = _plain(got.result)
  } catch e {
    _finish(b)
    fail e
  }
  _finish(b)
  return out
}

fn screenshot(tab: whole? "the tab's number, else the open one" = nothing) -> {path: path} "A picture of the web page as the browser shows it, saved as a PNG file, and its path: to see a page's layout, or to show it to an AI that takes pictures." {
  b = _begin()
  try {
    t = _tabbed(b, tab)
    data = _asked(b.socket, "browsingContext.captureScreenshot", {context: t.id}).data
  } catch e {
    _finish(b)
    fail e
  }
  _finish(b)
  p = b.place
  kept = p.folder + "/screenshot.txt"
  path = p.folder + "/screenshots/screenshot-" + std.text(math.round(compute.now() * 1000)) + ".png"
  files.write(kept, data)
  if p.windows {
    done = commands.run("[IO.File]::WriteAllBytes(" + _quoted(path, true) + ", [Convert]::FromBase64String([IO.File]::ReadAllText(" + _quoted(kept, true) + ")))")
  } else {
    done = commands.run("base64 -d " + _quoted(kept, false) + " > " + _quoted(path, false))
  }
  if done.code != 0 {fail "The screenshot couldn't be saved: {done.out}"}
  return {path: path}
}

fn tabs() -> [{tab: whole, title: text, url: url}] "Every tab open in the browser, by its number, with its title and address, to pick one for the other functions. A page the browser keeps to itself, like about:home, has no title here." {
  b = _begin()
  try {
    out = []
    for i, c in _asked(b.socket, "browsingContext.getTree", {maxDepth: 0}).contexts {
      title = ""
      try {title = _page(b, c.context, "return JSON.stringify(document.title);")} catch e {}
      out += [{tab: i + 1, title: title, url: c.url}]
    }
  } catch e {
    _finish(b)
    fail e
  }
  _finish(b)
  return out
}

fn close(tab: whole? "the tab's number, else the open one" = nothing) "Closes a tab in the browser, and the first becomes the open one. The last tab left is emptied instead." {
  b = _begin()
  try {
    t = _tabbed(b, tab)
    if std.length(_asked(b.socket, "browsingContext.getTree", {maxDepth: 0}).contexts) > 1 {
      _asked(b.socket, "browsingContext.close", {context: t.id})
    } else {
      _asked(b.socket, "browsingContext.navigate", {context: t.id, url: "about:blank", wait: "complete"})
    }
    _keep("tab", nothing)
  } catch e {
    _finish(b)
    fail e
  }
  _finish(b)
}

fn render(html: text "a whole page of HTML and CSS", tab: whole|text? "the number of the tab to use, new for a new one, else the open one" = nothing) -> {tab: whole, title: text} "Lays out a page of HTML and CSS a lingo made, like a report, a table or a card, in the browser, so it shows on the Browser screen beside the to-do and can be read, clicked or typed in like any page. Gives the tab and the page's title." {
  b = _begin()
  try {
    t = _tabbed(b, tab)
    _asked(b.socket, "browsingContext.navigate", {context: t.id, url: "about:blank", wait: "complete"})
    now = _page(b, t.id, "document.open(); document.write(" + std.to_json(std.text(html)) + "); document.close();\nreturn JSON.stringify(document.title);")
    out = {tab: t.number, title: now}
  } catch e {
    _finish(b)
    fail e
  }
  _finish(b)
  return out
}

////////// HELPERS //////////

fn _release() "The LibreWolf it fetches, by its version, where each computer's is, and its SHA-256, from LibreWolf's releases (codeberg.org/librewolf/bsys6/releases). A new version is a new entry here." {
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

fn _installed() "Where LibreWolf is on this computer, \{version, windows, folder, exe, profile}, kept in its notes, fetched and checked first when it isn't there, is another version, or the folder setting moved it." {
  r = _release()
  want = text.replace(std.text(setting("folder") ?? ""), "\\", "/")
  p = _kept("place")
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
      "New-Item -ItemType Directory -Force " + _quoted(dir, true) + ", " + _quoted(p.profile, true) + ", " + _quoted(folder + "/screenshots", true) + " | Out-Null",
      "$zip=" + _quoted(dir + "/librewolf.zip", true),
      "& (Join-Path $env:SystemRoot 'System32/curl.exe') -fsSL --retry 5 --retry-all-errors -C - -o $zip " + _quoted(r.from + one.file, true),
      "if ($LASTEXITCODE) \{ throw 'The download stopped partway. Try again and it carries on from there' \}",
      "if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne '" + one.sha256 + "') \{ Remove-Item $zip; throw 'The download did not match its checksum' \}",
      "& (Join-Path $env:SystemRoot 'System32/tar.exe') -xf $zip -C " + _quoted(dir, true),
      "Remove-Item $zip"
    ], "\n"))
  } else {
    done = commands.run(text.join([
      "set -e",
      "mkdir -p " + _quoted(dir, false) + " " + _quoted(p.profile, false) + " " + _quoted(folder + "/screenshots", false),
      "f=" + _quoted(dir + "/librewolf.tar.xz", false),
      "curl -fsSL --retry 5 --retry-all-errors -C - -o \"$f\" " + _quoted(r.from + one.file, false),
      "if [ \"$(sha256sum \"$f\" | cut -d' ' -f1)\" != '" + one.sha256 + "' ]; then rm -f \"$f\"; echo 'The download did not match its checksum'; exit 1; fi",
      "tar -xJf \"$f\" -C " + _quoted(dir, false),
      "rm -f \"$f\""
    ], "\n"))
  }
  if done.code != 0 {fail "LibreWolf couldn't be fetched: {done.out}"}
  _keep("place", p)
  return p
}

fn _begin(hidden=nothing) "A socket to the running browser with a session begun on it, as \{socket, place}, LibreWolf fetched and started first when it isn't running, always as one real window, which is moved out of sight or back in front, never started again for it, so its tabs, pages and sign ins simply carry on. hidden, given, is where it should be, else _hidden says." {
  p = _installed()
  hidden = hidden ?? _hidden()
  s = _reached(p)
  if s != nothing {
    b = {socket: s, place: p}
    _placed(b, hidden)
    return b
  }
  // A window out of sight is never told it is covered, so its pages keep running at full speed and draw for the screens.
  files.write(p.profile + "/user.js", "user_pref(\"widget.windows.window_occlusion_tracking.enabled\", false);\n")
  flags = "--no-remote --remote-debugging-port 0 --profile " + (p.windows ? "\"" + p.profile + "\"" : _quoted(p.profile, false))
  ready = p.profile + "/WebDriverBiDiServer.json"
  if p.windows {
    done = commands.run("Remove-Item -LiteralPath " + _quoted(ready, true) + " -ErrorAction SilentlyContinue\n[Console]::Out.Write((Start-Process -FilePath " + _quoted(p.exe, true) + " -ArgumentList " + _quoted(flags, true) + " -PassThru).Id)")
  } else {
    done = commands.run("rm -f " + _quoted(ready, false) + "\nnohup " + _quoted(p.exe, false) + " " + flags + " >/dev/null 2>&1 & echo $!")
  }
  if done.code != 0 {fail "LibreWolf couldn't start: {done.out}"}
  _keep("pid", text.trim(done.out))
  _keep("hidden", nothing)
  for turn in 1..60 {
    compute.wait(0.5)
    s = _reached(p)
    if s != nothing {
      b = {socket: s, place: p}
      _placed(b, hidden)
      return b
    }
  }
  fail "LibreWolf didn't start in 30 seconds. Try again, or delete {p.folder} so it's fetched again"
}

fn _placed(b, hidden) "Its window moved where it should be, only when that changes: out of sight, off the edge of the screen, where it still runs and draws; in sight, back where it was and in front. Kept as hidden, for _finish." {
  if _kept("hidden") == hidden {return nothing}
  try {
    w = _asked(b.socket, "browser.getClientWindows").clientWindows[0]
    if hidden {
      if w.x > -10000 {_keep("window", {x: w.x, y: w.y, width: w.width, height: w.height})}
      _asked(b.socket, "browser.setClientWindowState", {clientWindow: w.clientWindow, state: "normal", x: -20000, y: 0, width: w.width, height: w.height})
    } else {
      was = _kept("window") ?? {x: 80, y: 60, width: w.width, height: w.height}
      _asked(b.socket, "browser.setClientWindowState", {clientWindow: w.clientWindow, state: "normal", x: was.x, y: was.y, width: was.width, height: was.height})
      _asked(b.socket, "browsingContext.activate", {context: _asked(b.socket, "browsingContext.getTree", {maxDepth: 0}).contexts[0].context})
    }
  } catch e {}
  _keep("hidden", hidden)
  return nothing
}

fn _hidden(on=nothing) "Whether it works out of sight: unless the person is watching the to-do it works for, on when given, else as chat.showing says, or its show setting is on." {
  watched = on
  if watched == nothing {
    watched = false
    try {watched = chat.showing() == true} catch e {}
  }
  always = setting("show")
  return not (watched == true or always == true or text.lower(std.text(always ?? "")) in ["true", "on", "yes", "1"])
}

fn _reached(p) "A socket to the browser with a session begun, or nothing when it isn't running. One with a session left from a call that was stopped can't begin another, so it is ended, to be started again." {
  ready = p.profile + "/WebDriverBiDiServer.json"
  if not files.exists(ready) {return nothing}
  try {
    at = std.from_json(files.read(ready))
    s = socket.open("ws://{at.ws_host}:{at.ws_port}/session")
  } catch e {
    return nothing
  }
  try {
    _asked(s, "session.new", {capabilities: {}})
  } catch e {
    socket.close(s)
    if not ("Maximum number of active sessions" in e) {fail e}
    pid = std.number(_kept("pid") ?? "")
    if pid != nothing {
      try {commands.stop(pid)} catch e {}
    }
    compute.wait(1)
    return nothing
  }
  return s
}

////////// 11. USE DESKTOP //////////

fn shown(on: bool "whether the person watches the work now") "Onsay calls this the moment the person turns Use desktop on or off: on, the browser's window comes in front and its screens in Onsay close; off, the window goes out of sight and each tab is drawn on a screen in Onsay. The same window and tabs carry on, nothing reloaded. Nothing happens when it isn't running." {
  p = _kept("place")
  if p == nothing {return nothing}
  s = _reached(p)
  if s == nothing {return nothing}
  b = {socket: s, place: p}
  _placed(b, _hidden(on))
  _finish(b)
  return nothing
}

fn _finish(b) "Out of sight, every tab shown on a screen of its own in Onsay, Browser tab 1 and on, as the picture LibreWolf itself drew of it, a JPEG of what is in view; with its window shown, those screens closed. Then the session ended, so the next call can begin one, and the socket closed. A computer without screens shows nothing." {
  try {
    tabs = _asked(b.socket, "browsingContext.getTree", {maxDepth: 0}).contexts
    hidden = _kept("hidden") != false
    for n in 1..std.length(tabs) {
      at = screen.open("Browser tab {n}")
      if hidden {
        shot = {context: tabs[n - 1].context, format: {type: "image/jpeg", quality: 0.8}}
        screen.show(at, _asked(b.socket, "browsingContext.captureScreenshot", shot).data)
      } else {
        screen.close(at)
      }
    }
  } catch e {}
  try {_asked(b.socket, "session.end")} catch e {}
  socket.close(b.socket)
}

fn _asked(s, method, params={}) "One WebDriver BiDi command and its result, or its error as a failure." {
  socket.send(s, std.to_json({id: 1, method: method, params: params}))
  while true {
    got = socket.receive(s, 60)
    if got == nothing {fail "LibreWolf didn't answer {method} within a minute"}
    m = std.from_json(got)
    if m.type == "success" {return m.result}
    if m.type == "error" {fail "{method}: {m.message}"}
  }
}

fn _tabbed(b, tab) "The tab to use, \{id, number}, which becomes the open one: number tab, a new one for \"new\", else the open one, else the first, else a new one. Tabs are numbered from 1 in the browser's order, since the browser gives them new ids with every session." {
  made = tab == "new" ? _asked(b.socket, "browsingContext.create", {type: "tab"}).context : nothing
  ids = []
  for c in _asked(b.socket, "browsingContext.getTree", {maxDepth: 0}).contexts {ids += [c.context]}
  if std.length(ids) == 0 {
    made = _asked(b.socket, "browsingContext.create", {type: "tab"}).context
    ids = [made]
  }
  n = made != nothing ? list.index_of(ids, made) + 1 : std.number(tab ?? _kept("tab") ?? 1)
  if n == nothing or n < 1 or n > std.length(ids) {
    if tab != nothing {fail "There's no tab {tab}. tabs gives the tabs open"}
    n = 1
  }
  _keep("tab", n)
  return {id: ids[n - 1], number: n}
}

fn _page(b, t, body) "JavaScript run in a tab and its answer, body the inside of a function that gives JSON.stringify of it, with seen(element), whether it shows, said(element), the words it shows, and named(field), its label, there to use." {
  js = text.join([
    "(() => \{",
    "const seen = (e) => \{ const r = e.getBoundingClientRect(); return r.width > 0 && r.height > 0; \};",
    "const said = (e) => (e.innerText || e.value || e.getAttribute('aria-label') || e.title || '').trim().replace(/\\s+/g, ' ');",
    "const named = (f) => ((f.labels && f.labels[0] && f.labels[0].innerText) || f.getAttribute('aria-label') || f.placeholder || f.name || f.id || '').trim();",
    body,
    "\})()"
  ], "\n")
  got = _asked(b.socket, "script.evaluate", {expression: js, target: {context: t}, awaitPromise: true})
  if got.type == "exception" {fail "The page's script failed: {got.exceptionDetails.text}"}
  return std.from_json(got.result.value)
}

fn _settled(b, t) "The tab's title and address once its page has loaded, after a click or a key that may have begun loading another, waiting at most 10 seconds." {
  compute.wait(0.5)
  for turn in 1..20 {
    try {now = _page(b, t, "return JSON.stringify(\{state: document.readyState, title: document.title, url: location.href\});")} catch e {}
    if now != nothing and now.state == "complete" {break}
    compute.wait(0.5)
  }
  return {title: now.title, url: now.url}
}

fn _plain(v) "A value from the page, as WebDriver BiDi writes it, as a plain value." {
  if v.type == "array" or v.type == "set" or v.type == "nodelist" or v.type == "htmlcollection" {
    out = []
    for x in v.value ?? [] {out += [_plain(x)]}
    return out
  }
  if v.type == "object" or v.type == "map" {
    out = {}
    for pair in v.value ?? [] {out[json.kind(pair[0]) == "text" ? pair[0] : std.to_json(_plain(pair[0]))] = _plain(pair[1])}
    return out
  }
  return v.value
}

fn _notes() "Where it keeps what it knows between calls, browser.json in its folder: the folder setting, else lingo-browser in AppData\\Local on Windows and ~/.local/share on Linux." {
  want = text.replace(std.text(setting("folder") ?? ""), "\\", "/")
  folder = want != "" ? want : files.exists("C:/Windows") ? "~/AppData/Local/lingo-browser" : "~/.local/share/lingo-browser"
  return {folder: folder, file: folder + "/browser.json"}
}

fn _kept(key) "What it kept under key, or nothing." {
  n = _notes()
  if not files.exists(n.file) {return nothing}
  return std.from_json(files.read(n.file))[key]
}

fn _keep(key, value) "Keeps value under key, nothing taking it away." {
  n = _notes()
  all = files.exists(n.file) ? std.from_json(files.read(n.file)) : {}
  all[key] = value
  try {files.make_folder(n.folder)} catch e {}
  files.write(n.file, std.to_json(all))
}

fn _quoted(words, windows) "Text as one word of this computer's shell, in single quotes, PowerShell's on Windows, else sh's." {
  if windows {return "'" + text.replace(std.text(words), "'", "''") + "'"}
  return "'" + text.replace(std.text(words), "'", "'\\''") + "'"
}
