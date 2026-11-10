"calls a lingo this computer doesn't have, and a phrase that fails, so each asks the lingo search as it runs."

fn go() {return nope.thing(2)}

fn fails() {return std.from_json("nope")}

fn caught() {
  try {return std.from_json("nope")} catch e {return "caught"}
}
