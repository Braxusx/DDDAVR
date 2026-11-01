// DDDAVR - diagnostics (build 105). See diag.h.
// Design notes:
//  - Nothing here may call the loader from DllMain (no Toolhelp, no LoadLibrary there): the module snapshot and dbghelp
//    load happen on the watchdog thread, which starts after DllMain returns.
//  - A hang report suspends the stuck threads only long enough to copy their registers and stack (no locks, no Log while
//    suspended), then analyses the copy.
#include "diag.h"
#include "log.h"
#include <winternl.h>
#include <tlhelp32.h>
#include <dxgi.h>
#include <cpuid.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

namespace diag {
namespace {
// ---------------- settings / state
int g_on = 1, g_dumps = 1, g_hangMs = 4000, g_test = 0;
wchar_t g_dir[MAX_PATH];
volatile LONG g_exiting;
volatile double g_lastPresent;   // ms (GetTickCount64)
LPTOP_LEVEL_EXCEPTION_FILTER g_prevUef;
volatile LONG g_uefOurs;
double NowMs() { return (double)GetTickCount64(); }

// ---------------- module table (append-only, readable without a lock)
struct Mod { uintptr_t base; uint32_t size; char name[64]; const char* tag; volatile LONG ready; };
Mod g_mods[768]; volatile LONG g_modReserve; uintptr_t g_self;
// Known overlays / injectors / wrappers. Substring match on the lower-case file name.
const struct { const char* key; const char* what; } kTags[] = {
    { "rtsshooks", "RivaTuner Statistics Server overlay" }, { "gameoverlayrenderer", "Steam overlay" },
    { "discordhook", "Discord overlay" }, { "nvspcap", "NVIDIA overlay / ShadowPlay" }, { "nvppe", "NVIDIA overlay" },
    { "obs-graphics-hook", "OBS game capture" }, { "graphics-hook", "OBS / Streamlabs game capture" },
    { "reshade", "ReShade" }, { "specialk", "Special K" }, { "eosoverlay", "Epic overlay" }, { "overwolf", "Overwolf" },
    { "fraps", "Fraps" }, { "action_x86", "Mirillis Action" }, { "bdcam", "Bandicam" }, { "medal", "Medal" },
    { "xsplit", "XSplit" }, { "gedosato", "GeDoSaTo" }, { "uplay_overlay", "Ubisoft overlay" }, { "igo32", "EA / Origin overlay" },
    { "d3d9_dxvk", "DXVK (this mod's option)" }, { "rivatuner", "RivaTuner" }, { "mirillis", "Mirillis" },
    { "sbiedll", "Sandboxie" }, { "nahimic", "Nahimic audio overlay" }, { "ss3dev", "Nahimic / SteelSeries Sonar" },
    { "atkexcomlib", "ASUS Armoury Crate hook" }, { "razer", "Razer overlay" }, { "wallpaperengine", "Wallpaper Engine" },
};
const char* TagFor(const char* lowerName) {
    for (auto& t : kTags) if (t.what && strstr(lowerName, t.key)) return t.what;
    return nullptr;
}
void AddModule(uintptr_t base, uint32_t size, const wchar_t* name, int len, bool logIt, const char* how) {
    if (!base) return;
    LONG n = g_modReserve; if (n > 768) n = 768;
    for (LONG i = 0; i < n; i++) if (g_mods[i].ready && g_mods[i].base == base) return;
    LONG i = InterlockedIncrement(&g_modReserve) - 1; if (i >= 768) return;
    Mod& m = g_mods[i]; m.base = base; m.size = size;
    int k = 0; for (; k < len && k < 63 && name[k]; k++) m.name[k] = (char)(name[k] < 128 ? tolower(name[k]) : '?'); m.name[k] = 0;
    m.tag = TagFor(m.name);
    if (base == g_self) { strcpy(m.name, "ddda-vr-mod(d3d9.dll)"); m.tag = nullptr; }
    MemoryBarrier(); m.ready = 1;
    if (logIt) Log("module: %s %s at %08x size %u%s%s", how, m.name, (unsigned)base, size, m.tag ? " - " : "", m.tag ? m.tag : "");
}
const Mod* ModuleAt(uintptr_t a) {
    LONG n = g_modReserve; if (n > 768) n = 768;
    for (LONG i = 0; i < n; i++) { const Mod& m = g_mods[i]; if (m.ready && a >= m.base && a < m.base + m.size) return &m; }
    return nullptr;
}
void Where(uintptr_t a, char* out, size_t cap) {
    const Mod* m = ModuleAt(a);
    if (!m) snprintf(out, cap, "%08x", (unsigned)a);
    else if (strstr(m->name, ".exe")) snprintf(out, cap, "%s+%x (%08x)", m->name, (unsigned)(a - m->base), (unsigned)a);
    else snprintf(out, cap, "%s+%x", m->name, (unsigned)(a - m->base));
}

// ---------------- DLL load notifications (ntdll, Vista+)
struct LdrData { ULONG Flags; const UNICODE_STRING* FullDllName; const UNICODE_STRING* BaseDllName; PVOID DllBase; ULONG SizeOfImage; };
typedef VOID(CALLBACK* LdrCb)(ULONG, const LdrData*, PVOID);
typedef LONG(NTAPI* LdrReg_t)(ULONG, LdrCb, PVOID, PVOID*);
VOID CALLBACK OnDll(ULONG reason, const LdrData* d, PVOID) {
    if (!d || !d->BaseDllName || g_exiting) return;
    if (reason == 1) AddModule((uintptr_t)d->DllBase, d->SizeOfImage, d->BaseDllName->Buffer, d->BaseDllName->Length / 2, true, "loaded");
    else if (reason == 2) {
        char n[64]; int k = 0, len = d->BaseDllName->Length / 2; for (; k < len && k < 63; k++) n[k] = (char)tolower(d->BaseDllName->Buffer[k] & 0x7f); n[k] = 0;
        Log("module: unloaded %s", n);
    }
}

// ---------------- breadcrumbs
struct Slot { volatile DWORD tid; const char* volatile name; const char* volatile step; };
Slot g_slots[64]; volatile LONG g_slotN; DWORD g_tls = TLS_OUT_OF_INDEXES;
Slot* MySlot() {
    if (g_tls == TLS_OUT_OF_INDEXES) return nullptr;
    DWORD e = GetLastError();
    Slot* s = (Slot*)TlsGetValue(g_tls);
    if (!s) {
        LONG i = InterlockedIncrement(&g_slotN) - 1;
        if (i < 64) { s = &g_slots[i]; s->tid = GetCurrentThreadId(); TlsSetValue(g_tls, s); }
    }
    SetLastError(e);
    return s;
}
const Slot* SlotOf(DWORD tid) { LONG n = g_slotN; if (n > 64) n = 64; for (LONG i = 0; i < n; i++) if (g_slots[i].tid == tid) return &g_slots[i]; return nullptr; }

// ---------------- stacks
bool ReadMem(uintptr_t a, void* out, size_t n) { SIZE_T got = 0; return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)a, out, n, &got) && got == n; }
bool LooksLikeReturn(uintptr_t v) {   // the bytes before v form a call instruction
    if (!ModuleAt(v)) return false;
    uint8_t b[8]; if (!ReadMem(v - 8, b, 8)) return false;   // b[7] = byte at v-1
    if (b[3] == 0xE8) return true;                                           // call rel32
    if (b[6] == 0xFF && (b[7] & 0x38) == 0x10) return true;                   // call r32 / [r32]
    if (b[5] == 0xFF && (b[6] & 0x38) == 0x10) return true;                   // call [r32+d8]
    if (b[2] == 0xFF && (b[3] & 0x38) == 0x10) return true;                   // call [r32+d32] / [abs32]
    if (b[1] == 0xFF && (b[2] & 0x38) == 0x10) return true;                   // call [sib+d32]
    return false;
}
// Logs the caller chain found in a copied stack: values that point just after a call instruction inside a module.
void LogStack(const char* indent, const uint32_t* st, size_t words, int maxFrames) {
    int n = 0; char w[96];
    for (size_t i = 0; i < words && n < maxFrames; i++) {
        uintptr_t v = st[i];
        if (v < 0x10000 || !LooksLikeReturn(v)) continue;
        Where(v, w, sizeof w); Log("%s  [esp+%03x] %s", indent, (unsigned)(i * 4), w); n++;
    }
    if (!n) Log("%s  (no return addresses found on the stack)", indent);
}
void LogContext(const char* indent, const CONTEXT& c) {
    char eip[96]; Where(c.Eip, eip, sizeof eip);
    Log("%s eip %s | eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx", indent, eip,
        c.Eax, c.Ebx, c.Ecx, c.Edx, c.Esi, c.Edi, c.Ebp, c.Esp);
}
void LogThreadsSteps() {
    LONG n = g_slotN; if (n > 64) n = 64;
    for (LONG i = 0; i < n; i++) {
        const Slot& s = g_slots[i];
        if (s.step || s.name) Log("  thread %lu%s%s%s: %s", s.tid, s.name ? " [" : "", s.name ? s.name : "", s.name ? "]" : "", s.step ? s.step : "(no step yet)");
    }
}
// Suspends a thread, copies its registers and up to 16 KB of stack, resumes it, then logs.
void SampleThread(DWORD tid, const char* label) {
    if (tid == GetCurrentThreadId()) return;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) { Log("  %s thread %lu: cannot open (%lu)", label, tid, GetLastError()); return; }
    static uint32_t st[4096]; size_t words = 0;
    CONTEXT c = {}; c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    bool ok = false;
    if (SuspendThread(h) != (DWORD)-1) {
        ok = GetThreadContext(h, &c) != 0;
        if (ok) for (size_t chunk = 4096; chunk >= 256; chunk /= 2)   // as much stack as is readable
            if (ReadMem(c.Esp, st, chunk * 4)) { words = chunk; break; }
        ResumeThread(h);
    }
    CloseHandle(h);
    const Slot* s = SlotOf(tid);
    Log("  %s thread %lu, step: %s", label, tid, s && s->step ? s->step : "(none)");
    if (!ok) { Log("    (registers not readable)"); return; }
    LogContext("   ", c);
    LogStack("   ", st, words, 18);
}
DWORD g_presentTid, g_renderTid;

// ---------------- minidumps
typedef BOOL(WINAPI* MDWD_t)(HANDLE, DWORD, HANDLE, int, PVOID, PVOID, PVOID);
MDWD_t g_mdwd;
struct MdExc { DWORD ThreadId; EXCEPTION_POINTERS* ExceptionPointers; BOOL ClientPointers; };
void WriteDump(const wchar_t* name, EXCEPTION_POINTERS* ep) {
    if (!g_dumps || !g_mdwd) { if (g_dumps) Log("diag: minidump not written (dbghelp.dll not loaded)"); return; }
    wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%ls%ls", g_dir, name);
    HANDLE f = CreateFileW(p, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { Log("diag: cannot create %ls (%lu)", p, GetLastError()); return; }
    MdExc me = { GetCurrentThreadId(), ep, FALSE };
    const int type = 0x0020 /*UnloadedModules*/ | 0x1000 /*ThreadInfo*/ | 0x0004 /*HandleData*/;   // + all thread stacks (MiniDumpNormal)
    BOOL ok = g_mdwd(GetCurrentProcess(), GetCurrentProcessId(), f, type, ep ? &me : nullptr, nullptr, nullptr);
    DWORD sz = GetFileSize(f, nullptr); CloseHandle(f);
    Log("diag: minidump %ls %s (%lu KB)", p, ok ? "written" : "FAILED", sz / 1024);
}

// ---------------- exceptions
const char* ExcName(DWORD c) {
    switch (c) {
    case 0xC0000005: return "access violation"; case 0xC000001D: return "illegal instruction"; case 0xC0000096: return "privileged instruction";
    case 0xC0000094: return "integer divide by zero"; case 0xC00000FD: return "stack overflow"; case 0xC0000409: return "stack buffer overrun / fail-fast";
    case 0xC0000374: return "heap corruption"; case 0xC0000006: return "in-page error"; case 0xC000008E: case 0xC0000090: case 0xC0000091: case 0xC0000093: return "float exception";
    default: return nullptr;
    }
}
void LogException(const char* kind, EXCEPTION_POINTERS* ep, int frames) {
    const EXCEPTION_RECORD& r = *ep->ExceptionRecord; char at[96]; Where((uintptr_t)r.ExceptionAddress, at, sizeof at);
    const char* nm = ExcName(r.ExceptionCode);
    char extra[64] = "";
    if (r.ExceptionCode == 0xC0000005 && r.NumberParameters >= 2)
        snprintf(extra, sizeof extra, " %s %08lx", r.ExceptionInformation[0] == 0 ? "reading" : r.ExceptionInformation[0] == 8 ? "executing" : "writing", (unsigned long)r.ExceptionInformation[1]);
    const Slot* s = SlotOf(GetCurrentThreadId());
    Log("%s: %08lx (%s)%s at %s on thread %lu%s%s%s, step: %s", kind, r.ExceptionCode, nm ? nm : "?", extra, at, GetCurrentThreadId(),
        s && s->name ? " [" : "", s && s->name ? s->name : "", s && s->name ? "]" : "", s && s->step ? s->step : "(none)");
    LogContext("   ", *ep->ContextRecord);
    if (r.ExceptionCode == 0xC00000FD || frames <= 0) return;   // almost no stack left
    static uint32_t st[2048]; size_t words = 0;
    for (size_t chunk = 2048; chunk >= 128; chunk /= 2) if (ReadMem(ep->ContextRecord->Esp, st, chunk * 4)) { words = chunk; break; }
    LogStack("   ", st, words, frames);
}
DWORD g_vehTls = TLS_OUT_OF_INDEXES; uintptr_t g_seen[24]; volatile LONG g_seenN;
LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ExcName(ep->ExceptionRecord->ExceptionCode) || g_exiting) return EXCEPTION_CONTINUE_SEARCH;
    if (g_vehTls != TLS_OUT_OF_INDEXES && TlsGetValue(g_vehTls)) return EXCEPTION_CONTINUE_SEARCH;   // re-entry
    uintptr_t a = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    LONG n = g_seenN; if (n > 24) n = 24;
    for (LONG i = 0; i < n; i++) if (g_seen[i] == a) return EXCEPTION_CONTINUE_SEARCH;   // each place once
    LONG i = InterlockedIncrement(&g_seenN) - 1; if (i >= 24) return EXCEPTION_CONTINUE_SEARCH;
    g_seen[i] = a;
    if (g_vehTls != TLS_OUT_OF_INDEXES) TlsSetValue(g_vehTls, (void*)1);
    // First chance: the game (or a driver) may still handle it, so this is a note, not necessarily the crash.
    LogException("exception (first chance, may be handled)", ep, 10);
    LogFlush();
    if (g_vehTls != TLS_OUT_OF_INDEXES) TlsSetValue(g_vehTls, nullptr);
    return EXCEPTION_CONTINUE_SEARCH;
}
volatile LONG g_crashed;
LONG WINAPI Uef(EXCEPTION_POINTERS* ep) {
    if (InterlockedIncrement(&g_crashed) == 1 && ep && ep->ExceptionRecord) {
        Log("CRASH ======================================================================");
        LogException("CRASH: unhandled exception", ep, 24);
        Log("CRASH: what each thread was doing:"); LogThreadsSteps();
        WriteDump(L"ddda_vr_crash.dmp", ep);
        Log("CRASH: end of report. Please send ddda_vr.log and ddda_vr_crash.dmp.");
        LogFlush();
    }
    return g_prevUef ? g_prevUef(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// ---------------- system report
void Reg(HKEY root, const wchar_t* key, const wchar_t* val, wchar_t* out, DWORD cap, REGSAM extra = 0) {
    out[0] = 0; HKEY k; DWORD type, sz = cap * 2;
    if (RegOpenKeyExW(root, key, 0, KEY_READ | extra, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, val, nullptr, &type, (BYTE*)out, &sz) == ERROR_SUCCESS) {
            if (type == REG_DWORD) { DWORD v = *(DWORD*)out; swprintf(out, cap, L"%lu", v); }
            else out[cap - 1] = 0;
        } else out[0] = 0;
        RegCloseKey(k);
    }
}
void SystemReport(const wchar_t* ini) {
    // Windows / Wine
    typedef LONG(WINAPI* RtlGetVersion_t)(OSVERSIONINFOW*);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    OSVERSIONINFOW v = { sizeof v };
    if (auto f = (RtlGetVersion_t)GetProcAddress(nt, "RtlGetVersion")) f(&v);
    wchar_t disp[64], ubr[32], prod[128];
    Reg(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion", disp, 64, KEY_WOW64_64KEY);
    Reg(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", ubr, 32, KEY_WOW64_64KEY);
    Reg(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName", prod, 128, KEY_WOW64_64KEY);
    typedef const char*(CDECL* wine_t)(); auto wine = (wine_t)GetProcAddress(nt, "wine_get_version");
    Log("system: %ls %ls (%lu.%lu.%lu.%ls)%s%s", prod, disp, v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber, ubr[0] ? ubr : L"0",
        wine ? " - running under Wine/Proton " : "", wine ? wine() : "");
    // CPU / memory
    unsigned r[12] = {}; char cpu[49] = "";
    if (__get_cpuid(0x80000000, &r[0], &r[1], &r[2], &r[3]) && r[0] >= 0x80000004) {
        for (unsigned i = 0; i < 3; i++) __get_cpuid(0x80000002 + i, &r[i * 4], &r[i * 4 + 1], &r[i * 4 + 2], &r[i * 4 + 3]);
        memcpy(cpu, r, 48); cpu[48] = 0;
    }
    const char* c = cpu; while (*c == ' ') c++;
    SYSTEM_INFO si; GetNativeSystemInfo(&si);
    MEMORYSTATUSEX ms = { sizeof ms }; GlobalMemoryStatusEx(&ms);
    Log("system: CPU %s, %lu logical processors | RAM %.1f GB (%.1f GB free) | address space %.0f MB (%.0f MB free)", c, si.dwNumberOfProcessors,
        ms.ullTotalPhys / 1073741824.0, ms.ullAvailPhys / 1073741824.0, ms.ullTotalVirtual / 1048576.0, ms.ullAvailVirtual / 1048576.0);
    // GPUs + driver versions
    typedef HRESULT(WINAPI* CF_t)(REFIID, void**);
    HMODULE dx = GetModuleHandleW(L"dxgi.dll");   // already loaded by our import table
    if (auto cf = dx ? (CF_t)GetProcAddress(dx, "CreateDXGIFactory1") : nullptr) {
        IDXGIFactory1* f = nullptr;
        if (SUCCEEDED(cf(__uuidof(IDXGIFactory1), (void**)&f))) {
            IDXGIAdapter1* a = nullptr;
            for (UINT i = 0; f->EnumAdapters1(i, &a) == S_OK; i++) {
                DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
                LARGE_INTEGER umd = {}; char drv[64] = "?";
                if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
                    unsigned p1 = HIWORD(umd.HighPart), p2 = LOWORD(umd.HighPart), p3 = HIWORD(umd.LowPart), p4 = LOWORD(umd.LowPart);
                    if (d.VendorId == 0x10de) { unsigned nv = (p3 % 10) * 10000 + p4; snprintf(drv, sizeof drv, "%u.%u.%u.%u (NVIDIA %u.%02u)", p1, p2, p3, p4, nv / 100, nv % 100); }
                    else snprintf(drv, sizeof drv, "%u.%u.%u.%u", p1, p2, p3, p4);
                }
                Log("system: GPU %u '%ls' vendor %04x device %04x, %.0f MB VRAM, LUID %08lx:%08lx, driver %s%s", i, d.Description, d.VendorId, d.DeviceId,
                    d.DedicatedVideoMemory / 1048576.0, (unsigned long)d.AdapterLuid.HighPart, (unsigned long)d.AdapterLuid.LowPart, drv,
                    (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " (software)" : "");
                a->Release();
            }
            f->Release();
        }
    }
    // Displays
    DISPLAY_DEVICEW dd = { sizeof dd };
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); i++, dd.cb = sizeof dd) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        DEVMODEW dm = {}; dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm))
            Log("system: display %ls%s %lux%lu %lu Hz on '%ls'", dd.DeviceName, (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? " (primary)" : "",
                dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency, dd.DeviceString);
    }
    // OpenXR runtime registration (64-bit and 32-bit views)
    wchar_t rt[MAX_PATH];
    Reg(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", rt, MAX_PATH, KEY_WOW64_64KEY);
    Log("system: OpenXR active runtime (64-bit) %ls", rt[0] ? rt : L"(none registered)");
    Reg(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", rt, MAX_PATH, KEY_WOW64_32KEY);
    if (rt[0]) Log("system: OpenXR active runtime (32-bit) %ls", rt);
    // Process
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    Log("system: process %ls, pid %lu, command line: %ls", exe, GetCurrentProcessId(), GetCommandLineW());
    // Other DLL wrappers / injector configs in the game folder
    static const wchar_t* files[] = { L"dxgi.dll", L"d3d11.dll", L"d3d10.dll", L"d3d8.dll", L"dinput8.dll", L"dinput.dll", L"winmm.dll", L"version.dll",
        L"xinput1_3.dll", L"dsound.dll", L"opengl32.dll", L"ReShade.ini", L"ReShade32.dll", L"dxvk.conf", L"dgVoodoo.conf", L"SpecialK.ini" };
    char found[512] = ""; size_t o = 0;
    for (auto fn : files) {
        wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%ls%ls", g_dir, fn);
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) o += snprintf(found + o, sizeof found - o, " %ls", fn);
    }
    Log("system: other wrapper DLLs / configs in the game folder:%s", o ? found : " none");
    (void)ini;
}
void LogAllModules() {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (s == INVALID_HANDLE_VALUE) { Log("module: snapshot failed (%lu)", GetLastError()); return; }
    MODULEENTRY32W me = { sizeof me }; int n = 0, flagged = 0;
    for (BOOL ok = Module32FirstW(s, &me); ok; ok = Module32NextW(s, &me)) { AddModule((uintptr_t)me.modBaseAddr, me.modBaseSize, me.szModule, MAX_MODULE_NAME32, false, ""); n++; }
    CloseHandle(s);
    LONG cnt = g_modReserve; if (cnt > 768) cnt = 768;
    char line[1024]; size_t o = 0;
    for (LONG i = 0; i < cnt; i++) {
        const Mod& m = g_mods[i]; if (!m.ready) continue;
        if (m.tag) { Log("module: %s at %08x size %u - %s", m.name, (unsigned)m.base, m.size, m.tag); flagged++; }
        int w = snprintf(line + o, sizeof line - o, "%s%s", o ? ", " : "", m.name);
        if (w > 0 && o + w < sizeof line - 2) o += w; else { Log("modules: %s", line); o = 0; o += snprintf(line, sizeof line, "%s", m.name); }
    }
    if (o) Log("modules: %s", line);
    Log("module: %d loaded at start, %d known overlay / injector / wrapper DLLs", n, flagged);
}

// ---------------- watchdog
DWORD WINAPI Watchdog(LPVOID) {
    diag::NameThread("diag watchdog");
    { wchar_t p[MAX_PATH]; GetSystemDirectoryW(p, MAX_PATH); wcscat(p, L"\\dbghelp.dll");   // the system copy, not a game-folder one
      HMODULE h = LoadLibraryW(p); g_mdwd = h ? (MDWD_t)GetProcAddress(h, "MiniDumpWriteDump") : nullptr; }
    LogAllModules();
    LogFlush();
    int level = 0; double hangStart = 0;
    for (;;) {
        Sleep(250);
        if (g_exiting) return 0;
        double last = g_lastPresent; if (last <= 0) continue;
        double dt = NowMs() - last;
        if (level && dt < 1000) { Log("hang: the game is presenting again after %.1f s", (NowMs() - hangStart) / 1000.0); level = 0; continue; }
        if ((level == 0 && dt > g_hangMs) || (level == 1 && dt > 20000)) {
            if (!level) hangStart = last;
            Log("HANG =======================================================================");
            Log("HANG: the game has not presented a frame for %.1f s%s", dt / 1000.0, level ? " (second report)" : "");
            Log("HANG: what each thread was doing:"); LogThreadsSteps();
            if (g_presentTid) SampleThread(g_presentTid, "present (main)");
            if (g_renderTid && g_renderTid != g_presentTid) SampleThread(g_renderTid, "render");
            LONG n = g_slotN; if (n > 64) n = 64;
            for (LONG i = 0; i < n; i++) { const Slot& s = g_slots[i];
                if (s.tid != g_presentTid && s.tid != g_renderTid && s.tid != GetCurrentThreadId() && s.step) SampleThread(s.tid, s.name ? s.name : "other"); }
            if (!level) WriteDump(L"ddda_vr_hang.dmp", nullptr);
            Log("HANG: end of report. If the game stays frozen, please send ddda_vr.log and ddda_vr_hang.dmp.");
            LogFlush();
            level++;
        }
    }
}
}   // namespace

void Init(HMODULE self, const wchar_t* dir, const wchar_t* ini) {
    g_self = (uintptr_t)self;
    g_on = GetPrivateProfileIntW(L"VR", L"Diagnostics", 1, ini);
    g_dumps = GetPrivateProfileIntW(L"VR", L"DiagDumps", 1, ini);
    g_hangMs = GetPrivateProfileIntW(L"VR", L"DiagHangSeconds", 4, ini) * 1000; if (g_hangMs < 2000) g_hangMs = 2000;
    wcsncpy(g_dir, dir, MAX_PATH - 1);
    g_tls = TlsAlloc(); g_vehTls = TlsAlloc();
    g_test = GetPrivateProfileIntW(L"VR", L"DiagTest", 0, ini);   // 1 = fake a 10 s freeze, 2 = fake a crash, 45 s after the first frame
    if (!g_on) { Log("diag: Diagnostics=0 - system report, crash / hang reports off"); return; }
    SystemReport(ini);
    if (auto reg = (LdrReg_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification")) { PVOID cookie; reg(0, OnDll, nullptr, &cookie); }
    AddVectoredExceptionHandler(1, Veh);
    Rearm("start");
    CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr);
    Log("diag: crash / hang reports on (hang after %d s without a frame, minidumps %s)", g_hangMs / 1000, g_dumps ? "on" : "off");
}
void Rearm(const char* where) {
    if (!g_on) return;
    LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(Uef);
    if (prev != Uef) {
        if (g_uefOurs && prev) { char w[96]; Where((uintptr_t)prev, w, sizeof w); Log("diag: crash handler re-armed at %s (it had been replaced by %s)", where, w); }
        g_prevUef = prev;
    }
    g_uefOurs = 1;
}
void Step(const char* what) { if (Slot* s = MySlot()) s->step = what; }
void NameThread(const char* name) {
    if (Slot* s = MySlot()) s->name = name;
    if (name && !strcmp(name, "present")) g_presentTid = GetCurrentThreadId();
    if (name && !strcmp(name, "render")) g_renderTid = GetCurrentThreadId();
}
void OnPresent() {
    double now = NowMs(); g_lastPresent = now;
    static double first; if (!first) first = now;
    if (g_test && now - first > 45000) {
        int t = g_test; g_test = 0;
        if (t == 1) { Log("diag: DiagTest=1 - freezing the present thread for 10 s"); Step("diag test: deliberate 10 s freeze"); Sleep(10000); }
        if (t == 2) { Log("diag: DiagTest=2 - deliberate crash"); Step("diag test: deliberate crash"); *(volatile int*)8 = 1; }
    }
}
void OnExit(bool terminating) {
    g_exiting = 1;
    if (!g_crashed) LogUnlocked(terminating ? "exit: the game closed normally (process ending)" : "exit: mod unloaded");
}
void LogModules(const char* why) {
    if (!g_on) return;
    LONG cnt = g_modReserve; if (cnt > 768) cnt = 768; int flagged = 0;
    for (LONG i = 0; i < cnt; i++) { const Mod& m = g_mods[i]; if (m.ready && m.tag) { Log("module (%s): %s - %s", why, m.name, m.tag); flagged++; } }
    Log("module (%s): %ld modules, %d known overlay / injector / wrapper DLLs", why, cnt, flagged);
}
Scope::Scope(const char* what) { Slot* s = MySlot(); prev = s ? s->step : nullptr; if (s) s->step = what; }
Scope::~Scope() { if (Slot* s = MySlot()) s->step = prev; }
}
