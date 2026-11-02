# Lingo library

Lingos written in Lingo that aren't part of the language. `agent` ships with every install; `coding` is here to try and
is not shipped. The basics, like splitting, sorting, JSON, dates and web requests, are standard functions built into the lingo client
(`../lingos/standard/`). A lingo here is a folder of its name: its code, `<name>.ls`, and when it has settings, its
`openapi.json`. The first lines of its code list its functions.

`agent` is the library's AI: Onsay's own, free for 30 days and metered on the account, or the person's own key for
Claude, ChatGPT, Gemini, Grok, OpenRouter or any service that answers the way OpenAI does. Its `ask` talks with the person
through the whole conversation, its log with every tool it used, uses any lingo on the way, says a step for each tool so
the person can follow it from any computer, and asks one or two short questions when it must, in the person's chat where
there is one, or gives a program one answer. After 7 minutes or 60 turns it says what is done and what is left, so the
work can go on. It is also the person's AI for every other lingo: `prompt` gives one answer as text, and `chat` the
model's next message for a lingo running its own loop.

`coding` is Coding Agent: an AI software engineer for a project folder, made in Lingo with strong inspiration from
OpenCode (MIT, copyright opencode), none of its text copied. It reads, searches, writes and edits code, runs commands and
checks its work, and finds its AI through the lingo function, like `agent.chat`, never a key of its own. Its prompts and
PowerShell live in the code itself, one file.

## Putting one on a computer

`../core/build.ps1` packs `agent` into `core/bin/lingos/agent.lingo`, and the Windows app ships it. To add a lingo by hand,
put its `.lingo` file, or its folder, in the computer's lingos folder: on Windows, `%AppData%\Onsay\core\lingos`, which
Open lingos folder in the Onsay app's right click menu opens. The lingo client reads it within 5 seconds, stopped until
it is started: `onsay-core core.json start agent` (`../core/README.md`).

Settings are kept on this computer, never on Onsay: set them in the Onsay app's Lingos settings, or with
`onsay-core core.json set agent claude_key "your key"`.

## Writing one

A lingo's first comment lines say what it is, then an empty `//` and a numbered legend of its functions, and a blank
line. Each `fn` has its comment right above it, starting with its number: what it gives, when to use it, and plainly
when not to, in the words a person would ask with. Every `fn` is shared, so a helper's comment says so: "For request's
own use", which marks it `x-lingo-internal` in its manifest, so Onsay lists it nowhere. How a lingo is made, its
`openapi.json` and packing it, are in section 11 of `../docs/lingo.md`.
