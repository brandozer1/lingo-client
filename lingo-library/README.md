# Lingo library

The standard lingos, written in Lingo for any computer. The language is only its syntax and `lingo(...)`, `use(...)`
and `setting(...)`, and the core every computer has gives only what Lingo can't write for itself, so splitting, case,
rounding, sorting, dates, web requests and AI are written here. Each is its code, `<name>.lingo`, or a folder of its
name when it has settings, its `lingo.json` beside its code. The first lines of each file list its functions.

| Lingo | What it is for | Calls |
|-------|----------------|-------|
| `text` | splitting, joining, replacing, trimming, case, padding, slugs, templates, words and lines | |
| `list` | sorting, records by a part, and reversing | |
| `math` | rounding to the nearest, up or down, to places, and min and max | |
| `json` | JSON read with a fallback, written with an indent, a value at a path, deep merging, and a value's kind | `text` |
| `encode` | base64, web address parts as %XX, HTML entities and hex, on UTF-8 bytes | `text`, `math` |
| `url` | web addresses taken apart and built again, paths joined, and queries to and from records | `encode`, `text` |
| `time` | dates and times: reading ISO text, formatting, adding, the time between, how long ago, days, and now in words | `text`, `math` |
| `request` | web requests with headers, a query, a body, a timeout and tries again, on `http.send` | `url`, `json`, `text` |
| `agent` | the library's AI, on the person's own key for Claude, ChatGPT, OpenRouter or any service that answers the way OpenAI does: `ask`, an agent that talks with the person and uses any lingo on the way, and `answer`, one answer for a program | |
| `writer` | writes a new lingo from a plain description, or changes one, reviewed and fixed by a second look | `agent`, `json`, `text` |

Each needs every lingo it calls beside it, and those they call: `writer` needs `agent`, `json` and `text`, and
`request` needs `url`, `json`, `encode`, `text` and `math`. The simplest is to copy them all.

## Putting them on a computer

Copy the lingos you want, files and folders as they are here, into the computer's lingos folder: on Windows,
`%AppData%\Onsay\core\lingos`, which Open lingos folder in the Onsay app's right click menu opens. The lingo client reads
them within 5 seconds. The Windows app ships `agent` beside `windows`.

`agent` needs one key, and `writer` uses agent's. Settings are kept on this computer, never on Onsay: set them in the
Onsay app's Lingos settings, or with `onsay-core core.json --set agent claude_key "your key"` (`../core/README.md`).

## Writing one

A lingo's first comment lines say what it is, then an empty `//` and a numbered legend of its functions, and a blank
line. Each `fn` has its comment right above it, starting with its number: what it gives, when to use it, and plainly
when not to, in the words a person would ask with. Every `fn` is shared, so a helper's comment says so: "For request's
own use". How a lingo is made, and its `lingo.json`, are in section 11 of `../docs/lingo.md`.
