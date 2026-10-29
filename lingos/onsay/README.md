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
  `lingo`, `response`, `decide`, `stop`, `applied`, `log`) and each call to Onsay's API, as `{type: "api", method, path, body}`.
- **Signed requests** from other devices are checked by the lingo client, each signer's key looked up on Onsay's
  `/resolve` with Onsay's statement on it, and kept 5 minutes.
- **`/onsay`**, open to anyone by its own default rule, takes prompts with its prompt key, its secret setting
  `prompt_key`, made on its first start, as `Authorization: Bearer <prompt key>`:

  | Request | Answer |
  |---------|--------|
  | `POST /onsay` `{prompt, session, mode}` | `{session, text, steps}` or `{session, questions, steps}`, the reply and every step it took. With no session, a new one. `mode` is `run`, the default, or `decision`, where the person picks every call |
  | `GET /onsay/sessions` | `[{session, prompt}]`, the newest first, each named by its first prompt |
  | `GET /onsay/sessions/<session>` | `{session, turns}`, each turn `{prompt, text}` or `{prompt, questions}`, and `messages`, what its loop added to the log |
  | `GET /onsay/sessions/<session>/live` | `{running, steps, questions}`: while a prompt of it runs, its newest 100 steps in plain words, each `{text, depth}`, and the questions it waits on the person for, as a prompt's answer has them, or `null` |
  | `POST /onsay/sessions/<session>/answers` `{answers}` | the person's answers to those questions, in order. `{}`, or a 404 when nothing waits |

- **The agent loop.** Each prompt is the callback side of an agent loop, with Onsay's lingo function in the AI's seat
  (the onsay repo's `docs/lingo-function.md`, *The loop*):
  1. The whole log goes to Onsay, `{type: "decide", id, messages, mode}`: the session's newest 20 turns in the chat
     messages OpenAI and OpenRouter take, each turn's prompt the user's, `{role, content}`, then the messages its loop
     added, then this prompt and what its calls have given so far.
  2. Onsay sends back one decision: `{call, does, where, inputs, missing}`, or `{reply}`.
  3. The call runs here, as `me`, so this computer's own rules apply: a lingo on this computer, an address starting with
     its own name like `laptop.brandon/`, is handed in as a request, `POST` to its path with `inputs` as its body; any
     other address goes through `use(...)`, as Onsay reaches it. The call joins the log as
     `{role: "assistant", tool_calls: [{id, type: "function", function: {name: "use", arguments: "{address, values}"}}]}`,
     its values without the log itself, and what it gave as `{role: "tool", tool_call_id, content}`: its text, or its
     JSON, or `It failed: ...` with why.
  4. Back to 1, until the decision is the reply, at most 8 calls, then "That needed more steps than I take at once, so
     I stopped here. Ask again to go on." The reply is the last message of the turn.

  In decision mode each call is first a card at `/live`, "Which should I do?" with each of the decision's `choices` as
  "what it does, where" and "None of these", which ends the prompt with "Okay, I didn't do any of those. Say it another
  way and I'll look again.", and the pick runs. A value a call needs that nobody gave, its `missing`, is asked of the
  person first, "What city should I use?", the answer read as JSON when it is JSON. A card waits 2 minutes. A call whose
  answer has `questions` ends the prompt with them, and their answers come as the next prompt, a line each. A decision
  that carries `value`, a lingo Onsay's library wrote and ran for it, joins the log without running again.

  Its steps are kept as it goes for `/live`: those Onsay sends as it decides (`{type: "step"}`), and its own, "Picked:
  ..." with what the call does, "Running it ..." with where, "Waiting for you to choose" and "Waiting for your answer".
  A stop ends the prompt at once, the call running here with it and a decision at Onsay too (`{type: "stop"}`), and
  what it did stays in the session. An app that follows a prompt as it runs names its session itself, the way the
  onsay lingo makes one: the time in milliseconds as 12 hex digits, then 6 random ones. The lingo client answers its
  requests in turn, so while a call is busy in one long run, like the agent waiting on its AI, `/live` answers once
  that run ends; the answer has every step.

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
