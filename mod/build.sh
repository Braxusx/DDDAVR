#!/bin/sh
# Dragon's Dogma: Dark Arisen VR - the d3d9.dll proxy (32-bit) and the 64-bit DLSS helper.
# TC = the bin folder of llvm-mingw (https://github.com/mstorsjo/llvm-mingw), e.g. TC=$HOME/llvm-mingw/bin sh build.sh [buildnum]
set -e
TC=${TC:?set TC to the llvm-mingw bin folder}
cd "$(dirname "$0")"
N=${1:-107}
CC=$TC/i686-w64-mingw32-clang; CXX=$TC/i686-w64-mingw32-clang++
MH=third_party/minhook
mkdir -p build/obj
for f in $MH/src/buffer.c $MH/src/hook.c $MH/src/trampoline.c $MH/src/hde/hde32.c; do
  $CC -O2 -c $f -I$MH/include -o build/obj/$(basename $f).o
done
CXXFLAGS="-DDDDAVR_BUILD=\"$N\" -O2 -std=c++17 -fno-threadsafe-statics -Wall -Wno-unused-function -Isrc -Ithird_party/openxr_include -Ithird_party/vulkan -I$MH/include -DUNICODE -D_UNICODE -D__USE_MINGW_ANSI_STDIO=1"
for f in src/*.cpp; do $CXX $CXXFLAGS -c $f -o build/obj/$(basename $f).o; done
$CXX -shared -s -o build/d3d9.dll build/obj/*.o src/d3d9.def -static -luser32 -ladvapi32 -luuid -ld3d11 -ldxgi -lgdi32 -Wl,--kill-at
ls -la build/d3d9.dll
# the 64-bit DLSS helper: NGX SDK from https://github.com/NVIDIA/DLSS (NGX=<folder with sdk/include and sdk/lib>, plus
# NGX/dummy = an empty folder for the MSVC import stubs); MSVC CRT shims for the static NGX lib are in src/dlss_host/msvc_shim.cpp
if [ -z "$NGX" ]; then echo "NGX not set - DLSS helper skipped"; exit 0; fi
TC64=$TC/x86_64-w64-mingw32-clang++
$TC64 -std=c++17 -O2 -municode -mwindows -Wall -Wno-unused-function -Wno-unused-but-set-global -I $NGX/sdk/include \
  src/dlss_host/dlss_host.cpp src/dlss_host/msvc_shim.cpp $NGX/sdk/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib -L$NGX/dummy \
  -s -o build/ddda_vr_dlss64.exe -ld3d12 -ldxgi -static -static-libgcc -static-libstdc++
ls -la build/ddda_vr_dlss64.exe
