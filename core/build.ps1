# build.ps1 - builds the lingo client and packs the lingos it ships, from C with zig's C compiler (one download, no installer).
#   bin/onsay-core.exe          the lingo client for Windows
#   bin/onsay-core.lib          what it shares with native lingos, to build their libraries against
#   bin/test/onsay-core.exe     the same, talking to test/server.mjs. Same file name, since a native library binds to it
#   bin/lingos/                 every lingo it ships as a .lingo file: windows and native, their libraries built from
#                               ../lingos, and each of ../lingo-library, packed by the lingo client itself
#   bin/linux-x64/onsay-core    the lingo client for Linux, and bin/linux-arm64/onsay-core. glibc 2.28 or later, and
#                               libcurl, which it loads as it starts, so it builds here with nothing of Linux's installed
# What is only made on the way, like win64.o and the native lingos' folders before they are packed, goes in obj/.
$zig = (Get-Command zig -ErrorAction SilentlyContinue).Source
if (-not $zig) { $zig = Get-ChildItem "$env:LOCALAPPDATA\zig" -Recurse -Filter zig.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName }
if (-not $zig) { Write-Error "zig wasn't found. Get it from ziglang.org/download"; exit 1 }

$here = $PSScriptRoot
Remove-Item -Recurse -Force "$here/bin/lingos", "$here/obj/lingos" -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$here/bin/test", "$here/bin/lingos", "$here/obj/lingos/windows", "$here/obj/lingos/native" | Out-Null
function src($names) { $names | ForEach-Object { "$here/src/$_.c" } }
function build($what) { & $zig cc @flags @what | Out-Host; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } }
$flags = "-std=c11", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter"
$win = "-target", "x86_64-windows-gnu"

$core = src "value", "run", "phrase", "std", "compile", "zip", "puff", "core", "host", "platform_win"
build ($win + $core + "-DLINGO_CORE", "-lwinhttp", "-lws2_32", "-luser32", "-o", "$here/bin/onsay-core.exe", "-Wl,--out-implib,$here/bin/onsay-core.lib")
build ($win + $core + "-DLINGO_CORE", "-DTEST", "-lwinhttp", "-lws2_32", "-luser32", "-o", "$here/bin/test/onsay-core.exe")
# Linux. -rdynamic shares the core's words with a native lingo's library, as onsay-core.lib does on Windows.
$linux = src "value", "run", "phrase", "std", "compile", "zip", "puff", "core", "host", "platform_linux"
foreach ($arch in @{ x64 = "x86_64"; arm64 = "aarch64" }.GetEnumerator()) {
  New-Item -ItemType Directory -Force "$here/bin/linux-$($arch.Key)" | Out-Null
  build ("-target", "$($arch.Value)-linux-gnu.2.28", "-D_POSIX_C_SOURCE=200809L" + $linux + "-DLINGO_CORE", "-rdynamic", "-ldl", "-lpthread", "-o", "$here/bin/linux-$($arch.Key)/onsay-core")
}

# windows, and native: calls any Windows function by its layout, with a small vendored part of libffi
# (../lingos/native/libffi, MIT). Its chip's assembly compiles on its own with -DLIBFFI_ASM; the rest is plain C. Each is
# built into a folder of its lingo's files in obj/lingos, beside its openapi.json.
build ($win + "-shared", "-DLINGO_LIBRARY", "-I$here/src", "$here/../lingos/windows/windows.c", "$here/bin/onsay-core.lib",
  "-lole32", "-loleaut32", "-lshell32", "-lgdi32", "-luser32", "-o", "$here/obj/lingos/windows/windows.dll")
$ffi = "$here/../lingos/native/libffi"
build ($win + "-DLIBFFI_ASM", "-DX86_WIN64", "-I$ffi/include", "-I$ffi/x86", "-c", "$ffi/x86/win64.S", "-o", "$here/obj/win64.o")
build ($win + "-shared", "-DLINGO_LIBRARY", "-DFFI_BUILDING", "-DX86_WIN64", "-I$ffi/include", "-I$ffi/x86", "-I$here/src",
  "$here/../lingos/native/native.c", "$ffi/prep_cif.c", "$ffi/types.c", "$ffi/x86/ffiw64.c", "$here/obj/win64.o", "$here/bin/onsay-core.lib",
  "-o", "$here/obj/lingos/native/native.dll")
Get-ChildItem "$here/../lingos" -Directory | ForEach-Object { Copy-Item "$($_.FullName)/openapi.json" "$here/obj/lingos/$($_.Name)/" }
Get-ChildItem "$here/bin", "$here/obj" -Recurse -Include *.pdb, *.lib -Exclude onsay-core.lib | Remove-Item

# Every lingo packed into bin/lingos by the lingo client, with ../lingo-library as its lingos folder, so what each reaches
# in the others counts.
$slash = $here -replace '\\', '/'
Set-Content "$here/obj/pack.json" "{`"home`": `"$slash/obj/home`", `"lingos`": `"$slash/../lingo-library`"}"
foreach ($folder in @(Get-ChildItem "$here/obj/lingos" -Directory) + @(Get-ChildItem "$here/../lingo-library" -Directory)) {
  & "$here/bin/onsay-core.exe" "$here/obj/pack.json" pack $folder.FullName "$here/bin/lingos" 2>$null
  if ($LASTEXITCODE -ne 0) { Write-Error "$($folder.Name) couldn't be packed. To see why: bin/onsay-core.exe obj/pack.json pack $($folder.FullName)"; exit 1 }
}
Write-Output "built bin/onsay-core.exe, bin/test/onsay-core.exe, bin/linux-x64, bin/linux-arm64 and bin/lingos"
