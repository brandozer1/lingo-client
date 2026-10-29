# The onsay lingo

What makes a lingo client Onsay's, as a native lingo, `onsay.lingo`, shipped with every Onsay distribution and removable
like any other lingo. With it stopped or gone the lingo client runs bare. It has no screen: an app, like Onsay Desktop,
shows what it does. How it talks to Onsay is the contract in the onsay repo's `docs/security.md`, *The onsay lingo and
the wire*.

- **Its connection** to Onsay, one WebSocket on threads of its own, reconnecting when it drops, while it runs and is
  signed in. Onsay reaches this computer's lingos down it, and its lingos ask Onsay with `lingo(...)` and `use(...)`.
- **What this computer hosts** goes in its hello and again whenever it changes, `{type: "catalog", catalog}`: every
  running lingo as `{name, openapi}`, its whole manifest, or `{name, error}` for one that can't load. Onsay keeps each
  as a document of its index.
- **Signing in** keeps the token as its secret setting, `token`, and makes this device's key, kept by the device
  (`core/README.md`, `keys/`). Its hello carries the key, and Onsay's statement on it is checked as it is ready.
- **What Onsay signs is checked** against Onsay's key built in (`ONSAY_KEY` in `onsay.c`), never one told over the
  wire: its statement on this device's key, each request for the account's own, run as `me`, and the account's policy.
- **The account's policy** is fetched when Onsay says there's a new one, checked, kept unless its version is older,
  and what this computer's ceiling clamped is said back.
- **Every decision** of the lingo client goes to Onsay for the account's activity feed.
- **Everything it sends Onsay** is kept in the lingo client's home as `sent.log`, `{at, message}` a line, every token
  masked and pings left out, so the person can read what went: each message down the connection (`hello`, `catalog`,
  `lingo`, `response`, `prompt`, `applied`, `log`) and each call to Onsay's API, as `{type: "api", method, path, body}`.
- **Signed requests** from other devices are checked by the lingo client, each signer's key looked up on Onsay's
  `/resolve` with Onsay's statement on it, and kept 5 minutes.
- **`/onsay`**, open to anyone by its own default rule, takes prompts with its prompt key, its secret setting
  `prompt_key`, made on its first start, as `Authorization: Bearer <prompt key>`:

  | Request | Answer |
  |---------|--------|
  | `POST /onsay` `{prompt, session}` | `{session, text}` or `{session, questions}`, Onsay's answer. With no session, a new one |
  | `GET /onsay/sessions` | `[{session, prompt}]`, the newest first, each named by its first prompt |
  | `GET /onsay/sessions/<session>` | `{session, turns}`, each turn `{prompt, text}` or `{prompt, questions}` |

  A session is the string of its prompts and answers, kept in the store on this computer.

## Signing in

From an app on this computer, through the lingo client's `/.lingo/call` (`{fn, values}`), or on any computer through
its `call`:

```
onsay-core core.json call onsay.sign_in [name]   signs this computer in from any device's browser. In a lingo client
                                                 that serves, as an app calls it, it gives {url, code} at once, the
                                                 address to open and the code it shows, and waits for Connect in the
                                                 background, even with the onsay lingo stopped, so nothing waits on it.
                                                 On the command line it says them on the log and waits, giving
                                                 {handle, key}. name is what Onsay calls it, its host name when left out
onsay-core core.json call onsay.status           {signed_in, signing_in, failed, running, connected, handle, device, key}:
                                                 signing_in while a sign in waits for Connect, failed why the last one
                                                 didn't finish, in words, running whether the onsay lingo runs, handle
                                                 and device once connected
onsay-core core.json call onsay.devices          [{name, online, seen}]: the account's computers, as Onsay knows them
onsay-core core.json call onsay.sign_out         this computer forgets its sign in and its key
```

## Files

| File | Job |
|------|-----|
| `onsay.c` | the library: the connection, the checks, the policy, `/onsay`, `sent.log` and signing in, against `../../core/src/lingo.h` and `platform.h` |
| `openapi.json` | what it is, and its two settings |

`../../core/build.ps1` builds it into `bin/lingos/onsay.lingo` for Windows, `bin/linux-x64/lingos/onsay.lingo` and
`bin/linux-arm64/lingos/onsay.lingo`, and a test build, `bin/test/lingos/onsay.lingo`, which talks to
`core/test/server.mjs` and trusts its test key.
