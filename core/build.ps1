# build.ps1 - builds the core and the compiler from C with zig's C compiler (one download, no installer).
#   bin/onsay-core.exe          the core for Windows
#   bin/onsay-core.lib          what the core shares with native lingos, to build their libraries against
#   bin/test/onsay-core.exe     the same, talking to test/server.mjs. Same file name, since a native library binds to it
#   bin/lingos/                 every lingo in ../lingos, windows with windows.dll built, for the Windows app. Copied
#                               to test/lingos
#   bin/linux-x64/onsay-core    the core for Linux, and bin/linux-arm64/onsay-core. glibc 2.28 or later, and libcurl,
#                               which it loads as it starts, so it builds here with nothing of Linux's installed
#   bin/lingo.wasm              the Lingo compiler on its own, WebAssembly, for the tests and web authoring (web/compiler.js)
$zig = (Get-Command zig -ErrorAction SilentlyContinue).Source
if (-not $zig) { $zig = Get-ChildItem "$env:LOCALAPPDATA\zig" -Recurse -Filter zig.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName }
if (-not $zig) { Write-Error "zig wasn't found. Get it from ziglang.org/download"; exit 1 }

$here = $PSScriptRoot
$windows = "$here/../lingos/windows"
New-Item -ItemType Directory -Force "$here/bin/test", "$here/bin/lingos/windows" | Out-Null
function src($names) { $names | ForEach-Object { "$here/src/$_.c" } }
$flags = "-std=c11", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter"
$core = src "value", "run", "phrase", "std", "compile", "core", "host", "platform_win"
$win = "-target", "x86_64-windows-gnu"

function build($what) { & $zig cc @flags @what | Out-Host; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } }
build ($win + $core + "-DLINGO_CORE", "-lwinhttp", "-luser32", "-o", "$here/bin/onsay-core.exe", "-Wl,--out-implib,$here/bin/onsay-core.lib")
build ($win + $core + "-DLINGO_CORE", "-DTEST", "-lwinhttp", "-luser32", "-o", "$here/bin/test/onsay-core.exe")
# Linux. -rdynamic shares the core's words with a native lingo's library, as onsay-core.lib does on Windows.
$linux = src "value", "run", "phrase", "std", "compile", "core", "host", "platform_linux"
foreach ($arch in @{ x64 = "x86_64"; arm64 = "aarch64" }.GetEnumerator()) {
  New-Item -ItemType Directory -Force "$here/bin/linux-$($arch.Key)" | Out-Null
  build ("-target", "$($arch.Value)-linux-gnu.2.28", "-D_POSIX_C_SOURCE=200809L" + $linux + "-DLINGO_CORE", "-rdynamic", "-ldl", "-lpthread", "-o", "$here/bin/linux-$($arch.Key)/onsay-core")
}
build ($win + "-shared", "-DLINGO_LIBRARY", "-I$here/src", "$windows/windows.c", "$here/bin/onsay-core.lib",
  "-lole32", "-loleaut32", "-lshell32", "-lgdi32", "-luser32", "-o", "$here/bin/lingos/windows/windows.dll")
# native: calls any OS or library function by its layout, with a small vendored part of libffi (../lingos/native/libffi,
# MIT). Its chip's assembly compiles on its own with -DLIBFFI_ASM; the rest is plain C. Windows x64 only.
$ffi = "$here/../lingos/native/libffi"
$ffiinc = "-I$ffi/include", "-I$here/src"
$fficommon = "$ffi/prep_cif.c", "$ffi/types.c"   # call only, so libffi's closure and trampoline files are left out
New-Item -ItemType Directory -Force "$here/bin/lingos/native" | Out-Null
build ($win + "-DLIBFFI_ASM", "-DX86_WIN64", "-I$ffi/include", "-I$ffi/x86", "-c", "$ffi/x86/win64.S", "-o", "$here/bin/win64.o")
build ($win + "-shared", "-DLINGO_LIBRARY", "-DFFI_BUILDING", "-DX86_WIN64" + $ffiinc + "-I$ffi/x86" +
  "$here/../lingos/native/native.c" + $fficommon + "$ffi/x86/ffiw64.c", "$here/bin/win64.o", "$here/bin/onsay-core.lib",
  "-o", "$here/bin/lingos/native/native.dll")
# Every lingo in ../lingos goes beside it, sources of native ones (and libffi) left out, and a copy into test/lingos.
# One no longer in ../lingos is taken out, so the app never ships it.
Get-ChildItem "$here/bin/lingos" -Directory | Where-Object { -not (Test-Path "$here/../lingos/$($_.Name)") } | Remove-Item -Recurse -Force
Get-ChildItem "$here/../lingos" -Directory | ForEach-Object {
  New-Item -ItemType Directory -Force "$here/bin/lingos/$($_.Name)", "$here/test/lingos/$($_.Name)" | Out-Null
  Copy-Item "$($_.FullName)/*" "$here/bin/lingos/$($_.Name)/" -Exclude *.c, *.h, libffi
  Copy-Item "$here/bin/lingos/$($_.Name)/*" "$here/test/lingos/$($_.Name)/"
}
# The Lingo compiler on its own, as WebAssembly, for the tests and web authoring (web/compiler.js).
build ("-target", "wasm32-wasi", "-mexec-model=reactor", "-s" + (src "value", "compile", "wasm") + "-o", "$here/bin/lingo.wasm")
Get-ChildItem "$here/bin" -Recurse -Include *.pdb, *.lib -Exclude onsay-core.lib | Remove-Item
Get-ChildItem "$here/test/lingos/windows" -Include *.pdb, *.lib -Recurse | Remove-Item
Write-Output "built bin/onsay-core.exe, bin/test/onsay-core.exe, bin/linux-x64 and bin/linux-arm64, bin/lingo.wasm and bin/lingos"
