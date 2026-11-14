// DDDAVR build 108 - Windows timer fix (as in Far Cry 2 VR v230 and Sleeping Dogs VR).
// Windows 11 ignores a process's timer-resolution request (timeBeginPeriod) while its window is minimized, hidden or
// covered: sleeps then round up to 15.6 ms. In VR the game's window on the monitor is often in the background, so at
// start-up (ini TimerFix=1, the default) the process opts out of that throttling
// (PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION and EXECUTION_SPEED) and asks for a 1 ms timer.
#include <windows.h>
#include "log.h"

#ifndef PROCESS_POWER_THROTTLING_CURRENT_VERSION
#define PROCESS_POWER_THROTTLING_CURRENT_VERSION 1
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
typedef struct { ULONG Version, ControlMask, StateMask; } PROCESS_POWER_THROTTLING_STATE;
#endif
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif
typedef LONG(NTAPI* NtQueryTimerResolution_t)(PULONG, PULONG, PULONG);
typedef BOOL(WINAPI* ProcInfo_t)(HANDLE, int, LPVOID, DWORD);
typedef UINT(WINAPI* TimePeriod_t)(UINT);
static const int kProcessPowerThrottling = 4;   // PROCESS_INFORMATION_CLASS

static double TimerMs() {
    auto q = (NtQueryTimerResolution_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryTimerResolution");
    ULONG mn = 0, mx = 0, cur = 0;
    return q && q(&mn, &mx, &cur) == 0 ? cur / 10000.0 : -1;
}
void TimerFixStartup(const wchar_t* ini) {
    auto getPI = (ProcInfo_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetProcessInformation");
    auto setPI = (ProcInfo_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessInformation");
    PROCESS_POWER_THROTTLING_STATE st{ PROCESS_POWER_THROTTLING_CURRENT_VERSION, 0, 0 };
    if (getPI && getPI(GetCurrentProcess(), kProcessPowerThrottling, &st, sizeof st))
        Log("timer: resolution %.3f ms at start-up, power throttling control 0x%lx state 0x%lx", TimerMs(), st.ControlMask, st.StateMask);
    else Log("timer: resolution %.3f ms at start-up", TimerMs());
    if (!GetPrivateProfileIntW(L"VR", L"TimerFix", 1, ini)) { Log("timer fix off (TimerFix=0)"); return; }
    PROCESS_POWER_THROTTLING_STATE s{ PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                                      PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, 0 };
    BOOL ok = setPI ? setPI(GetCurrentProcess(), kProcessPowerThrottling, &s, sizeof s) : FALSE;
    if (!ok) {   // older Windows 10 builds don't know the timer flag
        s.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        ok = setPI ? setPI(GetCurrentProcess(), kProcessPowerThrottling, &s, sizeof s) : FALSE;
    }
    HMODULE mm = LoadLibraryW(L"winmm.dll"); auto tbp = mm ? (TimePeriod_t)GetProcAddress(mm, "timeBeginPeriod") : nullptr;
    UINT tr = tbp ? tbp(1) : 1;
    Log("timer fix (TimerFix=1): power throttling off %s (mask 0x%lx), timeBeginPeriod(1) %s, resolution now %.3f ms", ok ? "ok" : "failed",
        s.ControlMask, tr == 0 ? "ok" : "failed", TimerMs());
}
