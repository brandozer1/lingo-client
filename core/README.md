# Lingo client

The lingo client in C: a small program that runs lingos, built for Windows and Linux. It runs the lingos in its
computer's lingos folder and answers on its port, 6969, each running lingo at its own route, for whoever its policy lets
in: this computer always, anyone else only by a rule. Bare, it knows nothing beyond its port, and where Onsay says where an Onsay name goes (*Names*). A native lingo can
connect it to more through its hooks (*Hooks*): the onsay lingo (`../lingos/onsay`) connects it to Onsay, so Onsay and
the person's other devices can reach its lingos, and its lingos can ask Onsay with `lingo(...)` and `use(...)`. It has no
screen of its own: the Windows app (onsay-desktop) ships `bin/onsay-core.exe` and `bin/lingos`, and on a Linux server
`install.sh` puts it there with the onsay lingo and keeps it running.

The language, and what a lingo is, its `.lingo` file, its code, its `openapi.json` and `serve(request)`, are in
`../docs/lingo.md`.

## Settings

One JSON file, given as the first argument.

```json
{
  "home": "onsay-data",
  "port": 6969,
  "steps": 5000000,
  "ms": 600000
}
```

| Setting | What it is |
|---------|------------|
| `home` | its own folder, and its root: everything it keeps is in it (below), and its lingos reach files inside it freely |
| `lingos` | the lingos folder, `home/lingos` when left out |
| `port` | where it answers, 6969 when left out: on 127.0.0.1 alone, so the firewall never asks, until a rule written in the policy lets in `public` or an `ip`, then on every address the computer has, opened again within 5 seconds of a change |
| `steps`, `ms` | the most one request may take, in steps and milliseconds |
| `ask` | a web address or an Onsay name (*Names*) that answers `lingo(...)` and `use(...)` when no running native lingo does, at the command line too: each is POSTed there as JSON, as the `ask` hook is given it, and answered the same way (*Hooks*). Left out, they fail. Onsay runs its own library this way |

What it keeps in `home`:

| File | What it is |
|------|------------|
| `running.json` | the lingos running, a list of their names. A lingo newly put in the lingos folder is stopped until it is started |
| `policy.json` | the ceiling: the most any lingo may ever do here, changed only on this computer (*Who may do what*) |
| `policy.account.json` | the account's policy, kept as a native lingo hands it in (`lingo_policy`), with `device`, this computer's name on the account |
| `decisions.log` | every decision about who may reach what and what a lingo may do, a line of JSON each |
| `sent.log` | every message the onsay lingo sent Onsay, `{at, message}` a line, every token in it masked, for the person to read. Cut to its newest half past 4 MB |
| `settings/<lingo>.json` | a lingo's settings, unless it keeps them itself (`../docs/lingo.md`, section 11). The onsay lingo's sign in is its `token` here |
| `store/` | Onsay's own storage, the `store` phrases', one file a key |
| `keys/<name>` | this computer's own key pairs, like `device`: on Windows sealed by Windows (DPAPI), so the private key is never a plain file, and on Linux a file only its user can read |
| `libraries/<lingo>/` | a native lingo's library, taken out of its `.lingo` file to be loaded |

## Who may do what

Every request answers two questions, and both must say yes: may this caller reach this route, and may this lingo do what
each phrase it runs needs, here. A request from this computer is let in by its own, and local calls skip the first
question. The policy is ingress only.

**The caller**, from most to least trusted:

| Caller | Who it is | In the log |
|--------|-----------|------------|
| this computer | a request to the port from 127.0.0.1 or ::1, when its `Host` names this computer and no `Origin` or `Sec-Fetch-Site` says a page elsewhere sent it. It reaches every running lingo and the control routes | `this computer` |
| me | the account's own call, handed in by a native lingo that checked it (`lingo_hand`), like the onsay lingo for a request Onsay signed for this computer's account | `me` |
| my devices | a signed request from another computer on the same account | its keyid, like `laptop.brandon` |
| onsay | Onsay's own search and agent, signed `as: "onsay"` | `onsay` |
| a signed device | a request with an HTTP Message Signature (RFC 9421, Ed25519) that checks out, its signer's key looked up by the native lingo that knows keys (`key` in *Hooks*), so its account is known | its keyid, like `laptop.james` |
| anyone else | known by their address, the port's, or the one a native lingo hands in, like the first of a proxy's `x-forwarded-for` | `ip 203.0.113.7` |

A signed request's `Signature-Input` covers `@method`, `@target-uri` and `content-digest` at least, and has `created`,
`nonce` and `keyid`. It must check out, be made within 5 minutes, its nonce new in that time, and its `Content-Digest`
its body's sha-256. Its `@target-uri` is `x-forwarded-proto` (else `http`), `://`, `x-forwarded-host` (else `Host`), and
`x-forwarded-uri` (else the path), so a request a proxy passed on is checked as it was sent. One that doesn't check out
is a 401 that says why. Anyone not let in gets a 404, as if the lingo weren't there.

**The policy** is the account's, kept by Onsay and written to `policy.account.json` here by the onsay lingo
(`lingo_policy`). The website, the desktop app and the command line all edit the same one.

```json
{
  "version": 14,
  "device": "laptop",
  "default": "closed",
  "rules": [
    { "allow": true, "who": "me" },
    { "allow": true, "who": "my devices" },
    { "allow": true, "who": "onsay", "routes": ["/onsay"] },
    { "allow": true, "account": "acct_6acbe318259da01c630272e6", "handle": "james", "routes": ["/photos/share"] },
    { "allow": false, "ip": "203.0.113.0/24" }
  ]
}
```

`default` is always `"closed"`. `rules` is one list, read top to bottom: a rule that matches sets the answer, and a
later deny wins. If no rule matches, the caller stays out. A rule has:

| Field | Meaning |
|---|---|
| `allow` | `true` to let in, `false` to keep out. Required |
| `who` | one built-in: `me`, `my devices`, `onsay`, or `public` |
| `account` | an account id, `acct_...`. The permission key |
| `handle` | a handle or `handle#tag`, resolved to its account when the rule is written. The name shown |
| `ip` | an address or range, like `203.0.113.7` or `10.0.0.0/8` |
| `routes` | full paths, like `["/photos/share"]`. Absent means every route |
| `devices` | computer names, like `["laptop"]`. Absent means every computer |

A rule names its caller with one of `who`, a person (`account`, or a `handle` to resolve), or `ip`. A stored rule for a
person always has `account` and `handle`: the id is the key, and the handle is a label. `routes` and `devices` narrow the
rule. A route matches itself and everything under it, like `/photos` and `/photos/share`. There is no separate lingo
field: naming the route names the lingo.

`policy.json`, changed only on this computer, holds the ceiling:

```json
{
  "ceiling": ["net", "store", "files.read", "files.write", "screen.read", "screen.act", "run", "lingos"]
}
```

**The ceiling** is the most this computer ever allows, every need when left out. Nothing from elsewhere can raise it.
A policy stored in the old `{ access, grants, offline }` shape is read as the default policy at its stored version, so
nothing old is enforced.

**At every phrase**, each need the lingo's `openapi.json` declares for the route the request reached
(`x-lingo-needs`) must be under the ceiling. Then it is allowed: there is no per-caller grant and no prompt. `net`,
`store` and files inside `home` are routine; anything else is logged so the person can see what an app did. A need over
the ceiling is a 403.

**`decisions.log`** has a line for each request from anyone but this computer, let in or not, and each need over the
routine, allowed or refused. Each line also goes to every running native lingo that wants it, like the onsay lingo,
which sends it to Onsay for the account's activity feed.

```json
{"at":"2026-10-28T14:00:00Z","caller":"ip 203.0.113.7","lingo":"photos","route":"/photos/share","need":null,"allowed":true,"why":"rule: 203.0.113.7"}
{"at":"2026-10-28T14:00:01Z","caller":"this computer","lingo":"desk","route":"/desk/tidy","need":"run","allowed":true,"why":"allowed"}
```

## Command line

```
onsay-core settings.json                      runs it
onsay-core settings.json <verb> ...           does one thing and prints its answer as JSON, whether or not it runs
```

| Verb | What it does |
|------|--------------|
| `pack <folder> [<to>]` | makes the folder of a lingo's files into its `.lingo` file, in `to` or beside the folder (`../docs/lingo.md`, section 11) |
| `start <lingo>`, `stop <lingo>` | a lingo running or stopped. A running lingo client sees it within 5 seconds |
| `ceiling <needs...>` | sets the ceiling: `ceiling net store files.read` |
| `policy` | prints the policy in force, `{ceiling, default, rules, device}`, and `local` and `account`, the files as they are |
| `settings <lingo>` | prints every setting of a lingo, each with its value now, for a form of them |
| `set <lingo> <key> <value>` | changes one, wherever that lingo keeps it. The value is JSON, like `"metric"` or `5`, or text as it is when it isn't JSON |
| `manifest <lingo>` | prints a lingo's whole OpenAPI manifest, as the catalog has it, or why it can't load |
| `call <lingo.function> [values...]` | runs a function of a lingo, or a native lingo's phrase, as this computer, its values in order, each JSON or text, and prints its value: `call memo.recall milk`, `call onsay.sign_in` |

The base calls also write a line of JSON on stdout for each thing an app draws: Onsay's pointer gliding and clicking, a
ring around something on screen, and lines in the chat (`show` in `../lingos/windows/windows.c`).

## Control routes

The same verbs for an app on this computer, at `/.lingo/<verb>`, answering only this computer's own requests to its
port, never one handed in: a `GET` with its values in the query, or a `POST` of them as a JSON record by name. Each
answers what the command line prints, or `{error}` with a 400.

| Route | Values | Answer |
|-------|--------|--------|
| `/.lingo/start`, `/.lingo/stop` | `lingo` | `{lingo, running}` |
| `/.lingo/asks` | | the questions waiting, each `{id, lingo, need, path, caller}`, whether a lingo may, or `{id, lingo, questions, caller}`, a native lingo's questions for the person, like `chat.ask` (`lingo_ask`). Asking makes the app count as open for 10 seconds, so an app asks every second or so |
| `/.lingo/answer` | `id`, `answer`: `once`, `always` or `no`, or for questions the list of answers, in order | `null` |
| `/.lingo/settings`, `/.lingo/set` | `lingo`, and `key` and `value` | as on the command line |
| `/.lingo/manifest` | `lingo` | its whole manifest |
| `/.lingo/policy` | | the policy in force, with the files |
| `/.lingo/ceiling` | `needs`, a list | the policy in force |
| `/.lingo/call` | `fn`, and `values`, a list | the function's value |

## Names

Every web address the lingo client is given, to `http.send`, so to `request`, and as the `ask` setting, may name a host
on the web or an Onsay name, told apart by the one rule every Onsay program keeps: a host is on the web when it is an
IP, `localhost`, or a name with a dot ending in one of the web's top level domains (`tlds.txt`), like
`example.com`. Any other, like `laptop.brandon` or `brandon`, is an Onsay name, and the lingo client asks Onsay's
resolve endpoint, `https://api.onsay.dev/resolve/<name>` (`ONSAY` and `RESOLVE` in `lingo.c`, the only place it
is written), for its web address, then sends the request there, what followed the name after it. Today that is the free
proxy, `https://api.onsay.dev/proxy/laptop.brandon`. `http://` or `https://` may be left off, `https` for a name and
`http` for an IP. A signed request is signed over the address it is sent to, so it checks out as the proxy passes it
on (`x-forwarded-uri`, *Who may do what*).

## Requests

Each running lingo answers at its route: `/<name>`, or the `route` its `openapi.json`'s `x-lingo` names. A request
goes to the running lingo with the longest route its path starts with, over HTTP/1.1, one request a connection, its body
text of at most 4 MB sent with a `Content-Length`.

- **A lingo with `serve(request)`** gets every request to its route (`../docs/lingo.md`, section 11). A native lingo
  has one with a phrase `<name>.serve`, like the onsay lingo's `onsay.serve`.
- **Any other** is called function by function: `POST /<name>/<function>` with its values as a JSON record by name, or
  a list in order, or `GET` with its values in the query. `/<name>` alone is its `main`, and a native lingo's are
  `/<name>/<group>/<function>`, like `/onsay/window/list`. A value left out takes its default. The answer is the
  function's value as JSON.
- **`GET /`** lists the lingos, `{lingos: [{name, route, running}]}`: for this computer every lingo, a broken one with
  its `error` in place of its route, and for anyone else each running lingo they may reach.
- **An error** is `{error}` as JSON, in words: 400 for what isn't a web request, a body that isn't text, or a value the
  function doesn't take, 401 for a signature that doesn't check out, 403 for a need the lingo may not have here, 404
  for nothing there, or nothing the caller may reach, 411 for a body in chunks, 413 for one over 4 MB, and 500 for a
  function that failed, its message starting with its lingo's file and line when it failed in Lingo code.

**One at a time.** One request runs at a time, in the order they came, whether from the port or handed in. Each from
the port is read on its own thread (the host, in `lingo.c`), so a stop still lands at once. A request that comes while another
waits, on `lingo(...)` or `use(...)` or on the person's answer, runs inside that wait, so a lingo can be reached from
inside another's run, and an app's answer comes in.

## Hooks

A native lingo that connects the lingo client to more than its port fills `lingo_hooks` as it loads, with
`lingo_hook` (`lingo.h`, section 7). They are used only while it runs, in a lingo client that serves, never at the
command line.

| Hook | What it does |
|------|--------------|
| `running(catalog)` | called as it starts and each time the catalog changes, with the catalog as JSON, and with `NULL` as it stops |
| `decided(line)` | each line of `decisions.log` as it is written |
| `ask(values, n, stop, answer)` | answers `lingo(...)` and `use(...)`: `values` is `{ask, values, gives, context, asking, for}` or `{address, values, for}`, `for` the id of the request whose run asks, when one does, and the answer a record with `value`, or `error` and `code`. With no running lingo to ask, they are asked of the settings' `ask`, else fail with "This lingo client isn't connected to Onsay" |
| `key(a, keyid)` | a request's signer, by its keyid like `laptop.brandon`: `{key: "ed25519:<base64>", handle, mine}`, or `NULL` |
| `said(for, text)` | a line the run of request `for` says for the person, `chat.step` or `chat.say`: 1 when it sent it on to whoever asked, like the onsay lingo for a request Onsay sent, else 0 and it is a show line here |
| `called(line)` | each request this lingo client made, a line of JSON: `{at, ms, kind, address, ask, request {method, url, path, headers, body}, response {status, headers, body}, error, stopped}`, a key in a header or in the address taken out and each body cut to 4 KB. `kind` is `web`, `lingo` or `socket`, one for each WebSocket a lingo opened, as its run lets it go or as it fails to open (`../lingos/onsay/README.md`). The onsay lingo sends them to Onsay in chunks |

What the lingo client offers it back, from any thread:

| Word | What it does |
|------|--------------|
| `lingo_hand(request, n, caller, answered, arg)` | a web request from elsewhere, `{id, method, path, headers, body, base64}`, answered in turn as the port answers one. `caller` is `me` or `ip <address>`, and a signature on it is checked as on the port. `answered` gets `{id, status, headers, body, base64}` |
| `lingo_stop(id)` | stops the request whose id, as JSON, it is, between two steps or while it waits, with every request running inside its wait |
| `lingo_wait(done, stop, seconds)`, `lingo_done(done)` | waits on the job thread, answering requests meanwhile, until `done` is set from any thread |
| `lingo_ask(r, question, seconds)` | on the job thread, a question for the person, a record, waited for in `/.lingo/asks` until an app answers it, the run is stopped or `seconds` pass: its answer, `nothing` when none came, or `NULL` when no app is open to ask |
| `lingo_job()` | on the job thread, the id, as JSON, of the request running now, or `NULL` outside one |
| `lingo_push(path, text, n)` | one message for everything watching `path` on the port as a WebSocket. A GET to the port with `Upgrade: websocket` is answered like any other first, so every check applies, and on a 200 it stays open: what the lingo answered is the first message, then each push, until either side closes it. The agent loop streams `/onsay/screens` this way (`../lingos/onsay/agent.c`) |
| `lingo_policy(a, account, device, why)` | keeps the account's policy, with its version, as the one in force, its rules for this computer by `device`. Refused when older than the one kept. Gives an empty record, which callers report as `clamped`: there is nothing to clamp any more |
| `lingo_setting`, `lingo_set` | one of a lingo's settings, as this computer keeps them |
| `lingo_called(kind, address, ask, method, url, headers, body, n, got, error, stopped, ms)` | one request made, handed to every running native lingo with `called`: each `http.send` does, each socket is kept the same way, and a native lingo calls it for a request it makes itself |
| `lingo_home` | its home folder, as a whole path, for a file a native lingo keeps there, like the onsay lingo's `sent.log` |
| `lingo_said(text)` | on the job thread, a line the running request says, handed to the running native lingo that fills `said`: 1 when it took it |
| `key_of`, `key_sign`, `key_forget` | this computer's own key pairs, in `home/keys`, the private half never handed out |
| `sha256`, `base64`, `unbase64`, `ed25519_key`, `ed25519_sign`, `ed25519_check` | signing and checking (`lingo.c`, Signing) |

**The catalog** is every running lingo, never its code and never the values of its settings: `{name, openapi}`, its
whole manifest (`../docs/lingo.md`, section 11), or `{name, error}` for one set aside, with why.

## The lingos folder

It reads the folder as it starts and again within 5 seconds of any change, to it or to `running.json`: every `.lingo`
file and every folder of a lingo's files. It loads the native lingos, compiles every Lingo lingo against all of them,
checks each says in its `openapi.json` all its code needs, and sets aside any that can't load with the reason, like
`liar.ls: liar.fetch needs net, which its openapi.json doesn't say`, the rest loading without it. A call to what this computer
hasn't got loads, and asks the lingo search as it runs (`../docs/lingo.md`, section 6). A Lingo lingo whose name is
a native lingo's or one of its groups is set aside too: `input is already a part of onsay, this computer's own lingo`.
Every lingo here loads, running or not, so a running one can call a stopped one by its dotted name.

**A native lingo** is a library written in C against `lingo.h`, linked with
`bin/onsay-core.lib` (on Linux the core shares its words with `-rdynamic`). Its one entry, `lingo_phrases`, calls
`phrase_add(group, needs, line, fn)` for each phrase: `needs` everything it uses, like `"screen.act, screen.read"`, and
`line` its signature, then `-> what it gives` and `: what it does`, and `lingo_hook` when it has hooks. A file phrase
calls `may(r, need, path)` for each path it reaches, so the core checks it against the root and the ceiling. A library
is loaded once and kept, so a changed one is used after a restart, and one whose lingo is gone stops being offered.

**Lingo settings** are kept on this computer, never on Onsay: in `home/settings/<lingo>.json`, a record by name, or by
the lingo's own functions when its `openapi.json`'s `x-lingo` says `keep` (`../docs/lingo.md`, section 11).
`setting(...)` is answered here as the line runs, and `settings` and `set` read and change them wherever they are kept.

## Files

| File | Job |
|------|-----|
| `lingo.c` | the lingo client, whole, in sections: values and JSON, phrases, the compiler, the runner and its limits, `std`, signing, zips, the engine (where Onsay is, settings, the floor calls with `native.call`, Onsay names, `lingo(...)` and `use(...)`, lingos loaded, the catalog, the policy, callers, signatures and every check, the control verbs, keys, stops, requests at their routes, pack), Onsay (the link, offered to private lingos too, signing in, policy sync, the log sent), the host (the command line, the port, every request in turn on one thread), and the floor for Windows (WinHTTP, Winsock, DPAPI, `GetProcAddress`) and for Linux and Android (pthreads, POSIX files, plain sockets, `dlopen`, libcurl loaded as it starts) |
| `lingo.h` | what a lingo attaches with: values, buffers, phrases, running and compiling, signing, the hooks, and the floor of the computer it runs on (`pl_...`). Porting is writing that floor for the new device |
| `tlds.txt` | the web's top level domains, IANA's list, the same file Onsay keeps, made into `obj/tlds.h` by `build.ps1` for telling a name on the web from an Onsay name (*Names*) |
| `vendor/` | code taken as it came (`../NOTICE`): Monocypher for Ed25519, zlib's puff for deflated zips, and part of libffi for `native.call`, its C the same everywhere and each chip's own part (x86-64 and arm64) |
| `install.sh` | puts it on a Linux server: the program for its processor, the onsay lingo, running, its folder (`~/.onsay`, or `/var/lib/onsay` as root) with `core.json` in it, and a systemd service. `--sign-in` signs it in |
| `test/server.mjs` | a stand-in for Onsay, speaking the onsay lingo's contract, that runs every test against a real core |
| `test/lingos/` | the lingos the tests use, a folder each, beside the library's and the device's |

## Build and test

```
./build.ps1                                 bin/onsay-core.exe and bin/test/onsay-core.exe, with onsay and the base calls
                                            built in, bin/onsay-core.lib, bin/lingos (ai.lingo and agent.lingo, the lingos
                                            that ship, packed by the lingo client itself), and
                                            bin/linux-x64/onsay-core and bin/linux-arm64/onsay-core, with onsay and the base calls built in,
                                            for glibc 2.28 or later, all built on Windows with zig, one download, no
                                            installer
node test/server.mjs                        a stand-in for Onsay, then in another terminal:
bin/test/onsay-core.exe test/onsay.json     runs every test, each result shown on the stand-in's side
sh install.sh ./bin/linux-x64/onsay-core    on a Linux server: installs that one. With nothing after it, it downloads it
```

The test build of the onsay lingo talks to the stand-in on this computer instead of Onsay, and trusts the stand-in's
test key. The test core's home is `test/data`, made fresh each run, with a lingos folder of the test lingos as folders,
`memo` packed into `memo.lingo`, every `.lingo` file of `bin/lingos`, the onsay lingo's test build, and a deflated and a
broken one made by the stand-in, all running under a policy it writes, the onsay lingo signed in. Every case is a
request, down the onsay lingo's connection, signed by Onsay as the account's own or passed on from an address or a
signed device, or to its port, 6970, or a command: the language and the core's phrases (`test/lingos/checks/checks.ls`),
limits and stops, settings, `lingo(...)` and `use(...)` asked of the stand-in while it reaches this computer, web
requests to Onsay names, resolved by the stand-in, the library, the base calls for real (`test/lingos/desk/desk.ls`), which may read the screen but never act on it, the
catalog, packing and loading, running and stopped, ingress by address, range, route, handle, account and device,
signatures that check out and those that don't, deny rules that win, the account policy pushed, fetched, kept and
refused when older, the ceiling, the root, the person's answers through an app, the decision log and its lines
sent on, `/onsay`, its agent loop (calls run here, the step limit, a stop and Let me choose) and its sessions, `sent.log`, signing in and out from the command line and from an app, the control
routes, every command, `call` asking `lingo(...)` and `use(...)` of its settings' `ask`, a bare core with no onsay lingo
on port 6971, and coming back after a dropped connection and a statement that doesn't check out.

A release is made by `../.github/workflows/release.yml` when a version tag is pushed.
