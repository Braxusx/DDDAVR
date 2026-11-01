// notice.h - build 077: "Building shader cache" notice while DXVK compiles shaders.
// DXVK compiles on its own worker threads (named "dxvk-shader-h/n/l"); their CPU time is sampled a few
// times a second from the OpenXR keep-alive thread. Sustained compiling (>= 0.5 s) shows the notice; it
// stays at least 1.5 s and goes 1.5 s after the workers fall quiet. Never on Windows' own Direct3D 9.
#pragma once

namespace ntc {
void Configure(const wchar_t* ini);   // ShaderCacheNotice: 0 off, 1 on (default), 2 always shown (appearance test)
void Poll();                          // any thread, called often; samples every 250 ms
bool Showing();
}
