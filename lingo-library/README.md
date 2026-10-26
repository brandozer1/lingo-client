# Lingo library

Lingos written in Lingo: web requests, JSON, text, lists, numbers, web addresses, encodings, time, AI models through
OpenRouter, a chat agent, and a writer of lingos. Each is its code, `<name>.lingo`, one file, or a folder of its name when
it has settings, its `lingo.json` beside its code saying what they are: `agent/`, `openrouter/` and `writer/` (section 11
of `../docs/lingo.md`). Each is meant to be put in a computer's lingos folder, where it runs. Onsay keeps none of them:
it reads what each one shares from its code, and relays to the computer that has them. One lingo calls another in the
same folder by its dotted name, like `request.get(...)`, with nothing to list. Each needs nothing but the core, the floor
every computer has: `std`, `compute`, `store` and `http` (section 15 of `../docs/lingo.md`). The language is only its
syntax and `lingo(...)`, `use(...)` and `setting(...)`, so splitting, joining, case, rounding, sorting and dates are
written here in Lingo: `text`, `list`, `math` and `time`.

## Putting them on a computer

Copy the lingos you want, files and folders as they are here, into the computer's lingos folder, with every lingo each
one calls beside it: on
Windows that is `%AppData%\Onsay\core\lingos`, which Open lingos folder in the Onsay app's right click menu opens. The
core reads them within 5 seconds, and the dashboard's Lingos page then shows each function with its address, like
`laptop.brandon/lingos/agent.lingo/ask`. What each one calls:

- `agent` needs nothing else. `writer` needs `openrouter`, `request.lingo`, `url.lingo`, `json.lingo`, `encode.lingo`,
  `text.lingo` and `math.lingo`, and `openrouter` needs `request.lingo` and those it needs.
- `request.lingo` needs `url.lingo`, `json.lingo`, `encode.lingo`, `text.lingo` and `math.lingo`. `url.lingo` needs
  `encode.lingo` and `text.lingo`, `encode.lingo` needs `text.lingo` and `math.lingo`, `json.lingo` and `time.lingo`
  need `text.lingo` (and `time.lingo` `math.lingo`). `text.lingo`, `list.lingo` and `math.lingo` need nothing.

The simplest is to copy them all.

Then set a key in that lingo's settings in the Onsay app, kept on that computer and never on Onsay's servers: for
`agent`, `claude_key`, `openai_key` or `openrouter_key`, and for `openrouter` and `writer`, openrouter's `key`. On a
server, `onsay-core core.json --set openrouter key "your key"` does the same (*Lingo settings* in `../core/README.md`).
The Windows app ships `agent` beside `windows` (`../lingos`). `core/test/compiler.mjs` checks each one reads and
compiles, and `core/test/server.mjs` copies every lingo here, files and folders, into its test core's folder and runs
them.

## The web: `http.send` and `use(...)`

Every core has one web request of its own, `http.send(method, url, headers=nothing, body=nothing, timeout=30)`, giving
`{status, headers, text}`: headers a record, by lower case name, body text, redirects followed, at most 4 MB back. It
needs the `net` permission. On Windows the core makes it with WinHTTP, and in WebAssembly its host does. `request` is
built on it, like axios on fetch: headers, a query, a body, a timeout and tries again. For one plain request, a GET or a
POST of JSON, `use("https://...", values)` is a line of the language itself.

## The lingos

| Lingo | Calls | Functions |
|---|---|---|
| `request` | `url`, `json`, `text` | `send(method, address, options)`, `get(address, options)`, `post`, `put` and `patch(address, body, options)`, `delete(address, options)`. options: `headers`, `query` (a record), `json` or `text` (the body), `timeout` (seconds), `retries` (more tries after a 429, a 5xx or a network that failed, waiting for Retry-After or twice longer each time). Each gives `{status, ok, headers, text, json}`, and never fails on a status: check `ok` |
| `json` | `text` | `parse(text, fallback)` (the fallback when it isn't JSON), `stringify(value, indent)`, `get(value, path, fallback)` with a path like `a.b[0].c`, `merge(a, b)` (deep), `kind(value)`: nothing, bool, number, text, list or record |
| `text` | | `split(text, sep)` (into characters when sep is `""`), `join(list, sep)`, `replace(text, find, with)`, `trim(text)`, `lower(text)` and `upper(text)` (Latin with its accents, Greek and Cyrillic), `pad(text, width, fill, at)`, `truncate(text, most, ending)`, `slug(text)`, `case(text, style)`: title, sentence, camel, pascal, snake, kebab or constant, `template(text, values)` for `"Hi {name}"`, `words`, `lines`, `repeat(text, times, sep)`, and `starts_with`, `ends_with` and `contains`, each with `ignore_case` |
| `list` | | `sort(list, part)`: smallest first, records by a part, equal ones in their order, and `reverse(x)`, a list or text |
| `math` | | `round(n, places, mode)`: mode `nearest`, `up` or `down`, places below 0 for tens and hundreds, and `min(a, b)`, `max(a, b)` |
| `url` | `encode`, `text` | `parse(address)` to `{scheme, user, host, port, path, query, hash}` with the query a record, `build(parts)`, `join(base, path)`, `to_query(values)`, `from_query(text)`, `encode(text)`, `decode(text)` |
| `encode` | `text`, `math` | `base64(text, url_safe)` and `from_base64`, `url` and `from_url` (%XX), `html` and `from_html` (entities), `hex` and `from_hex` |
| `openrouter` | `request` | `ask(prompt, model)`, `chat(messages, model)` and `reply(messages, tools, model)`: an AI model through OpenRouter, on the person's own key, set in their settings for `openrouter` with the model and address |
| `agent` | | `ask(prompt, chat, model)`: an AI agent on your own key, Claude, ChatGPT, OpenRouter, or any service that answers the way OpenAI does (its `url`), chat the turns before the prompt, which the model reads as the conversation so far. Its settings: `claude_key`, `openai_key`, `openrouter_key` (or openrouter's `key`), `url`, `service` when more than one key is set, and `model`. The model has two tools: `lingo(task, values)`, which finds a lingo that does a task and runs it, values an optional record by name, and `use(address, values)`, anything named by its address, for at most 8 turns, and only its reply comes back. Its guide tells the model what it is, that the messages before are its conversation with the person, to quote them when asked, what its tools are, and to use its lingo tool for anything it can't do itself instead of saying it can't. A `lingo(...)` never picks the function asking, and its `use` refuses an address of `agent.lingo`, so it never asks itself |
| `time` | `text`, `math` | `now()`, `parse(text)` for ISO dates, `format(time, pattern, offset)` with a pattern like `"dddd D MMMM YYYY, h:mm a"`, `add(time, amount, unit)` from seconds to years (back when below 0), `difference(a, b, unit)` (`"days"` for the days between two ISO dates), `ago(time, now)` like "3 minutes ago", `start_of_day(time, offset)`, `day_of_week(time, offset)`, and `now_in_words(zone)`: the date and time now as a person reads it, like "Tuesday 20 October 2026, 3:42 PM", in UTC (said so) when no zone is given |
| `writer` | `openrouter`, `json`, `text` | `write(request, chat, model)`: a new lingo from a plain description, and `revise(name, code, change, model)`: one changed as asked. Each gives `{name, file, code, notes}`, file its `<name>.lingo` to put in the lingos folder, notes what it made, the settings to set and the lingos it calls. A model drafts it, a second pass reviews it against a checklist, and a third pass fixes what it found, at most twice. Its code isn't loaded to check it: the computer does that when the file is put in its lingos folder, and the notes say so. Best with a strong coding model, set as its `model` setting, else openrouter's |

Times are seconds since 1970 in UTC, as `compute.now()` gives them, and ISO text is taken wherever a time is. `offset` is
hours from UTC, since a device's own zone isn't known to Lingo. `zone` is the same hours, as a number or text like `-4`,
`+05:30` or `UTC-4`, or a local time with its offset, like `2026-10-19T16:31:07-04:00`, the way a device gives its time.

## Comments

The first comment lines of each file say what the lingo is, and the comment lines right above each `fn` say what it
does: what it gives, when to use it, and plainly when not to, in the words a person would ask with, short and concrete.
So `agent.ask` is for anything said to Onsay in a chat, greetings and questions about the conversation too, and
`openrouter`'s functions say they are for a program wanting one answer, never for chatting with the person. Every `fn`
is shared, so a helper's comment says so: "For request's own use".

`encode`, and `text`'s `lower`, `upper`, `trim` and `split`, work on UTF-8 bytes through two small std phrases,
`std.bytes(text)` and `std.from_bytes(list)`, since Lingo has no other way to reach a character's code.

## Writing one

Write the `.lingo` file: its first comment lines say what it is, then an empty `//` and a numbered legend of its
functions, and a blank line. Each `fn` has its comment right above it, starting with its number. Its settings are its
`setting("...")` calls. When it has any, make it a folder of its name with a `lingo.json` beside the file, listing each
with its `title` and `about`, and as it needs `default`, `required` and `secret`, like `openrouter/lingo.json`, so the
Onsay app can show a form of them. A lingo of the library is called by its dotted name. The core reads the file and makes its
listing from it, and sets it aside with the reason when it doesn't load. A text in Lingo can't hold `{` or `}` unescaped,
since `{...}` puts a value in it: write `\{` and `\}`. Nor can it write a carriage return: `std.from_bytes([13])` is one.
