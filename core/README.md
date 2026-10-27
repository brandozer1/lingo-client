# Lingo Core

The Lingo runtime, in C: a small program that runs lingos, built for Windows and for Linux. A lingo is an app that lets
Onsay talk to the device the lingo client is on. The core runs from that computer's lingos folder: it signs in to Onsay,
says what its lingos share, and runs a call to one when Onsay sends it, an answer sent to it after an `@` included. It
also answers web requests on port 6969, each lingo at its own route (*Routes*, below), so any program on the computer
can use its lingos, with or without Onsay. It has no screen: a product is this core plus its own app around it, like the Windows app, which lives in its own project,
onsay-desktop, beside this repo, and ships `bin/onsay-core.exe` and `bin/lingos`. On a Linux machine it is the product
on its own: `install.sh` puts it there, signs it in and keeps it running.

The core itself is an engine (`src/core.c`, see `src/core.h`) that never starts a thread or touches the connection, and
a host drives it: `src/host.c` on a device with threads and a WebSocket in C, like Windows and Linux. A new device is a
`platform_*.c`, and a host if it holds its connection differently.

The language, and what a lingo is, its code alone or in a folder with its `lingo.json`, are in `../docs/lingo.md`.

## The compiler

```
Lingo code
   |  parse: checked against the lingos it may use
   v
tree with phrases written in Lingo in it, like memo.remember, lingo(...) as lingo.run and use(...) as lingo.use,
asked of Onsay as they run, and setting(...) as lingo.setting, answered here. A dotted call is local, a phrase here or
a lingo in the same lingos folder, and any other is a compile error that says to use use(...)
   |  link: each phrase written in Lingo swapped for its fn, layer by layer down
   v
tree of native and host phrases only, and the permissions it really needs  ->  the runner
```

## Settings

Where Onsay is is built in, never a setting. Everything else is in one file, given as the first argument:

```json
{
  "token": "the device's sign in token",
  "name": "Office PC",
  "home": "onsay-data",
  "lingos": "onsay-data/lingos",
  "port": 6969,
  "listen": "here",
  "inbound": "onsay",
  "allow": ["none", "screen.read", "screen.act"],
  "steps": 5000000,
  "ms": 600000
}
```

| Setting | What it is |
|---------|------------|
| `token` | sent in hello. The Onsay app writes this file with its own token (`src/Core/Core.cs` in onsay-desktop). On a server, `onsay-core settings.json --sign-in` writes it: it says a web address and a code to open on any device, waits while the person signs in there and presses Connect, as the app does, and keeps the file's other settings. With none, the core runs bare: no connection to Onsay, answering its port only, and `lingo(...)` and `use(...)` fail with "This lingo client isn't connected to Onsay" |
| `port` | where it answers web requests. 6969 when left out |
| `listen` | `here`, the default, answers this computer only (127.0.0.1). `network` answers every address it has, so other computers can reach it |
| `inbound` | `onsay` takes web requests that come down its connection to Onsay too (*Messages*). Left out, it answers those with a 403. Onsay Desktop sets it when signed in |
| `install` | this device's own id, which `--sign-in` makes and keeps, so signing in again gets the device back, not a new one |
| `name` | what the person calls this device |
| `home` | the core's own folder. Its storage is in `home/store`, and the settings of its lingos in `home/settings` |
| `lingos` | the folder of lingos. `home/lingos` when left out |
| `allow` | the permissions the person gives. The core refuses any phrase that needs one not listed |
| `steps`, `ms` | the most any program may take. Onsay may ask for less, never more |

## Messages

These are what a device's core and Onsay say over their connection. Every `id` is any JSON value the sender chose, sent
back as it came.

| From Onsay | The core answers with a result |
|------------|--------------------------------|
| `{type:"ready", device}` | hello was accepted |
| `{type:"call", id, lingo, values}` | runs a function of a lingo in its folder, as an address's path names it: `test.lingo/find` is `test.find`, `test.lingo` alone `test.main`, and a native's by its group, `windows.lingo/window/list`. `values` is a list in order or a record by name, defaults for any left out, and a name it doesn't take is code `type`. An answer sent here after an `@` comes as one of these, the answer its first value |
| `{type:"call", id, phrase, values}` | runs one phrase the device has by its id, `values` in the phrase's order. A native or host phrase runs as it is. A phrase written in Lingo, like `memo.recall`, runs as the program `tool phrase(v) {return memo.recall(v[0], ...)}` compiled against its lingo, with the settings' limits, so a stop, a limit or a compile error ends it as a program's would. Its id must be only `a` to `z`, `0` to `9`, `_` and `.`. One the device hasn't got is code `unknown` |
| `{type:"run", id, code, names?, input, limits}` | compiles the code here against the lingos named, or all it has, then runs it. A compile error comes back as code `compile`, with its line. For the tests and other hosts |
| `{type:"run", id, program, input, limits}` | runs a tree compiled elsewhere. For the tests |
| `{type:"request", id, method, path, headers, body, base64}` | a web request that came to Onsay for this computer, answered as its port answers one (*Routes*), only when `inbound` is `onsay`, and as from the network, so only open lingos are there. `path` has its query, like `/hello?name=Ana`, `headers` are by lower case name, and `body` is text, or base64 when `base64` is true. The answer is a `response`, below, not a result |
| `{type:"stop", id}` | stops that job between two steps, or while it waits on `lingo(...)` |
| `{type:"error", error}` | something it said couldn't be done. The core logs it |

| To Onsay | When |
|----------|------|
| `{type:"hello", token, client, version, level, links, name, allow, manifests, catalog}` | on connecting. `client` is `onsay-core-c` and `version` this core's, `0.3.0` from when calls name a lingo's file. `manifests` is its native lingos' listings as text, core first, and `catalog` every lingo it has |
| `{type:"result", id, ok:true, value, log, pictures?}` | a run or call is done. `log` is the notes it made, as text, and `pictures` only there when it made some |
| `{type:"result", id, ok:false, error:{code, message, at, line}, log, pictures?}` | it failed. `code` is `compile`, `type`, `limit`, `stopped`, `denied`, `unknown`, `phrase` or `fail`, `at` its place in the tree, and `line` its line, or null |
| `{type:"lingo", id, ask, values, gives, context, asking}` | a program reached `lingo(...)`. `id` is a number the core counts up from 1. The program waits for the answer. `context` is what was around it, from `pl_context` in `src/platform.h`: `os`, `time` (local, with its offset), `zone`, `language`, and the `app` and `window` in front, those the device knows. A Linux server sends no `app` or `window`. `asking` is the function the running call is of, like `agent.ask` |
| `{type:"lingo", id, address, values}` | a program reached `use(...)`, like `use("laptop.brandon/lingos/text.lingo/slug", {text: "Hello there"})` or `use("https://api.example.com/v1/items")`. `values` is a record, a list or nothing. It waits the same way |
| `{type:"response", id, status, headers, body, base64}` | a `request` is answered: its status, its headers by lower case name, and its body as text, `base64` always false |
| `{type:"catalog", manifests, catalog}` | the lingos folder changed |

Its host adds `ping`, and on the device's stop key `halt`. Run with `--sent`, `host.c` prints every message it sends
to Onsay on stdout too, `{"sent": the message}` a line, so the program around it can keep a log the person reads, as
the Windows app does under Settings, Privacy.

**The catalog** is each lingo's listing, made from its code for a Lingo lingo (`listing` in `src/compile.c`): its
`name`, its `file`, like `memo.lingo`, or for a native one its `version` and `folder`, its `about`, whether it's `ok` or
`broken` and why, its `settings`, names alone, those its `lingo.json` lists and then any other its code reads with
`setting("...")`, never their values, and `phrases`, every function it
shares: `id`, `head`, `does` from the comments right above its fn, `gives` for a native
one, its `inputSchema`, and `lingos`, how many `lingo(...)` calls it makes, counted from its code through every function
it reaches, none for a native. Never its code.

**lingo(...) and use(...), as they run.** `lingo("what it does", {name: value}, "what it gives")` compiles to the phrase
`lingo.run(ask, values, gives)`, and `use(address, values)` to `lingo.use(address, values)`. Both are every core's own,
in no listing, and both reach the host's ask with their values by name. When a program reaches one, the core sends one of
the `lingo` messages above, and waits. Onsay answers `{type:"lingo", id, value}`, which the program goes on with, or
`{type:"lingo", id, error, code}`, which is the phrase's error (`phrase` when no code is given). While it waits, Onsay
may send `call`s, since what answers may be on this device: the core runs each inside the waiting job and answers it
as above, then waits on. Anything else waits its turn. A `stop` for the job ends the wait. Nothing is kept on the device.
`host.c` does this on the job's thread.

**http.send, the web.** `http.send(method, url, headers=nothing, body=nothing, timeout=30)` is every core's one web
request, needing `net`: `{status, headers, text}`, headers a record by lower case name, redirects followed, at most 4 MB
back, and a failed request the phrase's error. It goes to `pl_http` in `src/platform.h`. On Windows that is WinHTTP
(`src/platform_win.c`), so proxies and certificates work as they do for every other program. On Linux it is libcurl
(`src/platform_linux.c`), the same way. Lingos mostly
use `request` (`../lingo-library/request.lingo`), built on it, or `use(...)` for one plain request.

## Lingos

Every lingo is a tree rooted at the core, and the core is the only one built in: `core 0.1`, the same on every device,
used by every lingo and never listed. It is the floor, only what Lingo can't write for itself: `std` (`length`, `keys`,
`slice`, `number`, `text`, `to_json`, `from_json`, `bytes`, `from_bytes`), `compute` (`now`, `wait`, `log`), `store`,
`http` and `settings` (`get`, `set`), each with its reason in section 15 of `../docs/lingo.md`. The language itself is only its syntax and
`lingo(...)`, `use(...)` and `setting(...)`. Splitting, case, rounding, sorting and dates are library lingos written in
Lingo on this floor, `text`, `list`, `math` and `time` in `../lingo-library`. Everything else is in the lingos folder:

- **A lingo written in Lingo is its code there,** `<name>.lingo`, like the lingos of `../lingo-library`: one file, or a
  folder, `<name>/<name>.lingo` beside `<name>/lingo.json`, its manifest saying its settings (*Lingo settings*, below).
  Its listing is made from its code (section 11 of `../docs/lingo.md`). It may call the core, the native lingos here,
  and every other Lingo lingo in the folder by its dotted name, `text.slug(title)`, linked in here, and reaches anything
  else with `use(...)`.
- **A native or host lingo is a folder with a `lingo.json`,** its `runs` `native` or `host`, the device's own native
  lingo too: the Windows app ships `windows` (`../lingos/windows`) and puts it there. A folder without a `lingo.json`
  isn't a lingo, and one whose `lingo.json` runs neither, with no `<name>.lingo` beside it, is set aside with the reason.

The core reads them all at start and again within 5 seconds of any change: it loads the natives, then compiles every
Lingo lingo against all of them, and sets aside any that can't load with the reason, like `broken.lingo line 3: There's
no lingo or group called nope here`, the rest loading without it. A Lingo lingo whose name is that of a native lingo or
one of its groups is set aside too, like the test lingo `input.lingo` beside `windows`: `input is already a part of
windows, this device's own lingo`.

**Native lingos.** `library` is a library built for this device, like `windows.dll`, written in C against
`src/lingo.h` and `src/platform.h` and linked with `bin/onsay-core.lib`. Its one entry, `lingo_phrases`, adds its
groups, shapes and phrases, and the permissions each needs, so its `lingo.json` lists no functions. A phrase may need
several, like `screen.act, screen.read`, and runs only when every one was given:

```json
{
  "name": "windows",
  "version": "1.0.0",
  "about": "Windows itself: open apps and windows and their buttons and controls, the screen and reading it, ...",
  "runs": "native",
  "library": "windows.dll"
}
```

A native library is loaded once and kept for as long as the core runs, so a changed one is used after a restart. One
whose folder is removed stops being offered.

**Host lingos.** The phrases are run by the program around the core, in its own language, where the core can't load a
library (the test lingo `test/lingos/hosted`, below). The folder
holds only its `lingo.json`: its `groups` with what each needs, its `shapes`, and its `functions`, each with its
`group`, and in `onsay.needs` its permissions when they aren't its group's. Each phrase reaches the host through
`pl_call` in `src/platform.h`, as its id and its values as JSON, and the host answers its value or its error. A core
with no host, like the Windows exe, sets these aside with the reason:

```json
{
  "name": "hosted",
  "version": "1.0.0",
  "runs": "host",
  "shapes": { "echoed": "{x}" },
  "groups": [{ "name": "hosted", "needs": ["none"] }],
  "functions": [{
    "name": "echo", "group": "hosted", "description": "Gives back what it was given.",
    "inputSchema": { "type": "object", "properties": { "x": {} }, "required": ["x"] },
    "outputSchema": {}
  }]
}
```

**The compiler's ops** (`lingo_request` in `src/compile.c`, built on its own as `bin/lingo.wasm` for the tests, read by
`web/compiler.js`): `manifest` reads a native lingo's text listing, each phrase with its `inputSchema` and
`outputSchema`; `read` reads a native or host lingo's `lingo.json`, or, given `file` and `code`, makes a Lingo lingo's
listing from its code; `parse`, `face` and `prepare` compile code, `stacked` the Lingo lingos as `[{file, code}]`.

## Lingo settings

Any lingo may ask the person for settings it needs, like an account name or a key for a web service, by reading them
with `setting("name")`, part of the language. They are kept on this device, never on Onsay's servers. Its `lingo.json`
says what each one is, so the program around the core can build a form of them, the Onsay app's:

```json
{
  "settings": [
    { "name": "key", "title": "OpenRouter API key", "about": "Your OpenRouter key, from openrouter.ai/keys.", "required": true, "secret": true },
    { "name": "url", "title": "Address", "about": "Where OpenRouter answers, up to /v1.", "default": "https://openrouter.ai/api/v1" },
    "model"
  ]
}
```

Each is a name alone or a record: its `name`, and as it needs `title`, `about`, `default`, `required` and `secret`. A
setting its code reads that its `lingo.json` doesn't list is a setting too, by its name alone, and a lingo with no
`lingo.json` has only those.

`setting(key)` compiles to the phrase `lingo.setting(key, reference)`, the reference its lingo's own name, like
`openrouter`, and the core answers it on the device as the line runs (6. LINGO SETTINGS in `src/core.c`): the value
kept, else the `default` its `lingo.json` gives, else nothing, so the lingo may apply its own:

```
fn units() { return setting("units") ?? "metric" }
```

`setting("token", "github")` reads another lingo's, so one token serves many lingos, and isn't one of this lingo's
own. Values are kept in the core's own folder, `home/settings/<lingo>.json`, a record by name, unless its `lingo.json`
says `"keep": {"read": "read", "write": "write"}`: then the core calls the lingo's own `read(key)` for a value and
`write(key, value)` to keep one, so its maker decides where they live. Inside those two, `setting(...)` of that lingo
reads the core's file.

Two phrases of the core, and the same on the command line for the program around it, change and show them wherever
they are kept:

| Phrase | Command line | Does |
|--------|--------------|------|
| `settings.get(lingo)` | `onsay-core settings.json --settings <lingo>` | every setting of a lingo, its `lingo.json`'s first, each with what it says and its `value` now, as JSON, for a form |
| `settings.set(lingo, key, value)` | `onsay-core settings.json --set <lingo> <key> <value>` | changes one. On the command line value is JSON, like `"metric"` or `5`, or text as it is when it isn't JSON |

The catalog names a lingo's settings, never their values.

## Routes

The core answers web requests (HTTP/1.1) on its `port`, each lingo at its route: `/<name>`, like `/memo`, or the
`route` its `lingo.json` names, like `"/onsay"`. A request goes to the lingo with the longest route its path starts
with. One request is answered per connection, its body sent with a `Content-Length` of at most 4 MB, and anything else
is answered with why: a 400 for what isn't a web request, 411 for a body in chunks, 413 for one too big.

- **A lingo with `serve(request)`** gets every request to its route. `request` is `{method, path, query, headers, body,
  json}`: `path` the part after its route, starting with `/`, `query` a record, `headers` a record by lower case name,
  `body` text, and `json` the body read as JSON when it is JSON, else nothing. A record with a number `status` that
  serve gives is the response, `{status, headers, body}`, its body text, or written as JSON when it isn't text.
  Anything else is a 200: text as `text/plain`, and anything else as `application/json`.
- **Any other lingo** is called as `POST /<name>/<function>` with its values as a JSON record by name, or a list in
  order, or as `GET` with its values in the query, the same call `{type:"call", lingo:"<name>.lingo/<function>"}` makes,
  and `/<name>` alone is its `main`. The answer is the function's value as JSON.
- **`GET /`** lists the lingos the caller may reach, `{lingos: [{name, route}]}`.
- **An error** is `{error}`, as JSON: 404 for no such lingo or function, 400 for values it doesn't take, 403 for a
  permission not given (`allow` holds for every call), and 500 for a program that failed.

**Safe by default.** A request from this computer (127.0.0.1 or ::1) reaches every lingo. A request from the network,
or down the connection from Onsay, reaches only a lingo whose `lingo.json` says `"open": true`, and any other answers
404, exactly as if it weren't there. So does one from a page in a browser on this computer that another site sent: a
request counts as from this computer only when its `Host` names this computer and its `Origin` and `Sec-Fetch-Site`
say no other site sent it.

**One at a time.** The engine runs one program at a time, so a request waits its turn while another program runs,
and so does a call from Onsay. Each request is read on its own thread (`src/host.c`), so a stop still lands at once.

## Files

| File | Job |
|------|-----|
| `src/lingo.h` | the core's words: values, buffers, phrases, running, compiling |
| `src/value.c` | values in an arena freed all at once, JSON, numbers written exactly as JavaScript writes them |
| `src/run.c` | the runner |
| `src/jump.h` | `setjmp` from the C library, used by the runner for errors |
| `src/compile.c` | the compiler: lex, parse, manifests, a Lingo lingo's listing made from its code, `lingo(...)` as `lingo.run`, `use(...)` as `lingo.use` and `setting(...)` as `lingo.setting`, link, and loading a folder of lingos |
| `src/phrase.c` | the phrase table, and the listing made from it |
| `src/std.c` | the core lingo's `std` group: reaching inside values and converting them |
| `src/core.h`, `src/core.c` | the engine: settings, hello and the catalog, stops, jobs, a call by a lingo's file, web requests answered at their routes, lingos and their settings, and the floor phrases `compute`, `store`, `http` and `settings` |
| `src/host.c` | the core as a program: the settings file, signing in with `--sign-in`, a lingo's settings with `--settings` and `--set`, the connection, heartbeat, reconnecting, the port and reading each web request, and the engine on its own thread, every job in turn |
| `web/compiler.js` | the compiler on its own, run from JavaScript against `bin/lingo.wasm`, for the tests (`test/compiler.mjs`) |
| `src/platform.h` | everything the core needs from a device. Porting the core is writing this for the new device |
| `src/platform_win.c` | that, for Windows: WinHTTP WebSockets and `pl_http` (its own timer ends a request on time, since WinHTTP looks at its timeouts only every 4 seconds or so), threads, files, and the port through Winsock |
| `src/platform_linux.c` | that, for Linux: pthreads, POSIX files, the port on plain sockets, `dlopen` for native lingos, and the web through libcurl, loaded as it starts, so it builds with nothing of Linux's and says to install libcurl when it's missing. Curl only opens the connection, TCP, TLS and any proxy, and the WebSocket on it is written here, since many builds of curl have none |
| `install.sh` | puts the core on a Linux server: the program for its processor, its folder (`~/.onsay`, or `/var/lib/onsay` as root) with `core.json` in it, `--sign-in`, and a systemd service that keeps it running. Where it downloads the core from is `ONSAY_DOWNLOAD`, at its top |
| `../lingos/windows/` | the Windows lingo, a native lingo of its own: windows, controls through UI Automation, the screen and OCR, mouse, keyboard, files, clipboard, commands, notifications, `input` (the mouse and keyboard the way a person uses them) and `look` (Onsay's eyes) |
| `../lingos/native/` | the native lingo: one phrase, `native.call`, that calls a function in the operating system or a shared library by its name and layout, using a small vendored part of libffi (`libffi/`, MIT). Windows only |
| `src/wasm.c` | the compiler on its own, built as `bin/lingo.wasm`, for the tests |
| `test/reference/` | the first Lingo, in JavaScript. The tests hold the C to it, word for word |

## Build and test

```
./build.ps1                                     bin/onsay-core.exe, bin/test/onsay-core.exe, bin/lingo.wasm, bin/lingos/
                                                bin/lingos holds ../lingos, only windows: the app ships no other
                                                bin/linux-x64/onsay-core and bin/linux-arm64/onsay-core, for glibc 2.28
                                                or later, built on Windows with zig too
bin/onsay-core.exe settings.json --manifest     prints what it tells Onsay it can do
bin/onsay-core.exe settings.json --sign-in      signs the device in from any device's browser, and writes its token
bin/onsay-core.exe settings.json --settings agent
                                                prints every setting of agent as JSON, for a form of them
bin/onsay-core.exe settings.json --set agent model claude-opus-5-5
                                                changes one, wherever agent keeps its settings

sh install.sh ./bin/linux-x64/onsay-core        on a Linux server: installs that core, signs it in and starts it as a
                                                service. With nothing after it, it downloads the core

node test/compiler.mjs                          the C compiler against the reference, case by case: both must give the same JSON
node test/server.mjs                            a stand-in for Onsay, then in another terminal:
bin/test/onsay-core.exe test/onsay.json         runs every test, and prints each result on the stand-in's side
```

Every library lingo needs only the core: `test/server.mjs` copies every lingo of `../lingo-library`, file or folder,
into `test/lingos` as it starts, since the app ships none of them, and tests `text`, `list`, `math` and `time` on the cases the core once did
itself. The exe has no host, so the tests of `test/lingos/hosted`, a host lingo, are skipped.

The test build talks to the stand-in on this computer instead of Onsay. The stand-in compiles with the `bin/lingo.wasm`
compiler, runs every pure program on both the C core and the reference and requires identical results, then checks
stop, limits, memory, calls (of a phrase written in Lingo too, and by a lingo's file and function, main when none),
storage, settings answered on the device and never asked of Onsay (the value kept in `test/data/settings`, which it
writes as a device has them, else the `lingo.json` default, another lingo's by reference, the test lingo `vault` keeping
its own with its read and write, `settings.get` and `settings.set`, and `--settings` and `--set`), `lingo(...)` asked of it as
programs run (using the device meanwhile, saying which function asks, and stopped while it waits), `use(...)` asked by
its address (from a program and from a lingo's own code), a lingo calling one in the same folder by its dotted name, a
dotted call to one that isn't here refused, `http.send` against the stand-in's own pages (a timeout on time too), `request`
tried again after a 503, compiling on the device through the Lingo lingo `memo.lingo` in `test/lingos`, the Windows
lingo for real, the catalog (memo ready with its listing made from its code and never its code, broken set aside with its
reason, the test lingo `input.lingo` set aside since that is a group of windows, notalingo ignored), web requests to its
port, 6970 in `test/onsay.json` (the test lingo `page` and its `serve` at `/hello`, open, and `memo`'s functions called
by their routes, the open rule, a page elsewhere in a browser, and what isn't a web request) and down its connection,
a second core with no token answering its own port, ping, and coming back after a dropped connection. It never clicks or types: the test core isn't given `screen.act`.
