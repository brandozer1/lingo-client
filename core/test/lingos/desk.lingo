// desk - for the tests: the windows lingo, for real, on this computer. The test core may read the screen but never act
// on it, so nothing is clicked or typed.

// The windows open, and the one in front.
fn windows_open() {
  ws = window.list()
  f = window.current()
  return [std.length(ws) > 0, f == nothing or f.id > 0, ws[0].title != ""]
}

// A window's controls and words.
fn controls_read() {
  w = window.list()[0]
  c = controls.list(w, 5)
  return [std.length(c) <= 5, std.length(controls.read(w)) >= 0]
}

// Files made, read, copied, moved and found, then taken away.
fn files_used() {
  d = "~/onsay-test"
  files.make_folder(d)
  files.write(d + "/a.txt", "héllo")
  files.add_to(d + "/a.txt", " there")
  files.copy(d + "/a.txt", d + "/b.txt")
  files.move(d + "/b.txt", d + "/c.txt")
  l = files.list(d, "*.TXT")
  f = files.find("~/onsay-test", "c*")
  out = [files.read(d + "/a.txt"), files.exists(d + "/b.txt"), files.exists(d + "/c.txt"), std.length(l), f[0].name, l[0].size]
  commands.run("Remove-Item -Recurse -Force (Join-Path $HOME onsay-test)")
  return out + [files.exists(d)]
}

// A command, its answer, and what runs.
fn commands_run() {
  r = commands.run("Write-Output (2+3)")
  p = commands.running()
  return [r.code, r.out, std.length(p) > 10]
}

// A command a stop ends.
fn command_slow() {commands.run("Start-Sleep 30")}

// A picture of the screen, and its words by OCR.
fn screen_read() {
  s = look.screenshot()
  t = look.lines()
  commands.run("Get-ChildItem (Join-Path $HOME Onsay/shots) | ? LastWriteTime -gt (Get-Date).AddMinutes(-2) | Remove-Item")
  return [s.w > 0, s.h > 0, std.length(t) > 0, ".png" in s.path]
}

// A click on the first word on screen, which the test core may not do.
fn click_a_word() {input.click_text(text.split(text.trim(look.lines()[0]), " ")[0])}

// The front window in brief.
fn overview() {return look.overview(window.list()[0])}

// Lines on screen, where one is, and what is near it.
fn lines_near() {
  l = look.lines()
  w = text.split(text.trim(l[0]), " ")[0]
  return [std.length(l) > 0, std.length(look.find_text(w)) > 0, std.length(look.near(w, nothing, 400)) >= 0]
}

// The pointer's mode, changed and back.
fn cursor_modes() {return [input.cursor(), input.cursor("real"), input.cursor("virtual")]}

// Words on screen rung, then the ring taken away.
fn point_at_words() {
  w = text.split(text.trim(look.lines()[0]), " ")[0]
  b = input.point_text(w, "this one", nothing, 1)
  input.stop_pointing()
  return [b.w > 0, b.h > 0, w in b.text]
}

// Places in a window, wherever it is: pixels from its corner, shares of its size, and back from its far edges, and on
// the main screen when there is no window.
fn places() {
  w = window.list()[0]
  a = input.position_in(w, 0.5, 0.5)
  b = input.position_in(w, -20, -0.25)
  c = input.position_in(w, 10, 0)
  d = input.position_in({title: w.title}, 10, 0)
  e = input.position_in(nothing, 10, 20)
  return [a.x == w.x + math.round(w.w * 0.5), a.y == w.y + math.round(w.h * 0.5), b.x == w.x + w.w - 20, b.y == w.y + math.round(w.h * 0.75),
    c.x == w.x + 10, c.y == w.y, d == c, e.x == 10 and e.y == 20]
}

// A click in a window, which the test core may not do.
fn click_in_window() {input.click_in(window.list()[0], 0.5, 0.5)}

// Everything on screen numbered, by accessibility and OCR, inside the window.
fn elements() {
  w = window.list()[0]
  e = look.elements(w)
  ok = std.length(e) > 0
  for i, x in e {if x.id != i + 1 or x.x < 0 or x.y < 0 or x.x + x.w > w.w or x.y + x.h > w.h or x.kind == nothing {ok = false}}
  return ok
}
