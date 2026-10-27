# Lingo client

The lingo client in C: a small program that runs lingos, built for Windows and Linux. It runs the lingos in its
computer's lingos folder and answers on its port, 6969, each lingo at its own route, so any program on the computer can
use them. It comes bare: with no token in its settings it has no connection to Onsay and answers its port only. Signed
in, it keeps a connection open to Onsay, so Onsay can reach its lingos and its lingos can ask Onsay with `lingo(...)`
and `use(...)`. It has no screen of its own: the Windows app (onsay-desktop) ships `bin/onsay-core.exe` and
`bin/lingos`, and on a Linux server `install.sh` puts it there and keeps it running.

The language, and what a lingo is, its code, its `lingo.json` and `serve(request)`, are in `../docs/lingo.md`.

## Settings

One JSON file, given as the first argument. Where Onsay is is built in, never a setting.

```json
{
  "token": "the computer's sign in token",
  "home": "onsay-data",
  "port": 6969,
  "listen": "here",
  "inbound": "onsay",
  "allow": ["screen.read", "files.read", "net"],
  "steps": 5000000,
  "ms": 600000
}
```

| Setting | What it is |
|---------|------------|
| `token` | its sign in to Onsay. The Windows app writes it, and on a server `--sign-in` does. With none it runs bare, and `lingo(...)` and `use(...)` fail with "This lingo client isn't connected to Onsay" |
| `install`, `name` | this computer's own id, which `--sign-in` makes and keeps, so signing in again gets it back, and what `--sign-in` calls it, its host name when left out |
| `home` | its own folder: Onsay's storage on this computer in `home/store`, and its lingos' settings in `home/settings` |
| `lingos` | the lingos folder, `home/lingos` when left out |
| `port` | where it answers, 6969 when left out |
| `listen` | `here`, the default, answers this computer only (127.0.0.1). `network` answers every address it has |
| `inbound` | `onsay` takes the requests Onsay's proxy passes down its connection too. Left out, those are answered 403. Onsay's own requests for this computer's account are taken either way |
| `allow` | the permissions the person gives: `screen.read`, `screen.act`, `files.read`, `files.write`, `run` and `net`. A phrase that needs one not given is refused |
| `steps`, `ms` | the most one request may take, in steps and milliseconds |

## Command line

```
onsay-core settings.json                      runs it
onsay-core settings.json --sent               and prints every message it sends Onsay on stdout too, {"sent": the
                                              message} a line, so the program around it can keep a log the person reads
onsay-core settings.json --sign-in            signs this computer in to Onsay from any device's browser: it says an
                                              address and a code, waits while the person signs in there and presses
                                              Connect, and writes the token into the settings
onsay-core settings.json --settings agent     prints every setting of a lingo as JSON, each with its value now, for a
                                              form of them
onsay-core settings.json --set agent model claude-opus-5-5
                                              changes one, wherever that lingo keeps it. The value is JSON, like
                                              "metric" or 5, or text as it is when it isn't JSON
```

The windows lingo also writes a line of JSON on stdout for each thing the app draws: Onsay's pointer gliding and
clicking, a ring around something on screen, and his panel (`show` in `../lingos/windows/windows.c`).

## Requests

Each lingo answers at its route: `/<name>`, or the `route` its `lingo.json` names. A request goes to the lingo with the
longest route its path starts with, over HTTP/1.1, one request a connection, its body text of at most 4 MB sent with a
`Content-Length`.

- **A lingo with `serve(request)`** gets every request to its route (`../docs/lingo.md`, section 11).
- **Any other** is called function by function: `POST /<name>/<function>` with its values as a JSON record by name, or
  a list in order, or `GET` with its values in the query. `/<name>` alone is its `main`, and a native lingo's are
  `/<name>/<group>/<function>`, like `/windows/window/list`. A value left out takes its default. The answer is the
  function's value as JSON.
- **`GET /`** lists the lingos the caller may reach, `{lingos: [{name, route, open}]}`, open whether it answers from anywhere.
- **An error** is `{error}` as JSON, in words: 400 for what isn't a web request, a body that isn't text, or a value the
  function doesn't take, 403 for a permission not given, 404 for nothing there, 411 for a body in chunks, 413 for one
  over 4 MB, and 500 for a function that failed, its message starting with its lingo's file and line when it failed in
  Lingo code.

**Who reaches what.** A request from this computer reaches every lingo: from 127.0.0.1 or ::1, its `Host` naming this
computer, and no `Origin` or `Sec-Fetch-Site` saying another site sent it. So does one Onsay sends for this computer's
own account (`owner`, below). A request from the network, or one Onsay's proxy passes on, reaches only a lingo whose
`lingo.json` says `"open": true`, and any other answers 404, as if it weren't there.

**One at a time.** One request runs at a time, in the order they came. Each is read on its own thread (`src/host.c`),
so a stop still lands at once. A request that comes while another waits on Onsay for `lingo(...)` or `use(...)` runs
inside that wait, so a lingo can be reached from inside another's run.

## Messages

What the lingo client and Onsay say over its connection, one JSON record each. Every `id` is any JSON value the sender
chose, sent back as it came.

| From Onsay | What it is |
|------------|------------|
| `{type:"ready", device}` | hello was taken |
| `{type:"request", id, method, path, headers, body, base64, owner}` | a request for this computer, answered as its port answers one. `path` has its query, `headers` are by lower case name, and `body` is text: one in base64 is answered 400, since lingos take text. `owner: true` marks Onsay's own, for this computer's account: its lingo function's pick, a `use(...)`, an answer sent on after an `@`. It reaches every lingo, as a request from this computer would. Without it, it is one the proxy passed on: only open lingos, and only when `inbound` is `onsay` |
| `{type:"stop", id}` | stops that request, between two steps or while it waits on Onsay |
| `{type:"lingo", id, value}` or `{type:"lingo", id, error, code}` | the answer to a `lingo` message, below: the value the program goes on with, or the phrase's error |
| `{type:"pong"}` | the answer to its ping |

| To Onsay | When |
|----------|------|
| `{type:"hello", token, client, version, catalog}` | on connecting. `client` is `lingo-client` and `version` its own, `CORE_VERSION` in `src/core.h` |
| `{type:"response", id, status, headers, body, base64}` | a request answered: its status, its headers by lower case name, and its body as text, `base64` always false |
| `{type:"lingo", id, ask, values, gives, context, asking}` | a program reached `lingo(...)`, and waits. `id` counts up from 1. `context` is what was around it (`pl_context` in `src/platform.h`): `os`, `time` (local, with its offset), `zone`, `language`, and the `app` and `window` in front, those it knows. `asking` is the function the request is of, like `agent.ask`, so it is never the answer |
| `{type:"lingo", id, address, values}` | a program reached `use(...)`, and waits the same way |
| `{type:"catalog", catalog}` | the lingos folder changed |
| `{type:"ping"}` | every 20 seconds. One that answers pings and then goes silent for 60 is dropped, and it connects again |

**The catalog** is every lingo in the folder, never its code and never the values of its settings: `{name, status:
"ok", about, settings, phrases, native}`, or `{name, status: "broken", error}` for one set aside, with why. `settings`
is the names of its settings, `native` is true for a native lingo, and each of `phrases` is `{id, does, inputSchema,
lingos}`: `does` is the comment above a Lingo lingo's `fn`, or a native phrase's line after its signature, what it
gives and what it does, and `lingos` is how many `lingo(...)` calls it makes, through every function it reaches.

## The lingos folder

It reads the folder as it starts and again within 5 seconds of any change: it loads the native lingos, compiles every
Lingo lingo against all of them, and sets aside any that can't load with the reason, like `broken.lingo line 3: There's
no lingo or group called nope here`, the rest loading without it. A Lingo lingo whose name is a native lingo's or one of
its groups is set aside too: `input is already a part of windows, this computer's own lingo`.

**A native lingo** is a folder with a `lingo.json`, `{name, about, runs: "native", library}`, its name its folder's
and its library a file in it, like `windows.dll`. The library is written in C against `src/lingo.h` and
`src/platform.h`, linked with `bin/onsay-core.lib` (on Linux the core shares its words with `-rdynamic`), and its one
entry, `lingo_phrases`, calls `phrase_add(group, needs, line, fn)` for each phrase: `needs` every permission it uses,
like `"screen.act, screen.read"`, and `line` its signature, then `-> what it gives` and `: what it does`. A library is
loaded once and kept, so a changed one is used after a restart, and one whose folder is gone stops being offered.

**Lingo settings** are kept on this computer, never on Onsay: in `home/settings/<lingo>.json`, a record by name, or by
the lingo's own functions when its `lingo.json` says `keep` (`../docs/lingo.md`, section 11). `setting(...)` is
answered here as the line runs, and `--settings` and `--set` read and change them wherever they are kept.

## Files

| File | Job |
|------|-----|
| `src/lingo.h` | the core's words: values, buffers, phrases, running and compiling, shared with native lingos |
| `src/value.c` | values in an arena freed all at once, JSON, numbers written as JavaScript writes them, and headers |
| `src/compile.c` | the compiler: lex, parse, a Lingo lingo's listing made from its code, every lingo in the folder loaded, and a call of one linked down to native phrases |
| `src/run.c` | the runner, and its step, time and memory limits |
| `src/phrase.c` | the table of phrases this computer has |
| `src/std.c` | the core lingo's `std` group: reaching inside values and converting them |
| `src/core.h`, `src/core.c` | the engine: settings, the floor phrases `compute`, `store`, `http` and `settings`, `lingo(...)` and `use(...)` asked of Onsay, the lingos folder and the catalog, stops, and requests answered at their routes |
| `src/host.c` | the engine as a program: the command line, signing in, the connection, the heartbeat, reconnecting, the port and reading each request, and every job in turn on one thread |
| `src/platform.h` | everything the core needs from a device. Porting it is writing this for the new device |
| `src/platform_win.c` | that, for Windows: WinHTTP for the connection and `http.send`, so proxies and certificates work as for any Windows program, threads, files, and the port through Winsock |
| `src/platform_linux.c` | that, for Linux: pthreads, POSIX files, the port on plain sockets, `dlopen` for native lingos, and the web through libcurl, loaded as it starts, so it builds with nothing of Linux's and says to install libcurl when it's missing |
| `install.sh` | puts it on a Linux server: the program for its processor, its folder (`~/.onsay`, or `/var/lib/onsay` as root) with `core.json` in it, and a systemd service. It runs bare unless given `--sign-in`, and `--network` answers other computers too |
| `test/server.mjs` | a stand-in for Onsay that runs every test against a real core |
| `test/lingos/` | the lingos the tests use, beside the library's and the device's |

## Build and test

```
./build.ps1                                 bin/onsay-core.exe, bin/onsay-core.lib, bin/test/onsay-core.exe, bin/lingos
                                            (windows and native with their libraries), and bin/linux-x64/onsay-core and
                                            bin/linux-arm64/onsay-core, for glibc 2.28 or later, all built on Windows with
                                            zig, one download, no installer
node test/server.mjs                        a stand-in for Onsay, then in another terminal:
bin/test/onsay-core.exe test/onsay.json     runs every test, each result shown on the stand-in's side
sh install.sh ./bin/linux-x64/onsay-core    on a Linux server: installs that one. With nothing after it, it downloads it
```

The test build talks to the stand-in on this computer instead of Onsay. Its home is `test/data`, made fresh each run,
with a lingos folder of the test lingos, every lingo of `../lingo-library` and the device lingos as built. Every case is
a request, down the connection or to its port, 6970: the language and the core's phrases (`test/lingos/checks.lingo`),
limits and stops, settings, `lingo(...)` and `use(...)` asked of the stand-in while it reaches this computer, the
library, the windows lingo for real (`test/lingos/desk.lingo`), which may read the screen but never click or type, the
catalog, who reaches what, a bare core on port 6971, and coming back after a dropped connection.

A release is made by `../.github/workflows/release.yml` when a version tag is pushed.
