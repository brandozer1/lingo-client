# Lingo library

Lingos written in Lingo that aren't part of the language. `ai` and `agent` ship with every install; `browser`
ships with the Onsay app only, not the lingo client; `coding` is here to try and is not shipped. The basics, like splitting, sorting, JSON, dates and web requests, are standard functions built
into the lingo client (`../lingos/standard/`). A lingo here is a folder of its name: its code, `<name>.ls`, and when it
has settings or types, its `openapi.json`. The first lines of its code list its functions.

| Folder | What it is |
|--------|------------|
| `ai/` | The person's AI for every lingo: Onsay's own, free for 30 days and metered on the account, or their own key for Claude, ChatGPT, Gemini, Grok, OpenRouter or any service that answers the way OpenAI does. `prompt` gives one answer as text, and `chat` the model's next message for a lingo running its own loop, pictures included. It keeps the one key, so nothing else does |
| `agent/` | Onsay's agent, on `ai.chat`: talks with the person through the whole conversation, uses the lingos on the computer before doing anything by hand, makes and changes lingos so work done once is never done again, says a step for each tool, and asks one or two short questions when it must. After 7 minutes or 60 turns it says what is done and what is left |
| `browser/` | A web browser for lingos and agents, shipped with the Onsay app only: opens, reads, clicks, types, runs scripts and takes screenshots in LibreWolf, a fully open source Firefox with no telemetry, which it fetches on first use and drives over WebDriver BiDi with the core's `socket` phrases |
| `coding/` | Coding Agent, not shipped: an AI software engineer for a project folder |

`agent` knows the lingos running on its computer from `lingo.list()`, reads one with `lingo.code(name)` and writes or
changes one with `lingo.save(name, code)`, which starts it at once (`../docs/lingo.md`, section 15). It keeps each lingo
to one thing and each function to one operation, and adds a function to a lingo that exists rather than making another.
An agent for another kind of work is written the same way: a loop, its tools and its guide on `ai.chat`.

`coding` is made in Lingo with strong inspiration from OpenCode (MIT, copyright opencode), none of its text copied. It
reads, searches, writes and edits code, runs commands and checks its work, and finds its AI through the lingo function,
like `ai.chat`, never a key of its own. Its prompts and PowerShell live in the code itself, one file.

## Putting one on a computer

`../core/build.ps1` packs `ai` and `agent` into `core/bin/lingos/`, and the Windows app ships them, with `browser`, which
the app packs itself as it builds (onsay-desktop `Onsay.csproj`). To add a lingo by
hand, put its `.lingo` file, or its folder, in the computer's lingos folder: on Windows, `%AppData%\Onsay\core\lingos`,
which Open lingos folder in the Onsay app's right click menu opens. The lingo client reads it within 5 seconds, stopped
until it is started: `onsay-core core.json start browser` (`../core/README.md`).

Settings are kept on this computer, never on Onsay: set them in the Onsay app's Lingos settings, or with
`onsay-core core.json set ai claude_key "your key"`.

## Writing one

A lingo's first comment lines say what it is, then an empty `//` and a numbered legend of its functions, and a blank
line. Each `fn` has its comment right above it, starting with its number: what it gives, when to use it, and plainly
when not to, in the words a person would ask with. Every `fn` is shared, so a helper's comment says so: "For request's
own use", which marks it `x-lingo-internal` in its manifest, so Onsay lists it nowhere. How a lingo is made, its
`openapi.json` and packing it, are in section 11 of `../docs/lingo.md`.
