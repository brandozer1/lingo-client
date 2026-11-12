"Coding Agent: an AI software engineer for a project on this computer. It reads, searches, writes and edits code, runs commands and checks its work, the way the best coding agents do, and finds its AI on the network."
// 1. ask(task, folder, messages=nothing, mode="build") -> text; with messages, {text, messages, work, todos}: its reply,
//                                  after at most 40 steps or 8 minutes, then it says how far it got so it can go on.
//                                  folder is the project's. messages is the conversation so far as chat messages, OpenAI's
//                                  way, ending with task, and the messages given back are those it added, tool results cut
//                                  short. mode plan only looks and plans, changing nothing
//
// Its AI is the ai lingo on this computer, else found through the lingo function, never a key of its own: the first of
// lingo.ways for an AI model that runs a lingo's own agent, like ai.chat on another computer or Onsay's AI, kept in its storage and used every step
// with use(...). One that fails or answers in another shape is forgotten and the next is tried.
//
// Its tools, as its model sees them: read (a file with line numbers, or a folder), edit (exact text replaced, then looser
// matches when exact fails: ignoring spaces at line ends, spacing, indentation, or by first and last lines, each only
// when it matches one place), write, _glob(files by name), _grep(a regular expression in files), _shell(a command, its
// exit code and the end of its output), todowrite (its task list) and webfetch (a web page). It must read a file before
// changing it, and three identical calls in a row are refused. When the conversation grows past about 100,000 tokens, the
// older part is summed up by the same AI, and the newest kept as it is. On Windows, read, edit and grep run in
// PowerShell; elsewhere, sh and awk, edits matching exactly.

////////// 1. ASKING //////////

fn ask(task, folder, messages=nothing, mode="build") "A coding agent: does a software task in a project folder on this computer, like fix a bug, add a feature, refactor, explain or review code, or write tests, by reading and searching the code, editing files, running commands and tests, and checking its work, then says what it did. task is what to do, with everything it needs. folder is the project's folder. messages is the conversation so far, oldest first, ending with task, given to go on from earlier work. mode is build, the default, or plan, which only looks and plans, changing nothing." {
  if folder == nothing or folder == "" {fail "Say which folder the project is in, as folder"}
  place = _placed(folder, mode)
  said = []
  for m in messages ?? [] {
    if std.length(said) > 0 or m.role == "user" {said += [m]}
  }
  n = std.length(said)
  if n == 0 or said[n - 1].role != "user" or said[n - 1].content != task {said += [{role: "user", content: task}]}
  added = []
  state = {seen: _read_before(said, place), todos: [], calls: []}
  began = compute.now()
  for step in 1..40 {
    said = _compacted(said)
    last = step == 40 or compute.now() - began > 480
    // The person always gets an answer in words, never an error.
    try {m = _think(place, said, last ? nothing : _tools(mode), last)} catch e {
      return _ended("I couldn't reach an AI just now, so I stopped here. Send it again in a moment and I'll pick up where I left off.", messages, added, state)
    }
    said += [m]
    added += [m]
    if m.tool_calls == nothing {return _ended(m.content, messages, added, state)}
    for c in m.tool_calls {
      r = _acted(place, state, c)
      state = r.state
      got = r.said
      said += [{role: "tool", tool_call_id: c.id, content: got}]
      added += [{role: "tool", tool_call_id: c.id, content: std.slice(got, 0, 4000)}]
    }
  }
  return _ended("It stopped after 40 steps.", messages, added, state)
}

////////// 2. ITS AI //////////

fn _think(place, said, given, last) "The model's next message for the conversation, from the AI it uses: the one kept, else the ai lingo on this computer, else the first the lingo function finds. One that fails, or answers in another shape, is forgotten and the next found is tried, then the first again, with a pause between. last asks it to stop and say how far it got." {
  body = {messages: [{role: "system", content: place.guide}] + said + (last ? [{role: "user", content: "You have used every step you may take in one go. Use no tools now. Say what is done, what is left, and what should happen next, briefly, so the work can go on when the person asks."}] : [])}
  if given != nothing {body.tools = given}
  address = store.get("coding/ai") ?? "ai/chat"
  ways = nothing
  tried = []
  why = ""
  for attempt in 1..4 {
    if address == nothing or address in tried {
      if ways == nothing {ways = _found_ais()}
      address = ways[0]
      for w in ways {
        if not (w in tried) {
          address = w
          break
        }
      }
    }
    try {
      got = _heard(use(address, body))
      if store.get("coding/ai") != address {store.put("coding/ai", address)}
      return got
    } catch e {
      why = std.text(e)
      tried += [address]
      try {store.delete("coding/ai")} catch e {}
      if attempt < 4 {compute.wait(attempt * 2)}
    }
  }
  fail "Coding Agent couldn't get an answer from an AI: {why}"
}

fn _found_ais() "The addresses of the AIs the lingo function finds for it, best first." {
  ways = lingo.ways("An AI model for a lingo's own agent: the model's next message for a conversation of chat messages in OpenAI's format, which may call the tools it is given", {})
  out = []
  for w in ways ?? [] {out += [w.address]}
  if std.length(out) == 0 {fail "No AI model was found for it. Turn on the agent lingo, or sign in to Onsay for its AI"}
  return out
}

fn _heard(a) "An AI's answer as the next message, from OpenAI's whole answer or the message alone, or a failure when it is in another shape." {
  m = a.choices != nothing ? a.choices[0].message : a
  if m.content == nothing and m.tool_calls == nothing {fail "that AI didn't answer with a chat message"}
  out = {role: "assistant", content: m.content ?? ""}
  calls = []
  for c in m.tool_calls ?? [] {
    calls += [{id: c.id, type: "function", function: {name: c.function.name, arguments: c.function.arguments ?? "\{\}"}}]
  }
  if std.length(calls) > 0 {out.tool_calls = calls}
  if m.thinking != nothing {out.thinking = m.thinking}
  return out
}

fn _compacted(said) "The conversation summed up when it is past about 100,000 tokens, its newest 60,000 characters kept as they are from a message of the model's, and the rest written out and summed up by the AI." {
  if std.length(std.to_json(said)) < 400000 {return said}
  kept = 0
  cut = std.length(said)
  for i in 0..std.length(said) - 1 {
    at = std.length(said) - 1 - i
    kept += std.length(std.to_json(said[at]))
    if kept > 60000 {break}
    if said[at].role == "assistant" {cut = at}
  }
  if cut < 2 {return said}
  told = ""
  for m in std.slice(said, 0, cut) {
    if m.role == "user" {told += "[Person]: {std.text(m.content)}\n"}
    if m.role == "assistant" and m.content != "" {told += "[Agent]: {m.content}\n"}
    for c in m.tool_calls ?? [] {told += "[Tool call]: {c.function.name} {std.slice(std.text(c.function.arguments), 0, 2000)}\n"}
    if m.role == "tool" {told += "[Tool result]: {std.slice(std.text(m.content), 0, 2000)}\n"}
  }
  summary = _think({guide: "Sum up the coding session below so another coding agent can carry on with no other context. Keep exact paths, names, commands and error messages. Use terse bullets, and these headings in this order, each kept even when empty with \"(none)\":\n## Objective\n## Important details\n## Done\n## In progress\n## Blocked\n## Next\n## Relevant files\nDon't continue the work or answer anything in it. Only write the summary."}, [{role: "user", content: "<conversation>\n" + told + "</conversation>"}], nothing, false).content
  return [{role: "user", content: "What have we done so far?"}, {role: "assistant", content: summary}] + std.slice(said, cut)
}

////////// 3. ITS TOOLS //////////

fn _tools(mode) "Its tools as OpenAI writes them, plan mode's without those that change files." {
  s = {type: "string"}
  n = {type: "integer"}
  all = [
    {name: "read", does: "Reads a file, or lists a folder, on this computer. A file comes back with each line numbered as \"12: text\": the numbers are not part of the file. Up to 2000 lines from offset, lines over 2000 characters cut, 50 KB at most, with how to read on. Read several files at once when you need them. Read a larger window rather than many small ones. Use grep to find something in a large file", takes: {filePath: {type: "string", description: "The file or folder, absolute or relative to the project folder"}, offset: {type: "integer", description: "The first line to read, from 1"}, limit: {type: "integer", description: "How many lines, 2000 when left out"}}, needs: ["filePath"]},
    {name: "glob", does: "Finds files by name pattern, like **/*.ts or src/**/*.css, under a folder, leaving out node_modules, .git and build folders. Gives their paths, at most 100. Search several patterns at once when they may help", takes: {pattern: s, path: {type: "string", description: "The folder to look in, the project folder when left out"}}, needs: ["pattern"]},
    {name: "grep", does: "Searches the contents of files with a regular expression, like log.*Error or function\\s+\\w+, under a folder, leaving out node_modules, .git and build folders. Gives each file and its matching lines with their numbers, at most 100", takes: {pattern: {type: "string", description: "The regular expression"}, path: {type: "string", description: "The folder or file to search, the project folder when left out"}, include: {type: "string", description: "Only files whose names match, like *.js or *.\{ts,tsx\}"}}, needs: ["pattern"]},
    {name: "shell", does: "Runs a command in this computer's shell, Windows PowerShell on Windows and sh elsewhere, and gives its exit code and the end of its output, at most 50 KB. For git, builds, tests, package managers and running programs. Not for reading, finding, searching or editing files: the other tools do that. Quote paths with spaces. Use workdir instead of changing folder in the command. Commands that need the person, or never end, like a dev server, will be stopped after 10 minutes", takes: {command: s, workdir: {type: "string", description: "The folder to run it in, the project folder when left out"}}, needs: ["command"]},
    {name: "todowrite", does: "Writes your task list, the whole of it each time, to plan and track work of three or more steps. Exactly one item in_progress while work remains. Mark an item completed as soon as it is truly done and checked, never in batches, and add follow ups you find. Skip it for a single simple step", takes: {todos: {type: "array", items: {type: "object", properties: {content: {type: "string", description: "The task, short and specific"}, status: {type: "string", description: "pending, in_progress, completed or cancelled"}, priority: {type: "string", description: "high, medium or low"}}, required: ["content", "status"]}}}, needs: ["todos"]},
    {name: "webfetch", does: "Fetches a web page or file from a full http or https address and gives its text, at most 50 KB. Only addresses the person or the project gave, or documentation you are sure of: never guess an address", takes: {url: s}, needs: ["url"]}
  ]
  if mode != "plan" {
    all += [
      {name: "edit", does: "Replaces text in a file. oldString must be text read from the file, indentation exact and no line numbers, and match one place: include more lines around it when it is in several, or set replaceAll to change every one, like renaming. When it doesn't match exactly, looser matches are tried, each only when it matches one place. Read the file before editing it. To make a new file, give an empty oldString", takes: {filePath: s, oldString: s, newString: s, replaceAll: {type: "boolean"}}, needs: ["filePath", "oldString", "newString"]},
      {name: "write", does: "Writes a whole file, replacing what is there and making its folders. Read an existing file before writing over it. Prefer edit for changing part of a file. Never write documentation files unless asked", takes: {filePath: s, content: s}, needs: ["filePath", "content"]}
    ]
  }
  out = []
  for t in all {
    out += [{type: "function", function: {name: t.name, description: t.does, parameters: {type: "object", properties: t.takes, required: t.needs}}}]
  }
  return out
}

fn _acted(place, state, c) "What one tool call gave, \{said, state}: its result as text, or what went wrong, for the model to act on, and the state after it, since a function changes only its own copy: the files read and the calls made." {
  name = text.lower(std.text(c.function.name))
  a = {}
  try {
    a = std.from_json(c.function.arguments ?? "\{\}") ?? {}
  } catch e {
    return {said: "The arguments for {name} aren't valid JSON: {e}. Send them again as a JSON object.", state: state}
  }
  key = name + " " + std.to_json(a)
  state.calls += [key]
  n = std.length(state.calls)
  if n >= 3 and state.calls[n - 2] == key and state.calls[n - 3] == key {
    return {said: "You have made this same call three times in a row, and it gives the same answer. Try something else.", state: state}
  }
  try {chat.step(_said_step(name, a))} catch e {}
  if name == "todowrite" {state.todos = a.todos ?? []}
  got = _done_by(place, state, name, a)
  if (name == "read" or name == "write" or name == "edit") and not text.starts_with(got, "Error") {state.seen[_whole(place, a.filePath ?? a.path)] = true}
  return {said: got, state: state}
}

fn _done_by(place, state, name, a) "What a tool gave, as text, or what went wrong." {
  try {
    if name == "read" {return _read_file(place, a)}
    if name == "edit" {return _edit_file(place, state, a)}
    if name == "write" {return _write_file(place, state, a)}
    if name == "glob" {return _glob(place, a)}
    if name == "grep" {return _grep(place, a)}
    if name == "shell" or name == "bash" {return _shell(place, a)}
    if name == "todowrite" {return std.to_json(a.todos ?? [])}
    if name == "webfetch" {return _fetched(a)}
    return "There's no tool called {name}. Use read, edit, write, glob, grep, shell, todowrite or webfetch."
  } catch e {
    return "Error: {e}"
  }
}

fn _said_step(name, a) "What a call is doing, in a few words, for the person's chat." {
  if name == "read" {return "Reading {a.filePath}"}
  if name == "edit" or name == "write" {return "Changing {a.filePath}"}
  if name == "glob" or name == "grep" {return "Searching for {a.pattern}"}
  if name == "shell" {return "Running {std.slice(std.text(a.command), 0, 80)}"}
  if name == "todowrite" {return "Planning"}
  return "Working"
}

fn _whole(place, p) "A path the model gave, made whole: absolute as it is, else under the project folder." {
  if p == nothing or p == "" {return place.folder}
  p = text.replace(std.text(p), "\\", "/")
  if text.starts_with(p, "/") or text.starts_with(p, "~") or std.slice(p, 1, 2) == ":" {return p}
  return place.folder + "/" + p
}

fn _literal(words) "Text as a PowerShell here-string, which needs no escaping, kept to one line." {
  return "@'\n" + text.replace(text.replace(std.text(words), std.from_bytes([13]), " "), "\n", " ") + "\n'@"
}

fn _quoted(words) "Text as one sh word in single quotes." {
  return "'" + text.replace(std.text(words), "'", "'\\''") + "'"
}

fn _powershell(values, script) "A PowerShell script given its values as $a, from JSON, and its output, or its words as a failure." {
  done = commands.run("$a = " + _literal(std.to_json(values)) + " | ConvertFrom-Json\n" + script)
  if done.code != 0 {fail done.out}
  return done.out
}

fn _read_file(place, a) {
  p = _whole(place, a.filePath ?? a.path)
  if place.windows {return _powershell({path: p, offset: a.offset ?? 1, limit: a.limit ?? 2000}, "$ErrorActionPreference='Stop'\ntrap \{[Console]::Out.Write($_.Exception.GetBaseException().Message);exit 1\}\n$p=$a.path\nif(-not (Test-Path -LiteralPath $p))\{\n  $dir=Split-Path -Parent $p; $leaf=Split-Path -Leaf $p; $near=@()\n  if($dir -and (Test-Path -LiteralPath $dir))\{$near=@(Get-ChildItem -LiteralPath $dir -Force | Where-Object \{$_.Name -like \"*$leaf*\" -or $leaf -like \"*$($_.Name)*\"\} | Select-Object -First 3 | ForEach-Object \{$_.FullName\})\}\n  $m=\"File not found: $p\"; if($near.Count)\{$m+=\"`n`nDid you mean one of these?`n\"+($near -join \"`n\")\}\n  [Console]::Out.Write($m); exit 1\n\}\n$from=[Math]::Max(1,[int]$a.offset); $take=2000; if($a.limit)\{$take=[int]$a.limit\}\nif(Test-Path -LiteralPath $p -PathType Container)\{\n  $all=@(Get-ChildItem -LiteralPath $p -Force | Sort-Object Name | ForEach-Object \{if($_.PSIsContainer)\{$_.Name+'/'\}else\{$_.Name\}\})\n  $page=@($all | Select-Object -Skip ($from-1) -First $take); $last=$from-1+$page.Count\n  $out=\"$p (a folder)`n\"+($page -join \"`n\")\n  if($last -lt $all.Count)\{$out+=\"`n(Showing $from-$last of $($all.Count). Use offset=$($last+1) for more.)\"\}else\{$out+=\"`n($($all.Count) entries)\"\}\n  [Console]::Out.Write($out); exit 0\n\}\n$binary=' .zip .tar .gz .7z .exe .dll .so .dylib .class .jar .war .doc .docx .xls .xlsx .ppt .pptx .odt .ods .odp .bin .dat .obj .o .a .lib .wasm .pyc .pyo .png .jpg .jpeg .gif .webp .ico .pdf .mp3 .mp4 '\n$ext=[IO.Path]::GetExtension($p).ToLower()\nif($ext -and $binary.Contains(\" $ext \"))\{[Console]::Out.Write(\"Cannot read binary file: $p\"); exit 1\}\n$fs=[IO.File]::OpenRead($p); $head=New-Object byte[] 4096; $got=$fs.Read($head,0,4096); $fs.Close()\nfor($i=0;$i -lt $got;$i++)\{if($head[$i] -eq 0)\{[Console]::Out.Write(\"Cannot read binary file: $p\"); exit 1\}\}\n$lines=[IO.File]::ReadAllLines($p)\nif($lines.Count -eq 0)\{[Console]::Out.Write(\"$p is empty\"); exit 0\}\nif($from -gt $lines.Count)\{[Console]::Out.Write(\"Offset $from is past the end of the file, which has $($lines.Count) lines\"); exit 1\}\n$sb=New-Object Text.StringBuilder; $last=$from-1; $capped=$false\nfor($i=$from-1;$i -lt [Math]::Min($lines.Count,$from-1+$take);$i++)\{\n  $l=$lines[$i]; if($l.Length -gt 2000)\{$l=$l.Substring(0,2000)+'... (line cut at 2000 characters)'\}\n  if($sb.Length+$l.Length -gt 51200)\{$capped=$true; break\}\n  [void]$sb.Append($i+1).Append(': ').Append($l).Append(\"`n\"); $last=$i+1\n\}\nif($capped)\{[void]$sb.Append(\"`n(Cut at 50 KB: lines $from-$last shown. Use offset=$($last+1) to go on.)\")\}\nelseif($last -lt $lines.Count)\{[void]$sb.Append(\"`n(Lines $from-$last of $($lines.Count). Use offset=$($last+1) to go on.)\")\}\nelse\{[void]$sb.Append(\"`n(End of file, $($lines.Count) lines)\")\}\n[Console]::Out.Write($sb.ToString())")}
  from = a.offset ?? 1
  to = from - 1 + (a.limit ?? 2000)
  done = commands.run("if [ -d " + _quoted(p) + " ]; then ls -1Ap " + _quoted(p) + "; elif [ -f " + _quoted(p) + " ]; then awk -v s={from} -v e={to} 'NR>=s && NR<=e \{l=$0; if (length(l)>2000) l=substr(l,1,2000) \"... (line cut)\"; print NR \": \" l\} END \{print \"(\" NR \" lines)\"\}' " + _quoted(p) + "; else echo \"File not found: " + p + "\"; exit 1; fi")
  if done.code != 0 {fail done.out}
  return std.slice(done.out, 0, 51200)
}

fn _edit_file(place, state, a) {
  p = _whole(place, a.filePath)
  old = a.oldString ?? ""
  if old == (a.newString ?? "") {return "No change: oldString and newString are the same."}
  if not files.exists(p) {
    if old != "" {return "File not found: {p}. To make a new file, give an empty oldString."}
    files.write(p, a.newString ?? "")
    return "Made {p}."
  }
  if old == "" {return "oldString can't be empty for a file that exists. Give the text to replace, or use write to replace the whole file."}
  if state.seen[p] == nothing {return "Read {p} before editing it."}
  if place.windows {return _powershell({path: p, old: old, new: a.newString ?? "", all: a.replaceAll ?? false}, "$ErrorActionPreference='Stop'\ntrap \{[Console]::Out.Write($_.Exception.GetBaseException().Message);exit 1\}\n$bytes=[IO.File]::ReadAllBytes($a.path)\n$bom=$bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF\n$raw=[Text.Encoding]::UTF8.GetString($bytes); if($bom)\{$raw=$raw.Substring(1)\}\n$crlf=$raw.Contains(\"`r`n\")\n$t=$raw.Replace(\"`r`n\",\"`n\"); $old=([string]$a.old).Replace(\"`r`n\",\"`n\"); $new=([string]$a.new).Replace(\"`r`n\",\"`n\")\n$lines=$t.Split(\"`n\"); $ol=$old.Split(\"`n\")\nif($ol.Length -gt 1 -and $ol[-1] -eq '')\{$ol=$ol[0..($ol.Length-2)]\}\n$n=$ol.Length\nfunction Window($i,$k)\{@($lines[$i..($i+$k-1)])\}\nfunction Flat($s)\{($s -replace '\\s+',' ').Trim()\}\nfunction Dedent($rows)\{\n  $m=($rows | Where-Object \{$_.Trim()\} | ForEach-Object \{$_.Length-$_.TrimStart().Length\} | Measure-Object -Minimum).Minimum\n  if(-not $m)\{$m=0\}\n  ($rows | ForEach-Object \{if($_.Length -ge $m)\{$_.Substring($m)\}else\{$_.TrimStart()\}\}) -join \"`n\"\n\}\n$found=New-Object System.Collections.Generic.List[object]\n$found.Add(@('exact',$old))\n$flatOld=Flat ($ol -join \"`n\"); $dedentOld=Dedent $ol\nforeach($how in 'ignoring spaces at line ends','ignoring spacing','ignoring indentation')\{\n  for($i=0;$i -le $lines.Length-$n;$i++)\{\n    $w=Window $i $n; $same=$false\n    if($how -eq 'ignoring spaces at line ends')\{$same=$true; for($j=0;$j -lt $n;$j++)\{if($w[$j].Trim() -ne $ol[$j].Trim())\{$same=$false;break\}\}\}\n    elseif($how -eq 'ignoring spacing')\{$same=(Flat ($w -join \"`n\")) -eq $flatOld\}\n    else\{$same=(Dedent $w) -eq $dedentOld\}\n    if($same)\{$found.Add(@($how,($w -join \"`n\")))\}\n  \}\n\}\nif($n -ge 3)\{\n  $spread=[Math]::Max(1,[Math]::Floor($n*0.25)); $anchored=@()\n  for($i=0;$i -lt $lines.Length;$i++)\{\n    if($lines[$i].Trim() -ne $ol[0].Trim())\{continue\}\n    for($k=[Math]::Max(3,$n-$spread);$k -le $n+$spread;$k++)\{\n      $e=$i+$k-1; if($e -ge $lines.Length)\{break\}\n      if($lines[$e].Trim() -ne $ol[-1].Trim())\{continue\}\n      $w=Window $i $k; $mid=[Math]::Min($k,$n)-2; $alike=0\n      for($j=1;$j -le $mid;$j++)\{if($w[$j].Trim() -eq $ol[$j].Trim())\{$alike++\}\}\n      $score=1; if($mid -gt 0)\{$score=$alike/$mid\}\n      if($score -ge 0.5)\{$anchored+=,@($score,($w -join \"`n\"))\}\n      break\n    \}\n  \}\n  if($anchored.Count)\{foreach($c in @($anchored | Sort-Object \{$_[0]\} -Descending))\{$found.Add(@('by its first and last lines',$c[1]))\}\}\n\}\n$how=$null; $several=$false\nforeach($c in $found)\{\n  $s=$c[1]; if(-not $s)\{continue\}\n  $at=$t.IndexOf($s,[StringComparison]::Ordinal); if($at -lt 0)\{continue\}\n  if($c[0] -ne 'exact' -and $s.Split(\"`n\").Length -ge [Math]::Max($n+3,$n*2))\{continue\}\n  if($a.all)\{$out=$t.Replace($s,$new); $how=$c[0]; break\}\n  if($at -ne $t.LastIndexOf($s,[StringComparison]::Ordinal))\{$several=$true; continue\}\n  $out=$t.Substring(0,$at)+$new+$t.Substring($at+$s.Length); $how=$c[0]; break\n\}\nif(-not $how)\{\n  if($several)\{[Console]::Out.Write('oldString matches more than one place. Include more of the lines around it so it matches one, or set replaceAll to change every one.'); exit 1\}\n  [Console]::Out.Write('Could not find oldString in the file. It must match the file, indentation included. Read the file again and copy the lines exactly, without line numbers.'); exit 1\n\}\nif($crlf)\{$out=$out.Replace(\"`n\",\"`r`n\")\}\n[IO.File]::WriteAllText($a.path,$out,(New-Object Text.UTF8Encoding($bom)))\n$said='Edit applied.'; if($how -ne 'exact')\{$said+=\" It matched $how.\"\}\n[Console]::Out.Write($said)")}
  done = files.edit(p, old, a.newString ?? "", a.replaceAll ?? false)
  return "Edit applied, {done.replaced} replaced."
}

fn _write_file(place, state, a) {
  p = _whole(place, a.filePath)
  if files.exists(p) and state.seen[p] == nothing {return "Read {p} before writing over it."}
  files.write(p, a.content ?? "")
  return "Wrote {p}."
}

fn _glob(place, a) "Files by name pattern, the last part of the pattern matched by name and any folders before it by path, noise left out, at most 100." {
  pattern = text.replace(std.text(a.pattern), "\\", "/")
  parts = text.split(pattern, "/")
  name = parts[-1]
  inside = []
  for x in std.slice(parts, 0, std.length(parts) - 1) {
    if x != "**" and x != "." and x != "" {inside += ["/" + x + "/"]}
  }
  out = []
  more = false
  for f in files.find(_whole(place, a.path), name) {
    path = text.replace(f.path, "\\", "/")
    if f.folder or _noise(path) {continue}
    ok = true
    for x in inside {
      if not (x in path) {ok = false}
    }
    if not ok {continue}
    if std.length(out) == 100 {
      more = true
      break
    }
    out += [path]
  }
  if std.length(out) == 0 {return "No files found"}
  return text.join(out, "\n") + (more ? "\n(Cut at 100. Use a more specific path or pattern.)" : "")
}

fn _noise(path) "Whether a path is inside a folder no one searches, like node_modules or .git." {
  for x in ["/node_modules/", "/.git/", "/dist/", "/build/", "/bin/", "/obj/", "/target/", "/vendor/", "/.venv/", "/venv/", "/__pycache__/", "/.next/", "/coverage/"] {
    if x in path {return true}
  }
  return false
}

fn _grep(place, a) {
  where = _whole(place, a.path)
  if place.windows {return _powershell({path: where, pattern: a.pattern, include: a.include}, "$ErrorActionPreference='Stop'\ntrap \{[Console]::Out.Write($_.Exception.GetBaseException().Message);exit 1\}\n$skip='[\\\\/](node_modules|\\.git|dist|build|bin|obj|target|vendor|\\.venv|venv|__pycache__|\\.next|coverage)([\\\\/]|$)'\n$files=@(Get-ChildItem -LiteralPath $a.path -Recurse -File -Force -ErrorAction SilentlyContinue | Where-Object \{$_.FullName -notmatch $skip\})\nif($a.include)\{\n  $inc=[string]$a.include; $pats=@($inc)\n  if($inc -match '^(.*)\\\{(.+)\\\}(.*)$')\{$pre=$Matches[1]; $alts=$Matches[2]; $post=$Matches[3]; $pats=@($alts.Split(',') | ForEach-Object \{$pre+$_.Trim()+$post\})\}\n  $files=@($files | Where-Object \{$name=$_.Name; @($pats | Where-Object \{$name -like $_\}).Count -gt 0\})\n\}\n$hits=@($files | Select-String -Pattern $a.pattern -ErrorAction SilentlyContinue | Select-Object -First 101)\nif(-not $hits.Count)\{[Console]::Out.Write('No files found'); exit 0\}\n$more=$hits.Count -gt 100; $hits=@($hits | Select-Object -First 100)\n$sb=New-Object Text.StringBuilder; [void]$sb.Append(\"Found $($hits.Count) matches\"); if($more)\{[void]$sb.Append(' (more not shown)')\}\n$was=''\nforeach($h in $hits)\{\n  if($h.Path -ne $was)\{[void]$sb.Append(\"`n`n\"+$h.Path+':'); $was=$h.Path\}\n  $l=$h.Line; if($l.Length -gt 300)\{$l=$l.Substring(0,300)+'...'\}\n  [void]$sb.Append(\"`n  Line $($h.LineNumber): $l\")\n\}\nif($more)\{[void]$sb.Append(\"`n`n(Cut at 100 matches. Narrow the path, include or pattern.)\")\}\n[Console]::Out.Write($sb.ToString())")}
  include = a.include == nothing ? "" : " --include=" + _quoted(a.include)
  done = commands.run("grep -rnIE --exclude-dir=node_modules --exclude-dir=.git --exclude-dir=dist --exclude-dir=build" + include + " -e " + _quoted(a.pattern) + " " + _quoted(where) + " | head -100")
  if done.out == "" {return "No files found"}
  return done.out
}

fn _shell(place, a) "A command in the project folder, or workdir, with its exit code and the end of its output." {
  where = _whole(place, a.workdir)
  command = place.windows ? "Set-Location -LiteralPath " + _literal(where) + "\n" + std.text(a.command) : "cd " + _quoted(where) + " && " + std.text(a.command)
  done = commands.run(command)
  out = done.out
  size = std.length(out)
  if size > 51200 {out = "...(the start was cut: " + std.text(size - 51200) + " characters)\n" + std.slice(out, size - 51200)}
  return "Exit code {done.code}\n" + (out == "" ? "(no output)" : out)
}

fn _fetched(a) {
  url = std.text(a.url)
  if not (text.starts_with(url, "http://") or text.starts_with(url, "https://")) {return "The address must start with http:// or https://"}
  r = http.send("GET", url, {"user-agent": "Mozilla/5.0 Coding Agent"}, nothing, 30)
  if r.status < 200 or r.status > 299 {return "{url} answered {r.status}"}
  return std.slice(r.text, 0, 51200)
}

////////// 4. ITS SETTING //////////

fn _placed(folder, mode) "The project folder and what the model is told: its guide, this computer and project, and the project's own instructions, AGENTS.md or CLAUDE.md in the folder or those above it, nearest first." {
  f = text.replace(std.text(folder), "\\", "/")
  if text.ends_with(f, "/") and std.length(f) > 3 {f = std.slice(f, 0, std.length(f) - 1)}
  windows = files.exists("C:/Windows")
  guide = "You are Coding Agent, an expert software engineer working in a person's project on their own computer. You read, write and change code, run commands and check your work, with the tools below. Use them to do the task, not to talk.\n\n# How to answer\n- Be short and direct. Your text is shown to the person as it is, in Markdown. No emojis unless asked.\n- Say what you did and what you found, plainly. Never claim something works unless you ran it and saw it work.\n- Be objective: if the person's idea has a problem, say so and why. Check before agreeing.\n- When you name code, give it as path:line so the person can open it.
- End with what you changed, as path:line, how you checked it, and anything left or worth knowing.\n\n# How to work\n- Understand before changing. Find the relevant files with glob and grep, and read them, before editing. Follow the project's own style, names, libraries and structure; never assume a library is available, check that the project uses it.\n- Plan work of three or more steps with todowrite, keep exactly one item in progress, and mark each done as soon as it is, never in batches.\n- Make the smallest change that fully does the task. Don't refactor, rename or reformat code the task doesn't touch.
- Prefer editing existing files to creating new ones. Never write documentation files unless asked.\n- After a change, verify it: run the project's tests, build, linter or the program itself when you can find how. If something fails, read the error and fix its cause, then run it again. Never run the same failing command again unchanged, and never weaken, skip or delete a test to make it pass.
- Before you finish, check that the task as it was asked is done, not only that the tests pass.\n- Call several tools at once when they don't depend on each other, like reading three files.\n- Never guess at a value a tool needs. Find it first.\n- Keep going until the task is done. Stop to ask only when you truly can't decide something only the person can.\n\n# Tools\n- read gives a file with line numbers, as \"12: text\". Those numbers are not part of the file: never put them in an edit.\n- edit replaces exact text. Copy oldString from what read gave, with its indentation, and include enough lines to match one place.\n- shell runs a command. Use it for git, builds, tests and package managers, not for reading, searching or editing files, which the other tools do better.\n- Never commit, push, change git settings or delete work unless the person asks.\n- Never put secrets or keys in code or show them."
  guide += "\n\n<env>\nProject folder: {f}\nIs it a git repository: " + (files.exists(f + "/.git") ? "yes" : "no")
  guide += "\nPlatform: " + (windows ? "Windows, its shell Windows PowerShell 5.1: chain commands with ; not &&, test $? or $LASTEXITCODE to run one only when the last worked, and there is no grep, sed, head or cat -n: use the tools" : "not Windows, its shell sh") + "\nToday: {time.now_in_words()}\n</env>"
  dir = f
  found = 0
  for depth in 1..12 {
    for name in ["AGENTS.md", "CLAUDE.md"] {
      if found < 3 and files.exists(dir + "/" + name) {
        guide += "\n\nInstructions from " + dir + "/" + name + ":\n" + std.slice(files.read(dir + "/" + name), 0, 20000)
        found += 1
        break
      }
    }
    up = text.split(dir, "/")
    if std.length(up) <= 1 or (std.length(up) == 2 and up[1] == "") {break}
    dir = text.join(std.slice(up, 0, std.length(up) - 1), "/")
    if dir == "" or std.slice(dir, std.length(dir) - 1) == ":" {break}
  }
  if mode == "plan" {guide += "\n\n" + "You are in plan mode: look, read and think only. Change no files and run nothing that changes anything; shell is only for inspecting. End with a clear, concise plan the person can approve, and ask about anything you would otherwise have to assume."}
  return {folder: f, windows: windows, guide: guide}
}

fn _read_before(said, place) "The files a conversation already read, so they may be edited." {
  seen = {}
  for m in said {
    for c in m.tool_calls ?? [] {
      if c.function.name == "read" {
        try {seen[_whole(place, std.from_json(c.function.arguments).filePath)] = true} catch e {}
      }
    }
  }
  return seen
}

fn _ended(answer, messages, added, state) "What ask gives: the answer alone, or, given a conversation, with what it added, its work in short and its task list." {
  if messages == nothing {return answer}
  steps = []
  calls = {}
  for m in added {
    for c in m.tool_calls ?? [] {calls[c.id] = "{c.function.name} {std.slice(std.text(c.function.arguments), 0, 200)}"}
    if m.role == "tool" {steps += ["- {calls[m.tool_call_id]}\n  gave: {std.slice(std.text(m.content), 0, 300)}"]}
  }
  out = {text: answer, messages: added, todos: state.todos}
  if std.length(steps) > 0 {out.work = std.slice(text.join(list.reverse(steps), "\n"), 0, 6000)}
  return out
}
