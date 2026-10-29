# Lingo 1.0

**The language is its syntax and three calls: `lingo(...)`, `use(...)` and `setting(...)`.** The syntax is values,
names, operators and control flow. The three calls are references: `lingo(...)` asks for something in words,
`use(...)` reaches anything at an address, and `setting(...)` reads a lingo's setting, kept on the person's computer. Nothing else is part of
the language.

Lingo, LingoScript in full, its files `.ls`, is an orchestration language, made for AI to read and write as easily as
possible. On its own it does little but run code written in C. It is Turing complete, so any program can be written in
it, and lingos stack on lingos, so any computation, anywhere, is some number of layers of lingos or steps away. Every
function a program can call is a **phrase**, supplied by a **lingo**: Lingo code, or a native library for the computer,
in a `.lingo` file (section 11). Even what every computer has is a lingo, the core's own (section 15), never the
language. The language never changes from computer to computer, and only the lingos do.

This document describes the language exactly as the lingo client in `core/` runs it: `compile.c` reads lingos and
links them through the lingos they use, the same C on every computer, and `run.c` runs them.

## Contents

1. Words and layout
2. Functions
3. Values
4. Statements
5. Expressions
6. Calls: functions and phrases
7. lingo(...): asking in words
8. How a function runs
9. Errors
10. Lingos and what they need
11. A .lingo file: its program and its openapi.json
12. Where lingos run
13. The tree: what the compiler gives
14. Laid out and dense
15. The core: what every computer has
16. The Windows lingos

## 1. Words and layout

**Whitespace never matters.** A program means the same written on one line with no spaces as it does indented. A space
is only needed between two words, as in `return n`. Statements end where the next one begins, and `;` may separate them
anywhere, which is only needed when the boundary would otherwise be unclear.

```
fn f(n){if n<2{return n}return f(n-1)+f(n-2)}return f(15)
```

**Comments** start with `//` and run to the end of the line.

**Names** are letters, digits and `_`, not starting with a digit: `total`, `row_2`, `_tmp`. Case matters.

**Keywords** (19 words, plus `null`, which means the same as `nothing`):

| Kind      | Words                                          |
|-----------|------------------------------------------------|
| Control   | `if` `else` `while` `for` `in` `break` `continue` |
| Functions | `fn` `return` `tool`                           |
| Errors    | `try` `catch` `fail`                           |
| Logic     | `and` `or` `not`                               |
| Values    | `true` `false` `nothing`                       |

A keyword can't be a name, except after a `.`: `row.in`, `dev.fail(...)` and `{if: 1}` are all allowed.

`lingo(...)`, `use(...)` and `setting(...)` aren't keywords, but they are part of the language too (sections 6 and 7).

**Symbols:** `= += -= *= /=`, `+ - * / %`, `== != < > <= >=`, `&& || !`, `?? ? :`, `..`, `( ) [ ] { }`, `, . ;`

**Numbers** are digits with an optional fraction: `3`, `0.5`, `1048576`. A minus sign is the operator, so `-3` is minus
three.

**Text** is written in double quotes. Inside it:
- `\n` is a new line, `\t` a tab, and `\` before any other character is that character: `\"`, `\\`, `\{`.
- `{expression}` puts a value inside the text: `"Hi {name}, you have {n+1} new"`. The expression inside can't contain
  quotes: put such a value in a name first.

## 2. Functions

A lingo's code is a list of functions, in any order, each name once, and nothing else (section 11):

```
fn name(param, param=default, ...){ ...steps... }
```

- **Functions** are called by name, before or after where they are written. Their params are given in order. Called
  at the lingo's route, from outside it, they are given by name or in order (section 11).
- **Defaults** are expressions, worked out when the value is missing or `nothing`, left to right, so a default can use
  an earlier param: `fn f(a, b=a*2)`. A value with no default that is missing is `nothing`.
- `tool` is a keyword with no use in a lingo, whose file holds `fn` blocks only.

Functions are not values. They can't be stored in a name, passed, returned or built while running. That is what keeps
every program fully known before it runs.

## 3. Values

| Kind       | Written as                     | Notes                                                   |
|------------|--------------------------------|---------------------------------------------------------|
| nothing    | `nothing` (or `null`)          | What anything missing reads as                          |
| true/false | `true`, `false`                |                                                         |
| number     | `3`, `0.5`                     | 64 bit floating point, so whole numbers are exact to 2^53 |
| text       | `"hello"`                      | Unicode                                                 |
| list       | `[1, "a", [2]]`                | Ordered, counted from 0. A comma after the last item is allowed |
| record     | `{name: "Ann", "two words": 1}` | Parts named by text, kept in the order they were added. A part name is a name, a keyword or quoted text |

**Truth:** only `nothing` and `false` count as false. `0`, `""` and `[]` count as true.

**Copying:** setting a name copies the value, so two names never share one list or record.
`a=[1]; b=a; b+=[2]` leaves `a` as `[1]`.

**As text** (in `+` with text, in `{}` inside text, and wherever text is needed): `nothing` is empty, `true` and
`false` are those words, numbers are written plainly, and lists and records are written as JSON.

## 4. Statements

| Statement                                  | Does                                                     |
|--------------------------------------------|----------------------------------------------------------|
| `x = e`                                    | Sets a name                                              |
| `x.part = e`, `x[i] = e`                   | Sets a part of a record, or an item of a list            |
| `x += e`, `-=`, `*=`, `/=`                 | Short for `x = x + e` and so on. `list += [item]` adds to a list |
| `if c {...}`                               | Runs the block when `c` is true                          |
| `if c {...} else if c {...} else {...}`    | The first true branch, or the else                       |
| `while c {...}`                            | Repeats while `c` is true                                |
| `for x in e {...}`                         | Once for each item, part name, character or count, see below |
| `for k, v in e {...}`                      | The same, with the position or part name in `k` and the value in `v` |
| `break`                                    | Leaves the innermost loop                                |
| `continue`                                 | Skips to the next turn of the innermost loop             |
| `return e`, `return`                       | Ends the function or tool with a value, or with nothing  |
| `fail e`                                   | Stops with an error whose message is `e` as text         |
| `try{...}catch e{...}`                     | Runs the first block, and the second if it fails, with the error's message in `e` |
| `f(...)`, `group.phrase(...)`              | A call, run for what it does                             |

- Conditions need no parentheses: `if x>1 {`, `while n<10 {`, `for x in list {`. The condition ends at the block's `{`,
  which can't be mistaken for a record: a record only starts where a value is expected, and once the condition is a
  whole value nothing can continue it with `{`. So any expression works as a condition, records included:
  `if "a" in {a:1} {`. The one case to watch is a condition that is itself a record from its very first `{`, which
  must be in parentheses: `if ({a:1}).a==1 {`. Parentheses are otherwise just grouping, so `if(x>1){` and
  `for(x in list){` still read the same.
- Blocks are always in braces, even for one step.
- A step on its own must be a call. `x` alone, or `x+1`, is an error, because it would work something out and throw it away.
- `return` takes a value unless the next thing is `}`, `;` or the end.
- `break` and `continue` belong inside loops.

**What `for` goes through:**

| `e` is   | `for x in e` gives x                  | `for k, v in e` gives k and v           |
|----------|---------------------------------------|-----------------------------------------|
| a list   | each item                             | its position from 0, and the item       |
| a record | each part name                        | the part name, and its value            |
| text     | each character                        | its position from 0, and the character  |
| `a..b`   | each count from a to b, both included | the turn from 0, and the count          |
| nothing  | no turns                              | no turns                                |

Anything else is an error. A range in a `for` is counted, never built into a list first.

**Names** belong to the function or tool they are set in. Blocks don't make new ones: a name set inside a loop or an
`if` is still there after it. Reading a name that was never set gives `nothing`.

## 5. Expressions

From loosest to tightest:

| Level | Operators                          | Meaning                                                   |
|-------|------------------------------------|-----------------------------------------------------------|
| 1     | `c ? a : b`                        | `a` when `c` is true, else `b`. Groups to the right       |
| 2     | `a ?? b`                           | `a`, or `b` when `a` is nothing                           |
| 3     | `or`, `\|\|`                       | `a` when it is true, else `b`. `b` is only worked out when needed |
| 4     | `and`, `&&`                        | `b` when `a` is true, else `false`. `b` is only worked out when needed |
| 5     | `not`, `!`                         | `true` or `false`                                         |
| 6     | `== != < > <= >= in`               | One comparison: `a < b < c` is not allowed                |
| 7     | `a..b`                             | The list of whole steps from a to b, both included, empty when a > b |
| 8     | `+ -`                              |                                                           |
| 9     | `* / %`                            |                                                           |
| 10    | unary `-`                          |                                                           |
| 11    | `x.part`, `x[e]`, calls            |                                                           |

**Operators on values:**

| Operator     | Works with                                              | Else           |
|--------------|---------------------------------------------------------|----------------|
| `+`          | numbers add, lists join, and if either side is text, both join as text | error |
| `- * /`      | numbers                                                 | error          |
| `%`          | numbers. The remainder has the sign of the divisor, so `-1 % 3` is `2` | error |
| `/` and `%` by zero |                                                  | error          |
| `== !=`      | anything. Lists and records compare by their contents. Different kinds are never equal | |
| `< > <= >=`  | two numbers, or two texts (by character code)           | error          |
| `a in b`     | `b` a list: a is one of its items. `b` text: a, as text, is part of it. `b` a record: a is one of its part names | false |

**Reading parts and items** never fails:
- **Lists and text** take a whole number, counted from 0. Negative numbers count from the end, so `-1` is the last.
  Anything out of range, or not a whole number, gives `nothing`.
- **Records** take a part name, and give `nothing` when there is no such part.
- **Anything else** gives `nothing`, so `row.address.city` is `nothing` when there is no address.

**Setting parts and items:**
- **Lists** take a position from 0 up to their length, where the length adds a new item at the end. Negative counts
  from the end.
- **Records** take any part name.
- **Anything else** is an error.

## 6. Calls: functions and phrases

`group.name(a, b)` calls a phrase. Any name with a dot in front of `(` is a phrase. A group's name may have as many
levels as it needs to tell it apart, so `media.player.play()` is the phrase `play` in the group `media.player`.

**Local things need no operator.** A dotted call is always to something on this computer, checked before the program
runs, its name and how many values it takes, as below:

- **A group the core or a native lingo has,** like `std.length(x)`, `store.get(key)`, `http.send(...)` or the Windows
  lingo's `files.read(path)` and `window.list()`. A file is reached by its path, as text, through `files`:
  `files.read("~/notes/today.txt")`.
- **A lingo in the same lingos folder,** by its name: `text.slug(title)` calls the function `slug` of the lingo `text`,
  there beside it. The compiler finds it in the folder, so there is nothing to import or list.
- **Anything else doesn't compile,** and says to use `use(...)`: `There's no lingo or group called notes here. To reach
  one somewhere else, write use("its address"), like use("laptop.brandon/notes/add")`.

**`use(address, values)`** reaches anything somewhere else, in one line, and gives back its answer, like `lingo(...)` part
of the language. It is to Lingo what fetch is to JavaScript: for what is off this computer.

```
forecast = use("laptop.brandon/weather/forecast", {place: "Paris", days: 3})
windows = use("laptop.brandon/windows/window/list")
shown = use("laptop.brandon/check@desktop.brandon/show/print")
issues = use("https://mcp.example.com/mcp#search_issues", {repo: "onsay"})
page = use("https://api.example.com/v1/items?limit=5")
saved = use("https://api.example.com/v1/items", {name: "milk"})
```

- **A lingo at an address:** a place, then the lingo and its function: a computer, `laptop.brandon`, then the lingo's
  route and the function, `test/find`, the path its lingo client answers, `main` at the route alone, and the query any
  values. Onsay finds the place by its name. It runs where it lives, and only its answer comes back.
- **An MCP tool:** the server's URL and the tool after a `#`.
- **A plain web address**, any address on the web: a plain web request, a GET
  with no values and a POST of the values as JSON with some. The answer is read as JSON when it says it is JSON, else it
  is its text, and any status but 2xx fails in plain words with the status and the start of what it said:
  `https://api.example.com/v1/x answered 404 Not Found: no such item`. For headers, other methods or tries again, use
  the `request` lingo, on `http.send`.
- **After an `@`,** where the answer goes: it is sent on to that address as a plain call, the answer its first value.
  `https://` may be left off.
- It compiles to the phrase `lingo.use(address, values)`, every core's own, and is asked of Onsay when the line runs,
  through the onsay lingo (`lingos/onsay`). On a lingo client without it, it fails: `This lingo client isn't connected
  to Onsay`.
- **`values`** is a record, matched to the function's values by name, a list, in order, or nothing. With a record, a
  name it doesn't take fails (`... takes place, not city`), a required one left out fails (`... needs where`), and an
  optional one left out takes its default. A query and a record are merged, the record winning, and a query with a
  non-empty list fails.
- **Errors in writing it:** no address, an empty one, or more than two values. A lingo's own `fn` can't be called
  `use`: `use(...)` always means this.

**`setting(key)`** reads one of this lingo's settings, like a key or a token, kept on the person's own computer, also
part of the language:

```
key = setting("key")
units = setting("units") ?? "metric"
token = setting("token", "github")
```

- `setting("key")` is the lingo's own: the compiler fills in its name, like `agent`. `setting("key", "github")` reads
  another lingo's, so one token serves many lingos.
- It compiles to the phrase `lingo.setting(key, reference)`, every core's own, answered by the core on the computer when
  the line runs, never asked of Onsay: the value kept for that lingo on this computer, else the default its
  `openapi.json` gives, else `nothing`. Settings are never kept on Onsay's servers.
- A lingo's settings are those its `openapi.json` lists, with what each one is (section 11), then any other its code
  reads with `setting("...")`. The Onsay app builds a form from them, and the lingo client's `set` changes one
  (`core/README.md`).
- **Errors in writing it:** no key, or more than two values. A lingo's own `fn` can't be called `setting`:
  `setting(...)` always means this.

`name(a, b)` is settled once every function of the program is known, in this order:
1. **A function of the program** with that name, if there is one. Its own functions always come first.
2. **The one phrase with that name,** when only one group the program can use has it: `length(x)` is `std.length(x)`,
   and `running()` is `commands.running()`. The compiled tree holds the full name.
3. **Otherwise an error.** When two groups have the name, it says which: `write is in files and clipboard: write
   files.write or clipboard.write`.

Models write bare names out of habit, so allowing them where they can only mean one thing removes the most common
mistake, and saves the group's tokens on every call.

Every other call is checked before the program runs. A function must exist, and a phrase of a group the program can see
must be in its listing. The values must number between the params without defaults and all the params. A phrase's
values are given in the listing's order, and missing ones take its defaults when it runs.

Errors name what is wrong and what exists instead:
```
There's no phrase files.size. files has: read, write, add_to, exists, list, find, copy, move, delete, make_folder
std.slice(x, from, to=null) takes 2 to 3 values, not 1
list is in window and controls and files: write window.list or controls.list or files.list
There's no function or phrase called tidy. Write it with fn tidy(...){...}
There's no lingo or group called notes here. To reach one somewhere else, write use("its address"), like use("laptop.brandon/notes/add")
```

A `use(...)`'s errors come when its line runs, and a `try` catches them:
```
There's no route called forcast
laptop.brandon is offline, so laptop.brandon/text/slug can't run there now
There's no route called laptop.sarah
There's no handle called nobody
https://api.example.com/v1/x answered 404 Not Found: no such item
```

## 7. lingo(...): asking in words

```
weather = lingo("the weather in a city", {city: "Paris"}, "the forecast")
```

`lingo(ask, values, gives)` asks for something to be done in words, instead of naming a phrase. `ask` is what is wanted,
`values` is a record of the values to work with, and `gives` is what should come back. Only `ask` is needed. Any of them
may be worked out while the program runs, like a goal an AI chose: `lingo(goal, {city: city})`.

It is an ordinary call, made when the program reaches that line. The program asks Onsay, waits, gets back only the
answer, and goes on. Nothing is added to the computer. Every core compiles `lingo(...)` the same way, as the phrase
`lingo.run`, and asks Onsay over the onsay lingo's connection, exactly as for `use(...)` (section 6), saying which of its functions
asks, so it never gets itself as the answer.

There are two ways to reach a function: name it, with a dotted call here or `use(...)` elsewhere, or ask for it in
words with `lingo(...)` when you don't know which.

A lingo's own code may call `lingo(...)` too.

A lingo client with no onsay lingo running asks the web address its settings name as `ask` instead, the same way
(`core/README.md`, *Settings*). Onsay runs its own library like that.

## 8. How a function runs

1. A function is called at its lingo's route (section 11), its values given by name or in order, and defaults fill the
   rest.
2. Steps run in order. Every statement and every expression worked out is one **step**.
3. Its `return` value is the answer. With no `return`, the answer is `nothing`.

**Calling a function** gives it its own names, starting from its params. Its `return` is the call's value, or `nothing`.
A function may call itself, up to 1000 deep.

**Calling a phrase** works out its values in order, a value left out or `nothing` taking its default, checks its
needs (section 10), runs it, and gives back what it returned, or `nothing`. Values in a list or record are worked
out left to right, so phrases always run in the order they are written.

**Limits:** a call stops with a `limit` error after too many steps or too long, the lingo client's settings `steps` and
`ms` (5,000,000 steps and 10 minutes when left out). Stopping it from outside, like Onsay's stop key, ends it between
two steps with `stopped`.

## 9. Errors

An error stops the function, unless a `try` catches it. Every error has a code and a message, the message starting
with the lingo's file and line when it happened in Lingo code: `memo.ls line 4: files.read: no such file`. A request
that fails is answered with its message, and a status by its code (`core/README.md`, *Requests*).

| Code      | Means                                                    | `try` catches it | Status |
|-----------|----------------------------------------------------------|------------------|--------|
| `fail`    | The code said `fail`                                     | yes              | 500    |
| `phrase`  | A phrase failed. Its message starts with the phrase's id | yes              | 500    |
| `type`    | A value was the wrong kind, like `"a" < 3`, or division by zero | yes       | 500    |
| `unknown` | The computer doesn't have a phrase the code uses         | yes              | 404    |
| `denied`  | A phrase needs something the lingo may not do here      | no               | 403    |
| `limit`   | Too many steps, too long, or calls 1000 deep             | no               | 500    |
| `stopped` | Stopped from outside                                     | no               | 500    |

Errors in reading a lingo, like a missing `}` or an unknown phrase, are found when it loads, before anything runs, and
give its file, the line and what was expected. The lingo is set aside with them until it is fixed.

## 10. Lingos and what they need

A **lingo** is a set of phrases, and nothing else can be called. A lingo's phrases come from one of two places, and the
code calling them can't tell them apart:

- **Lingo:** written in Lingo, its code a `.ls` file, on top of other lingos, its listing made from its code. See
  section 11.
- **Native:** a library built for the computer, like `windows.dll`, its phrases added as it loads (`core/README.md`).

**What a model reads.** The onsay lingo tells Onsay what every running lingo shares, its catalog: each lingo's whole
manifest (section 11), every function with what it does and its values as JSON Schema. A native phrase says what it
gives in its `description`, the way the Windows lingo writes it: `-> [{name,path,size,folder,changed}]` is a list of those records, `x?` is x or
nothing, `a|b` is either, and `bool` is true or false.

**Needs** are what a phrase uses on the computer, from one shared list: `net`, `store`, `files.read`, `files.write`,
`screen.read`, `screen.act` and `run`. A native phrase says which it needs as it is added, and a function written in
Lingo needs everything the phrases it reaches need, worked out from its code. A lingo's `openapi.json` says what each
of its routes needs (`x-lingo-needs`, section 11), and the computer decides what it actually gets, every time a phrase
runs: only what is under its ceiling and said in the manifest, and `files` outside the lingo client's own folder, `run`
and the screen only with a grant or the person's yes. A phrase that may not stops the call with `denied`, and `try`
can't hide that. How the computer decides is in `core/README.md`, *Who may do what*.

## 11. A .lingo file: its program and its openapi.json

A lingo is a `.lingo` file in a computer's lingos folder: a zip of two files, its program and its manifest,
`openapi.json`. Its program is its code in Lingo, LingoScript, `<name>.ls`, or a native library, `<name>.dll` or
`<name>.so`. While it is written, the same files in a folder of its name, `lingos/<name>/`, load the same way.

**Writing one.** Its maker writes only its code, `memo.ls`, and an `openapi.json` when the code can't say something
itself, like its settings. Its listing, what it shares, is made from the code by the compiler (`listing` in
`core/src/compile.c`):

- **Every `fn`** is a function it shares, called at its route, like `/memo/recall`, or by its dotted name,
  `memo.recall(k)`, from a lingo in the same folder. Its values and their plain defaults are its `fn`'s own. A default
  worked out as it runs, like `o={a: 1}`, shows as `nothing`.
- **The comment lines right above a `fn`** say what it does: what it gives, when to use it and when not to, in the
  words a person would ask with. A number at their start, like `1.`, a legend's, is left off.
- **The first comment lines of the file,** up to a blank line or an empty `//`, are what the lingo is, its about, with
  `memo - ` or `memo.ls - ` at their start left off. They are never a `fn`'s too, so a blank line goes after them.
- **Its settings** are those its `openapi.json` lists, below, then any other its code reads with `setting("key")`.
- **Its name** is its file's: lower case letters, digits, `_` and `-`, not starting or ending with `-` or `_`. `lingo` is
  never one.

The test lingo `memo` (`core/test/lingos/memo/memo.ls`, shortened here):

```
// memo - things to remember, kept in Onsay's own storage on this device.
//
// 1. recall(k)         -> text?: what was kept under k
// 2. all(prefix="")    -> [text]: every name kept

// 1. What was kept under that name.
fn recall(k) {
  return store.get(key(k))
}

// 2. Every name kept, only those that start with prefix when it is given.
fn all(prefix="") {
  out = []
  for k in store.keys(key(prefix)) {out += [std.slice(k, 5)]}
  return out
}

// Where a name is kept in Onsay's own storage.
fn key(k) {return "memo/" + k}
```

A file holds only `fn` blocks: no `tool`, and no steps outside them. Its code may call its own functions, the phrases of
the core and the computer's native lingos, the functions of every lingo in the same folder by their dotted names,
`lingo(...)`, `use(...)` and `setting(...)`, and nothing else.

**Packing it.** `onsay-core core.json pack lingos/memo` makes `memo.lingo` from the folder (`core/README.md`, *Command
line*). It loads the folder with every lingo in the lingos folder, so what it reaches in them counts, and writes the
whole `openapi.json`, a valid OpenAPI 3.1 document: every function a path, with its words, its values, and what it
needs, worked out from its code, or for a native lingo from its library's phrases. Its files are stored as they are, and
a `.lingo` file whose files are deflated, as most zip programs make them, loads too. `memo`'s, shortened:

```json
{
  "openapi": "3.1.0",
  "info": { "title": "memo", "version": "1.0.0", "description": "things to remember, kept in Onsay's own storage on this device." },
  "paths": {
    "/memo/recall": {
      "post": {
        "summary": "What was kept under that name.",
        "description": "What was kept under that name.",
        "requestBody": { "content": { "application/json": {
          "schema": { "type": "object", "properties": { "k": {} }, "required": ["k"] } } } },
        "responses": { "200": { "description": "The function's value, as JSON" } },
        "x-lingo-function": "memo.recall",
        "x-lingo-needs": ["store"],
        "x-lingo-calls": 0
      }
    }
  },
  "x-lingo": { "name": "memo", "program": "memo.ls", "route": "/memo" }
}
```

- **`paths`**, one for each function it shares, absolute: `/<route>/<function>`, the route alone for `main`, and for a
  native lingo `/<route>/<group>/<function>`, like `/windows/window/list`. Each is a `post`, its values a JSON record by
  name as its `requestBody`, and `GET` with them in the query works too. A lingo with `serve(request)`, below, shares
  `serve` alone, a `get` at its route itself, which answers every method and every path under it.
- **`summary` and `description`** are the comment above the function, its first sentence and all of it.
- **`x-lingo-function`** is the function the path calls, like `memo.recall`, or a native's phrase, like `window.list`.
- **`x-lingo-needs`** is everything it needs on the computer (section 10), through every function it reaches.
- **`x-lingo-calls`** is how many `lingo(...)` calls it makes, through every function it reaches.
- **`x-lingo`** is the lingo's own: its `name`, its `program`, its `route`, and its `settings` and `keep` when it has
  them, below.

**Loading checks it.** When a `.lingo` file's `openapi.json` says its paths, the lingo client works out what its code
needs and sets the lingo aside when it says less, with why: `liar.ls: liar.fetch needs net, which its openapi.json
doesn't say. Pack it again to say all it needs`. With no paths, like a folder while it is written, they are made from
its code as it loads, as `pack` makes them.

**What its maker writes in `openapi.json`**, all optional, a part of the whole: its `info`, like a `description` for a
native lingo, which has no comments to read, and `x-lingo` with its settings, a route and where its settings are kept
(the `agent` of `lingo-library/`, shortened, with a `keep` and a `route` it hasn't got added to show them):

```json
{
  "x-lingo": {
    "settings": [
      { "name": "claude_key", "title": "Claude API key", "about": "A Claude API key, from console.anthropic.com.", "secret": true },
      { "name": "model", "title": "Model", "about": "The model to use." },
      "service"
    ],
    "keep": { "read": "read", "write": "write" },
    "route": "/ai"
  }
}
```

- **`settings`** is a list. Each is a name alone, or a record of its `name`, and as it needs: `title`, the words a form
  shows, `about`, what it is and where to get it, `default`, its value when none is kept, `required`, true when the lingo
  can't work without it, and `secret`, true for a key or a password, which a form hides.
- **`keep`**, which most lingos leave out, names two of the lingo's own functions that keep its settings wherever its
  maker wants them: `read(key)` gives one's value and `write(key, value)` keeps one. Inside them, `setting(...)` of
  that lingo reads the lingo client's own file. Without it, the lingo client keeps them in its own folder,
  `home/settings/<name>.json`.
- **`route`** is where it answers on its computer, like `"/onsay"`. Without it, it answers at `/<name>`.
- **`program`** names its program when its maker wants to say it. Without it, it is whichever of `<name>.ls`,
  `<name>.dll` and `<name>.so` is beside it.

Who may reach a lingo is never in its manifest: that is the computer's policy (`core/README.md`). A lingo whose
`openapi.json` lists its settings wrongly is set aside with the reason. The lingo client answers `setting(...)` from
them, `settings.get(lingo)` gives every one with what its manifest says and its value now, and
`settings.set(lingo, key, value)` changes one wherever it is kept (section 15). A lingo's settings are never sent to
Onsay: its manifest names them only.

**`serve(request)`.** A lingo with a function `serve` answers every request to its route itself, like a page:

```
// hello - a page that says hello to anyone.

// Every request to /hello: hello to whoever the query names.
fn serve(request) {
  if request.method != "GET" {return {status: 405, body: {error: "Only GET"}}}
  return "hello " + (request.query.name ?? "there")
}
```

`request` is `{method, path, query, headers, body, json}`: `path` is the part after its route, starting with `/`,
`query` a record, `headers` a record by lower case name, `body` text, and `json` the body read as JSON when it is
JSON, else nothing. What serve gives is the answer: a record with a number `status` is `{status, headers, body}`, its
body text, or written as JSON when it isn't text, and anything else is the body of a 200, as text when it is text and
as JSON when it isn't. A lingo without `serve` is called function by function, `POST /memo/recall` with its values as a
JSON record by name, `{"k": "milk"}`, or a list in order, or `GET /memo/recall?k=milk`, answered with the function's
value as JSON (`core/README.md`, *Requests*).

**A native lingo** is its library and its `openapi.json`, like `windows.lingo` holding `windows.dll`, which adds its
phrases as it loads (`core/README.md`). Its paths are made from those phrases. A folder with neither an `openapi.json`
nor a program isn't a lingo and is left alone.

**Loading.** The lingo client compiles every lingo in its folder when it loads it, so a broken one is set aside with
the reason then, never in the middle of a task. A dotted call to a lingo that isn't in the folder is one such reason.
A lingo whose name is that of a native lingo or one of its groups is set aside too, like an old `input` on Windows:
`input is already a part of windows, this computer's own lingo`. Every lingo here loads, and a lingo calls another by
its dotted name whether that one is running or stopped: running is only whether its routes answer (`core/README.md`).

**Linking.** When a function is called, it is linked against the lingos here: every function written in Lingo it
reaches becomes a call to its `fn`, with any value left out taking its default, and only the functions it actually
reaches are kept. What is left calls nothing but native phrases. An error inside a lingo names its own file and line:
`memo.ls line 7: memo needs files.write, which this computer never allows`.

## 12. Where lingos run

Code runs where it lives. A running lingo in a computer's lingos folder runs in that computer's lingo client, when a
request reaches its route (section 11) from that computer, from another, or from Onsay through the onsay lingo, and the
caller may reach it, and only the answer goes back. Its code may call `lingo(...)` and `use(...)`, which ask Onsay and
wait, or `setting(...)`,
answered by that computer's lingo client itself. Anything at any other address runs there, its own way, reached by a
plain web request. Onsay runs no Lingo: it passes requests and answers. An answer sent to a computer after an `@` is a
request too, the answer the function's first value.

Each computer decides what its lingos may do, in its policy, and the lingo client refuses any phrase that needs what it
may not have, and stops any call the moment Onsay or the person says stop. `core/README.md` has the policy and the
hooks a native lingo fills, `lingos/onsay/README.md` how the onsay lingo talks with Onsay, and `core/test/server.mjs`
runs real lingos on a real lingo client.


## 13. The tree: what the compiler gives

The compiler reads a lingo into a JSON tree, and the runner only ever runs trees, never text. A call of a function is
a tree too: a tool of one step that gives back what the function gives, with every function it reaches linked in:

```json
{
  "tool": { "name": "call", "params": [["a0"]], "body": [ ["return", ["call", "memo.recall", [["var", "a0"]]]] ] },
  "fns": { "memo.recall": { "params": [["k"]], "body": [ ... ] }, "memo.key": { "params": [["k"]], "body": [ ... ] } },
  "lines": { "fn:memo.recall/0": "memo.ls line 16" }
}
```

- **`params`:** each is `[name]` or `[name, default]`, where the default is an expression.
- **`lines`:** maps each statement's path to its file and line, for errors. A path is `tool` or `fn:name`, then the
  statement's position in each block on the way down. An `if` has one block per branch then the else, and a `try` has
  its body then its catch.

**Expressions.** A JSON `null`, `true`, `false`, number or text is itself. Every other expression is an array:

| Node                       | Meaning                              |
|----------------------------|--------------------------------------|
| `["var", name]`            | A name's value                       |
| `["list", e...]`           | A new list                           |
| `["map", {part: e}]`       | A new record                         |
| `["get", e, key]`          | A part or an item                    |
| `["op", o, a, b]`          | `o` is `+ - * / % == != < > <= >= in` |
| `["and", a, b]`, `["or", a, b]` | Stop early                      |
| `["not", e]`, `["neg", e]` |                                      |
| `["pick", c, a, b]`        | `c ? a : b`                          |
| `["else", a, b]`           | `a ?? b`                             |
| `["range", a, b]`          | `a..b`                               |
| `["call", name, [e...]]`   | A function                           |
| `["say", id, {param: e}]`  | A phrase, its values named by its params |

Text with `{}` inside arrives as `+` nodes, starting from `""`, so it always joins as text.

**Statements:**

| Node                                   | Meaning                                  |
|----------------------------------------|------------------------------------------|
| `["set", name, e]`                     | Sets a name                              |
| `["put", target, key, e]`              | Sets a part or item of `target`          |
| `["if", [[c, [body]]...], [else]]`     | Branches                                 |
| `["while", c, [body]]`                 |                                          |
| `["each", item, e, [body], key?]`      | `for`, with `key` present for two names  |
| `["break"]`, `["next"]`                | `break`, `continue`                      |
| `["return", e]`                        |                                          |
| `["fail", e]`                          |                                          |
| `["try", [body], name, [catch]]`       |                                          |
| `["do", e]`                            | A call run for what it does              |

Compound assignment arrives as a plain `set` or `put` with an `op`: `x+=1` is `["set","x",["op","+",["var","x"],1]]`.

A lingo client may run the tree any way it likes, an interpreter, a compiler or anything else, as long as the result
is the same.

## 14. Laid out and dense

Whitespace carries no meaning, so all code has two standard ways of being written, and both read back into the same
tree:

```
fn big_programs(limit=500) {
  big = []
  for p in commands.running() {
    if p.memory > limit {
      big += [{name: p.name, mb: math.round(p.memory)}]
    }
  }
  return list.reverse(list.sort(big, "mb"))
}
```
```
fn big_programs(limit=500){big=[]for p in commands.running(){if p.memory>limit{big+=[{name:p.name,mb:math.round(p.memory)}]}}return list.reverse(list.sort(big,"mb"))}
```

- **Laid out** is for people: two space indents, spaces around operators, one statement a line.
- **Dense** is for the fewest tokens: no space or new line that isn't needed, and a `;` only where two names would
  otherwise run together, like `a=b;c=1`. Measured on realistic code, dense takes about a quarter fewer tokens.

Dense suits a model writing code, since what a model writes costs far more than what it reads, and nothing a person
sees has to be dense: since whitespace carries no meaning, the same code can always be laid out again for reading.
Keyword spellings are not worth shortening: words like `return` and `continue` are already a single token each.

## 15. The core: what every computer has

Every lingo client has one lingo of its own, `core`, on any computer, never listed in its catalog. It is the floor:
only what Lingo truly can't write for itself, each phrase called with its group, like `std.length(x)`. Anything Lingo
can write in a few lines is a lingo instead, written in Lingo on this floor (`lingo-library/`).

| Phrase | Why it is in the core |
|--------|-----------------------|
| `std.length(x)` | Items, characters or parts. Counting by a loop each time would cost a step for every one |
| `std.keys(r)` | A record's part names as a list, with no loop |
| `std.slice(x, from, to=nothing)` | Part of a list or text, by characters. Building text one character at a time is slow and costs memory |
| `std.number(x)` | Text to a number, like "$1,204.50". Lingo has no other way from digits to a number |
| `std.text(x)` | Any value as text, the way `"{x}"` writes it, by name |
| `std.to_json(x)`, `std.from_json(text)` | JSON, exactly and fast, for every web answer |
| `std.bytes(text)`, `std.from_bytes(list)` | A character's UTF-8 bytes and back, Lingo's only way to a letter's code: case, encodings |
| `compute.now()` | The clock: seconds since 1970 |
| `compute.wait(seconds)` | Pausing, at most 60 seconds, still stopping when asked |
| `compute.log(x)` | A line in the lingo client's log, which the Windows app keeps |
| `store.get`, `put`, `keys`, `delete` | Onsay's own storage on the computer, its files, needing `store` |
| `settings.get(lingo)`, `settings.set(lingo, key, value)` | A lingo's settings, kept on the computer wherever that lingo keeps them, for a form of them and to change one (section 11) |
| `http.send(method, url, headers=nothing, body=nothing, timeout=30)` | The computer's own web request, needing `net`. `request` and `use(...)` stand on it |

`http.send` gives `{status, headers, text}`, its headers a record by lower case name, redirects followed, at most 4 MB
back, and fails when it takes longer than its timeout. Windows makes it with WinHTTP and Linux with libcurl, so proxies
and certificates work as they do for every other program. Most lingos use `request` (`lingo-library/request/request.ls`),
built on it, or `use(...)` for one plain request.

**Onsay names work wherever web addresses do.** A web address given to `http.send`, so to `request`, may name a host on
the web or an Onsay name, told apart by one rule every Onsay program keeps: a host is on the web when it is an IP,
`localhost`, or a name with a dot ending in one of the web's top level domains (IANA's list), like `example.com`. Any
other, like `laptop.brandon` or `brandon`, is an Onsay name: the lingo client asks Onsay where it goes, at
`https://api.onsay.dev/resolve/laptop.brandon`, and sends the request to the web address it gives, what followed the
name after it. So `request.get("laptop.brandon/weather/now")` reaches that computer's `weather` lingo through the free
proxy, as anyone on the web would, when it is open to them. `http://` or `https://` may be left off. A name with nowhere to go fails with why:
`There's no computer or route called nobody.brandon`. `use(...)` reads both kinds the same way, through Onsay.

Everything else is a library lingo in the same lingos folder, called by its dotted name: `text` (`split`, `join`,
`replace`, `trim`, `lower`, `upper` and more), `list` (`sort`, `reverse`), `math` (`round` to the nearest, up or down,
`min`, `max`), `time` (dates, like `time.difference(a, b, "days")`) and the rest of `lingo-library/`. Whether text
holds a part is the operator `in`. What is around a `lingo(...)`, the computer and its time, goes with it, so no phrase
says what the computer is.

## 16. The Windows lingos

The Windows app ships two native lingos for Windows, built from `lingos/` into `windows.lingo` and `native.lingo`, beside
the onsay lingo, which every computer has (`lingos/onsay/README.md`), and the lingos of `lingo-library`, `agent` among
them. Their phrases, with what each gives, does and needs, are in their `openapi.json`
and the catalog, and in their `lingo_phrases` in C.

- **`windows`**, Windows itself:

  | Group       | Phrases                                                              |
  |-------------|----------------------------------------------------------------------|
  | `window`    | `list`, `current`, `open`, `wait_for`, `focus`, `close`, `resize`, `move` |
  | `controls`  | `find`, `list`, `read`, `press`, `fill`, `toggle`, `pick`, `scroll`, `wait_for` |
  | `files`     | `read`, `write`, `add_to`, `exists`, `list`, `find`, `copy`, `move`, `delete`, `make_folder` |
  | `clipboard` | `read`, `write`                                                      |
  | `commands`  | `run`, `running`, `stop`                                             |
  | `notify`    | `show`                                                               |
  | `panel`     | `show`, `hide`: a small window of Onsay's own showing a page of HTML |
  | `input`     | the mouse and keyboard the way a person uses them, through Onsay's own pointer: `cursor`, `click_text`, `click_on`, `type_into`, `type`, `paste`, `shortcut`, `type_lines`, `scroll_to_text`, `wait_for_text`, `copy_all`, `point_text`, `point_on`, `point_at`, `stop_pointing`, `click_in`, `move_in`, `drag_in`, `scroll_in`, `position_in` |
  | `look`      | the screen read: `screenshot`, `elements`, `overview`, `focused`, `page`, `lines`, `buttons`, `fields`, `near`, `find_text`, `open_windows` |

- **`native`**, Windows only: `native.call(library, function, returns, args)` calls any function in Windows or a DLL by
  its name and layout, needing `run`.

Conventions that matter in practice:
- **Paths:** write them with `/`, as in `"~/notes/today.txt"`, so text never needs escapes. A path may start with `~`
  (home) or `downloads`, `desktop` or `documents`, which are found through Windows even when they've been moved. Any
  other path that doesn't start at a drive starts at the person's home.
- **Onsay's pointer:** every phrase that clicks, moves, drags, scrolls, presses, types, pastes or fills shows it, with
  no asking: his arrow glides there and a click ripples, and where he types it is a text cursor that ticks while the
  keys go in. The Windows app draws it from the lines of JSON the `windows` lingo writes on the lingo client's output.
- **Places:** `input`'s `click_in`, `move_in`, `drag_in`, `scroll_in` and `position_in` take a place in a window,
  wherever it is now, or on the main screen when the window is `nothing`.
- **New windows:** a window builds its controls a moment after it appears, so wait for one with `controls.wait_for`
  before pressing anything.
- **Same names:** `get`, `keys`, `list`, `read`, `write`, `find`, `delete`, `move`, `press`, `scroll`, `wait_for`,
  `show` and `hide` are in more than one group, so they are written with theirs, like `window.wait_for` or
  `files.read`.
