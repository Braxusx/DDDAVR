// DDDAVR - diagnostics (build 105): system report, loaded modules / overlays, crash and hang reports, breadcrumbs.
// Everything goes into ddda_vr.log; a crash or a hang also writes a small minidump next to it.
#pragma once
#include <windows.h>

namespace diag {
// DllMain: system / GPU / module report, exception handlers, DLL-load notifications, hang watchdog.
void Init(HMODULE self, const wchar_t* dir, const wchar_t* ini);
// Put our unhandled-exception filter back on top (the game or a tool may replace it); chains to the previous one.
void Rearm(const char* where);
// What this thread is doing right now (a string literal). Shown in crash / hang reports. Cheap: one TLS write.
void Step(const char* what);
// Name the calling thread for the reports ("present", "render").
void NameThread(const char* name);
// Present was called (feeds the hang watchdog).
void OnPresent();
// DllMain DLL_PROCESS_DETACH: logs a normal exit, so a missing line means the process was killed or crashed.
void OnExit(bool processTerminating);
// Log every loaded module again (e.g. once the game is in VR), flagging overlays / injectors.
void LogModules(const char* why);
// RAII step: restores the previous step when it goes out of scope.
struct Scope {
    const char* prev;
    explicit Scope(const char* what);
    ~Scope();
};
}
