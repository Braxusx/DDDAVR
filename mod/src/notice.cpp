// notice.cpp - see notice.h
#include "notice.h"
#include "vkbridge.h"
#include "log.h"
#include <windows.h>
#include <tlhelp32.h>
#include <vector>
#include <cwchar>

namespace ntc {
namespace {
int g_mode = 1;
struct Worker { DWORD tid; HANDLE h; ULONG64 t; };
std::vector<Worker> g_w;
std::vector<DWORD> g_other;            // named threads that are not DXVK compilers (never looked at again)
double g_scanAt, g_sampleAt, g_busySince, g_quietSince, g_shownAt, g_peak;
bool g_show, g_logged;
long g_windows;
typedef HRESULT(WINAPI* GetDesc_t)(HANDLE, PWSTR*);
GetDesc_t g_getDesc;

double Ms() { static double k; if (!k) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); k = 1000.0 / f.QuadPart; } LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart * k; }
ULONG64 CpuTime(HANDLE h) { FILETIME c, e, k, u; if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0;
    return ((ULONG64)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONG64)u.dwHighDateTime << 32 | u.dwLowDateTime); }   // 100 ns units
bool Known(DWORD tid) { for (auto& w : g_w) if (w.tid == tid) return true; for (DWORD o : g_other) if (o == tid) return true; return false; }
void Scan() {
    if (!g_getDesc) { g_getDesc = (GetDesc_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"); if (!g_getDesc) return; }
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); if (s == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{ sizeof te }; DWORD pid = GetCurrentProcessId(); size_t before = g_w.size();
    for (BOOL ok = Thread32First(s, &te); ok; ok = Thread32Next(s, &te)) {
        if (te.th32OwnerProcessID != pid || Known(te.th32ThreadID)) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID); if (!h) continue;
        PWSTR d = nullptr; bool keep = false;
        if (SUCCEEDED(g_getDesc(h, &d)) && d) {
            if (!wcsncmp(d, L"dxvk-shader", 11)) { g_w.push_back({ te.th32ThreadID, h, CpuTime(h) }); keep = true; }
            else if (d[0]) g_other.push_back(te.th32ThreadID);   // unnamed threads are asked again (DXVK names its workers once they run)
            LocalFree(d);
        }
        if (!keep) CloseHandle(h);
    }
    CloseHandle(s);
    if (g_w.size() != before) Log("notice: %u DXVK shader compiler threads found", (unsigned)g_w.size());
}
} // namespace

void Configure(const wchar_t* ini) {
    g_mode = GetPrivateProfileIntW(L"VR", L"ShaderCacheNotice", 1, ini);
    Log("notice: ShaderCacheNotice=%d", g_mode);
}
bool Showing() { return g_mode == 2 || (g_mode == 1 && g_show); }
void Poll() {
    if (g_mode != 1 || !vkb::Active()) return;
    const double now = Ms();
    if (now - g_scanAt > 2000) { g_scanAt = now; Scan(); }
    if (now - g_sampleAt < 250) return;
    const double dt = g_sampleAt ? now - g_sampleAt : 0; g_sampleAt = now;
    ULONG64 sum = 0;
    for (auto& w : g_w) { ULONG64 t = CpuTime(w.h); if (t >= w.t) sum += t - w.t; w.t = t; }
    if (dt <= 0) return;
    const double cores = (sum / 10000.0) / dt;   // CPU ms per wall ms = busy cores
    const bool busy = cores >= 0.5;
    if (busy) { if (!g_busySince) g_busySince = now; g_quietSince = 0; if (cores > g_peak) g_peak = cores; }
    else { g_busySince = 0; if (!g_quietSince) g_quietSince = now; }
    if (!g_show && busy && now - g_busySince >= 500) {
        g_show = true; g_shownAt = now; g_windows = 0;
        Log("notice: DXVK is compiling shaders (%.1f cores busy) - \"Building shader cache\" shown", cores);
    }
    if (g_show) {
        ++g_windows;
        if (!busy && now - g_quietSince >= 1500 && now - g_shownAt >= 1500) {
            g_show = false;
            Log("notice: shader compiling finished - notice hidden after %.1f s (peak %.1f cores)", (now - g_shownAt) / 1000.0, g_peak);
            g_peak = 0;
        }
    }
    if (cores >= 0.1 && !g_logged) { g_logged = true; Log("notice: first DXVK compiler activity, %.2f cores busy", cores); }
}
}
