# build.ps1 - builds the lingo client and packs the lingos it ships, from C with zig's C compiler (one download, no installer).
#   bin/onsay-core.exe          the lingo client for Windows, with onsay, windows and native built in
#   bin/onsay-core.lib          what it shares with native lingos, to build their libraries against
#   bin/test/onsay-core.exe     the same, its onsay talking to test/server.mjs. Same file name, since a native library binds to it
#   bin/lingos/                 agent.lingo, the one lingo that ships, packed by the lingo client itself
#   bin/linux-x64/onsay-core    the lingo client for Linux, with onsay built in, and bin/linux-arm64/onsay-core. glibc 2.28
#                               or later, and libcurl, which it loads as it starts, so it builds here with nothing of
#                               Linux's installed
# What is only made on the way, like win64.o, tlds.h, builtins.h and standard.h, goes in obj/.
$zig = (Get-Command zig -ErrorAction SilentlyContinue).Source
if (-not $zig) { $zig = Get-ChildItem "$env:LOCALAPPDATA\zig" -Recurse -Filter zig.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName }
if (-not $zig) { Write-Error "zig wasn't found. Get it from ziglang.org/download"; exit 1 }

$here = $PSScriptRoot
$lingos = "$here/../lingos"
Remove-Item -Recurse -Force "$here/bin/lingos", "$here/bin/test/lingos", "$here/obj" -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$here/bin/test", "$here/bin/lingos", "$here/obj/windows", "$here/obj/linux" | Out-Null
function src($names) { $names | ForEach-Object { "$here/src/$_.c" } }
function build($what) { & $zig cc @flags @what | Out-Host; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } }

# The web's top level domains (src/tlds.txt, IANA's list, the same file Onsay keeps) as one C text, " aaa aarp ... ", for
# core.c's rule telling a name on the web from an Onsay name.
$tlds = Get-Content "$here/src/tlds.txt" | Where-Object { $_ -and -not $_.StartsWith("#") } | ForEach-Object { $_.ToLower() }
Set-Content "$here/obj/tlds.h" "#define TLDS `" $($tlds -join ' ') `""

# A file's words as one C text. ? is escaped too, so ?? in Lingo is never read as a C trigraph.
function ctext($path) {
  return [IO.File]::ReadAllText($path) -replace '\\', '\\' -replace '"', '\"' -replace '\?', '\?' -replace "`r?`n", '\n'
}

# The standard functions, as obj/standard.h for core.c's STANDARD: each file of ../lingos/standard by its name, in Lingo.
$rows = foreach ($file in Get-ChildItem "$lingos/standard" -Filter *.ls) { "  { `"$($file.BaseName)`", `"$(ctext $file.FullName)`" }," }
[IO.File]::WriteAllLines("$here/obj/standard.h", @("// standard.h - made by build.ps1: the standard functions.", "static const standard STANDARD[] = {") + $rows + "};")

# The lingos built into the lingo client for one platform, as obj/<platform>/builtins.h for core.c's BUILTINS: each one's
# name, its openapi.json as C text, and <name>_lingo, the function in its C that adds its phrases.
function builtins($platform, $names) {
  $lines = @("// builtins.h - made by build.ps1: the lingos built into the lingo client for $platform.")
  foreach ($name in $names) { $lines += "void ${name}_lingo(void);" }
  $lines += "static const builtin BUILTINS[] = {"
  foreach ($name in $names) {
    $lines += "  { `"$name`", `"$(ctext "$lingos/$name/openapi.json")`", ${name}_lingo },"
  }
  $lines += "};"
  Set-Content "$here/obj/$platform/builtins.h" $lines
}
builtins "windows" @("onsay", "windows", "native")
builtins "linux" @("onsay")

$flags = "-std=c11", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter", "-I$here/obj", "-I$here/src"
$both = "value", "run", "phrase", "std", "compile", "zip", "puff", "sign", "monocypher", "monocypher-ed25519", "core", "host"

# Windows. native calls any Windows function by its layout, with a small vendored part of libffi (../lingos/native/libffi,
# MIT). Its chip's assembly compiles on its own; the rest is plain C.
$win = "-target", "x86_64-windows-gnu"
$ffi = "$lingos/native/libffi"
build ($win + "-DX86_WIN64", "-I$ffi/include", "-I$ffi/x86", "-c", "$ffi/x86/win64.S", "-o", "$here/obj/win64.o")
$windows = (src ($both + "platform_win")) + "$lingos/onsay/onsay.c", "$lingos/windows/windows.c", "$lingos/native/native.c"
$windows += "$ffi/prep_cif.c", "$ffi/types.c", "$ffi/x86/ffiw64.c", "$here/obj/win64.o"
$windows += "-I$here/obj/windows", "-DLINGO_CORE", "-DFFI_BUILDING", "-DX86_WIN64", "-I$ffi/include", "-I$ffi/x86"
$windows += "-lwinhttp", "-lws2_32", "-luser32", "-lbcrypt", "-lcrypt32", "-lole32", "-loleaut32", "-lshell32", "-lgdi32"
build ($win + $windows + "-o", "$here/bin/onsay-core.exe", "-Wl,--out-implib,$here/bin/onsay-core.lib")
build ($win + $windows + "-DTEST", "-o", "$here/bin/test/onsay-core.exe")

# Linux. -rdynamic shares the core's words with a native lingo's library, as onsay-core.lib does on Windows.
$linux = (src ($both + "platform_linux")) + "$lingos/onsay/onsay.c", "-I$here/obj/linux", "-DLINGO_CORE"
foreach ($arch in @{ x64 = "x86_64"; arm64 = "aarch64" }.GetEnumerator()) {
  New-Item -ItemType Directory -Force "$here/bin/linux-$($arch.Key)" | Out-Null
  $target = "-target", "$($arch.Value)-linux-gnu.2.28", "-D_POSIX_C_SOURCE=200809L"
  build ($target + $linux + "-rdynamic", "-ldl", "-lpthread", "-o", "$here/bin/linux-$($arch.Key)/onsay-core")
}
Get-ChildItem "$here/bin", "$here/obj" -Recurse -Include *.pdb, *.lib -Exclude onsay-core.lib | Remove-Item

# The agent, the one lingo that ships, packed into bin/lingos by the lingo client.
$slash = $here -replace '\\', '/'
Set-Content "$here/obj/pack.json" "{`"home`": `"$slash/obj/home`", `"lingos`": `"$slash/../lingo-library`"}"
& "$here/bin/onsay-core.exe" "$here/obj/pack.json" pack "$here/../lingo-library/agent" "$here/bin/lingos" 2>$null
if ($LASTEXITCODE -ne 0) { Write-Error "agent couldn't be packed. To see why: bin/onsay-core.exe obj/pack.json pack ../lingo-library/agent"; exit 1 }
Write-Output "built bin/onsay-core.exe, bin/test/onsay-core.exe, bin/linux-x64, bin/linux-arm64 and bin/lingos"
