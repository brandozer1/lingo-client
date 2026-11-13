"Your agents' memory on this computer: what is worth knowing next time about the person, their computers and what worked, kept as lines of one file they can open and change, Onsay/memory.md in their home folder."
// 1. remember(fact) -> text          one fact kept, a line of its own, unless it is there already
//    recall(words=nothing) -> text   what is kept: all of it, or, when it is long, the lines holding any of the words
//    forget(words) -> text           every line holding those words taken out
//
// Each line of the file that isn't empty, a heading or a > note is a fact, a leading "- " left out, so the person may write it by
// hand too.

////////// 1. MEMORY //////////

fn remember(fact: text "one thing worth knowing next time, in a plain sentence, like Brandon's chess.com name is brandozer, or the project at documents/site builds with npm run build") -> text "Keeps a fact for next time, for every agent on this computer: what the person prefers, who and what they mean by a name, where something is, what worked and what didn't. Not for what only this conversation needs." {
  fact = text.trim(text.replace(std.text(fact), "\n", " "))
  if fact == "" {fail "Say what to remember"}
  kept = _kept()
  for line in kept {
    if text.lower(line) == text.lower(fact) {return "Already remembered"}
  }
  _write(kept + [fact])
  return "Remembered"
}

fn recall(words: text? "words of what to find, or nothing for all of it" = nothing) -> text "What is remembered on this computer, a line each: all of it, or, when it is long, the lines that hold any of the words." {
  kept = _kept()
  all = text.join(kept, "\n")
  if std.length(all) <= 6000 or words == nothing {return std.slice(all, 0, 6000)}
  out = []
  for line in kept {
    for w in text.words(text.lower(std.text(words))) {
      if std.length(w) > 2 and w in text.lower(line) and not (line in out) {out += [line]}
    }
  }
  return std.slice(text.join(out, "\n"), 0, 6000)
}

fn forget(words: text "words every line to forget holds, like chess.com name") -> text "Takes out every remembered line that holds these words, as when the person says it is wrong or no longer so." {
  find = text.lower(text.trim(std.text(words)))
  if find == "" {fail "Say what to forget"}
  kept = []
  for line in _kept() {
    if not (find in text.lower(line)) {kept += [line]}
  }
  gone = std.length(_kept()) - std.length(kept)
  if gone > 0 {_write(kept)}
  return "Forgot {gone}"
}

////////// HELPERS //////////

fn _file() {
  return "~/Onsay/memory.md"
}

fn _kept() "The facts in the file, in order, none when there is no file yet." {
  got = ""
  try {got = files.read(_file())} catch e {return []}
  out = []
  for line in text.lines(got) {
    line = text.trim(line)
    if text.starts_with(line, "- ") {line = text.trim(std.slice(line, 2))}
    if line != "" and not text.starts_with(line, "#") and not text.starts_with(line, ">") {out += [line]}
  }
  return out
}

fn _write(facts) {
  lines = ["# What Onsay remembers", "", "> One fact a line. Change or delete any of them: Onsay's agents read this file as it is.", ""]
  for f in facts {lines += ["- " + f]}
  files.write(_file(), text.join(lines, "\n") + "\n")
}
