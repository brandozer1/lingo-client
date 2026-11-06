// video - plays an MP4 on a screen, to try screens out: its frames shown one after another as pictures, at the speed
// the video plays, with no sound.
//
// 1. play(file=nothing)   -> {frames, seconds}: the video at file, else the one its file setting names, played on a
//                            screen of its own, Video, until it ends
//
// It turns the video into frames with ffmpeg, which it fetches the first time, one version pinned in release() and
// checked against its SHA-256, into a folder of its own, lingo-video in AppData\Local. Windows only, so far. ffmpeg
// writes each frame as a JPEG in time with the video, 12 a second and 640 wide; a PowerShell loop beside it turns each
// into base64 text, the way a lingo reads a picture, keeping only the newest; and play shows each new one on the screen,
// so one it falls behind on is skipped and the video keeps its speed. Starting another stops the one before. A play is
// one run of the lingo client, so a video plays for 10 minutes at most.

////////// 1. PLAYING //////////

// 1. Plays an MP4 video on a screen, frame by frame, without sound, until it ends: file is its path on this computer,
// else the video named in video's settings.
fn play(file=nothing) {
  path = text.replace(std.text(file ?? setting("file") ?? ""), "\\", "/")
  if path == "" {fail "Choose the video to play in video's settings: its file, like C:/Videos/clip.mp4"}
  if not files.exists(path) {fail "There's no video at {path}"}
  place = installed()
  dir = place.folder + "/play" + std.text(compute.now())
  started = commands.start(played(place, path, dir))
  s = screen.open("Video")
  began = compute.now()
  last = ""
  shown = 0
  while true {
    n = ""
    try {n = files.read(dir + "/count.txt")} catch e {}
    if n != "" and n != last {
      try {
        screen.show(s, files.read(dir + "/frame.txt"))
        last = n
        shown += 1
      } catch e {}
    } else if files.exists(dir + "/done.txt") {
      break
    } else {
      compute.wait(0.02)
    }
  }
  if shown == 0 {
    why = commands.output(started.id).out
    try {why = files.read(dir + "/errors.txt") + why} catch e {}
    fail "ffmpeg couldn't play {path}: {why}"
  }
  return {frames: shown, seconds: compute.now() - began}
}

////////// HELPERS //////////

// For video's own use: the ffmpeg it fetches, by its version, from Gyan's builds on GitHub, and its SHA-256. A new
// version is a new entry here.
fn release() {
  return {
    version: "9.0.2",
    url: "https://github.com/GyanD/codexffmpeg/releases/download/9.0.2/ffmpeg-9.0.2-essentials_build.zip",
    sha256: "60f467265b1e312373dbcd92200c2618a74850f98d3d078e94296bb3fa2047ba",
    exe: "ffmpeg-9.0.2-essentials_build/bin/ffmpeg.exe"
  }
}

// For video's own use: where ffmpeg and the frames are on this computer, {exe, folder}, kept in store, fetched and
// checked first when it isn't there or is another version.
fn installed() {
  r = release()
  kept = store.get("video/" + r.version)
  p = kept == nothing ? nothing : std.from_json(kept)
  if p != nothing and files.exists(p.exe) {return p}
  if not files.exists("C:/Windows") {fail "video plays on Windows so far"}
  done = commands.run(text.join([
    "$ErrorActionPreference='Stop'",
    "$folder = Join-Path $env:LOCALAPPDATA 'lingo-video'",
    "$dir = Join-Path $folder " + quoted(r.version),
    "$exe = Join-Path $dir " + quoted(r.exe),
    "if (-not (Test-Path $exe)) \{",
    "  New-Item -ItemType Directory -Force $dir | Out-Null",
    "  $zip = Join-Path $dir 'ffmpeg.zip'",
    "  & (Join-Path $env:SystemRoot 'System32/curl.exe') -fsSL --retry 5 --retry-all-errors -C - -o $zip " + quoted(r.url),
    "  if ($LASTEXITCODE) \{ throw 'The download stopped partway. Try again and it carries on from there' \}",
    "  if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne '" + r.sha256 + "') \{ Remove-Item $zip; throw 'The download did not match its checksum' \}",
    "  & (Join-Path $env:SystemRoot 'System32/tar.exe') -xf $zip -C $dir",
    "  if ($LASTEXITCODE) \{ throw 'The download could not be unpacked' \}",
    "  Remove-Item $zip",
    "\}",
    "[Console]::Out.Write($exe.Replace('\\', '/') + '|' + $folder.Replace('\\', '/'))"
  ], "\n"))
  if done.code != 0 {fail "ffmpeg couldn't be fetched: {done.out}"}
  parts = text.split(text.trim(done.out), "|")
  p = {exe: parts[0], folder: parts[1]}
  store.put("video/" + r.version, std.to_json(p))
  return p
}

// For video's own use: the PowerShell that plays path into dir, run in the background. It stops any play before it,
// each in a folder of its own beside dir, and takes those away. Then ffmpeg writes frames in time with the video, and
// each finished one becomes frame.txt, base64, then count.txt, its number, until ffmpeg ends or this play is stopped,
// and done.txt says it ended. What ffmpeg says goes in errors.txt.
fn played(place, path, dir) {
  opts = "-loglevel error -re -i \"" + path + "\" -vf fps=12,scale=640:-2 -q:v 5 \"" + dir + "/f%06d.jpg\""
  return text.join([
    "$dir = " + quoted(dir),
    "Get-ChildItem -LiteralPath " + quoted(place.folder) + " -Directory -Filter 'play*' | ForEach-Object \{ New-Item -ItemType File -Force (Join-Path $_.FullName 'stop') | Out-Null \}",
    "Start-Sleep -Milliseconds 300",
    "Get-ChildItem -LiteralPath " + quoted(place.folder) + " -Directory -Filter 'play*' | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue",
    "New-Item -ItemType Directory -Force $dir | Out-Null",
    "$p = Start-Process -FilePath " + quoted(place.exe) + " -ArgumentList " + quoted(opts) + " -NoNewWindow -PassThru -RedirectStandardError (Join-Path $dir 'errors.txt')",
    "$n = 1",
    "while ($true) \{",
    "  if ((Test-Path (Join-Path $dir 'stop')) -or -not (Test-Path $dir)) \{ $p.Kill(); break \}",
    "  $f = Join-Path $dir ('f' + $n.ToString('000000') + '.jpg')",
    "  $next = Join-Path $dir ('f' + ($n + 1).ToString('000000') + '.jpg')",
    "  if ((Test-Path $next) -or ($p.HasExited -and (Test-Path $f))) \{",
    "    [IO.File]::WriteAllText((Join-Path $dir 'frame.part'), [Convert]::ToBase64String([IO.File]::ReadAllBytes($f)))",
    "    Move-Item -Force (Join-Path $dir 'frame.part') (Join-Path $dir 'frame.txt')",
    "    [IO.File]::WriteAllText((Join-Path $dir 'count.txt'), [string]$n)",
    "    Remove-Item $f",
    "    $n++",
    "  \} elseif ($p.HasExited) \{ break \} else \{ Start-Sleep -Milliseconds 10 \}",
    "\}",
    "[IO.File]::WriteAllText((Join-Path $dir 'done.txt'), [string]$p.ExitCode)"
  ], "\n")
}

// For video's own use: words as one quoted word in PowerShell.
fn quoted(words) {return "'" + text.replace(std.text(words), "'", "''") + "'"}
