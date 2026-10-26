# Lingo 1.0

**The language is its syntax and three calls: `lingo(...)`, `use(...)` and `setting(...)`.** The syntax is values,
names, operators and control flow. The three calls are references: `lingo(...)` asks for something in words,
`use(...)` reaches anything at an address, and `setting(...)` reads the person's own setting. Nothing else is part of
the language.

Lingo is an orchestration language, made for AI to read and write as easily as possible. On its own it does little but
run code written in C. It is Turing complete, so any program can be written in it, and lingos
stack on lingos, so any computation, anywhere, is some number of layers of lingos or steps away. Every function a
program can call is a **phrase**, supplied by a **lingo**: one file of Lingo code, or the set of phrases one platform
offers, listed in its **manifest**. Even what every computer has is a lingo, the core's own (section 15), never the
language. The language never changes from device to device, and only the lingos do.

This document describes the language exactly as the core in `core/` runs it: `compile.c` reads programs and links them
through the lingos they use, the same C on every device, and `run.c` runs them.
`core/test/reference/` keeps the first implementation, in JavaScript, which the tests hold the C to.

## Contents

1. Words and layout
2. Programs, tools and functions
3. Values
4. Statements
5. Expressions
6. Calls: functions and phrases
7. lingo(...): asking in words
8. How a program runs
9. Errors
10. Lingos and permissions
11. A lingo is one file
12. Where programs run
13. The tree: what the compiler gives
14. Laid out and dense
15. The core: what every computer has
16. The Windows lingo

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

## 2. Programs, tools and functions

A program is a list of top level items, in any order:

```
tool name(param, param=default, ...){ ...steps... }    // at most one: what gets run
fn name(param, param=default, ...){ ...steps... }      // any number, each name once
```

- **The tool** is the entry point. Its params are its inputs, given by name when the program is run. An input that is
  missing or `nothing` takes its default, or `nothing` when it has none.
- **Functions** are called by name, before or after where they are written. Their params are given in order.
- **Defaults** are expressions, worked out when the value is missing, left to right, so a default can use an earlier
  param: `fn f(a, b=a*2)`.
- **A program with no tool** is just steps. It runs as a tool named `steps` that takes nothing. That's what a single
  quick instruction is: `notify.show("Hi","there")`.
- **When there is a tool,** every step must be inside it or inside a function.

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
- **A lingo in the same lingos folder,** by its name: `text.slug(title)` calls the function `slug` of `text.lingo`, there
  beside it. The compiler finds it in the folder, so there is nothing to import or list.
- **Anything else doesn't compile,** and says to use `use(...)`: `There's no lingo or group called notes here. To reach
  one somewhere else, write use("its address"), like use("laptop.brandon/lingos/notes.lingo/add")`.

**`use(address, values)`** reaches anything somewhere else, in one line, and gives back its answer, like `lingo(...)` part
of the language. It is to Lingo what fetch is to JavaScript: for what is off this computer.

```
forecast = use("laptop.brandon/lingos/weather.lingo/forecast", {place: "Paris", days: 3})
windows = use("laptop.brandon/lingos/windows.lingo/window/list")
shown = use("laptop.brandon/lingos/check.lingo@desktop.brandon/lingos/show.lingo/print")
issues = use("https://mcp.example.com/mcp#search_issues", {repo: "onsay"})
page = use("https://api.example.com/v1/items?limit=5")
saved = use("https://api.example.com/v1/items", {name: "milk"})
```

- **A lingo at an address:** a place, then the lingo and its function: a computer, `laptop.brandon`, its lingos under
  `/lingos/`, the file and the function, `test.lingo/find`, `main` when none is named, and the query any values. Onsay
  finds the place by its name. It runs where it lives, and only its answer comes back.
- **An MCP tool:** the server's URL and the tool after a `#`.
- **A plain web address**, any address on the web whose path names no `/lingos/<file>.lingo`: a plain web request, a GET
  with no values and a POST of the values as JSON with some. The answer is read as JSON when it says it is JSON, else it
  is its text, and any status but 2xx fails in plain words with the status and the start of what it said:
  `https://api.example.com/v1/x answered 404 Not Found: no such item`. For headers, other methods or tries again, use
  the `request` lingo, on `http.send`.
- **After an `@`,** where the answer goes: it is sent on to that address as a plain call, the answer its first value.
  `https://` may be left off.
- It compiles to the phrase `lingo.use(address, values)`, every core's own, and is asked of Onsay when the line runs.
- **`values`** is a record, matched to the function's values by name, a list, in order, or nothing. With a record, a
  name it doesn't take fails (`... takes place, not city`), a required one left out fails (`... needs where`), and an
  optional one left out takes its default. A query and a record are merged, the record winning, and a query with a
  non-empty list fails.
- **Errors in writing it:** no address, an empty one, or more than two values. A program's own `fn` can't be called
  `use`: `use(...)` always means this.

**`setting(key)`** reads a setting the person keeps on their account for this lingo, like a key or a token, also part of
the language:

```
key = setting("key")
units = setting("units") ?? "metric"
token = setting("token", "github")
```

- Inside a lingo, `setting("key")` is that lingo's own: the compiler fills in its name, like `openrouter`.
  `setting("key", "github")` reads another lingo's, so one token serves many lingos. A program that isn't a lingo always
  says whose.
- It compiles to the phrase `lingo.setting(key, reference)`, every core's own, and is asked of Onsay over the computer's
  connection when the line runs, the way `use(...)` is. Onsay answers from the account the run is for, never the
  lingo's maker. One that isn't set is `nothing`.
- A lingo's settings are its own `setting("...")` calls: its listing names each one, and the dashboard shows a field for
  each in Settings, a name like key, token, secret or password hidden. See *Lingo settings* in `core/README.md`.
- **Errors in writing it:** no key, more than two values, or no lingo named outside a lingo:
  `setting(...) outside a lingo says whose setting it is too, like setting("key", "openrouter")`. A program's own
  `fn` can't be called `setting`: `setting(...)` always means this.

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
There's no lingo or group called notes here. To reach one somewhere else, write use("its address"), like use("laptop.brandon/lingos/notes.lingo/add")
```

A `use(...)`'s errors come when its line runs, and a `try` catches them:
```
There's no route called forcast
laptop.brandon is offline, so laptop.brandon/lingos/text.lingo/slug can't run there now
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
`lingo.run`, and asks Onsay over its connection, exactly as for `use(...)` (section 6), saying which of its functions
asks, so it never gets itself as the answer.

There are two ways to reach a function: name it, with a dotted call here or `use(...)` elsewhere, or ask for it in
words with `lingo(...)` when you don't know which.

A lingo's own code may call `lingo(...)` too.

## 8. How a program runs

1. The tool's inputs are given by name, and defaults fill the rest.
2. Steps run in order. Every statement and every expression worked out is one **step**.
3. The tool's `return` value is the result. With no `return`, the result is `nothing`.

**Calling a function** gives it its own names, starting from its params. Its `return` is the call's value, or `nothing`.
A function may call itself, up to 1000 deep.

**Calling a phrase** works out its values in order, checks its permission (section 10), runs it, and gives back what it
returned, or `nothing`. Values in a list or record are worked out left to right, so phrases always run in the order they
are written.

**Limits:** a program stops with a `limit` error after too many steps (the reference default is 5,000,000) or too long
(10 minutes). Stopping it from outside, like Ctrl+C or Alt+Shift+O, ends it between two steps with `stopped`.

## 9. Errors

An error stops the program, unless a `try` catches it. Every error has a code, a message, and the place it happened:

```json
{ "code": "phrase", "message": "files.read: no such file", "at": "tool/2/0/1", "line": 4 }
```

| Code      | Means                                                    | `try` catches it |
|-----------|----------------------------------------------------------|------------------|
| `fail`    | The program said `fail`                                  | yes              |
| `phrase`  | A phrase failed. Its message starts with the phrase's id | yes              |
| `type`    | A value was the wrong kind, like `"a" < 3`, or division by zero | yes       |
| `unknown` | The core doesn't have a phrase the program uses          | yes              |
| `denied`  | A phrase needed a permission that wasn't given           | no               |
| `limit`   | Too many steps, too long, or calls 1000 deep             | no               |
| `stopped` | Stopped from outside                                     | no               |

Errors in reading the program, like a missing `}` or an unknown phrase, are found before anything runs, and give the
line and what was expected. A core asked to run code it couldn't compile answers with the code `compile`, and the line.

## 10. Lingos and permissions

A **lingo** is a set of phrases, and its listing, or for a native lingo its **manifest**, lists every one: a program
can call nothing else. A lingo's
phrases come from one of three places, and a program can't tell them apart:

- **Lingo:** written in Lingo, one file, on top of other lingos, its listing made from its code. See section 11.
- **Native:** a library built for the device, like `windows.dll`.
- **Host:** run by the program around the core, in its own language, where the core can't load a library.
  Its folder holds only its `lingo.json`.

How native and host lingos are built is in `core/README.md`.

**The listing a model reads.** A native lingo's phrases are also written out as plain text, the way they are called. A
core says hello with this text for each native lingo it has, and it is what a model reads to write programs, so it is
laid out for the fewest tokens that still read clearly:

```
lingo windows 1.0, Lingo 1.0
file = {name,path,size,folder,changed}

files: files.read
read(path) -> text
list(folder, pattern="*") -> [file]
write(path, text) [files.write]: replaces it
delete(path) [files.write]: to the Recycle Bin
```

| Line | Means |
|------|-------|
| `lingo <name> <version>, Lingo <version>, uses <lingo> <version>, ...` | The first line: the lingo, its version, the version of Lingo it speaks, and the lingos it is built on, if any |
| `name = {a,b,c}` | A record shape, named once, so phrases can say they give `file` or `[file]` |
| `group: permission, ...` | Starts a group. Its phrases are called `group.name` and need those permissions. A group's name may have levels, like `media.player`. `, pure` last marks every phrase in the group pure |
| `name(params) -> gives [permission]: does` | A phrase. Only the signature is required |
| `// ...` | A comment line. Blank lines are ignored |

A phrase line, part by part:
- **The signature** is written exactly like a Lingo `fn` header, and read by the same parser: its values in order, with
  `name=default` for one that may be left out. A default must be a plain value: a number, text, `true`, `false` or
  `nothing`.
- **`-> gives`** is what it gives back, left out when it gives nothing. `[x]` is a list of x, `x?` is x or nothing,
  `a|b` is either, `bool` is true or false, and a shape's name stands for that record.
- **`[permission]`** is only written when it differs from the group's. It may list several, like
  `[screen.act, screen.read]`, and then the phrase needs every one.
- **`: does`** is one short line on what it does, left out when the signature already says it.

A host lingo lists the same phrases in its `lingo.json`, as JSON (section 11).

**Pure** means same values in, same result out, and no effect on the world, so it can be cached or run ahead of time.

**Permissions** come from one shared list: `none`, `screen.read`, `screen.act`, `files.read`, `files.write`, `run`
and `net`. The person gives them to the device, never to the program, so no program can do more than was allowed.
A phrase that needs any permission that wasn't given stops the program with `denied`, and `try` can't hide that.

## 11. A lingo is one file

A lingo written in Lingo is one file, `<name>.lingo`, directly in a computer's lingos folder, and nothing else: no folder,
no manifest, nothing to list. The person writes only the code. Its listing, what it shares, is made from the code by the
compiler (`listing` in `core/src/compile.c`):

- **Every `fn`** is a function it shares, called at its address, like `laptop.brandon/lingos/memo.lingo/recall`, or by
  its dotted name, `memo.recall(k)`, from a lingo in the same folder. Its values and their plain defaults are its `fn`'s
  own. A default worked out as it runs, like `o={a: 1}`, shows as `nothing`.
- **The comment lines right above a `fn`** say what it does: what it gives, when to use it and when not to, in the
  words a person would ask with. A number at their start, like `1.`, a legend's, is left off.
- **The first comment lines of the file,** up to a blank line or an empty `//`, are what the lingo is, its about, with
  `memo - ` or `memo.lingo - ` at their start left off. They are never a `fn`'s too, so a blank line goes after them.
- **Its settings** are the names its code reads with `setting("key")`.
- **Its name** is its file's: lower case letters, digits, `_` and `-`, not starting or ending with `-` or `_`. `lingo` is
  never one.

The test lingo `memo` (`core/test/lingos/memo.lingo`, shortened here):

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

and the listing made from it, as the core sends Onsay in its catalog (6. CONNECTION in `core/src/core.c`):

```json
{
  "name": "memo", "file": "memo.lingo", "status": "ok",
  "about": "things to remember, kept in Onsay's own storage on this device.",
  "settings": [],
  "phrases": [
    { "id": "memo.recall", "head": "recall(k)", "does": "What was kept under that name.", "lingos": 0,
      "inputSchema": { "type": "object", "properties": { "k": {} }, "required": ["k"] } },
    { "id": "memo.all", "head": "all(prefix=\"\")", "does": "Every name kept, only those that start with prefix when it is given.", "lingos": 0,
      "inputSchema": { "type": "object", "properties": { "prefix": { "default": "" } }, "required": [] } },
    { "id": "memo.key", "head": "key(k)", "does": "Where a name is kept in Onsay's own storage.", "lingos": 0,
      "inputSchema": { "type": "object", "properties": { "k": {} }, "required": ["k"] } }
  ]
}
```

A file holds only `fn` blocks: no `tool`, and no steps outside them. Its code may call its own functions, the phrases of
the core and the device's native lingos, the functions of every lingo in the same folder by their dotted names,
`lingo(...)`, `use(...)` and `setting(...)`, and nothing else.

**What Onsay is told.** A core tells Onsay what each lingo shares, never its code: each function's head and what it
does, its `inputSchema`, and how many `lingo(...)` calls it makes, counted from the code through every function it
reaches. The dashboard shows the same.

**Native and host lingos** are a folder with a `lingo.json`, their manifest: `name`, `version` (three numbers, like
`1.0.0`, a label only), `about`, and `runs`, `native` with its `library`, like `windows.dll`, or `host`. A native one
lists no functions: the library adds them, and the text listing in section 10 is what a core says hello with. A host one
lists its `shapes`, its `groups` with what each needs, like `{"name": "tabs", "needs": ["screen.read"]}`, and its
`functions`, each shaped like an MCP tool: a `name`, its `group`, a `description`, and an `inputSchema` and
`outputSchema` in JSON Schema, its permissions in `onsay.needs` when they aren't its group's. A folder without a
`lingo.json` isn't a lingo and is left alone. A `lingo.json` of a lingo written in Lingo is set aside, saying it is one
file now.

**Loading.** A core compiles every lingo in its folder when it loads it, so a broken one is set aside with the reason
then, never in the middle of a task. A dotted call to a lingo that isn't in the folder is one such reason. A lingo whose
name is that of a native lingo or one of its groups is set aside too, like an old `input.lingo` on Windows:
`input is already a part of windows, this device's own lingo`.

**Real permissions.** A phrase written in Lingo needs exactly what it reaches: every native phrase down every path,
through other functions and other lingos. These are worked out, never written down, so a lingo can't claim to need less
than it does. `memo.recall` above needs no permission, and a phrase that calls `files.write` needs `files.write`.

**Linking.** A program is compiled against the lingos it may use, and is refused before anything runs if a call to
one of them is wrong (section 6). Then every phrase written in Lingo becomes a call to its `fn`, with any value left out
taking its default, and only the functions something actually reaches are kept. What is left calls nothing but native
and host phrases, and runs like any program. An error inside a lingo names its own file and line: `memo.lingo line 7:
files.add_to needs permission for files.write`.

## 12. Where programs run

Code runs where it lives. A lingo in a computer's lingos folder runs in that computer's core, when Onsay relays a call to
it, `{type:"call", lingo:"test.lingo/find", values}`, and only the answer goes back. Its code may call `lingo(...)`,
`use(...)` or `setting(...)`, which asks Onsay and waits. Anything at any other address runs there, its own way, reached
by a plain web request. Onsay runs no Lingo: it relays calls and answers. An answer sent to a computer after an `@` is a
plain call too, the answer the function's first value.

The person gives the permissions on each device, in the Onsay app. The core refuses any phrase that needs one not given,
and stops any program the moment Onsay or the person says stop. `core/README.md` lists the messages between a core and
Onsay, and `core/test/server.mjs` runs real programs on a real core.

## 13. The tree: what the compiler gives

The compiler reads a program into a JSON tree, and the runner only ever runs trees, never text:

```json
{
  "lingo": 1,
  "tool": { "name": "big_spenders", "params": [["limit", 50]], "body": [ ... ] },
  "fns": { "fact": { "params": [["n"]], "body": [ ... ] } },
  "needs": ["web.open", "web.table"],
  "lines": { "tool/0": 2, "tool/1": 3 }
}
```

- **`params`:** each is `[name]` or `[name, default]`, where the default is an expression.
- **`needs`:** every phrase the program can call, so every permission is known before it starts.
- **`lines`:** maps each statement's path to its line, for errors. A path is `tool` or `fn:name`, then the statement's
  position in each block on the way down. An `if` has one block per branch then the else, and a `try` has its body
  then its catch.

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
| `["call", name, [e...]]`   | A function of the program            |
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

A core may run the tree any way it likes, an interpreter, a compiler or anything else, as long as the result is the
same.

## 14. Laid out and dense

Whitespace carries no meaning, so every program has two standard ways of being written, and both read back into the
same tree:

```
tool big_programs(limit=500) {
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
tool big_programs(limit=500){big=[]for p in commands.running(){if p.memory>limit{big+=[{name:p.name,mb:math.round(p.memory)}]}}return list.reverse(list.sort(big,"mb"))}
```

- **Laid out** is for people: two space indents, spaces around operators, one statement a line.
- **Dense** is for the fewest tokens: no space or new line that isn't needed, and a `;` only where two names would
  otherwise run together, like `a=b;c=1`. Measured on realistic programs, dense takes about a quarter fewer tokens.

Dense suits a model writing a program, since what a model writes costs far more than what it reads, and nothing a person sees has to be
dense: since whitespace carries no meaning, the same program can always be laid out again for reading. Keyword
spellings are not worth shortening: words like `return` and `continue` are already a single token each.

## 15. The core: what every computer has

Every core has one lingo of its own, `core 0.1`, on any device. It is the floor: only what Lingo truly can't write for
itself, each phrase called with its group, like `std.length(x)`. Anything Lingo can write in a few lines is a lingo
instead, written in Lingo on this floor (`lingo-library/`).

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
| `compute.log(x)` | A line shown to the person, which nothing else can show |
| `store.get`, `put`, `keys`, `delete` | Onsay's own storage on the device, its files |
| `http.send(method, url, headers=nothing, body=nothing, timeout=30)` | The device's own web request, needing `net`. `request` and `use(...)` stand on it |

`http.send` gives `{status, headers, text}`, its headers a record by lower case name, redirects followed, at most 4 MB
back, and fails when it takes longer than its timeout. Windows makes it with WinHTTP, and a core in WebAssembly has its
host make it. Most lingos use `request` (`lingo-library/request.lingo`), built on it, or `use(...)` for one plain
request.

Everything else is a library lingo in the same lingos folder, called by its dotted name: `text` (`split`, `join`,
`replace`, `trim`, `lower`, `upper` and more), `list` (`sort`, `reverse`), `math` (`round` to the nearest, up or down,
`min`, `max`) and `time` (dates, like `time.difference(a, b, "days")`). Whether text holds a part is the operator `in`.
What is around a `lingo(...)`, the device and its time, goes with it, so no phrase says what the device is.

## 16. The Windows lingo

The Windows app ships `windows 1.0`, a native lingo in its own folder (`lingos/windows`), and `agent`, the AI agent from
`lingo-library`. windows has these groups.
`core/bin/onsay-core.exe <settings> --manifest` prints every line of it, and of the core's, when it is in that core's
lingos folder.

- **Windows** adds:

  | Group       | Phrases                                                              |
  |-------------|----------------------------------------------------------------------|
  | `window`    | `list`, `current`, `open`, `wait_for`, `focus`, `close`, `resize`, `move` |
  | `controls`  | `find`, `list`, `read`, `press`, `fill`, `toggle`, `pick`, `scroll`, `wait_for` |
  | `screen`    | `screenshot`, `see`, `read_text`                                     |
  | `mouse`     | `move`, `click`, `double_click`, `right_click`, `drag`, `scroll`, `cursor` |
  | `keyboard`  | `type`, `press`                                                      |
  | `files`     | `read`, `write`, `add_to`, `exists`, `list`, `find`, `copy`, `move`, `delete`, `make_folder` |
  | `clipboard` | `read`, `write`                                                      |
  | `commands`  | `run`, `running`, `stop`                                             |
  | `notify`    | `show`                                                               |
  | `pointer`   | `show`, `hide`                                                       |
  | `input`     | the mouse and keyboard the way a person uses them: `cursor`, `click_text`, `click_on`, `type_into`, `paste`, `shortcut`, `type_lines`, `move_and_click`, `scroll_to_text`, `wait_for_text`, `copy_all`, `point_text`, `point_on`, `stop_pointing`, `click_in`, `move_in`, `drag_in`, `scroll_in`, `position_in` |
  | `look`      | Onsay's eyes: `see`, `elements`, `overview`, `focused`, `page`, `lines`, `buttons`, `fields`, `near`, `find_text`, `open_windows` |

Conventions that matter in practice:
- **Paths:** write them with `/`, as in `"~/notes/today.txt"`, so text never needs escapes. A path may start with `~`
  (home) or `downloads`, `desktop` or `documents`, which are found through Windows even when they've been moved. Any
  other path that doesn't start at a drive starts at the person's home.
- **New windows:** a window builds its controls a moment after it appears, so wait for one with `controls.wait_for` before
  pressing anything.
- **Same names:** `get`, `keys`, `list`, `read`, `write`, `find`, `delete`, `move`, `press`, `scroll`, `wait_for`,
  `cursor`, `show` and `see` are in more than one group, so they are written with theirs, like `window.wait_for`,
  `files.read` or `look.see`.
