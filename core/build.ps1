# build.ps1 - builds the lingo client and packs the lingos it ships, from C with zig's C compiler (one download, no installer).
#   bin/onsay-core.exe          the lingo client for Windows, with onsay and the base calls built in
#   bin/onsay-core.lib          what it shares with native lingos, to build their libraries against
#   bin/test/onsay-core.exe     the same, its onsay talking to test/server.mjs. Same file name, since a native library binds to it
#   bin/lingos/                 ai.lingo and agent.lingo, the lingos that ship, packed by the lingo client itself
#   bin/linux-x64/onsay-core    the lingo client for Linux, with onsay and the base calls built in, and bin/linux-arm64/onsay-core. glibc 2.28
#                               or later, and libcurl, which it loads as it starts, so it builds here with nothing of
#                               Linux's installed
# What is only made on the way, like win64.o, tlds.h, builtins.h and standard.h, goes in obj/.
$zig = (Get-Command zig -ErrorAction SilentlyContinue).Source
if (-not $zig) { $zig = Get-ChildItem "$env:LOCALAPPDATA\zig" -Recurse -Filter zig.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName }
if (-not $zig) { Write-Error "zig wasn't found. Get it from ziglang.org/download"; exit 1 }

$here = $PSScriptRoot
$lingos = "$here/../lingos"
Remove-Item -Recurse -Force "$here/bin/lingos", "$here/bin/test/lingos", "$here/obj" -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$here/bin/test", "$here/bin/lingos", "$here/obj" | Out-Null
# Each program linked stripped, with no symbols or debug information, and what is never used left out.
$small = "-s", "-Wl,--gc-sections"
function build($what) { & $zig cc @flags @what | Out-Host; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } }

# The web's top level domains (tlds.txt, IANA's list, the same file Onsay keeps) as one C text, " aaa aarp ... ", for
# lingo.c's rule telling a name on the web from an Onsay name.
$tlds = Get-Content "$here/tlds.txt" | Where-Object { $_ -and -not $_.StartsWith("#") } | ForEach-Object { $_.ToLower() }
Set-Content "$here/obj/tlds.h" "#define TLDS `" $($tlds -join ' ') `""

# A file's words as one C text. ? is escaped too, so ?? in Lingo is never read as a C trigraph.
function ctext($path) {
  return [IO.File]::ReadAllText($path) -replace '\\', '\\' -replace '"', '\"' -replace '\?', '\?' -replace "`r?`n", '\n'
}

# The standard functions, as obj/standard.h for lingo.c's STANDARD: each file of ../lingos/standard by its name, in Lingo.
$rows = foreach ($file in Get-ChildItem "$lingos/standard" -Filter *.ls) { "  { `"$($file.BaseName)`", `"$(ctext $file.FullName)`" }," }
[IO.File]::WriteAllLines("$here/obj/standard.h", @("// standard.h - made by build.ps1: the standard functions.", "static const standard STANDARD[] = {") + $rows + "};")

# The lingos built into the lingo client, as obj/builtins.h for lingo.c's BUILTINS: each one's name, its openapi.json as C
# text, and <name>_lingo, the function in its C that adds its phrases.
$names = @("onsay")
$lines = @("// builtins.h - made by build.ps1: the lingos built into the lingo client.")
foreach ($name in $names) { $lines += "void ${name}_lingo(void);" }
$lines += "static const builtin BUILTINS[] = {"
foreach ($name in $names) { $lines += "  { `"$name`", `"$(ctext "$lingos/$name/openapi.json")`", ${name}_lingo }," }
$lines += "};"
Set-Content "$here/obj/builtins.h" $lines

# Small: built for size, each function and value in a section of its own, so the linker drops those never used. The
# test build is the same, so what is tested is what ships.
$flags = "-std=c11", "-Os", "-ffunction-sections", "-fdata-sections"
$flags += "-Wall", "-Wextra", "-Wno-unused-parameter", "-I$here/obj", "-I$here", "-I$here/vendor"
# native.call calls any function of the system by its layout, with a small part of libffi (vendor/libffi, MIT): the
# same C everywhere, and each chip's own part, its assembly compiled on its own.
$ffi = "$here/vendor/libffi"
$both = "$here/lingo.c", "$here/vendor/puff.c", "$here/vendor/monocypher.c", "$here/vendor/monocypher-ed25519.c"
$both += "$ffi/prep_cif.c", "$ffi/types.c", "-DFFI_BUILDING", "-I$ffi/include"

# Windows.
$win = "-target", "x86_64-windows-gnu"
build ($win + "-DX86_WIN64", "-I$ffi/include", "-I$ffi/x86", "-c", "$ffi/x86/win64.S", "-o", "$here/obj/win64.o")
$windows = $both + "$lingos/windows/windows.c", "$lingos/onsay/agent.c", "-I$lingos/onsay"
$windows += "$ffi/x86/ffiw64.c", "$here/obj/win64.o", "-DLINGO_CORE", "-DX86_WIN64", "-I$ffi/x86"
$windows += "-lwinhttp", "-lws2_32", "-luser32", "-lbcrypt", "-lcrypt32", "-lole32", "-loleaut32", "-lshell32", "-lgdi32"
build ($win + $windows + $small + "-o", "$here/bin/onsay-core.exe", "-Wl,--out-implib,$here/bin/onsay-core.lib")
build ($win + $windows + $small + "-DTEST", "-o", "$here/bin/test/onsay-core.exe")

# The floor, the new lingo client: Lingo, its checks, its log and the hardware calls, nothing else (floor.c).
$floor = "$here/floor.c", "$here/vendor/monocypher.c", "$here/vendor/monocypher-ed25519.c"
$floorwin = "-lwinhttp", "-lws2_32", "-luser32", "-lbcrypt", "-lcrypt32", "-lole32", "-loleaut32", "-lshell32", "-lgdi32"
build ($win + $floor + $floorwin + $small + "-o", "$here/bin/floor.exe")
build ($win + $floor + $floorwin + $small + "-DTEST", "-o", "$here/bin/test/floor.exe")
# And with the lingos a Windows computer carries built in: its base calls (files, commands, the screen and input) and the web.
$withwin = "$lingos/windows/windows.c", "$lingos/web/web.c", "-I$lingos/onsay", "-DLINGOS=X(base_calls)X(web_calls)"
New-Item -ItemType Directory -Force "$here/bin/windows", "$here/bin/test/windows" | Out-Null
build ($win + $floor + $withwin + $floorwin + $small + "-o", "$here/bin/windows/floor.exe")
build ($win + $floor + $withwin + $floorwin + $small + "-DTEST", "-o", "$here/bin/test/windows/floor.exe")
# The same for 32-bit Windows, 7 and later: bare, and with the lingos a Windows computer carries.
$win32 = "-target", "x86-windows-gnu"
New-Item -ItemType Directory -Force "$here/bin/x86/windows", "$here/bin/test/x86/windows" | Out-Null
build ($win32 + $floor + $floorwin + $small + "-o", "$here/bin/x86/floor.exe")
build ($win32 + $floor + $withwin + $floorwin + $small + "-o", "$here/bin/x86/windows/floor.exe")
build ($win32 + $floor + $floorwin + $small + "-DTEST", "-o", "$here/bin/test/x86/floor.exe")
build ($win32 + $floor + $withwin + $floorwin + $small + "-DTEST", "-o", "$here/bin/test/x86/windows/floor.exe")

# Linux. -rdynamic shares the core's words with a native lingo's library, as onsay-core.lib does on Windows.
$linux = $both + "$lingos/linux/linux.c", "$lingos/onsay/agent.c", "-I$lingos/onsay", "-DLINGO_CORE"
# Each chip: its target, its folder in vendor/libffi, the define libffi wants, and its files there, GNU C and assembly,
# each compiled on its own.
$chips = @{ x64 = @("x86_64", "x86", "X86_64", "ffi64.c ffiw64.c unix64.S win64.S"); arm64 = @("aarch64", "aarch64", "AARCH64", "ffi.c sysv.S") }
foreach ($arch in $chips.GetEnumerator()) {
  $cpu, $dir, $define, $files = $arch.Value
  New-Item -ItemType Directory -Force "$here/bin/linux-$($arch.Key)" | Out-Null
  $target = "-target", "$cpu-linux-gnu.2.28", "-D_POSIX_C_SOURCE=200809L", "-D$define", "-I$ffi/$dir"
  $chip = @()
  foreach ($one in $files -split " ") {
    build ($target + "-std=gnu11", "-DFFI_BUILDING", "-I$ffi/include", "-c", "$ffi/$dir/$one", "-o", "$here/obj/$one-$cpu.o")
    $chip += "$here/obj/$one-$cpu.o"
  }
  build ($target + $linux + $chip + $small + "-rdynamic", "-ldl", "-lpthread", "-o", "$here/bin/linux-$($arch.Key)/onsay-core")
  build ($target + $floor + $small + "-ldl", "-lpthread", "-o", "$here/bin/linux-$($arch.Key)/floor")
}
Get-ChildItem "$here/bin", "$here/obj" -Recurse -Include *.pdb, *.lib -Exclude onsay-core.lib | Remove-Item

# The lingos that ship, ai, the person's AI, and agent, on it, packed into bin/lingos by the lingo client.
$slash = $here -replace '\\', '/'
Set-Content "$here/obj/pack.json" "{`"home`": `"$slash/obj/home`", `"lingos`": `"$slash/../lingo-library`"}"
foreach ($name in @("ai", "agent")) {
  & "$here/bin/onsay-core.exe" "$here/obj/pack.json" pack "$here/../lingo-library/$name" "$here/bin/lingos" 2>$null
  if ($LASTEXITCODE -ne 0) { Write-Error "$name couldn't be packed. To see why: bin/onsay-core.exe obj/pack.json pack ../lingo-library/$name"; exit 1 }
}
Write-Output "built bin/onsay-core.exe, bin/floor.exe, bin/test, bin/linux-x64, bin/linux-arm64 and bin/lingos"
