# Lingo library

Lingos written in Lingo that aren't part of the language. `agent` is the one that ships with every install. The basics,
like splitting, sorting, JSON, dates and web requests, are standard functions built into the lingo client
(`../lingos/standard/`). Each lingo here is a folder of its name: its code, `<name>.ls`, and when it has settings, its
`openapi.json`. The first lines of each file list its functions.

| Lingo | What it is for | Calls |
|-------|----------------|-------|
| `agent` | the library's AI: Onsay's own, free to start and metered on the account, or the person's own key for Claude, ChatGPT, OpenRouter or any service that answers the way OpenAI does: `ask`, an agent that talks with the person through the whole conversation, its log with every tool it used, uses any lingo on the way and asks one or two short questions when it must, in the person's chat where there is one, or gives a program one answer | |
| `writer` | writes a new lingo from a plain description, or changes one, reviewed and fixed by a second look | `agent`, `json`, `text` |

`writer` needs `agent` beside it.

## Putting them on a computer

`../core/build.ps1` packs each into its `.lingo` file in `core/bin/lingos`, and the Windows app ships them all. To add
one by hand, put its `.lingo` file, or its folder as it is here, in the computer's lingos folder: on Windows,
`%AppData%\Onsay\core\lingos`, which Open lingos folder in the Onsay app's right click menu opens. The lingo client reads
it within 5 seconds, stopped until it is started: `onsay-core core.json start text` (`../core/README.md`).

`agent` needs one key, and `writer` uses agent's. Settings are kept on this computer, never on Onsay: set them in the
Onsay app's Lingos settings, or with `onsay-core core.json set agent claude_key "your key"`.

## Writing one

A lingo's first comment lines say what it is, then an empty `//` and a numbered legend of its functions, and a blank
line. Each `fn` has its comment right above it, starting with its number: what it gives, when to use it, and plainly
when not to, in the words a person would ask with. Every `fn` is shared, so a helper's comment says so: "For request's
own use", which marks it `x-lingo-internal` in its manifest, so Onsay lists it nowhere. How a lingo is made, its `openapi.json` and packing it, are in section 11 of `../docs/lingo.md`.
