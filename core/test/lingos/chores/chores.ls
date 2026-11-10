"for the tests: work that takes the person's yes, a command run, or a file outside this lingo client's home."

fn run_it() "What a small command writes, which needs run." {return commands.run("Write-Output 7").out}

fn read(path) "A file's text, from anywhere, which needs files.read." {return files.read(path)}
