# build.ps1 - builds the lingo client and its device lingos from C with zig's C compiler (one download, no installer).
#   bin/onsay-core.exe          the lingo client for Windows
#   bin/onsay-core.lib          what it shares with native lingos, to build their libraries against
#   bin/test/onsay-core.exe     the same, talking to test/server.mjs. Same file name, since a native library binds to it
#   bin/lingos/                 every lingo in ../lingos, windows and native with their libraries built, for the Windows app
#   bin/linux-x64/onsay-core    the lingo client for Linux, and bin/linux-arm64/onsay-core. glibc 2.28 or later, and
#                               libcurl, which it loads as it starts, so it builds here with nothing of Linux's installed
# What is only made on the way, like win64.o, goes in obj/.
$zig = (Get-Command zig -ErrorAction SilentlyContinue).Source
if (-not $zig) { $zig = Get-ChildItem "$env:LOCALAPPDATA\zig" -Recurse -Filter zig.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName }
if (-not $zig) { Write-Error "zig wasn't found. Get it from ziglang.org/download"; exit 1 }

$here = $PSScriptRoot
New-Item -ItemType Directory -Force "$here/bin/test", "$here/bin/lingos/windows", "$here/bin/lingos/native", "$here/obj" | Out-Null
function src($names) { $names | ForEach-Object { "$here/src/$_.c" } }
function build($what) { & $zig cc @flags @what | Out-Host; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } }
$flags = "-std=c11", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter"
$win = "-target", "x86_64-windows-gnu"

$core = src "value", "run", "phrase", "std", "compile", "core", "host", "platform_win"
build ($win + $core + "-DLINGO_CORE", "-lwinhttp", "-lws2_32", "-luser32", "-o", "$here/bin/onsay-core.exe", "-Wl,--out-implib,$here/bin/onsay-core.lib")
build ($win + $core + "-DLINGO_CORE", "-DTEST", "-lwinhttp", "-lws2_32", "-luser32", "-o", "$here/bin/test/onsay-core.exe")
# Linux. -rdynamic shares the core's words with a native lingo's library, as onsay-core.lib does on Windows.
$linux = src "value", "run", "phrase", "std", "compile", "core", "host", "platform_linux"
foreach ($arch in @{ x64 = "x86_64"; arm64 = "aarch64" }.GetEnumerator()) {
  New-Item -ItemType Directory -Force "$here/bin/linux-$($arch.Key)" | Out-Null
  build ("-target", "$($arch.Value)-linux-gnu.2.28", "-D_POSIX_C_SOURCE=200809L" + $linux + "-DLINGO_CORE", "-rdynamic", "-ldl", "-lpthread", "-o", "$here/bin/linux-$($arch.Key)/onsay-core")
}

# windows, and native: calls any Windows function by its layout, with a small vendored part of libffi
# (../lingos/native/libffi, MIT). Its chip's assembly compiles on its own with -DLIBFFI_ASM; the rest is plain C.
build ($win + "-shared", "-DLINGO_LIBRARY", "-I$here/src", "$here/../lingos/windows/windows.c", "$here/bin/onsay-core.lib",
  "-lole32", "-loleaut32", "-lshell32", "-lgdi32", "-luser32", "-o", "$here/bin/lingos/windows/windows.dll")
$ffi = "$here/../lingos/native/libffi"
build ($win + "-DLIBFFI_ASM", "-DX86_WIN64", "-I$ffi/include", "-I$ffi/x86", "-c", "$ffi/x86/win64.S", "-o", "$here/obj/win64.o")
build ($win + "-shared", "-DLINGO_LIBRARY", "-DFFI_BUILDING", "-DX86_WIN64", "-I$ffi/include", "-I$ffi/x86", "-I$here/src",
  "$here/../lingos/native/native.c", "$ffi/prep_cif.c", "$ffi/types.c", "$ffi/x86/ffiw64.c", "$here/obj/win64.o", "$here/bin/onsay-core.lib",
  "-o", "$here/bin/lingos/native/native.dll")

# Each lingo's lingo.json beside its library, sources left out. One no longer in ../lingos is taken out, so the app never
# ships it, and the .pdb and .lib files zig leaves beside each library go.
Get-ChildItem "$here/bin/lingos" -Directory | Where-Object { -not (Test-Path "$here/../lingos/$($_.Name)") } | Remove-Item -Recurse -Force
Get-ChildItem "$here/../lingos" -Directory | ForEach-Object { Copy-Item "$($_.FullName)/lingo.json" "$here/bin/lingos/$($_.Name)/" }
Get-ChildItem "$here/bin" -Recurse -Include *.pdb, *.lib -Exclude onsay-core.lib | Remove-Item
Write-Output "built bin/onsay-core.exe, bin/test/onsay-core.exe, bin/linux-x64, bin/linux-arm64 and bin/lingos"
