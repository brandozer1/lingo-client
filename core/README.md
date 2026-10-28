# Lingo client

The lingo client in C: a small program that runs lingos, built for Windows and Linux. It runs the lingos in its
computer's lingos folder and answers on its port, 6969, each running lingo at its own route, for whoever its policy lets
in: this computer always, anyone else only by an access rule. It comes bare: with no token in its settings it has no
connection to Onsay and answers its port only. Signed in, it keeps a connection open to Onsay, so Onsay can reach its
lingos and its lingos can ask Onsay with `lingo(...)` and `use(...)`. It has no screen of its own: the Windows app
(onsay-desktop) ships `bin/onsay-core.exe` and `bin/lingos`, and on a Linux server `install.sh` puts it there and keeps
it running.

The language, and what a lingo is, its `.lingo` file, its code, its `openapi.json` and `serve(request)`, are in
`../docs/lingo.md`.

## Settings

One JSON file, given as the first argument. Where Onsay is is built in, never a setting.

```json
{
  "token": "the computer's sign in token",
  "home": "onsay-data",
  "port": 6969,
  "steps": 5000000,
  "ms": 600000
}
```

| Setting | What it is |
|---------|------------|
| `token` | its sign in to Onsay. The Windows app writes it, and on a server `sign-in` does. With none it runs bare, and `lingo(...)` and `use(...)` fail with "This lingo client isn't connected to Onsay" |
| `install`, `name` | this computer's own id, which `sign-in` makes and keeps, so signing in again gets it back, and what `sign-in` calls it, its host name when left out. `name` also picks this computer's grants from the account's policy |
| `home` | its own folder, and its root: everything it keeps is in it (below), and its lingos reach files inside it freely |
| `lingos` | the lingos folder, `home/lingos` when left out |
| `port` | where it answers, 6969 when left out: on 127.0.0.1 alone, so the firewall never asks, until an access rule lets in `public` or an `ip`, then on every address the computer has, opened again within 5 seconds of a change |
| `steps`, `ms` | the most one request may take, in steps and milliseconds |

What it keeps in `home`:

| File | What it is |
|------|------------|
| `running.json` | the lingos running, a list of their names. A lingo newly put in the lingos folder is stopped until it is started |
| `policy.json` | who may reach what and what each lingo may do here, changed only on this computer (*Who may do what*) |
| `policy.account.json` | the account's policy, kept from Onsay, read when it is there |
| `decisions.log` | every decision about who may reach what and what a lingo may do, a line of JSON each |
| `settings/<lingo>.json` | a lingo's settings, unless it keeps them itself (`../docs/lingo.md`, section 11) |
| `store/` | Onsay's own storage, the `store` phrases', one file a key |
| `libraries/<lingo>/` | a native lingo's library, taken out of its `.lingo` file to be loaded |

## Who may do what

Every request answers two questions, and both must say yes: may this caller reach this lingo's route, and may this
lingo do what each phrase it runs needs, here.

**The caller.** A request from 127.0.0.1 or ::1 is this computer's, when its `Host` names this computer and no
`Origin` or `Sec-Fetch-Site` says a page elsewhere sent it. So is one Onsay sends for this computer's own account
(`owner`, *Messages*). This computer reaches every running lingo. Anyone else is known by their address, and through
Onsay's proxy by the first address in its `x-forwarded-for`, and reaches a lingo only when an access rule lets them in.
Anyone not let in gets a 404, as if the lingo weren't there.

**`policy.json`**, changed only on this computer, by hand, by an app through the control routes, or on the command line:

```json
{
  "ceiling": ["net", "store", "files.read", "files.write", "screen.read", "screen.act", "run"],
  "overrides": {
    "access": [
      { "who": "public", "lingos": "page" },
      { "who": "ip 192.168.1.0/24", "lingos": "*" },
      { "who": "ip 203.0.113.7", "lingos": "photos", "routes": ["/share"] }
    ],
    "block": ["198.51.100.23"],
    "grants": { "photos": ["files.read C:\\Users\\me\\Pictures"], "desk": ["run", "screen.read"] }
  },
  "defaults": { "access": [{ "who": "my devices", "lingos": "*" }] }
}
```

- **`ceiling`** is the most this computer ever allows, every need when left out. Nothing from Onsay can raise it.
- **`access`** rules say who may reach what. `who` is `public`, anyone, `ip` and an address or a range, like
  `ip 10.0.0.0/8` or `ip 2001:db8::/32`, or a handle like `james` or `my devices`, which name signed callers, so they
  match no one until requests are signed. `lingos` is a lingo's name, a list of them, `"*"` for every one, or `"none"`,
  which lets that caller reach nothing. `routes`, when given, are the only routes they may reach, after the lingo's own,
  like `/share` for `/photos/share`. Rules are read in order, the overrides' first: the first that names the caller and
  the lingo and route lets them in, and one that names the caller with `"none"` keeps them out.
- **`block`** is the deny list: addresses and ranges that reach nothing, whatever the rules say.
- **`grants`** say what each lingo may do beyond the routine: a need, or `files.read` or `files.write` and a path,
  which covers everything inside it. `~` at a path's start is the person's own folder.

**The policy in force** is the account's, `policy.account.json`, its grants for this computer by its `name`, with the
`overrides` on top, checked first; with no account policy, the `defaults` with the `overrides` on top. Every grant is
clamped to the ceiling. It is read as each request comes, so a change holds at once.

**At every phrase**, each need it has must be under the ceiling and said in its lingo's `openapi.json` for the route
the request reached (`x-lingo-needs`). Then `net`, `store` and files inside `home` are given. Files outside `home`,
`run`, `screen.read` and `screen.act` need a grant, else the person's yes: when an app is open (it asked for the
questions in the last 10 seconds), the lingo client asks it and waits up to a minute, answering other requests
meanwhile. `once` allows it for this request, `always` writes the grant into `policy.json`'s overrides, and `no`
refuses it. With no app open it is refused, saying how to grant it. A refusal is a 403.

**`decisions.log`** has a line for each decision but the routine: each request from anyone but this computer, let in or
not, and each need given by a grant or an answer, or refused.

```json
{"at":"2026-10-28T14:00:00Z","caller":"ip 203.0.113.7","lingo":"photos","route":"/photos/share","need":null,"allowed":true,"why":"rule: ip 203.0.113.7"}
{"at":"2026-10-28T14:00:01Z","caller":"this computer","lingo":"desk","route":"/desk/tidy","need":"run","allowed":true,"why":"asked: once"}
```

## Command line

```
onsay-core settings.json                      runs it
onsay-core settings.json --sent               and prints every message it sends Onsay on stdout too, {"sent": the
                                              message} a line, so the program around it can keep a log the person reads
onsay-core settings.json <verb> ...           does one thing and prints its answer as JSON, whether or not it runs
```

| Verb | What it does |
|------|--------------|
| `pack <folder> [<to>]` | makes the folder of a lingo's files into its `.lingo` file, in `to` or beside the folder (`../docs/lingo.md`, section 11) |
| `start <lingo>`, `stop <lingo>` | a lingo running or stopped. A running lingo client sees it within 5 seconds |
| `share <lingo> <who> [--route <r>]` | adds an access rule: `share photos ip 192.168.1.20 --route /share`. `<lingo>` may be `"*"` |
| `grant <lingo> <need>` | adds a grant: `grant desk run`, `grant photos files.read C:\Users\me\Pictures` |
| `ceiling <needs...>` | sets the ceiling: `ceiling net store files.read` |
| `policy` | prints the policy in force, `{ceiling, access, block, grants}`, and `local` and `account`, the files as they are |
| `settings <lingo>` | prints every setting of a lingo, each with its value now, for a form of them |
| `set <lingo> <key> <value>` | changes one, wherever that lingo keeps it. The value is JSON, like `"metric"` or `5`, or text as it is when it isn't JSON |
| `sign-in` | signs this computer in to Onsay from any device's browser: it says an address and a code, waits while the person signs in there and presses Connect, and writes the token into the settings |

The windows lingo also writes a line of JSON on stdout for each thing the app draws: Onsay's pointer gliding and
clicking, a ring around something on screen, and his panel (`show` in `../lingos/windows/windows.c`).

## Control routes

The same verbs for an app on this computer, at `/.lingo/<verb>`, answering only this computer's own requests to its
port, never one through Onsay: a `GET` with its values in the query, or a `POST` of them as a JSON record by name. Each
answers what the command line prints, or `{error}` with a 400.

| Route | Values | Answer |
|-------|--------|--------|
| `/.lingo/start`, `/.lingo/stop` | `lingo` | `{lingo, running}` |
| `/.lingo/asks` | | the questions waiting, each `{id, lingo, need, path, caller}`. Asking makes the app count as open for 10 seconds, so an app asks every second or so |
| `/.lingo/answer` | `id`, `answer`: `once`, `always` or `no` | `null` |
| `/.lingo/settings`, `/.lingo/set` | `lingo`, and `key` and `value` | as on the command line |
| `/.lingo/policy` | | the policy in force, with the files |
| `/.lingo/share`, `/.lingo/grant`, `/.lingo/ceiling` | `lingo`, `who` and `route`; `lingo` and `need`; `needs`, a list | the policy in force |

## Requests

Each running lingo answers at its route: `/<name>`, or the `route` its `openapi.json`'s `x-lingo` names. A request
goes to the running lingo with the longest route its path starts with, over HTTP/1.1, one request a connection, its body
text of at most 4 MB sent with a `Content-Length`.

- **A lingo with `serve(request)`** gets every request to its route (`../docs/lingo.md`, section 11).
- **Any other** is called function by function: `POST /<name>/<function>` with its values as a JSON record by name, or
  a list in order, or `GET` with its values in the query. `/<name>` alone is its `main`, and a native lingo's are
  `/<name>/<group>/<function>`, like `/windows/window/list`. A value left out takes its default. The answer is the
  function's value as JSON.
- **`GET /`** lists the lingos, `{lingos: [{name, route, running}]}`: for this computer every lingo, a broken one with
  its `error` in place of its route, and for anyone else each running lingo they may reach.
- **An error** is `{error}` as JSON, in words: 400 for what isn't a web request, a body that isn't text, or a value the
  function doesn't take, 403 for a need the lingo may not have here, 404 for nothing there, or nothing the caller may
  reach, 411 for a body in chunks, 413 for one over 4 MB, and 500 for a function that failed, its message starting with
  its lingo's file and line when it failed in Lingo code.

**One at a time.** One request runs at a time, in the order they came. Each is read on its own thread (`src/host.c`),
so a stop still lands at once. A request that comes while another waits, on Onsay for `lingo(...)` or `use(...)` or on
the person's answer, runs inside that wait, so a lingo can be reached from inside another's run, and an app's answer
comes in.

## Messages

What the lingo client and Onsay say over its connection, one JSON record each. Every `id` is any JSON value the sender
chose, sent back as it came.

| From Onsay | What it is |
|------------|------------|
| `{type:"ready", device}` | hello was taken |
| `{type:"request", id, method, path, headers, body, base64, owner}` | a request for this computer, answered as its port answers one. `path` has its query, `headers` are by lower case name, and `body` is text: one in base64 is answered 400, since lingos take text. `owner: true` marks Onsay's own, for this computer's account: its lingo function's pick, a `use(...)`, an answer sent on after an `@`. It counts as this computer, but never reaches the control routes. Without it, it is one the proxy passed on, its caller the first address in its `x-forwarded-for` |
| `{type:"stop", id}` | stops that request, between two steps or while it waits |
| `{type:"lingo", id, value}` or `{type:"lingo", id, error, code}` | the answer to a `lingo` message, below: the value the program goes on with, or the phrase's error |
| `{type:"pong"}` | the answer to its ping |

| To Onsay | When |
|----------|------|
| `{type:"hello", token, client, version, catalog}` | on connecting. `client` is `lingo-client` and `version` its own, `CORE_VERSION` in `src/core.h` |
| `{type:"response", id, status, headers, body, base64}` | a request answered: its status, its headers by lower case name, and its body as text, `base64` always false |
| `{type:"lingo", id, ask, values, gives, context, asking}` | a program reached `lingo(...)`, and waits. `id` counts up from 1. `context` is what was around it (`pl_context` in `src/platform.h`): `os`, `time` (local, with its offset), `zone`, `language`, and the `app` and `window` in front, those it knows. `asking` is the function the request is of, like `agent.ask`, so it is never the answer |
| `{type:"lingo", id, address, values}` | a program reached `use(...)`, and waits the same way |
| `{type:"catalog", catalog}` | the lingos folder, or which lingos run, changed |
| `{type:"ping"}` | every 20 seconds. One that answers pings and then goes silent for 60 is dropped, and it connects again |

**The catalog** is every running lingo, never its code and never the values of its settings: `{name, status: "ok",
about, settings, phrases, native}`, or `{name, status: "broken", error}` for one set aside, with why. `about` is its
`openapi.json`'s `info.description`, `settings` the names of its settings, `native` true for a native lingo, and each
of `phrases` is `{id, does, inputSchema, lingos}`: `does` is the comment above a Lingo lingo's `fn`, or a native
phrase's line after its signature, what it gives and what it does, and `lingos` is how many `lingo(...)` calls it
makes, through every function it reaches.

## The lingos folder

It reads the folder as it starts and again within 5 seconds of any change, to it or to `running.json`: every `.lingo`
file and every folder of a lingo's files. It loads the native lingos, compiles every Lingo lingo against all of them,
checks each says in its `openapi.json` all its code needs, and sets aside any that can't load with the reason, like
`broken.ls line 3: There's no lingo or group called nope here`, the rest loading without it. A Lingo lingo whose name is
a native lingo's or one of its groups is set aside too: `input is already a part of windows, this computer's own lingo`.
Every lingo here loads, running or not, so a running one can call a stopped one by its dotted name.

**A native lingo** is a library written in C against `src/lingo.h` and `src/platform.h`, linked with
`bin/onsay-core.lib` (on Linux the core shares its words with `-rdynamic`). Its one entry, `lingo_phrases`, calls
`phrase_add(group, needs, line, fn)` for each phrase: `needs` everything it uses, like `"screen.act, screen.read"`, and
`line` its signature, then `-> what it gives` and `: what it does`. A file phrase calls `may(r, need, path)` for each
path it reaches, so the core checks it against the root and the grants. A library is loaded once and kept, so a
changed one is used after a restart, and one whose lingo is gone stops being offered.

**Lingo settings** are kept on this computer, never on Onsay: in `home/settings/<lingo>.json`, a record by name, or by
the lingo's own functions when its `openapi.json`'s `x-lingo` says `keep` (`../docs/lingo.md`, section 11).
`setting(...)` is answered here as the line runs, and `settings` and `set` read and change them wherever they are kept.

## Files

| File | Job |
|------|-----|
| `src/lingo.h` | the core's words: values, buffers, phrases, running and compiling, shared with native lingos |
| `src/value.c` | values in an arena freed all at once, JSON, numbers written as JavaScript writes them, and headers |
| `src/compile.c` | the compiler: lex, parse, a Lingo lingo's listing made from its code, what each function needs, every lingo loaded together, and a call of one linked down to native phrases |
| `src/run.c` | the runner, and its step, time and memory limits |
| `src/phrase.c` | the table of phrases this computer has |
| `src/std.c` | the core lingo's `std` group: reaching inside values and converting them |
| `src/core.h`, `src/core.c` | the engine: settings, the floor phrases `compute`, `store`, `http` and `settings`, `lingo(...)` and `use(...)` asked of Onsay, `.lingo` files and folders, their manifests, running and stopped, the catalog, the policy and every check, the control verbs, stops, requests answered at their routes, and pack |
| `src/zip.c` | a file read out of a `.lingo` file, stored or deflated, and one made |
| `src/puff.c`, `src/puff.h` | inflate, for a deflated file in a zip: zlib's puff, by Mark Adler, as it comes (`../NOTICE`) |
| `src/host.c` | the engine as a program: the command line, signing in, the connection, the heartbeat, reconnecting, the port and reading each request, and every job in turn on one thread |
| `src/platform.h` | everything the core needs from a device. Porting it is writing this for the new device |
| `src/platform_win.c` | that, for Windows: WinHTTP for the connection and `http.send`, so proxies and certificates work as for any Windows program, threads, files, and the port through Winsock |
| `src/platform_linux.c` | that, for Linux: pthreads, POSIX files, the port on plain sockets, `dlopen` for native lingos, and the web through libcurl, loaded as it starts, so it builds with nothing of Linux's and says to install libcurl when it's missing |
| `install.sh` | puts it on a Linux server: the program for its processor, its folder (`~/.onsay`, or `/var/lib/onsay` as root) with `core.json` in it, and a systemd service. It runs bare unless given `--sign-in` |
| `test/server.mjs` | a stand-in for Onsay that runs every test against a real core |
| `test/lingos/` | the lingos the tests use, a folder each, beside the library's and the device's |

## Build and test

```
./build.ps1                                 bin/onsay-core.exe, bin/onsay-core.lib, bin/test/onsay-core.exe, bin/lingos
                                            (windows, native and every lingo of ../lingo-library, each a .lingo file
                                            packed by the lingo client itself), and bin/linux-x64/onsay-core and
                                            bin/linux-arm64/onsay-core, for glibc 2.28 or later, all built on Windows with
                                            zig, one download, no installer
node test/server.mjs                        a stand-in for Onsay, then in another terminal:
bin/test/onsay-core.exe test/onsay.json     runs every test, each result shown on the stand-in's side
sh install.sh ./bin/linux-x64/onsay-core    on a Linux server: installs that one. With nothing after it, it downloads it
```

The test build talks to the stand-in on this computer instead of Onsay. Its home is `test/data`, made fresh each run,
with a lingos folder of the test lingos as folders, `memo` packed into `memo.lingo`, every `.lingo` file of
`bin/lingos`, and a deflated and a broken one made by the stand-in, all running under a policy it writes. Every case is
a request, down the connection or to its port, 6970, or a command: the language and the core's phrases
(`test/lingos/checks/checks.ls`), limits and stops, settings, `lingo(...)` and `use(...)` asked of the stand-in while it
reaches this computer, the library, the windows lingo for real (`test/lingos/desk/desk.ls`), which may read the screen
but never act on it, the catalog, packing and loading, running and stopped, access by address, range and route, blocks,
the account policy, grants, the ceiling, the root, the person's answers through an app, the decision log, the control
routes, every command, a bare core on port 6971, and coming back after a dropped connection.

A release is made by `../.github/workflows/release.yml` when a version tag is pushed.
