// chores - for the tests: work that takes the person's yes, a command run, or a file outside this lingo client's home.

// What a small command writes, which needs run.
fn run_it() {return commands.run("Write-Output 7").out}

// A file's text, from anywhere, which needs files.read.
fn read(path) {return files.read(path)}
