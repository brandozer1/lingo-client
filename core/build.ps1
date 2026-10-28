# build.ps1 - builds the lingo client and packs the lingos it ships, from C with zig's C compiler (one download, no installer).
#   bin/onsay-core.exe          the lingo client for Windows
#   bin/onsay-core.lib          what it shares with native lingos, to build their libraries against
#   bin/test/onsay-core.exe     the same, talking to test/server.mjs. Same file name, since a native library binds to it
#   bin/lingos/                 every lingo it ships as a .lingo file: onsay, windows and native, their libraries built
#                               from ../lingos, and each of ../lingo-library, packed by the lingo client itself
#   bin/test/lingos/onsay.lingo the onsay lingo for the tests, talking to test/server.mjs and trusting its key
#   bin/linux-x64/onsay-core    the lingo client for Linux, and bin/linux-arm64/onsay-core. glibc 2.28 or later, and
#                               libcurl, which it loads as it starts, so it builds here with nothing of Linux's installed.
#                               Beside each, lingos/onsay.lingo, the onsay lingo for Linux
# What is only made on the way, like win64.o and the native lingos' folders before they are packed, goes in obj/.
$zig = (Get-Command zig -ErrorAction SilentlyContinue).Source
if (-not $zig) { $zig = Get-ChildItem "$env:LOCALAPPDATA\zig" -Recurse -Filter zig.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName }
if (-not $zig) { Write-Error "zig wasn't found. Get it from ziglang.org/download"; exit 1 }

$here = $PSScriptRoot
Remove-Item -Recurse -Force "$here/bin/lingos", "$here/bin/test/lingos", "$here/obj" -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$here/bin/test/lingos", "$here/bin/lingos", "$here/obj/lingos/windows", "$here/obj/lingos/native", "$here/obj/lingos/onsay", "$here/obj/test/onsay" | Out-Null
function src($names) { $names | ForEach-Object { "$here/src/$_.c" } }
function build($what) { & $zig cc @flags @what | Out-Host; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } }
$flags = "-std=c11", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter"
$win = "-target", "x86_64-windows-gnu"

$both = "value", "run", "phrase", "std", "compile", "zip", "puff", "sign", "monocypher", "monocypher-ed25519", "core", "host"
$core = src ($both + "platform_win")
$libs = "-lwinhttp", "-lws2_32", "-luser32", "-lbcrypt", "-lcrypt32"
build ($win + $core + "-DLINGO_CORE" + $libs + "-o", "$here/bin/onsay-core.exe", "-Wl,--out-implib,$here/bin/onsay-core.lib")
build ($win + $core + "-DLINGO_CORE", "-DTEST" + $libs + "-o", "$here/bin/test/onsay-core.exe")
# Linux. -rdynamic shares the core's words with a native lingo's library, as onsay-core.lib does on Windows.
$linux = src ($both + "platform_linux")
foreach ($arch in @{ x64 = "x86_64"; arm64 = "aarch64" }.GetEnumerator()) {
  New-Item -ItemType Directory -Force "$here/bin/linux-$($arch.Key)/lingos", "$here/obj/linux-$($arch.Key)" | Out-Null
  $target = "-target", "$($arch.Value)-linux-gnu.2.28", "-D_POSIX_C_SOURCE=200809L"
  build ($target + $linux + "-DLINGO_CORE", "-rdynamic", "-ldl", "-lpthread", "-o", "$here/bin/linux-$($arch.Key)/onsay-core")
  build ($target + "-shared", "-fPIC", "-DLINGO_LIBRARY", "-I$here/src", "$here/../lingos/onsay/onsay.c", "-o", "$here/obj/linux-$($arch.Key)/onsay.so")
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
# onsay, for any computer, and again for the tests, which talk to test/server.mjs.
build ($win + "-shared", "-DLINGO_LIBRARY", "-I$here/src", "$here/../lingos/onsay/onsay.c", "$here/bin/onsay-core.lib", "-o", "$here/obj/lingos/onsay/onsay.dll")
build ($win + "-shared", "-DLINGO_LIBRARY", "-DTEST", "-I$here/src", "$here/../lingos/onsay/onsay.c", "$here/bin/onsay-core.lib", "-o", "$here/obj/test/onsay/onsay.dll")
Copy-Item "$here/../lingos/onsay/openapi.json" "$here/obj/test/onsay/"
Get-ChildItem "$here/../lingos" -Directory | ForEach-Object { Copy-Item "$($_.FullName)/openapi.json" "$here/obj/lingos/$($_.Name)/" }
Get-ChildItem "$here/bin", "$here/obj" -Recurse -Include *.pdb, *.lib -Exclude onsay-core.lib | Remove-Item

# Every lingo packed into bin/lingos by the lingo client, with ../lingo-library as its lingos folder, so what each reaches
# in the others counts.
$slash = $here -replace '\\', '/'
Set-Content "$here/obj/pack.json" "{`"home`": `"$slash/obj/home`", `"lingos`": `"$slash/../lingo-library`"}"
function pack($folder, $to) {
  & "$here/bin/onsay-core.exe" "$here/obj/pack.json" pack $folder $to 2>$null
  if ($LASTEXITCODE -ne 0) { Write-Error "$folder couldn't be packed. To see why: bin/onsay-core.exe obj/pack.json pack $folder"; exit 1 }
}
foreach ($folder in @(Get-ChildItem "$here/obj/lingos" -Directory) + @(Get-ChildItem "$here/../lingo-library" -Directory)) { pack $folder.FullName "$here/bin/lingos" }
pack "$here/obj/test/onsay" "$here/bin/test/lingos"
# onsay for Linux, zipped here, since Windows can't load its library to pack it: its paths are made as it loads.
foreach ($arch in "x64", "arm64") {
  Copy-Item "$here/../lingos/onsay/openapi.json" "$here/obj/linux-$arch/"
  Compress-Archive -Force "$here/obj/linux-$arch/openapi.json", "$here/obj/linux-$arch/onsay.so" "$here/obj/linux-$arch/onsay.zip"
  Move-Item -Force "$here/obj/linux-$arch/onsay.zip" "$here/bin/linux-$arch/lingos/onsay.lingo"
}
Write-Output "built bin/onsay-core.exe, bin/test/onsay-core.exe, bin/linux-x64, bin/linux-arm64 and bin/lingos"
