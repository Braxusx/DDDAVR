// DDDAVR - d3d9.dll proxy (build 001: measurement only)
// Loads the system d3d9.dll, forwards the exports, and hooks the device to measure what the game does each frame:
// device/back-buffer setup, Present-to-Present CPU time, time blocked inside Present, GPU frame time (timestamp
// queries), draw calls, render-target changes, scenes and vertex-constant uploads. Logged every 5 s to ddda_vr.log.
// ini ddda_vr.ini [VR]: ForceWidth/ForceHeight (back buffer), ForceWindowed=1, NoVsync=1 (uncapped measurement).
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include "MinHook.h"
#include "log.h"
#include "xr.h"
#include "dlss_dd.h"
#include <d3dcompiler.h>

static HMODULE g_real;
static int g_useEx = 1; static bool g_isEx = false;   // build 018: the game runs on D3D9Ex (needed to share images with D3D11 / OpenXR)
static volatile LONG g_convTex, g_convCube, g_convVol, g_convVB, g_convIB;
static wchar_t g_ini[MAX_PATH];
static int g_forceW, g_forceH, g_forceWindowed, g_noVsync, g_noMt;
static int g_vrW = 3072, g_vrH = 3264;
// build 094: game version address table. Steam (SteamStub, PE timestamp 0x5a314c31) and GOG (0x5936abab, an older build) have
// the same functions at different places (found by masked signatures; code identical apart from addresses).
struct GameAddrs { const char* name; uint32_t ts, camSetup, draw, ctx, sRender, camRectGlobal; const wchar_t* noGlowArc; };
static const GameAddrs kGames[] = {
    { "Steam", 0x5a314c31, 0xdc1f80, 0xdab480, 0xda6da0, 0x18d08f4, 0x18d0b6c, L"bbsrpg_core_noglow.arc" },
    { "GOG",   0x5936abab, 0xdc0700, 0xda9be0, 0xda5530, 0x18b18d4, 0x18b1b4c, L"bbsrpg_core_noglow_gog.arc" },
};
static const GameAddrs* g_game;   // null = unknown version: the game hooks stay off
#define SRENDER_PTR ((uintptr_t)g_game->sRender)
#define CAMRECT_PTR ((uintptr_t)g_game->camRectGlobal)
static void DetectGame() {
    uint8_t* b = (uint8_t*)GetModuleHandleW(nullptr);
    uint32_t ts = *(uint32_t*)(b + *(int32_t*)(b + 0x3c) + 8);
    for (auto& g : kGames) if (g.ts == ts) { g_game = &g; Log("game: %s version (exe timestamp 0x%08x)", g.name, ts); return; }
    Log("game: unknown version (exe timestamp 0x%08x) - camera / stereo hooks OFF (only the Steam and GOG versions are known)", ts);
}   // VRModeWidth / VRModeHeight (build 075: the render size when DLSS upscales)
static int g_prof; static int g_devKeys, g_perfStats;   // build 074: developer keys / extra GPU timing (off in normal use)

// ---------------------------------------------------------------- build 095: draw distance
// The game's View Range option (settings object [settings]+0x38: 3 Farthest, 2 Farther) picks a distance factor 3.0 / 2.0 / 1.0
// at five places (LOD / fade / cull distances). The 3.0 and 2.0 come from constants shared with hundreds of other places, so
// the loads themselves are pointed at the mod's own factors (3 x DrawDistance, 2 x DrawDistance). One place copies 3.0 out of
// a register kept for something else; it is sent through a small stub instead.
struct DdAddrs { uint32_t ts, c3, c2, far3[4], far2[5], reg3; };
static const DdAddrs kDd[] = {
    { 0x5a314c31, 0x1520140, 0x14f9e04, { 0x4387d2, 0x44d88a, 0x5038f8, 0xaa8a76 }, { 0x4387e1, 0x44d899, 0x503907, 0xaa8a85, 0x50773c }, 0x507732 },   // Steam
    { 0x5936abab, 0x15016e8, 0x14db3d4, { 0x4382bf, 0x44d29a, 0x50309a, 0xaa6056 }, { 0x4382ce, 0x44d2a9, 0x5030a9, 0xaa6065, 0x506ecc }, 0x506ec2 },   // GOG
};
static float g_dd = 1.0f;                       // DrawDistance multiplier
static volatile float g_dd3 = 3.0f, g_dd2 = 2.0f;
static bool g_ddOn;
static int g_ddViewRange, g_ddRings;
static void Hook(void* target, void* det, void** orig, const char* what);   // build 098: the View Range factor sites (095) showed no visible gain - only scaled with ViewRangeScale=1
static void DdSet(float m) { g_dd = m; g_ddRings = (int)m - 1; if (g_ddRings > 2) g_ddRings = 2; if (g_ddRings < 0) g_ddRings = 0; float v = g_ddViewRange ? m : 1.0f; g_dd3 = 3.0f * v; g_dd2 = 2.0f * v; }
static bool DdWrite(uint32_t at, const void* b, size_t n) {
    DWORD o; if (!VirtualProtect((void*)(uintptr_t)at, n, PAGE_EXECUTE_READWRITE, &o)) return false;
    memcpy((void*)(uintptr_t)at, b, n); VirtualProtect((void*)(uintptr_t)at, n, o, &o);
    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)at, n); return true;
}
// build 098: scenery / instancing classes, every virtual method counted through generated stubs (which methods run per
// frame, how many objects) and per-object distances multiplied by DrawDistance whenever any method of the object runs.
// Classes (Steam vtables): uGrass2 0x1436000 (fade +0x40/+0x44, shader constants +0x70..), uGrass 0x1447000 (fade +0x34/+0x38,
// constants +0x90..), cInstancingCulling 0x1442610 and its two subclasses 0x1442718 / 0x1442828 (billboard distance +0x2c),
// sGrass 0x1435664 (fade +0x54/+0x58).
struct DdCls { const char* name; uint32_t vt; int n; int ob, oe, oc; int inv; volatile long calls[32]; long objs; };
static DdCls g_ddCls[] = {
    { "uGrass2", 0x1436000, 19, 0x40, 0x44, 0x70, 0 }, { "uGrass", 0x1447000, 19, 0x34, 0x38, 0x90, 0 },
    // build 100: the tree instancing classes (cInstancingDynamic, billboard value +0x2c) had no visible effect either way - dropped
    { "sGrass", 0x1435664, 11, 0x54, 0x58, -1, 0 },
};
struct DdObj { void* p; int cls; float b0, e0, bs, es; };
static DdObj g_ddH[8192];
static CRITICAL_SECTION g_ddCs; static long g_ddLogN; static volatile long g_ddDirty;
static uint32_t g_ddOrig[8][32];
static void DdDumpObj(DdCls& c, void* self) {
    const float* f = (const float*)self; char b[600]; int n = 0;
    for (int i = 8; i < 32 && n < 560; i++) n += snprintf(b + n, sizeof b - n, " %x:%g", i * 4, f[i]);
    Log("draw distance: new %s %p:%s", c.name, self, b);
}
static void __cdecl DdRec(uint32_t key, void* self) {
    const int ci = key >> 8, slot = key & 0xff; DdCls& c = g_ddCls[ci];
    InterlockedIncrement(&c.calls[slot]);
    if (slot == 0 || slot == 3 || slot == 4) return;   // destructor / property list / type info
    EnterCriticalSection(&g_ddCs);
    uint32_t h = ((uintptr_t)self >> 4) * 2654435761u >> 19; DdObj* r = nullptr;
    for (int k = 0; k < 64; k++) { DdObj& x = g_ddH[(h + k) & 8191]; if (x.p == self || !x.p) { r = &x; break; } }
    if (r) {
        float* b = (float*)((uint8_t*)self + c.ob); float* e = c.oe >= 0 ? (float*)((uint8_t*)self + c.oe) : b;
        bool fresh = r->p != self || r->cls != ci || !(*b == r->bs && *e == r->es);
        if (fresh) {
            r->p = self; r->cls = ci; r->b0 = *b; r->e0 = *e; r->bs = r->es = -1; c.objs++;
            if (g_ddLogN < 400) { g_ddLogN++; DdDumpObj(c, self); }
        }
        const float m = c.inv ? 1.0f / g_dd : g_dd;
        if (fresh || r->bs != r->b0 * m) {
            r->bs = *b = r->b0 * m; if (c.oe >= 0) r->es = *e = r->e0 * m; else r->es = r->bs;
            if (c.oc >= 0 && r->es > r->bs) { float* k = (float*)((uint8_t*)self + c.oc), w = r->es - r->bs;
                k[0] = r->es / w + 0.5f; k[1] = -1.0f / w; k[2] = r->bs; k[3] = r->es; }
        }
    }
    LeaveCriticalSection(&g_ddCs);
}
static void DdReport() {
    for (auto& c : g_ddCls) {
        char b[700]; int n = 0;
        for (int i = 0; i < c.n && i < 32; i++) { long v = InterlockedExchange(&c.calls[i], 0); if (v) n += snprintf(b + n, sizeof b - n, " s%d:%ld", i, v); }
        if (n || c.objs) Log("draw distance: %s calls in 10 s:%s | objects seen %ld", c.name, n ? b : " none", c.objs);
    }
}
static void DdGrassInstall(uint32_t ts) {
    if (ts != 0x5a314c31) { Log("draw distance: scenery hooks only known for the Steam version so far"); return; }
    InitializeCriticalSection(&g_ddCs);
    uint8_t* code = (uint8_t*)VirtualAlloc(nullptr, 8 * 32 * 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); if (!code) return;
    int ci = 0, hooked = 0;
    for (auto& c : g_ddCls) {
        uint32_t* vt = (uint32_t*)(uintptr_t)c.vt;
        for (int i = 0; i < c.n; i++) {
            uint32_t fn = vt[i]; g_ddOrig[ci][i] = fn;
            if (fn < 0x401000 || fn >= 0x1400000) { Log("draw distance: %s vtable slot %d not code - class skipped", c.name, i); break; }
            if (fn == 0x12ee1a1 || fn == 0x5f35c0 || fn == 0x401060 || fn == 0xc82980) continue;   // shared stubs
            uint8_t* s = code + (ci * 32 + i) * 32; int k = 0;
            s[k++] = 0x60; s[k++] = 0x9c; s[k++] = 0x51;                               // pushad, pushfd, push ecx
            s[k++] = 0x68; *(uint32_t*)(s + k) = (ci << 8) | i; k += 4;                 // push key
            s[k++] = 0xe8; *(int32_t*)(s + k) = (int32_t)((uintptr_t)DdRec - (uintptr_t)(s + k + 4)); k += 4;
            s[k++] = 0x83; s[k++] = 0xc4; s[k++] = 0x08; s[k++] = 0x9d; s[k++] = 0x61;    // add esp,8, popfd, popad
            s[k++] = 0xff; s[k++] = 0x25; *(uint32_t**)(s + k) = &g_ddOrig[ci][i]; k += 4;  // jmp [orig]
            void* p = s; DdWrite((uint32_t)(uintptr_t)&vt[i], &p, 4); hooked++;
        }
        ci++;
    }
    Log("draw distance: %d scenery / instancing methods hooked for counting", hooked);
}

// build 100: world streaming. uStageSplitCtrl (vtable 0x160cd58) loads the detailed content of the map cells inside a window
// around the player's cell; the window sizes (cells, width x height) for its three content types are ints in the current area
// object (sArea [0x18d099c] -> getter 0x419380): +0x7f0/+0x7f4, +0x7f8/+0x7fc, +0x800/+0x804. The distance check 0x5075d0 also
// scales with them. The getter is hooked: each area's own sizes are remembered and widened by 2 x StreamRings (one ring of
// cells on each side per step).
struct DdArea { void* p; int base[6], cur[6]; };
static DdArea g_ddArea[16]; static int g_ddAreaLog;
typedef void*(__fastcall* AreaGet_t)(void*, void*);
static AreaGet_t oAreaGet;
static void* __fastcall hkAreaGet(void* self, void* edx) {
    void* a = oAreaGet(self, edx);
    if (!a) return a;
    int* w = (int*)((uint8_t*)a + 0x7f0);
    DdArea* r = nullptr; for (auto& x : g_ddArea) if (x.p == a) { r = &x; break; }
    if (r && memcmp(r->cur, w, sizeof r->cur)) r = nullptr;   // the game set new sizes (area reloaded)
    if (!r) {
        for (auto& x : g_ddArea) if (x.p == a || !x.p) { r = &x; break; }
        if (!r) r = &g_ddArea[(g_ddAreaLog++) & 15];
        r->p = a; memcpy(r->base, w, sizeof r->base); memcpy(r->cur, w, sizeof r->cur);
        if (g_ddAreaLog < 60) { g_ddAreaLog++; Log("draw distance: area %p streaming windows %dx%d, %dx%d, %dx%d (cells) -> +%d rings", a, w[0], w[1], w[2], w[3], w[4], w[5], g_ddRings); }
    }
    for (int i = 0; i < 6; i++) { int v = r->base[i] > 0 ? r->base[i] + 2 * g_ddRings : r->base[i]; if (w[i] != v) w[i] = v; r->cur[i] = v; }
    return a;
}
static void DdStreamInstall(uint32_t ts) {
    const uint32_t at = ts == 0x5a314c31 ? 0x419380 : ts == 0x5936abab ? 0x419260 : 0;   // Steam / GOG (same code, same offsets)
    if (!at) return;
    static const uint8_t pro[16] = { 0x8b, 0x81, 0x24, 0x38, 0x00, 0x00, 0x56, 0x85, 0xc0, 0x74, 0x09, 0x8b, 0xb4, 0x81, 0x28, 0x38 };
    if (memcmp((void*)(uintptr_t)at, pro, 16)) { Log("draw distance: area getter not as expected - DrawDistance OFF"); return; }
    Hook((void*)(uintptr_t)at, (void*)hkAreaGet, (void**)&oAreaGet, "area getter (streaming windows)");
}
static void DdInstall(uint32_t ts) {   // HookDevice (after SteamStub has decrypted the code)
    DdStreamInstall(ts);   // build 102: DrawDistance = streaming rings; the 097-101 counting stubs are no longer installed
    g_ddOn = oAreaGet != nullptr;
    Log("draw distance: DrawDistance=%.0f (%d extra rings of map cells)%s", g_dd, g_ddRings, g_ddOn ? "" : " - not available");
    const DdAddrs* a = nullptr; for (auto& x : kDd) if (x.ts == ts) a = &x;
    if (!a || !g_ddViewRange) return;   // build 102: the View Range factor patch (095) only with ViewRangeScale=1 (no visible gain)
    // check every place first: movss xmmN,[const] (f3 0f 10 05+8N) and movaps xmm2,xmm4 / jmp +0d
    bool ok = true;
    auto isLoad = [](uint32_t at, uint32_t c) { const uint8_t* p = (const uint8_t*)(uintptr_t)at;
        return p[0] == 0xf3 && p[1] == 0x0f && p[2] == 0x10 && (p[3] & 0xc7) == 0x05 && *(const uint32_t*)(p + 4) == c; };
    for (uint32_t x : a->far3) ok = ok && isLoad(x, a->c3);
    for (uint32_t x : a->far2) ok = ok && isLoad(x, a->c2);
    static const uint8_t reg[5] = { 0x0f, 0x28, 0xd4, 0xeb, 0x0d };
    ok = ok && !memcmp((void*)(uintptr_t)a->reg3, reg, 5);
    if (!ok) { Log("draw distance: the View Range code is not where expected - DrawDistance OFF"); return; }
    for (uint32_t x : a->far3) { const float* p = (const float*)&g_dd3; DdWrite(x + 4, &p, 4); }
    for (uint32_t x : a->far2) { const float* p = (const float*)&g_dd2; DdWrite(x + 4, &p, 4); }
    // stub: movss xmm2,[g_dd3] ; jmp back to the shared tail (reg3 + 0x12)
    uint8_t* s = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (s) {
        s[0] = 0xf3; s[1] = 0x0f; s[2] = 0x10; s[3] = 0x15; *(const volatile float**)(s + 4) = &g_dd3;
        s[8] = 0xe9; *(int32_t*)(s + 9) = (int32_t)(a->reg3 + 0x12) - (int32_t)(uintptr_t)(s + 13);
        uint8_t j[5] = { 0xe9 }; *(int32_t*)(j + 1) = (int32_t)(uintptr_t)s - (int32_t)(a->reg3 + 5);
        DdWrite(a->reg3, j, 5);
    }
    Log("draw distance: ViewRangeScale=1 - DrawDistance=%.2f (View Range Farthest factor 3 -> %.2f, Farther 2 -> %.2f) - only applies with View Range Farther / Farthest", g_dd, g_dd3, g_dd2);
}

template <class T> static T Real(const char* name) { return g_real ? (T)GetProcAddress(g_real, name) : nullptr; }

// ---------------------------------------------------------------- frame statistics
static LARGE_INTEGER g_qpf;
static inline double NowMs() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart * 1000.0 / g_qpf.QuadPart; }
// build 055: CPU profile of our device hooks (sampled 1 in 16): calls per frame, time in the whole hook, time in the real call
enum { HP_VSC, HP_PSC, HP_DIP, HP_DP, HP_SetRT, HP_SetTex, HP_SetRS, HP_SetSS, HP_SetVS, HP_SetPS, HP_Clear, HP_SetVP, HP_SetSc, HP_SetDS, HP_N };
static const char* const g_hpName[HP_N] = { "VSConst", "PSConst", "DrawIdx", "Draw", "SetRT", "SetTex", "SetRS", "SetStream", "SetVS", "SetPS", "Clear", "SetVP", "SetScissor", "SetDS" };
struct HookProf { long calls; unsigned tick, rtick; double tot, real; };

static HookProf g_hp[HP_N];
struct HPScope { int id; bool s; double t0; HPScope(int i) : id(i) { s = false; if (!g_prof) return; HookProf& h = g_hp[i]; h.calls++; s = (++h.tick & 15) == 0; t0 = s ? NowMs() : 0; } ~HPScope() { if (s) g_hp[id].tot += (NowMs() - t0) * 16.0; } };
struct HPReal { int id; bool s; double t0; HPReal(int i) : id(i) { s = (++g_hp[i].rtick & 15) == 0; t0 = s ? NowMs() : 0; } ~HPReal() { if (s) g_hp[id].real += (NowMs() - t0) * 16.0; } };
struct Stats { long frames, draws, drawsUP, rts, scenes, vsc, clears; double cpu, present, gpu; long gpuN; double cpuMax, drawMs, submitMs, gpuR0, gpuR1, gpuR1end; long gpuRN; double xrMs; };
static double g_firstScene;
static Stats g_s;
static long g_fDraws, g_fRts, g_fScenes, g_fVsc;
static double g_lastPresentEnd, g_winStart, g_startMs;
static DWORD g_presentTid, g_devTid;

// GPU timing: timestamp at frame start and end, read back 3 frames later
enum { RING = 4 };
static IDirect3DQuery9 *g_qDis[RING], *g_qFreq[RING], *g_qBeg[RING], *g_qEnd[RING];
static int g_ring, g_ringValid[RING];
// build 054: per-replay GPU timestamps (left eye replay, right eye replay), issued from hkDraw into the current ring slot
static IDirect3DQuery9* g_qR[RING][3]; static int g_qRValid[RING];
static void GpuReplayMark(IDirect3DDevice9* d, int k) { if (!g_perfStats) return;
    int i = g_ring;
    if (!g_qDis[i]) return;
    if (!g_qR[i][k] && FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &g_qR[i][k]))) return;
    g_qR[i][k]->Issue(D3DISSUE_END);
    if (k == 2) g_qRValid[i] = 1;
}
static void GpuBegin(IDirect3DDevice9* d) { if (!g_perfStats) return;
    int i = g_ring;
    if (!g_qDis[i]) {
        if (FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &g_qDis[i])) || FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &g_qFreq[i])) ||
            FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &g_qBeg[i])) || FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &g_qEnd[i]))) {
            LOG_FIRST(1, "gpu: timestamp queries not available");
            return;
        }
    }
    g_qDis[i]->Issue(D3DISSUE_BEGIN);
    g_qBeg[i]->Issue(D3DISSUE_END);
}
static void GpuEnd() { if (!g_perfStats) return;
    int i = g_ring;
    if (!g_qDis[i]) return;
    g_qEnd[i]->Issue(D3DISSUE_END);
    g_qFreq[i]->Issue(D3DISSUE_END);
    g_qDis[i]->Issue(D3DISSUE_END);
    g_ringValid[i] = 1;
    g_ring = (i + 1) % RING;
    int r = g_ring;   // oldest: issued RING-1 frames ago
    if (!g_ringValid[r]) return;
    BOOL dis = TRUE; UINT64 f = 0, b = 0, e = 0;
    if (g_qDis[r]->GetData(&dis, sizeof dis, 0) == S_OK && g_qFreq[r]->GetData(&f, sizeof f, 0) == S_OK && g_qBeg[r]->GetData(&b, sizeof b, 0) == S_OK &&
        g_qEnd[r]->GetData(&e, sizeof e, 0) == S_OK && !dis && f && e > b) {
        g_s.gpu += (e - b) * 1000.0 / f; g_s.gpuN++;
        UINT64 t0 = 0, t1 = 0, t2 = 0;
        if (g_qRValid[r] && g_qR[r][0]->GetData(&t0, sizeof t0, 0) == S_OK && g_qR[r][1]->GetData(&t1, sizeof t1, 0) == S_OK &&
            g_qR[r][2]->GetData(&t2, sizeof t2, 0) == S_OK && t1 >= t0 && t2 >= t1 && e >= t2) {
            g_s.gpuR0 += (t1 - t0) * 1000.0 / f; g_s.gpuR1 += (t2 - t1) * 1000.0 / f; g_s.gpuR1end += (e - t2) * 1000.0 / f; g_s.gpuRN++;
        }
    }
    g_qRValid[r] = 0;
    g_ringValid[r] = 0;
}


// ---------------------------------------------------------------- build 006: one-frame capture of the render structure
// F8 (or automatically, once, on the first frame with > 3000 draws after 20 s) logs every render target, depth
// surface, viewport, scissor, clear, StretchRect and draw run of one frame, with the game address that issued it.
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
static int g_capState;           // 0 idle, 1 armed (capture next frame), 2 capturing
static std::string g_cap;
static long g_capRun, g_capRunPrims, g_capEvents; static void* g_capRunCaller[4]; static int g_capRunCallers;
static void CapF(const char* fmt, ...) {
    if (g_capEvents++ > 6000) return;
    char b[512]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    g_cap += b; g_cap += '\n';
}
static void CapFlushRun() {
    if (!g_capRun) return;
    char c[128] = ""; int o = 0;
    for (int i = 0; i < g_capRunCallers; i++) o += snprintf(c + o, sizeof c - o, " %p", g_capRunCaller[i]);
    CapF("    draws %ld (prims %ld) from%s", g_capRun, g_capRunPrims, c);
    g_capRun = g_capRunPrims = 0; g_capRunCallers = 0;
}
static inline void CapDraw(void* caller, UINT prims) {
    g_capRun++; g_capRunPrims += prims;
    for (int i = 0; i < g_capRunCallers; i++) if (g_capRunCaller[i] == caller) return;
    if (g_capRunCallers < 4) g_capRunCaller[g_capRunCallers++] = caller;
}
static void SurfDesc(IDirect3DSurface9* s, char* out, size_t n) {
    if (!s) { snprintf(out, n, "null"); return; }
    D3DSURFACE_DESC d; s->GetDesc(&d);
    snprintf(out, n, "%p %ux%u fmt %d%s", (void*)s, d.Width, d.Height, (int)d.Format, d.MultiSampleType ? " msaa" : "");
}
#define CAPON (g_capState == 2)

// ---------------------------------------------------------------- device hooks
typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(STDMETHODCALLTYPE* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* Scene_t)(IDirect3DDevice9*);
typedef HRESULT(STDMETHODCALLTYPE* SetRT_t)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
typedef HRESULT(STDMETHODCALLTYPE* DP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* DIP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* DPUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
typedef HRESULT(STDMETHODCALLTYPE* DIPUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
typedef HRESULT(STDMETHODCALLTYPE* VSC_t)(IDirect3DDevice9*, UINT, const float*, UINT);
typedef HRESULT(STDMETHODCALLTYPE* Clear_t)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
static Present_t oPresent; static Reset_t oReset; static Scene_t oBegin, oEnd; static SetRT_t oSetRT; static DP_t oDP; static DIP_t oDIP;
static DPUP_t oDPUP; static DIPUP_t oDIPUP; static VSC_t oVSC; static Clear_t oClear;

static void LogPP(const char* what, const D3DPRESENT_PARAMETERS* p) {
    Log("%s: back buffer %ux%u format %d count %u, msaa %d/%lu, swap effect %d, windowed %d, depth %d (auto %d), flags 0x%lx, refresh %u, interval 0x%x, window %p",
        what, p->BackBufferWidth, p->BackBufferHeight, (int)p->BackBufferFormat, p->BackBufferCount, (int)p->MultiSampleType, p->MultiSampleQuality,
        (int)p->SwapEffect, p->Windowed, (int)p->AutoDepthStencilFormat, p->EnableAutoDepthStencil, p->Flags, p->FullScreen_RefreshRateInHz,
        p->PresentationInterval, p->hDeviceWindow);
}
static void ApplyOverrides(D3DPRESENT_PARAMETERS* p) {
    if (g_forceW > 0 && g_forceH > 0) { p->BackBufferWidth = g_forceW; p->BackBufferHeight = g_forceH; }
    if (g_forceWindowed) { p->Windowed = TRUE; p->FullScreen_RefreshRateInHz = 0; }
    if (g_noVsync) p->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
}

static void CaptureTick(long lastDraws);
static long g_uiDraws, g_uiFrames; static bool g_movieSeen; static int g_movieHold;
static void FullRectTick();
static void UiEndFrame(IDirect3DDevice9* d);
static void VignetteTick();
static void WatchTick();
static void EndOfFrameStats(double presentMs) {
    double now = NowMs();
    if (g_lastPresentEnd > 0) {
        double cpu = now - g_lastPresentEnd;
        g_s.cpu += cpu; if (cpu > g_s.cpuMax) g_s.cpuMax = cpu;
    }
    g_s.present += presentMs;
    g_s.frames++;
    CaptureTick(g_fDraws);
    FullRectTick();
    VignetteTick();
    WatchTick();
    g_s.draws += g_fDraws; g_s.rts += g_fRts; g_s.scenes += g_fScenes; g_s.vsc += g_fVsc;
    g_fDraws = g_fRts = g_fScenes = g_fVsc = 0;
    if (g_winStart == 0) g_winStart = now;
    if (now - g_winStart >= 5000 && g_s.frames) {
        double secs = (now - g_winStart) / 1000.0, n = (double)g_s.frames;
        Log("perf: %.1f fps | frame %.2f ms (worst %.1f) of which inside Present %.2f ms | GPU %.2f ms | per frame: draws %.0f (UP %.0f), RT changes %.0f, scenes %.0f, VS constant uploads %.0f, clears %.0f | render submit %.2f ms (first BeginScene to Present), inside DrawIndexedPrimitive %.2f ms",
            n / secs, (g_s.cpu + g_s.present) / n, g_s.cpuMax, g_s.present / n, g_s.gpuN ? g_s.gpu / g_s.gpuN : -1.0, g_s.draws / n, g_s.drawsUP / n,
            g_s.rts / n, g_s.scenes / n, g_s.vsc / n, g_s.clears / n, g_s.submitMs / n, g_s.drawMs / n);
        if (g_prof) {   // build 055: our hooks, per frame
            char b[1600]; int k = 0; double hookSum = 0, realSum = 0;
            for (int i = 0; i < HP_N; i++) {
                const HookProf& h = g_hp[i]; if (!h.calls) continue;
                hookSum += h.tot; realSum += h.real;
                k += snprintf(b + k, sizeof b - k, " | %s %.0f calls %.2f/%.2f ms", g_hpName[i], h.calls / n, h.tot / n, h.real / n);
                if (k > (int)sizeof b - 80) break;
            }
            Log("perf: hooks per frame (calls, whole hook / real call): total %.2f / %.2f ms%s", hookSum / n, realSum / n, b);
            memset(g_hp, 0, sizeof g_hp);
        }
        if (g_s.gpuRN) Log("perf: GPU left-eye replay %.2f ms | right-eye replay %.2f ms | after the replays (copies, UI, present) %.2f ms | XR wait/submit at Present %.2f ms per frame",
            g_s.gpuR0 / g_s.gpuRN, g_s.gpuR1 / g_s.gpuRN, g_s.gpuR1end / g_s.gpuRN, g_s.xrMs / n);
        if (g_uiFrames) Log("ui: layer used in %ld frames, %.0f UI draws per frame", g_uiFrames, (double)g_uiDraws / g_uiFrames);
        g_uiFrames = g_uiDraws = 0;
        if (g_isEx) Log("ex: managed resources made default/dynamic so far: textures %ld, cube %ld, volume %ld, vertex buffers %ld, index buffers %ld", g_convTex, g_convCube, g_convVol, g_convVB, g_convIB);
        LogFlush();
        memset(&g_s, 0, sizeof g_s);
        g_winStart = now;
    }
    g_lastPresentEnd = NowMs();
}

// build 026: a 768x816 screenshot of the captured frame (ddda_shot_N.bmp)
static int g_shotN;
static void Screenshot(IDirect3DDevice9* d, IDirect3DSurface9* src = nullptr, const wchar_t* name = L"ddda_shot_%d.bmp") {
    IDirect3DSurface9 *bb = nullptr, *rt = nullptr, *sm = nullptr;
    const UINT W = 768, H = 816;
    if (src) { bb = src; bb->AddRef(); }   // build 031: also the UI layer (ddda_uishot_N.bmp)
    else { if (FAILED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return; ++g_shotN; }
    if (SUCCEEDED(d->CreateRenderTarget(W, H, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr)) &&
        SUCCEEDED(d->CreateOffscreenPlainSurface(W, H, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sm, nullptr)) &&
        SUCCEEDED(d->StretchRect(bb, nullptr, rt, nullptr, D3DTEXF_LINEAR)) && SUCCEEDED(d->GetRenderTargetData(rt, sm))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sm->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            std::string f(54 + W * H * 3, '\0'); uint8_t* p = (uint8_t*)&f[0];
            p[0] = 'B'; p[1] = 'M'; *(uint32_t*)(p + 2) = (uint32_t)f.size(); *(uint32_t*)(p + 10) = 54; *(uint32_t*)(p + 14) = 40;
            *(int32_t*)(p + 18) = W; *(int32_t*)(p + 22) = -(int32_t)H; *(uint16_t*)(p + 26) = 1; *(uint16_t*)(p + 28) = 24; *(uint32_t*)(p + 34) = W * H * 3;
            for (UINT y = 0; y < H; y++) { const uint8_t* s = (const uint8_t*)lr.pBits + y * lr.Pitch; uint8_t* o = p + 54 + y * W * 3; for (UINT x = 0; x < W; x++) { o[x * 3] = s[x * 4]; o[x * 3 + 1] = s[x * 4 + 1]; o[x * 3 + 2] = s[x * 4 + 2]; } }
            sm->UnlockRect();
            wchar_t path[MAX_PATH]; wcscpy(path, g_ini); wchar_t* sl = wcsrchr(path, L'\\'); swprintf(sl ? sl + 1 : path, 64, name, g_shotN);
            HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
            DWORD w = 0; if (h != INVALID_HANDLE_VALUE) { WriteFile(h, f.data(), (DWORD)f.size(), &w, nullptr); CloseHandle(h); }
            Log("capture: screenshot %ls", sl ? sl + 1 : path);
        }
    }
    if (sm) sm->Release(); if (rt) rt->Release(); bb->Release();
}
static void UiShot(IDirect3DDevice9* d); static void RecFrame(IDirect3DDevice9* d); static void RecStart();
static HRESULT STDMETHODCALLTYPE hkPresent(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
    if (!g_presentTid) { g_presentTid = GetCurrentThreadId(); Log("present: first Present on thread %lu (device created on %lu)", g_presentTid, g_devTid); }
    GpuEnd();
    if (g_capState == 2) { Screenshot(d); UiShot(d); RecStart(); }
    RecFrame(d);
    UiEndFrame(d);
    double tx = NowMs();
    dxr::OnPresent(d);
    double t0 = NowMs(); g_s.xrMs += t0 - tx;
    if (g_firstScene > 0) { g_s.submitMs += t0 - g_firstScene; g_firstScene = 0; }
    HRESULT hr = oPresent(d, a, b, w, r);
    double t1 = NowMs();
    EndOfFrameStats(t1 - t0);
    GpuBegin(d);
    return hr;
}
static void ReleaseQueries() {
    for (int i = 0; i < RING; i++) {
        if (g_qDis[i]) g_qDis[i]->Release(); if (g_qFreq[i]) g_qFreq[i]->Release(); if (g_qBeg[i]) g_qBeg[i]->Release(); if (g_qEnd[i]) g_qEnd[i]->Release();
        g_qDis[i] = g_qFreq[i] = g_qBeg[i] = g_qEnd[i] = nullptr; g_ringValid[i] = 0;
    }
}
static HRESULT STDMETHODCALLTYPE hkReset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* p) {
    LogPP("reset (game)", p);
    ApplyOverrides(p);
    ReleaseQueries();
    HRESULT hr = oReset(d, p);
    Log("reset -> 0x%08lx", (unsigned long)hr);
    return hr;
}
#define RA __builtin_return_address(0)
static volatile bool g_inScene;   // build 075: draws of our own passes need a scene
static HRESULT STDMETHODCALLTYPE hkBegin(IDirect3DDevice9* d) { g_inScene = true; if (!g_fScenes) g_firstScene = NowMs(); g_fScenes++; if (CAPON) { CapFlushRun(); CapF("BeginScene from %p", RA); } return oBegin(d); }
static HRESULT STDMETHODCALLTYPE hkEnd(IDirect3DDevice9* d) { g_inScene = false; if (CAPON) { CapFlushRun(); CapF("EndScene from %p", RA); } return oEnd(d); }
static IDirect3DSurface9* g_curRT0; static int g_dumpPost;
static UINT g_rtW, g_rtH; static int g_widen; static uintptr_t g_bandAddr; static int g_killBand; static bool g_inBandRT; static int g_fullRect, g_capCountdown;
// build 027: UI layer. Once the game uploads its UI screen constants (c10 = 2/1280, -2/UIheight) in a frame, the UI draws
// that go to the scene target are redirected into a transparent shared texture, shown in the headset as a panel.
static bool g_uiArmed, g_uiActive; static IDirect3DSurface9* g_uiScene; static IDirect3DSurface9* g_uiSurf; static UINT g_uiW, g_uiH;
// build 042: synchronous stereo replay state: -1 none, 0 left-eye replay, 1 right-eye replay
static volatile int g_replay = -1; static long g_r1Skipped, g_r1SrSkipped;
// build 067: replay 1 starts at the first top-level command entry that writes an eye target in replay 0 (skips the shadow /
// environment / simulation prefix instead of running its state setup and discarding its draws). 0xda6da0 builds the walker
// context: ctx+0x220 = level-0 cursor (8-byte entries {key, cmd*}), ctx+0x224 = end.
static uint8_t* g_ctx; static uint8_t* g_ctxBegin; static long g_r1Start = -1, g_r1StartFrame = -1, g_r1Total; static int g_r1Prefix = 0;
static long g_pfxSum, g_pfxTotSum, g_pfxN;
typedef void(__fastcall* Ctx_t)(void*, void*, uint8_t*); static Ctx_t oCtx;
static void __fastcall hkCtx(void* self, void* edx, uint8_t* ctx) {
    oCtx(self, edx, ctx);
    g_ctx = ctx; g_ctxBegin = *(uint8_t**)(ctx + 0x220);
    uint8_t* end = *(uint8_t**)(ctx + 0x224);
    if (g_replay == 0) { g_r1Start = -1; g_r1Total = (long)((end - g_ctxBegin) / 8);
        IDirect3DDevice9* dev = *(IDirect3DDevice9**)((uint8_t*)self + 0x98); IDirect3DSurface9* rt = nullptr;
        if (dev && SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt) { D3DSURFACE_DESC dd; if (SUCCEEDED(rt->GetDesc(&dd))) { g_rtW = dd.Width; g_rtH = dd.Height; } rt->Release(); } }
    else if (g_replay == 1 && g_r1Prefix && g_r1Start > 0 && g_r1Start < g_r1Total && (end - g_ctxBegin) / 8 == g_r1Total) {
        *(uint8_t**)(ctx + 0x220) = g_ctxBegin + 8 * g_r1Start;
        g_pfxSum += g_r1Start; g_pfxTotSum += g_r1Total; g_pfxN++;
    }
}
static inline void R1NoteEyeWrite();
static inline void R1NoteIfEye() {   // replay 0, before any eye write is known: is the current RT0 an eye target?
    if (g_replay != 0 || g_r1Start >= 0 || !g_uiW) return;
    for (int k = 0; k < 6; k++) if (g_rtW == (g_uiW >> k) && g_rtH == (g_uiH >> k)) { R1NoteEyeWrite(); return; }
}
static inline void R1NoteEyeWrite() {   // replay 0: the first command entry that touches an eye target
    if (g_replay != 0 || g_r1Start >= 0 || !g_ctx) return;
    uint8_t* cur = *(uint8_t**)(g_ctx + 0x220);
    long k = (long)((cur - g_ctxBegin) / 8) - 1;   // the cursor is already past the entry being executed
    g_r1Start = k < 0 ? 0 : k;
}
// build 047: the shadow map (2048x6144 R32F) is updated part-way through the frame and only on some frames; the scene reads
// the state it had before. The right-eye replay skips shadow rendering, so it must see the map as replay 0 saw it: keep a
// copy from before replay 0, put it back for replay 1, then restore the new map for the next frame.
static IDirect3DSurface9 *g_shadowRT, *g_shBefore, *g_shAfter; static bool g_shadowUsed; static int g_shadowSync = 0;
// build 048: per-frame replay diagnostics while the stereo recorder runs (ddda_stereo_N.txt)
static bool g_diagOn; static void DiagConst(bool ps, UINT r, const float* raw, const float* act, UINT n); static void DiagRT(IDirect3DSurface9* s);
static bool g_uiSkip; static int g_uiHideMode; static int g_pixelUi = 1;
// build 032: the game lays out its pixel-space "prim" 2D draws (minimap image, map tiles) in a virtual 16:9 screen of the
// buffer width at y = 0, while the GUI (markers, HUD) is centred in the buffer: shift prims down by the band offset.
// The UI is clipped to the 16:9 band, as a flat screen would (the game parks hidden icons just above the screen).
static int g_primShift = 1, g_uiClip = 1, g_postAll = 0; static void* g_primVS[16]; static int g_primN;
static bool IsPrimVS(void* vs) { for (int i = 0; i < g_primN; i++) if (g_primVS[i] == vs) return true; return false; }
static uint16_t g_primLen[16]; static int g_hideBandOverlay = 1; static long g_bandOvlN; static bool g_mapSeen, g_mapActive;   // build 073
// only the simplest prim shader (minimap image, map tiles) is laid out at y = 0; others (screen effects) use the real band
static bool IsPrimShiftVS(void* vs) { for (int i = 0; i < g_primN; i++) if (g_primVS[i] == vs) return g_primLen[i] < 640; return false; }
// build 075/076: the INTZ depth-stencils we made (the game makes two at the back-buffer size); draws per replay on each
struct IntzDS { IDirect3DTexture9* tex; IDirect3DSurface9* surf; long draws; };
static IntzDS g_intz[4]; static int g_intzN;
typedef HRESULT(STDMETHODCALLTYPE* SetVP_t)(IDirect3DDevice9*, const D3DVIEWPORT9*);
static SetVP_t oSetVP;   // (declared early: build 083)
static int g_worldGui = 1; static float g_wmScale = 1.0f, g_wmLift = 0.0f;   // build 087: world markers in the eye pictures (Home toggles)
bool g_wlog; long g_wlogFrame, g_wlogLines;   // build 085
struct WEl { float x, y, z; };
struct WGrp { float z, x0, x1, y0, y1; };   // build 091: GUI pieces sharing one depth this frame, and their spread
static WGrp g_gPrev[64], g_gCur[64]; static int g_gPrevN, g_gCurN;
static WEl g_wPrev[64], g_wCur[64]; static int g_wPrevN, g_wCurN;
static bool g_uiLast;   // build 080
static bool g_talkSeen, g_talkActive;   // build 081: NPC conversation box drawn this / last frame
static IDirect3DSurface9* g_curDS; static RECT g_scGame; static DWORD g_rsScissor; static bool g_uiClipped;
typedef HRESULT(STDMETHODCALLTYPE* SetScF_t)(IDirect3DDevice9*, const RECT*); static SetScF_t oSetScF;
// build 031: the "image plane" filter (PS constant fImagePlaneColor) lays a full-screen texture over the scene: the camera vignette
// and the red low-health overlay. HideScreenOverlay=1 skips those draws.
static int g_hideOverlay = 0; static void* g_ipPS[64]; static int g_ipN; static long g_ipSkipped;
// build 033: the depth-of-field composite (PS sampler tDOFMap) blends the 640x680 band target into the 16:9 band of the scene. The
// band chain is killed (KillBandEffect), so that target only holds stale data: the red low-health "blur" and the drifting dot.
// build 038: the screen-space sun shafts (PS fGodRaysColor, additive) - F10 toggles them, ini KillGodRays
static int g_killGodRays = 1; static void* g_grPS[16]; static int g_grN;
static bool IsGodRaysPS(void* ps) { for (int i = 0; i < g_grN; i++) if (g_grPS[i] == ps) return true; return false; }
// build 039: lens flare (VS constant table CBPrimitiveMetaDataLensFlare): ghosts and halo around the sun, ini HideLensFlare
static int g_hideFlare = 1; static void* g_flareVS[16]; static int g_flareN; static long g_flareSkipped;
static bool IsFlareVS(void* vs) { for (int i = 0; i < g_flareN; i++) if (g_flareVS[i] == vs) return true; return false; }
static int g_noGlow = 1;
static int g_killDof = 1; static void* g_dofPS[32]; static int g_dofN; static long g_dofSkipped;
static bool IsDofPS(void* ps) { for (int i = 0; i < g_dofN; i++) if (g_dofPS[i] == ps) return true; return false; }
static bool IsImagePlanePS(void* ps) { for (int i = 0; i < g_ipN; i++) if (g_ipPS[i] == ps) return true; return false; }
static DWORD g_rsSep = FALSE, g_rsSrcA = D3DBLEND_ONE, g_rsDstA = D3DBLEND_ZERO, g_rsOpA = D3DBLENDOP_ADD;
static HRESULT STDMETHODCALLTYPE hkSetRT(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s) { HPScope _hp(HP_SetRT);
    g_fRts++;
    if (i == 0 && g_uiActive && s && s == g_uiScene && g_replay != 1) {   // the game rebinds its scene target during the UI: keep drawing into the UI layer
        g_curRT0 = s; g_inBandRT = false;
        if (CAPON) { CapFlushRun(); CapF("RT0 = scene target -> UI layer  from %p", RA); }
        return oSetRT(d, 0, g_uiSurf);
    }
    if (g_diagOn && s) DiagRT(s);
    if (i == 0) {
        g_rtW = g_rtH = 0; if (s) { D3DSURFACE_DESC sd; if (SUCCEEDED(s->GetDesc(&sd))) { g_rtW = sd.Width; g_rtH = sd.Height; } }
        g_inBandRT = false; g_curRT0 = s;
        if (g_rtW == 1280 && g_rtH == 720) g_movieSeen = true;
        if (s && g_rtW == 2048 && g_rtH == 6144) { if (g_shadowRT != s) { g_shadowRT = s; Log("stereo: shadow map target %p", (void*)s); } if (g_replay == 0) g_shadowUsed = true; }
        R1NoteIfEye();
        if (g_killBand && g_fullRect && g_rtW && g_rtW < 1024 && g_rtH * 16 > g_rtW * 9 + 32) {
            // a small target whose height is not 16:9: is it drawn in a 16:9 band? (decided at SetViewport)
        }
    }
    if (CAPON) { CapFlushRun(); char b[96]; SurfDesc(s, b, sizeof b); CapF("RT%lu = %s  from %p", i, b, RA); }
    return oSetRT(d, i, s);
}
// ---------------------------------------------------------------- build 019: post-pass recorder (vignette hunt)
// Shadows the bound pixel shader, textures, a few render states and PS constants c0..c31 at all times (cheap),
// and during a capture frame records them for every draw in a ring; the last 512 draws are dumped at Present,
// with the bytecode of their pixel shaders, to ddda_post_N.txt / ddda_ps_<ptr>.bin.
static IDirect3DPixelShader9* g_curPS; static IDirect3DVertexShader9* g_curVS;
static IDirect3DBaseTexture9* g_curTex[8];
static float g_psc[32][4];
static DWORD g_rsAlpha, g_rsSrc = 2, g_rsDst = 1, g_rsOp = 1, g_rsCW = 15, g_rsZ = 1, g_rsSrgbW;
struct TexRec { void* p; uint16_t w, h; int16_t fmt; uint8_t type; };
struct DrawRec { long idx; uint16_t rtW, rtH; void* rt; void* ps; void* vs; void* caller; UINT prims; TexRec tex[8]; DWORD alpha, src, dst, op, cw, z, srgbw; float psc[32][4];
                 uint8_t kind, ptype, ui; int8_t rep; UINT nv, stride; float bb[4]; float v0[8]; float vsc[16][4]; };
// build 025: draw arguments and stream 0, for the vertex rectangle of UI draws
struct DrawArgs { int kind; D3DPRIMITIVETYPE t; UINT a, b, c; const void* up; UINT upStride; UINT si, prims; IDirect3DDevice9* dev; } static g_da;
static IDirect3DVertexBuffer9* g_vb0; static UINT g_vbOff, g_vbStride; static float g_vsShadow[16][4];
static UINT VertsFor(D3DPRIMITIVETYPE t, UINT prims) { return t == D3DPT_TRIANGLELIST ? prims * 3 : t == D3DPT_TRIANGLESTRIP || t == D3DPT_TRIANGLEFAN ? prims + 2 : t == D3DPT_LINELIST ? prims * 2 : prims + 1; }
static void VertexInfo(DrawRec& r) {
    r.kind = (uint8_t)g_da.kind; r.ptype = (uint8_t)g_da.t; r.nv = 0; r.stride = 0; memset(r.bb, 0, sizeof r.bb); memset(r.v0, 0, sizeof r.v0);
    memcpy(r.vsc, g_vsShadow, sizeof r.vsc);
    const uint8_t* base = nullptr; UINT first = 0, n = 0, stride = 0; void* locked = nullptr;
    if (g_da.kind == 3 || g_da.kind == 4) { base = (const uint8_t*)g_da.up; stride = g_da.upStride; first = g_da.kind == 4 ? g_da.a : 0; n = g_da.kind == 4 ? g_da.b : VertsFor(g_da.t, g_da.c); }
    else if (g_vb0 && g_vbStride) {
        stride = g_vbStride;
        if (g_da.kind == 1) { first = g_da.a; n = VertsFor(g_da.t, g_da.c); } else { first = g_da.a + g_da.b; n = g_da.c; }
        if (n > 4096) n = 4096;
        if (SUCCEEDED(g_vb0->Lock(g_vbOff + first * stride, n * stride, &locked, D3DLOCK_READONLY)) && locked) { base = (const uint8_t*)locked; first = 0; }
        else base = nullptr;
    }
    // build 031: a small indexed draw out of a big shared buffer -> the rectangle of the vertices its indices really use
    if (base && stride >= 8 && n && g_da.kind == 2 && g_da.dev && g_da.prims && g_da.prims <= 64) {
        IDirect3DIndexBuffer9* ib = nullptr;
        if (SUCCEEDED(g_da.dev->GetIndices(&ib)) && ib) {
            D3DINDEXBUFFER_DESC idd; UINT ni = VertsFor(g_da.t, g_da.prims); void* ip = nullptr;
            if (SUCCEEDED(ib->GetDesc(&idd))) {
                UINT isz = idd.Format == D3DFMT_INDEX32 ? 4 : 2;
                if (SUCCEEDED(ib->Lock(g_da.si * isz, ni * isz, &ip, D3DLOCK_READONLY)) && ip) {
                    float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f; UINT used = 0;
                    for (UINT k = 0; k < ni; k++) {
                        UINT ix = isz == 4 ? ((const uint32_t*)ip)[k] : ((const uint16_t*)ip)[k];
                        if (ix < g_da.b || ix - g_da.b >= n) continue;
                        const float* fv = (const float*)(base + (size_t)(first + ix - g_da.b) * stride);
                        if (!used) memcpy(r.v0, fv, stride >= 32 ? 32 : stride);
                        used++; mnx = fminf(mnx, fv[0]); mxx = fmaxf(mxx, fv[0]); mny = fminf(mny, fv[1]); mxy = fmaxf(mxy, fv[1]);
                    }
                    ib->Unlock();
                    if (used) { r.nv = used; r.stride = stride; r.bb[0] = mnx; r.bb[1] = mny; r.bb[2] = mxx; r.bb[3] = mxy; if (locked) g_vb0->Unlock(); ib->Release(); return; }
                }
            }
            ib->Release();
        }
    }
    if (base && stride >= 8 && n) {
        if (n > 4096) n = 4096;
        r.nv = n; r.stride = stride;
        memcpy(r.v0, base + (size_t)first * stride, stride >= 32 ? 32 : stride);
        float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f;
        for (UINT i = 0; i < n; i++) { const float* f = (const float*)(base + (size_t)(first + i) * stride); if (f[0] < mnx) mnx = f[0]; if (f[0] > mxx) mxx = f[0]; if (f[1] < mny) mny = f[1]; if (f[1] > mxy) mxy = f[1]; }
        r.bb[0] = mnx; r.bb[1] = mny; r.bb[2] = mxx; r.bb[3] = mxy;
    }
    if (locked) g_vb0->Unlock();
}
static void UiDump(int nCap);
static long POSTN = 512;
static DrawRec* g_post; static long g_postN;
static void TexInfo(IDirect3DBaseTexture9* t, TexRec& r) {
    r.p = t; r.w = r.h = 0; r.fmt = 0; r.type = 0; if (!t) return;
    D3DRESOURCETYPE ty = t->GetType(); r.type = (uint8_t)ty;
    if (ty == D3DRTYPE_TEXTURE) { D3DSURFACE_DESC d; if (SUCCEEDED(((IDirect3DTexture9*)t)->GetLevelDesc(0, &d))) { r.w = (uint16_t)d.Width; r.h = (uint16_t)d.Height; r.fmt = (int16_t)d.Format; } }
    else if (ty == D3DRTYPE_CUBETEXTURE) { D3DSURFACE_DESC d; if (SUCCEEDED(((IDirect3DCubeTexture9*)t)->GetLevelDesc(0, &d))) { r.w = (uint16_t)d.Width; r.h = (uint16_t)d.Height; r.fmt = (int16_t)d.Format; } }
}
static void PostRec(void* caller, UINT prims) {
    if (!g_post) { POSTN = 8192; /* build 080: the whole frame (the UI is drawn in the left replay) */ g_post = (DrawRec*)calloc(POSTN, sizeof(DrawRec)); }
    if (!g_post) return;
    DrawRec& r = g_post[g_postN % POSTN]; r.idx = g_postN++;
    r.rt = g_curRT0; r.rtW = (uint16_t)g_rtW; r.rtH = (uint16_t)g_rtH; r.ps = g_curPS; r.vs = g_curVS; r.caller = caller; r.prims = prims;
    for (int i = 0; i < 8; i++) TexInfo(g_curTex[i], r.tex[i]);
    r.ui = g_uiLast; r.rep = (int8_t)g_replay;
    r.alpha = g_rsAlpha; r.src = g_rsSrc; r.dst = g_rsDst; r.op = g_rsOp; r.cw = g_rsCW; r.z = g_rsZ; r.srgbw = g_rsSrgbW;
    memcpy(r.psc, g_psc, sizeof g_psc);
    VertexInfo(r);
}
static void PostDump(int nCap) {
    if (!g_post || !g_postN) return;
    std::string o; char b[1024];
    long first = g_postN > POSTN ? g_postN - POSTN : 0;
    snprintf(b, sizeof b, "frame draws %ld, recorded %ld..%ld\n", g_postN, first, g_postN - 1); o += b;
    void* psSeen[256]; int nps = 0;
    for (long i = first; i < g_postN; i++) {
        DrawRec& r = g_post[i % POSTN];
        snprintf(b, sizeof b, "#%ld rt %p %ux%u ps %p vs %p prims %u from %p | blend %lu src %lu dst %lu op %lu cw 0x%lx z %lu srgbw %lu\n",
                 r.idx, r.rt, r.rtW, r.rtH, r.ps, r.vs, r.prims, r.caller, r.alpha, r.src, r.dst, r.op, r.cw, r.z, r.srgbw); o += b;
        for (int s = 0; s < 8; s++) if (r.tex[s].p) { snprintf(b, sizeof b, "    s%d %p %ux%u fmt %d type %u\n", s, r.tex[s].p, r.tex[s].w, r.tex[s].h, r.tex[s].fmt, r.tex[s].type); o += b; }
        for (int c = 0; c < 32; c++) { float* f = r.psc[c]; if (f[0] || f[1] || f[2] || f[3]) { snprintf(b, sizeof b, "    c%d %.5f %.5f %.5f %.5f\n", c, f[0], f[1], f[2], f[3]); o += b; } }
        bool seen = false; for (int k = 0; k < nps; k++) if (psSeen[k] == r.ps) seen = true;
        if (!seen && r.ps && nps < 256) {
            psSeen[nps++] = r.ps;
            UINT sz = 0; IDirect3DPixelShader9* ps = (IDirect3DPixelShader9*)r.ps;
            if (SUCCEEDED(ps->GetFunction(nullptr, &sz)) && sz && sz < (1u << 20)) {
                std::string code(sz, '\0');
                if (SUCCEEDED(ps->GetFunction(&code[0], &sz))) {
                    wchar_t path[MAX_PATH]; wcscpy(path, g_ini); wchar_t* sl = wcsrchr(path, L'\\');
                    swprintf(sl ? sl + 1 : path, 64, L"ddda_ps_%p.bin", r.ps);
                    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
                    DWORD w = 0; if (h != INVALID_HANDLE_VALUE) { WriteFile(h, code.data(), sz, &w, nullptr); CloseHandle(h); }
                }
            }
        }
    }
    wchar_t path[MAX_PATH]; wcscpy(path, g_ini); wchar_t* sl = wcsrchr(path, L'\\'); swprintf(sl ? sl + 1 : path, 64, L"ddda_post_%d.txt", nCap);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD w = 0; if (h != INVALID_HANDLE_VALUE) { WriteFile(h, o.data(), (DWORD)o.size(), &w, nullptr); CloseHandle(h); }
    Log("post: %ld draws, %d pixel shaders dumped, ddda_post_%d.txt %lu bytes", g_postN, nps, nCap, w);
    g_postN = 0;
}
typedef HRESULT(STDMETHODCALLTYPE* SetTex_t)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
typedef HRESULT(STDMETHODCALLTYPE* SetPS_t)(IDirect3DDevice9*, IDirect3DPixelShader9*);
typedef HRESULT(STDMETHODCALLTYPE* SetVS_t)(IDirect3DDevice9*, IDirect3DVertexShader9*);
typedef HRESULT(STDMETHODCALLTYPE* SetRS_t)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
static SetTex_t oSetTex; static SetPS_t oSetPS; static SetVS_t oSetVS; static SetRS_t oSetRS; static VSC_t oPSC;
static HRESULT STDMETHODCALLTYPE hkSetTex(IDirect3DDevice9* d, DWORD s, IDirect3DBaseTexture9* t) { HPScope _hp(HP_SetTex); if (s < 8) g_curTex[s] = t; return oSetTex(d, s, t); }
// build 056: the shader classes are looked up when a shader is set (about 4000 times a frame), not at every draw (9000+)
enum { VSF_PRIM = 1, VSF_PRIMSHIFT = 2, VSF_FLARE = 4, VSF_GUI = 8, PSF_GODRAYS = 1, PSF_DOF = 2, PSF_IMAGEPLANE = 4 };
static unsigned g_curVSF, g_curPSF;
static HRESULT STDMETHODCALLTYPE hkSetPS(IDirect3DDevice9* d, IDirect3DPixelShader9* p) { HPScope _hp(HP_SetPS); g_curPS = p;
    g_curPSF = p ? ((g_grN && IsGodRaysPS(p)) ? PSF_GODRAYS : 0) | ((g_dofN && IsDofPS(p)) ? PSF_DOF : 0) | ((g_ipN && IsImagePlanePS(p)) ? PSF_IMAGEPLANE : 0) : 0;
    return oSetPS(d, p); }
static HRESULT STDMETHODCALLTYPE hkSetVS(IDirect3DDevice9* d, IDirect3DVertexShader9* p) { HPScope _hp(HP_SetVS); g_curVS = p;
    extern bool IsGuiVS(void* p);
    g_curVSF = p ? (IsGuiVS(p) ? VSF_GUI : 0) | ((g_primN && IsPrimVS(p)) ? VSF_PRIM : 0) | ((g_primN && IsPrimShiftVS(p)) ? VSF_PRIMSHIFT : 0) | ((g_flareN && IsFlareVS(p)) ? VSF_FLARE : 0) : 0;
    return oSetVS(d, p); }
static HRESULT STDMETHODCALLTYPE hkSetRS(IDirect3DDevice9* d, D3DRENDERSTATETYPE s, DWORD v) { HPScope _hp(HP_SetRS);
    switch (s) {
    case D3DRS_ALPHABLENDENABLE: g_rsAlpha = v; break; case D3DRS_SRCBLEND: g_rsSrc = v; break; case D3DRS_DESTBLEND: g_rsDst = v; break;
    case D3DRS_BLENDOP: g_rsOp = v; break; case D3DRS_COLORWRITEENABLE: g_rsCW = v; break; case D3DRS_ZENABLE: g_rsZ = v; break;
    case D3DRS_SRGBWRITEENABLE: g_rsSrgbW = v; break; case D3DRS_SCISSORTESTENABLE: g_rsScissor = v; break;
    case D3DRS_SEPARATEALPHABLENDENABLE: g_rsSep = v; break; case D3DRS_SRCBLENDALPHA: g_rsSrcA = v; break;
    case D3DRS_DESTBLENDALPHA: g_rsDstA = v; break; case D3DRS_BLENDOPALPHA: g_rsOpA = v; break; default: break;
    }
    return oSetRS(d, s, v);
}
static HRESULT STDMETHODCALLTYPE hkPSC(IDirect3DDevice9* d, UINT r, const float* f, UINT n) { HPScope _hp(HP_PSC);
    if (g_replay != 1 && !g_diagOn && g_capState == 0) return oPSC(d, r, f, n);   // build 056: only the right-eye replay and the captures need it
    if (f && (g_diagOn || g_capState)) for (UINT i = 0; i < n && r + i < 32; i++) memcpy(g_psc[r + i], f + i * 4, 16);
    if (g_replay == 1 && f && n <= 256 && (g_diagOn || dxr::SyncMaybe(f, n, true))) {   // build 043: right eye - camera position / inverse matrices in pixel shaders
        float buf[1024]; memcpy(buf, f, n * 16);
        if (dxr::SyncPS(buf, n)) { for (UINT i = 0; i < n && r + i < 32; i++) memcpy(g_psc[r + i], buf + i * 4, 16); if (g_diagOn) DiagConst(true, r, f, buf, n); return oPSC(d, r, buf, n); }
    }
    if (g_diagOn && f) DiagConst(true, r, f, f, n);
    return oPSC(d, r, f, n);
}
typedef HRESULT(STDMETHODCALLTYPE* SetSS_t)(IDirect3DDevice9*, UINT, IDirect3DVertexBuffer9*, UINT, UINT);
static SetSS_t oSetSS;
static HRESULT STDMETHODCALLTYPE hkSetSS(IDirect3DDevice9* d, UINT n, IDirect3DVertexBuffer9* vb, UINT off, UINT st) { HPScope _hp(HP_SetSS); if (n == 0) { g_vb0 = vb; g_vbOff = off; g_vbStride = st; } return oSetSS(d, n, vb, off, st); }
static void RefreshStream(IDirect3DDevice9* d) {
    IDirect3DVertexBuffer9* vb = nullptr; UINT off = 0, st = 0;
    if (SUCCEEDED(d->GetStreamSource(0, &vb, &off, &st))) { g_vb0 = vb; g_vbOff = off; g_vbStride = st; if (vb) vb->Release(); }   // the game keeps its reference
}
static void RefreshTexState(IDirect3DDevice9* d) {
    for (DWORD i = 0; i < 8; i++) { IDirect3DBaseTexture9* t = nullptr; if (SUCCEEDED(d->GetTexture(i, &t))) { g_curTex[i] = t; if (t) t->Release(); } else g_curTex[i] = nullptr; }
    RefreshStream(d);
}
static void UiDump(int nCap) {   // the last 300 draws of the captured frame: one line each, with the vertex rectangle
    if (!g_post || !g_postN) return;
    std::string o; char b[1200];
    // build 080: the UI is drawn at the end of the left replay; dump the last 320 draws of the left replay (or of the single pass)
    long lastL = -1, first0 = g_postN > POSTN ? g_postN - POSTN : 0;
    for (long i = g_postN - 1; i >= first0; i--) { const DrawRec& r = g_post[i % POSTN]; if (r.rep <= 0) { lastL = i; break; } }
    if (lastL < 0) lastL = g_postN - 1;
    long first = lastL - 320 > first0 ? lastL - 320 : first0;
    for (long i = first; i <= lastL; i++) {
        DrawRec& r = g_post[i % POSTN];
        {   // build 030: every vertex shader of the dumped draws, for offline disassembly (ddda_vs_<ptr>.bin)
            static void* vdone[256]; static int nvd; bool vseen = false; for (int k = 0; k < nvd; k++) if (vdone[k] == r.vs) vseen = true;
            UINT vz = 0; IDirect3DVertexShader9* vsh = (IDirect3DVertexShader9*)r.vs;
            if (!vseen && vsh && nvd < 256 && SUCCEEDED(vsh->GetFunction(nullptr, &vz)) && vz && vz < 65536) {
                vdone[nvd++] = r.vs; std::string code(vz, '\0');
                if (SUCCEEDED(vsh->GetFunction(&code[0], &vz))) {
                    wchar_t pp[MAX_PATH]; wcscpy(pp, g_ini); wchar_t* q = wcsrchr(pp, L'\\'); swprintf(q ? q + 1 : pp, 64, L"ddda_vs_%p.bin", r.vs);
                    HANDLE h = CreateFileW(pp, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
                    DWORD w = 0; if (h != INVALID_HANDLE_VALUE) { WriteFile(h, code.data(), vz, &w, nullptr); CloseHandle(h); }
                }
            }
        }
        if (r.ui || r.rep >= 0) { char u[96]; snprintf(u, sizeof u, "%s%s", r.ui ? "UI " : "", r.rep == 0 ? "L " : r.rep == 1 ? "R " : ""); o += u; }
        int n = snprintf(b, sizeof b, "#%ld rt %ux%u ps %p vs %p from %p k%u pt%u prims %u nv %u st %u | bb %.1f %.1f %.1f %.1f | v0 %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f | blend %lu %lu %lu z %lu | t0 %p %ux%u f%d | vs c0 %.4f %.4f %.4f %.4f c1 %.4f %.4f %.4f %.4f c3 %.4f %.4f %.4f %.4f\n",
            r.idx, r.rtW, r.rtH, r.ps, r.vs, r.caller, r.kind, r.ptype, r.prims, r.nv, r.stride, r.bb[0], r.bb[1], r.bb[2], r.bb[3],
            r.v0[0], r.v0[1], r.v0[2], r.v0[3], r.v0[4], r.v0[5], r.v0[6], r.v0[7], r.alpha, r.src, r.dst, r.z, r.tex[0].p, r.tex[0].w, r.tex[0].h, r.tex[0].fmt,
            r.vsc[0][0], r.vsc[0][1], r.vsc[0][2], r.vsc[0][3], r.vsc[1][0], r.vsc[1][1], r.vsc[1][2], r.vsc[1][3], r.vsc[3][0], r.vsc[3][1], r.vsc[3][2], r.vsc[3][3]);
        o.append(b, n > 0 ? (size_t)n : 0);
        std::string c; for (int k = 0; k < 16; k++) { const float* v = r.vsc[k]; if (v[0] || v[1] || v[2] || v[3]) { char t[96]; snprintf(t, sizeof t, " c%d %.4g %.4g %.4g %.4g", k, v[0], v[1], v[2], v[3]); c += t; } }
        o += "    vs:" + c + "\n";
        { std::string q; for (int k = 0; k < 12; k++) { const float* v = r.psc[k]; if (v[0] || v[1] || v[2] || v[3]) { char t[96]; snprintf(t, sizeof t, " c%d %.4g %.4g %.4g %.4g", k, v[0], v[1], v[2], v[3]); q += t; } }
          if (!q.empty()) o += "    ps:" + q + "\n"; }
        if (r.ps) {   // build 030: every pixel shader of the dumped draws
            static void* done[256]; static int nd; bool seen = false; for (int k = 0; k < nd; k++) if (done[k] == r.ps) seen = true;
            UINT sz = 0; IDirect3DPixelShader9* ps = (IDirect3DPixelShader9*)r.ps;
            if (!seen && nd < 256 && SUCCEEDED(ps->GetFunction(nullptr, &sz)) && sz && sz < 65536) {
                done[nd++] = r.ps; std::string code(sz, '\0');
                if (SUCCEEDED(ps->GetFunction(&code[0], &sz))) {
                    wchar_t pp[MAX_PATH]; wcscpy(pp, g_ini); wchar_t* q = wcsrchr(pp, L'\\'); swprintf(q ? q + 1 : pp, 64, L"ddda_ps_%p.bin", r.ps);
                    HANDLE h = CreateFileW(pp, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
                    DWORD w = 0; if (h != INVALID_HANDLE_VALUE) { WriteFile(h, code.data(), sz, &w, nullptr); CloseHandle(h); }
                }
            }
        }
    }
    wchar_t path[MAX_PATH]; wcscpy(path, g_ini); wchar_t* sl = wcsrchr(path, L'\\'); swprintf(sl ? sl + 1 : path, 64, L"ddda_ui_%d.txt", nCap);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD w = 0; if (h != INVALID_HANDLE_VALUE) { WriteFile(h, o.data(), (DWORD)o.size(), &w, nullptr); CloseHandle(h); }
    Log("ui: ddda_ui_%d.txt written (%lu bytes, %ld draws in frame)", nCap, w, g_postN);
}
static bool MovieBar(D3DPRIMITIVETYPE t, UINT first, UINT prims) {   // a 1280 x <=64 rectangle in UI units = movie letterbox bar
    if (g_movieHold <= 0 || prims != 2 || !g_vb0 || g_vbStride < 8 || g_rtW < 1024 || g_rtW == 1280) return false;
    void* p = nullptr; bool bar = false;
    if (SUCCEEDED(g_vb0->Lock(g_vbOff + first * g_vbStride, 4 * g_vbStride, &p, D3DLOCK_READONLY)) && p) {
        float mnx = 1e30f, mxx = -1e30f, mny = 1e30f, mxy = -1e30f;
        for (int i = 0; i < 4; i++) { const float* v = (const float*)((uint8_t*)p + i * g_vbStride); mnx = fminf(mnx, v[0]); mxx = fmaxf(mxx, v[0]); mny = fminf(mny, v[1]); mxy = fmaxf(mxy, v[1]); }
        g_vb0->Unlock();
        bar = fabsf(mnx) < 1 && fabsf(mxx - 1280) < 1 && mxy - mny > 8 && mxy - mny <= 64;
        if (bar) LOG_FIRST(4, "movie: letterbox bar %.0f,%.0f-%.0f,%.0f skipped", mnx, mny, mxx, mxy);
    }
    return bar;
}
static inline bool UiPreDrawX(IDirect3DDevice9* d);
static inline bool UiPreDraw(IDirect3DDevice9* d) { bool u = UiPreDrawX(d); g_uiLast = u; return u; }   // build 080: remembered for the capture
static inline bool UiPreDrawX(IDirect3DDevice9* d) {
    if (!g_uiArmed || !g_curRT0) return false;
    if (g_replay == 1) {   // right-eye replay: the UI layer was already drawn in the left replay - skip UI draws
        if (!g_uiActive) { if (g_rtW != g_uiW || g_rtH != g_uiH) return false; g_uiScene = g_curRT0; g_uiActive = true; }
        if (g_curRT0 != g_uiScene) return false;
        g_uiSkip = true; return true;
    }
    if (!g_uiActive) {
        IDirect3DSurface9* u = dxr::UiSurface();
        if (!u) { g_uiArmed = false; return false; }
        if (u != g_uiSurf) { D3DSURFACE_DESC sd; u->GetDesc(&sd); g_uiW = sd.Width; g_uiH = sd.Height; g_uiSurf = u; }
        if (g_rtW != g_uiW || g_rtH != g_uiH) return false;
        g_uiScene = g_curRT0; g_uiActive = true; g_uiFrames++;
        oSetRT(d, 0, g_uiSurf);
        DWORD sc = FALSE; d->GetRenderState(D3DRS_SCISSORTESTENABLE, &sc); if (sc) oSetRS(d, D3DRS_SCISSORTESTENABLE, FALSE);
        oClear(d, 0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
        if (sc) oSetRS(d, D3DRS_SCISSORTESTENABLE, TRUE);
        if (CAPON) { CapFlushRun(); CapF("UI layer starts (scene target %p)", (void*)g_uiScene); }
    }
    if (g_curRT0 != g_uiScene) return false;
    // premultiplied alpha into the transparent layer: colour as the game blends it, alpha = coverage
    oSetRS(d, D3DRS_SEPARATEALPHABLENDENABLE, TRUE); oSetRS(d, D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    oSetRS(d, D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA); oSetRS(d, D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    g_uiDraws++;
    // build 031: F9 test modes for the "tiny dot": 1 = hide UI draws the game depth-tests (world markers), 2 = hide UI draws
    // anchored at a world depth (GUI c4.z != 0)
    g_uiSkip = false;
    if (g_uiHideMode == 1) g_uiSkip = g_rsZ != 0;
    else if (g_uiHideMode == 2) g_uiSkip = g_vsShadow[4][2] != 0.0f && g_vsShadow[4][3] == 1.0f;
    if (g_rsZ) oSetRS(d, D3DRS_ZENABLE, FALSE);   // world name bars test depth, but the depth buffer is the head-tracked view's
    g_uiClipped = false;
    if (g_uiClip && oSetScF && g_uiW && g_uiH > g_uiW * 9 / 16) {
        LONG top = (LONG)(g_uiH - g_uiW * 9 / 16) / 2; RECT r = { 0, top, (LONG)g_uiW, (LONG)g_uiH - top };
        if (g_rsScissor) { r.left = std::max(r.left, g_scGame.left); r.top = std::max(r.top, g_scGame.top); r.right = std::min(r.right, g_scGame.right); r.bottom = std::min(r.bottom, g_scGame.bottom); if (r.right < r.left) r.right = r.left; if (r.bottom < r.top) r.bottom = r.top; }
        oSetScF(d, &r); if (!g_rsScissor) oSetRS(d, D3DRS_SCISSORTESTENABLE, TRUE); g_uiClipped = true;
    }
    return true;
}
// build 034: full-resolution reads for the dot hunt: the centre 1536x1632 of the back buffer (ddda_full_N.bmp) and the UI layer's
// alpha, max-pooled 4x4 so a tiny dot survives (ddda_uialpha_N.bmp: grey = alpha, red = alpha with near-black colour)
static void WriteBmp(const wchar_t* name, int n, UINT W, UINT H, const std::vector<uint8_t>& rgb) {
    std::string f(54 + W * H * 3, '\0'); uint8_t* p = (uint8_t*)&f[0];
    p[0] = 'B'; p[1] = 'M'; *(uint32_t*)(p + 2) = (uint32_t)f.size(); *(uint32_t*)(p + 10) = 54; *(uint32_t*)(p + 14) = 40;
    *(int32_t*)(p + 18) = W; *(int32_t*)(p + 22) = -(int32_t)H; *(uint16_t*)(p + 26) = 1; *(uint16_t*)(p + 28) = 24; *(uint32_t*)(p + 34) = W * H * 3;
    memcpy(p + 54, rgb.data(), (size_t)W * H * 3);
    wchar_t path[MAX_PATH]; wcscpy(path, g_ini); wchar_t* sl = wcsrchr(path, L'\\'); swprintf(sl ? sl + 1 : path, 64, name, n);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD w = 0; if (h != INVALID_HANDLE_VALUE) { WriteFile(h, f.data(), (DWORD)f.size(), &w, nullptr); CloseHandle(h); }
    Log("capture: %ls (%lu bytes)", sl ? sl + 1 : path, w);
}
static bool ReadRT(IDirect3DDevice9* d, IDirect3DSurface9* src, std::vector<uint32_t>& out, UINT& W, UINT& H) {
    D3DSURFACE_DESC sd; if (!src || FAILED(src->GetDesc(&sd))) return false;
    if (sd.Format != D3DFMT_A8R8G8B8 && sd.Format != D3DFMT_X8R8G8B8) { Log("capture: surface format %d not read", (int)sd.Format); return false; }
    IDirect3DSurface9* sm = nullptr; bool ok = false;
    if (SUCCEEDED(d->CreateOffscreenPlainSurface(sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &sm, nullptr)) && SUCCEEDED(d->GetRenderTargetData(src, sm))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sm->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            W = sd.Width; H = sd.Height; out.resize((size_t)W * H);
            for (UINT y = 0; y < H; y++) memcpy(&out[(size_t)y * W], (const uint8_t*)lr.pBits + (size_t)y * lr.Pitch, W * 4);
            sm->UnlockRect(); ok = true;
        }
    } else Log("capture: GetRenderTargetData failed");
    if (sm) sm->Release(); return ok;
}
static void FullShots(IDirect3DDevice9* d) {
    std::vector<uint32_t> px; UINT W = 0, H = 0;
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        if (ReadRT(d, bb, px, W, H) && W >= 4 && H >= 4) {
            UINT cw = W / 2, ch = H / 2, x0 = W / 4, y0 = H / 4; std::vector<uint8_t> rgb((size_t)cw * ch * 3);
            for (UINT y = 0; y < ch; y++) for (UINT x = 0; x < cw; x++) { uint32_t c = px[(size_t)(y0 + y) * W + x0 + x]; uint8_t* o = &rgb[((size_t)y * cw + x) * 3]; o[0] = c & 255; o[1] = (c >> 8) & 255; o[2] = (c >> 16) & 255; }
            WriteBmp(L"ddda_full_%d.bmp", g_shotN, cw, ch, rgb);
        }
        bb->Release();
    }
    if (g_uiActive && g_uiSurf && ReadRT(d, g_uiSurf, px, W, H) && W >= 4 && H >= 4) {
        UINT ow = W / 4, oh = H / 4; std::vector<uint8_t> rgb((size_t)ow * oh * 3);
        for (UINT y = 0; y < oh; y++) for (UINT x = 0; x < ow; x++) {
            uint8_t am = 0, dark = 0;
            for (UINT j = 0; j < 4; j++) for (UINT i = 0; i < 4; i++) {
                uint32_t c = px[(size_t)(y * 4 + j) * W + x * 4 + i]; uint8_t a = c >> 24;
                uint32_t lum = (c & 255) + ((c >> 8) & 255) + ((c >> 16) & 255);
                if (a > am) am = a; if (a > 8 && lum * 4 < (uint32_t)a && a > dark) dark = a;
            }
            uint8_t* o = &rgb[((size_t)y * ow + x) * 3]; o[0] = dark ? 0 : am; o[1] = dark ? 0 : am; o[2] = dark ? (uint8_t)std::max<int>(dark, 128) : am;
        }
        WriteBmp(L"ddda_uialpha_%d.bmp", g_shotN, ow, oh, rgb);
    }
}
// build 042: between the two replays: close the UI layer of the left replay (its picture stays in the UI surface)
static void UiBetweenReplays(IDirect3DDevice9* d) {
    if (g_uiActive) { oSetRS(d, D3DRS_SEPARATEALPHABLENDENABLE, g_rsSep); oSetRS(d, D3DRS_SRCBLENDALPHA, g_rsSrcA); oSetRS(d, D3DRS_DESTBLENDALPHA, g_rsDstA); oSetRS(d, D3DRS_BLENDOPALPHA, g_rsOpA); }
    g_uiArmed = g_uiActive = false; g_uiScene = nullptr;
}
// ---- build 048: replay diagnostics (only while the F8 stereo recorder runs) ----
// For every draw of replay 0 the device constants (VS c0..63, PS c0..31: as uploaded by the game = raw, and as sent = act)
// and shaders are kept; replay 1 compares draw k with draw k of replay 0. Per frame: list of register-difference masks,
// draws whose shaders or raw constants differ (= the command list changed between the replays), and reads of non-eye
// render targets (e.g. shadow maps) whose content changed later in replay 0 (replay 1 then reads the final content).
static inline bool EyeSized(UINT w, UINT h) { if (!g_uiW) return true; for (int k = 0; k < 6; k++) if (w == (g_uiW >> k) && h == (g_uiH >> k)) return true; return false; }
struct DiagRec { void* vs; void* ps; float rv[64][4], rp[32][4], av[64][4], ap[32][4]; };
static DiagRec* g_dRec; static const int DMAX = 8000; static int g_dN[2];
static float g_dRV[64][4], g_dRP[32][4], g_dAV[64][4], g_dAP[32][4];
struct DiagRT_t { IDirect3DBaseTexture9* tex; UINT w, h; int fmt; int gen; int reads; int stale; };
static DiagRT_t g_dRT[96]; static int g_dRTn;
struct DiagRead { short e; short gen; }; static DiagRead g_dReads[65536]; static int g_dReadN;
struct DiagHist { uint64_t mv; uint32_t mp; uint8_t kind; int n; int first; }; static DiagHist g_dH[96]; static int g_dHn;
static int g_dDiverge, g_dDivFirst, g_dRawDiff, g_dRawFirst; static uint64_t g_dRawMv; static uint32_t g_dRawMp;
static HANDLE g_dFile = INVALID_HANDLE_VALUE; static int g_dFrame; static int g_dDump[2];
static void DiagF(const char* fmt, ...) { if (g_dFile == INVALID_HANDLE_VALUE) return; char b[2048]; va_list a; va_start(a, fmt); int n = vsnprintf(b, sizeof b - 2, fmt, a); va_end(a); if (n < 0) return; if (n > (int)sizeof b - 2) n = sizeof b - 2; b[n++] = '\n'; DWORD w; WriteFile(g_dFile, b, n, &w, nullptr); }
static void DiagConst(bool ps, UINT r, const float* raw, const float* act, UINT n) {
    UINT lim = ps ? 32 : 64;
    for (UINT i = 0; i < n && r + i < lim; i++) { memcpy(ps ? g_dRP[r + i] : g_dRV[r + i], raw + i * 4, 16); memcpy(ps ? g_dAP[r + i] : g_dAV[r + i], act + i * 4, 16); }
}
static int DiagTexEntry(IDirect3DBaseTexture9* t) { for (int i = 0; i < g_dRTn; i++) if (g_dRT[i].tex == t) return i; return -1; }
static void DiagRT(IDirect3DSurface9* s) {
    if (g_replay != 0) return;
    IDirect3DTexture9* t = nullptr; if (FAILED(s->GetContainer(__uuidof(IDirect3DTexture9), (void**)&t)) || !t) return;
    int e = DiagTexEntry(t);
    if (e < 0 && g_dRTn < 96) { D3DSURFACE_DESC sd; s->GetDesc(&sd); e = g_dRTn++; g_dRT[e] = { t, sd.Width, sd.Height, (int)sd.Format, 0, 0, 0 }; }
    if (e >= 0) g_dRT[e].gen++;
    t->Release();
}
static void DiagDraw() {
    int rp = g_replay; if (rp < 0 || !g_dRec) return;
    int k = g_dN[rp]++; bool exec = EyeSized(g_rtW, g_rtH);
    // build 051: values of the first eye-target draws that sample the shadow map (the resolve pass), both replays
    if (exec && g_shadowRT && g_dDump[rp] < 2) {
        static IDirect3DBaseTexture9* st; static IDirect3DSurface9* sr;
        if (sr != g_shadowRT) { sr = g_shadowRT; IDirect3DTexture9* t = nullptr; if (SUCCEEDED(g_shadowRT->GetContainer(__uuidof(IDirect3DTexture9), (void**)&t)) && t) { st = t; t->Release(); } }
        bool b = false; for (int i = 0; i < 8; i++) if (st && g_curTex[i] == st) b = true;
        if (b) {
            g_dDump[rp]++; float cl[3] = {}, cr[3] = {}; dxr::SyncDebugCam(cl, cr);
            DiagF("  R%d draw %d vs %p ps %p camL %.3f %.3f %.3f camR %.3f %.3f %.3f", rp, k, (void*)g_curVS, (void*)g_curPS, cl[0], cl[1], cl[2], cr[0], cr[1], cr[2]);
            for (int i = 16; i < 32; i++) DiagF("    vs c%d raw %.4f %.4f %.4f %.4f | act %.4f %.4f %.4f %.4f", i, g_dRV[i][0], g_dRV[i][1], g_dRV[i][2], g_dRV[i][3], g_dAV[i][0], g_dAV[i][1], g_dAV[i][2], g_dAV[i][3]);
            for (int i = 0; i < 32; i++) DiagF("    ps c%d raw %.4f %.4f %.4f %.4f | act %.4f %.4f %.4f %.4f", i, g_dRP[i][0], g_dRP[i][1], g_dRP[i][2], g_dRP[i][3], g_dAP[i][0], g_dAP[i][1], g_dAP[i][2], g_dAP[i][3]);
        }
    }
    if (rp == 0) {
        if (k < DMAX) { DiagRec& d = g_dRec[k]; d.vs = g_curVS; d.ps = g_curPS; memcpy(d.rv, g_dRV, sizeof g_dRV); memcpy(d.rp, g_dRP, sizeof g_dRP); memcpy(d.av, g_dAV, sizeof g_dAV); memcpy(d.ap, g_dAP, sizeof g_dAP); }
        if (exec) for (int i = 0; i < 8; i++) if (g_curTex[i]) {
            int e = DiagTexEntry(g_curTex[i]); if (e < 0 || EyeSized(g_dRT[e].w, g_dRT[e].h)) continue;
            if (g_dReadN < 65536) g_dReads[g_dReadN++] = { (short)e, (short)g_dRT[e].gen };
        }
        return;
    }
    if (k >= DMAX || k >= g_dN[0]) return;
    DiagRec& d = g_dRec[k];
    if (d.vs != g_curVS || d.ps != g_curPS) { if (!g_dDiverge++) g_dDivFirst = k; return; }
    uint64_t rv = 0, av = 0; uint32_t rpm = 0, apm = 0;
    for (int i = 0; i < 64; i++) { if (memcmp(d.rv[i], g_dRV[i], 16)) rv |= 1ull << i; if (memcmp(d.av[i], g_dAV[i], 16)) av |= 1ull << i; }
    for (int i = 0; i < 32; i++) { if (memcmp(d.rp[i], g_dRP[i], 16)) rpm |= 1u << i; if (memcmp(d.ap[i], g_dAP[i], 16)) apm |= 1u << i; }
    if ((rv || rpm) && exec) { if (!g_dRawDiff++) { g_dRawFirst = k; g_dRawMv = rv; g_dRawMp = rpm; } }
    bool shadowTex = false; for (int i = 0; i < 8; i++) if (g_curTex[i]) { int e = DiagTexEntry(g_curTex[i]); if (e >= 0 && g_dRT[e].w == 2048 && g_dRT[e].h == 6144) shadowTex = true; }
    uint8_t kind = (exec ? 1 : 0) | (shadowTex ? 2 : 0);
    if (!exec) return;
    for (int i = 0; i < g_dHn; i++) if (g_dH[i].mv == av && g_dH[i].mp == apm && g_dH[i].kind == kind) { g_dH[i].n++; return; }
    if (g_dHn < 96) g_dH[g_dHn++] = { av, apm, kind, 1, k };
}
static void DiagReplayStart(void* self, int rp, int info[3]) {
    uint8_t* r = (uint8_t*)self; int idx = *(int*)(r + 0x33db8) & 1;
    info[0] = *(int*)(r + 0x33db8); info[1] = *(int*)(r + 0x433dd8 + idx * 4); info[2] = *(int*)(r + 0x433de0 + idx * 4);
    g_dN[rp] = 0; g_dDump[rp] = 0;
    if (rp == 0) { for (int i = 0; i < g_dRTn; i++) { g_dRT[i].gen = 0; g_dRT[i].reads = g_dRT[i].stale = 0; } g_dReadN = 0; g_dHn = 0; g_dDiverge = g_dRawDiff = 0; }
}
static void DiagFrameEnd(int i0[3], int i1[3]) {
    for (int i = 0; i < g_dReadN; i++) { DiagRT_t& e = g_dRT[g_dReads[i].e]; e.reads++; if (g_dReads[i].gen != e.gen) e.stale++; }
    DiagF("F %d  list0 idx %d ptr %08x n %d | list1 idx %d ptr %08x n %d | draws %d / %d | diverge %d (first %d) | rawdiff %d (first %d vs %016llx ps %08x)",
          g_dFrame, i0[0], i0[1], i0[2], i1[0], i1[1], i1[2], g_dN[0], g_dN[1], g_dDiverge, g_dDivFirst, g_dRawDiff, g_dRawFirst, (unsigned long long)g_dRawMv, g_dRawMp);
    for (int i = 0; i < g_dRTn; i++) if (g_dRT[i].reads || g_dRT[i].gen) DiagF("   rt %ux%u fmt %d writes %d reads %d stale %d", g_dRT[i].w, g_dRT[i].h, g_dRT[i].fmt, g_dRT[i].gen, g_dRT[i].reads, g_dRT[i].stale);
    for (int i = 0; i < g_dHn; i++) DiagF("   mask vs %016llx ps %08x %s%s draws %d (first %d)", (unsigned long long)g_dH[i].mv, g_dH[i].mp, g_dH[i].kind & 1 ? "eye" : "off", g_dH[i].kind & 2 ? " shadowtex" : "", g_dH[i].n, g_dH[i].first);
    g_dFrame++;
}
// build 046: stereo recorder - F8 also records both eyes for 2 s (180 frames), side by side at 512x544 per eye, into
// ddda_stereo_N.raw (raw BGRA frames 1024x544, no header) for offline review of right-eye-only problems.
static int g_recLeft; static int g_recN; static bool g_recPending; static HANDLE g_recFile = INVALID_HANDLE_VALUE;
static IDirect3DSurface9 *g_recRT, *g_recSys; static const UINT RW = 768, RH = 408;   // build 048: smaller, BGR, 30 frames per file
static void RecGrab(IDirect3DDevice9* d, int eye) {   // back buffer -> half of the recording target
    if (g_recLeft <= 0) return;
    if (!g_recRT && FAILED(d->CreateRenderTarget(RW, RH, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &g_recRT, nullptr))) { g_recLeft = 0; return; }
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) { RECT r = { eye ? (LONG)RW / 2 : 0, 0, eye ? (LONG)RW : (LONG)RW / 2, (LONG)RH }; d->StretchRect(bb, nullptr, g_recRT, &r, D3DTEXF_LINEAR); bb->Release(); }
}
static void RecFrame(IDirect3DDevice9* d) {   // at Present: right eye, then read back and append
    if (g_recPending) { g_recPending = false; g_recN++; g_recLeft = 180; return; }   // starts with the next frame (diagnostics aligned)
    if (g_recLeft <= 0) return;
    if (!dxr::SyncActive()) RecGrab(d, 0);   // mono: same picture both halves
    RecGrab(d, 1);
    if (!g_recSys && FAILED(d->CreateOffscreenPlainSurface(RW, RH, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &g_recSys, nullptr))) { g_recLeft = 0; return; }
    int fi = 180 - g_recLeft;   // frame index in this recording
    if (g_recFile == INVALID_HANDLE_VALUE) {
        wchar_t path[MAX_PATH]; wcscpy(path, g_ini); wchar_t* sl = wcsrchr(path, L'\\'); swprintf(sl ? sl + 1 : path, 64, L"ddda_stereo_%d_%d.raw", g_recN, fi / 30);
        g_recFile = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (fi == 0) Log("capture: stereo recording ddda_stereo_%d_0..5.raw (%ux%u BGR per frame, left | right, 30 frames per file) + ddda_stereo_%d.txt", g_recN, RW, RH, g_recN);
    }
    D3DLOCKED_RECT lr; DWORD w;
    if (g_recRT && SUCCEEDED(d->GetRenderTargetData(g_recRT, g_recSys)) && SUCCEEDED(g_recSys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        static uint8_t line[RW * 3];
        for (UINT y = 0; y < RH; y++) { const uint8_t* src = (const uint8_t*)lr.pBits + (size_t)y * lr.Pitch; for (UINT x = 0; x < RW; x++) { line[x * 3] = src[x * 4]; line[x * 3 + 1] = src[x * 4 + 1]; line[x * 3 + 2] = src[x * 4 + 2]; } WriteFile(g_recFile, line, RW * 3, &w, nullptr); }
        g_recSys->UnlockRect();
    }
    if ((fi + 1) % 30 == 0 || g_recLeft == 1) { CloseHandle(g_recFile); g_recFile = INVALID_HANDLE_VALUE; }
    if (--g_recLeft == 0) {
        if (g_dFile != INVALID_HANDLE_VALUE) { CloseHandle(g_dFile); g_dFile = INVALID_HANDLE_VALUE; }
        Log("capture: stereo recording done");
    }
}
static void RecStart() { if (g_recLeft <= 0) g_recPending = true; }
typedef void(__fastcall* Draw_t)(void* self, void* edx);
static Draw_t oDraw;
static void DepthEncode(IDirect3DDevice9* d, int eye);   // build 075
static void __fastcall hkDraw(void* self, void* edx) {   // BbsRpg 0xdab480: the render thread replays the frame's command list
    if (!dxr::SyncActive()) {   // build 058: time the game's command player in flat / mono too, to compare with the VR replays
        static double ts; static long dn, n; double a = NowMs(); long d = g_fDraws;
        oDraw(self, edx);
        ts += NowMs() - a; dn += g_fDraws - d;
        if (++n >= 450) { Log("render thread: %.2f ms (%ld draws) per frame - single pass", ts / n, dn / n); ts = 0; dn = n = 0; }
        return;
    }
    IDirect3DDevice9* dev = *(IDirect3DDevice9**)((uint8_t*)self + 0x98);
    static double t0s, t1s; static long dr0, dr1, nf;
    double a0 = NowMs(); long d0 = g_fDraws;
    bool sh = false;
    if (g_shadowSync && g_shadowRT) {
        D3DSURFACE_DESC sd; g_shadowRT->GetDesc(&sd);
        if (!g_shBefore) { dev->CreateRenderTarget(sd.Width, sd.Height, sd.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_shBefore, nullptr); dev->CreateRenderTarget(sd.Width, sd.Height, sd.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_shAfter, nullptr);
                          Log("stereo: shadow map copies %s (%ux%u format %d)", g_shBefore && g_shAfter ? "created" : "FAILED", sd.Width, sd.Height, (int)sd.Format); }
        sh = g_shBefore && g_shAfter && SUCCEEDED(dev->StretchRect(g_shadowRT, nullptr, g_shBefore, nullptr, D3DTEXF_NONE));
    }
    g_shadowUsed = false;
    int i0[3], i1[3];
    g_diagOn = g_recLeft > 0;
    if (g_diagOn) {
        if (!g_dRec) g_dRec = (DiagRec*)VirtualAlloc(nullptr, sizeof(DiagRec) * DMAX, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (g_dFile == INVALID_HANDLE_VALUE) {
            wchar_t path[MAX_PATH]; wcscpy(path, g_ini); wchar_t* sl = wcsrchr(path, L'\\'); swprintf(sl ? sl + 1 : path, 64, L"ddda_stereo_%d.txt", g_recN);
            g_dFile = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr); g_dFrame = 0;
        }
        DiagReplayStart(self, 0, i0);
    }
    GpuReplayMark(dev, 0);
    dxr::SyncSetEye(0); g_replay = 0; oDraw(self, edx);
    DepthEncode(dev, 0);
    bool shRestore = sh && g_shadowUsed;
    if (shRestore) { dev->StretchRect(g_shadowRT, nullptr, g_shAfter, nullptr, D3DTEXF_NONE); dev->StretchRect(g_shBefore, nullptr, g_shadowRT, nullptr, D3DTEXF_NONE); }
    double a1 = NowMs(); long d1 = g_fDraws;
    dxr::SyncLeftDone(dev); UiBetweenReplays(dev); RecGrab(dev, 0);
    if (g_diagOn) DiagReplayStart(self, 1, i1);
    GpuReplayMark(dev, 1);
    dxr::SyncSetEye(1); g_replay = 1; oDraw(self, edx);
    DepthEncode(dev, 1);
    GpuReplayMark(dev, 2);
    if (g_diagOn) { DiagFrameEnd(i0, i1); g_diagOn = false; }
    double a2 = NowMs(); long d2 = g_fDraws;
    if (shRestore) dev->StretchRect(g_shAfter, nullptr, g_shadowRT, nullptr, D3DTEXF_NONE);
    g_replay = -1; dxr::SyncSetEye(-1);
    t0s += a1 - a0; t1s += a2 - a1; dr0 += d1 - d0; dr1 += d2 - d1; nf++;
    if (nf >= 450) {   // every ~5 s: CPU time of the two replays (render thread) and their draw counts
        Log("stereo: render thread left eye %.2f ms (%ld draws) | right eye %.2f ms per frame", t0s / nf, dr0 / nf, t1s / nf);
        t0s = t1s = 0; dr0 = dr1 = 0; nf = 0;
    }
}
static void UiShot(IDirect3DDevice9* d) { if (g_uiActive && g_uiSurf) Screenshot(d, g_uiSurf, L"ddda_uishot_%d.bmp"); FullShots(d); }
static void UiEndFrame(IDirect3DDevice9* d) {   // at Present
    if (g_mapSeen != g_mapActive) Log("overlay: map screen %s - screen-overlay hiding %s", g_mapSeen ? "open" : "closed", g_mapSeen ? "paused" : "resumed");
    g_mapActive = g_mapSeen; g_mapSeen = false;
    if (g_talkActive && !g_talkSeen) Log("ui: NPC conversation box closed");
    g_talkActive = g_talkSeen; g_talkSeen = false;   // build 081
    memcpy(g_wPrev, g_wCur, sizeof g_wCur); g_wPrevN = g_wCurN; g_wCurN = 0;   // build 088
    memcpy(g_gPrev, g_gCur, sizeof g_gCur); g_gPrevN = g_gCurN; g_gCurN = 0;   // build 091
    // build 103: the test keys (End study log, Home world-marker toggle, Ctrl+PageUp/Down draw distance) were removed;
    // world markers and DrawDistance come from the ini (launcher)
    g_wlogFrame++;   // build 104: frame counter for the world-marker groups (093) - it lived in the removed End-key block
    dxr::SetUiFrame(g_uiActive);
    if (g_movieSeen) g_movieHold = 30; else if (g_movieHold > 0) g_movieHold--;
    dxr::SetMovieFrame(g_movieHold > 0); g_movieSeen = false;
    if (g_uiActive) { oSetRS(d, D3DRS_SEPARATEALPHABLENDENABLE, g_rsSep); oSetRS(d, D3DRS_SRCBLENDALPHA, g_rsSrcA); oSetRS(d, D3DRS_DESTBLENDALPHA, g_rsDstA); oSetRS(d, D3DRS_BLENDOPALPHA, g_rsOpA); }
    g_uiArmed = g_uiActive = false; g_uiScene = nullptr;
}
#define POSTREC(p) do { if (CAPON) PostRec(RA, (p)); } while (0)

// replay 1 executes only draws into the eye's own targets (scene resolution and its 1/2 .. 1/32 chain)
static inline bool R1Skip() {
    if (g_replay != 1 || !g_uiW) return false;
    for (int k = 0; k < 6; k++) if (g_rtW == (g_uiW >> k) && g_rtH == (g_uiH >> k)) return false;   // build 045: also 1/16, 1/32 (192x204, 96x102)
    g_r1Skipped++; return true;
}
#define OVERLAY_SKIP() do { if (R1Skip()) return D3D_OK; \
    if (g_hideFlare && (g_curVSF & VSF_FLARE)) { if (g_flareSkipped++ < 2) Log("flare: lens flare draw skipped"); return D3D_OK; } \
    if (g_killGodRays && (g_curPSF & PSF_GODRAYS)) return D3D_OK; \
    if ((g_curPSF & PSF_DOF) && g_killDof && g_killBand && g_fullRect) { if (g_dofSkipped++ < 2) Log("dof: depth-of-field composite skipped (its band input is killed)"); return D3D_OK; } \
    if ((g_curPSF & PSF_IMAGEPLANE) && g_hideOverlay) { if (g_ipSkipped++ < 3) Log("overlay: image-plane draw skipped (tex %p)", (void*)g_curTex[0]); return D3D_OK; } } while (0)
static inline void UiPost(IDirect3DDevice9* d, bool ui) {
    if (!ui) return;
    if (g_rsZ) oSetRS(d, D3DRS_ZENABLE, g_rsZ);
    if (g_uiClipped) { oSetScF(d, &g_scGame); if (!g_rsScissor) oSetRS(d, D3DRS_SCISSORTESTENABLE, FALSE); g_uiClipped = false; }
}
static bool WorldGuiNow();
#define UI_SKIP() do { if (ui && g_uiSkip && !(g_replay == 1 && WorldGuiNow())) { UiPost(d, ui); PrimPost(d); return D3D_OK; } } while (0)
// build 032: bounds of the vertices an indexed draw really uses (pixel-space prims: x, y are the first two floats)
static long g_dbCalls, g_shHit, g_shMiss; static double g_dbMs;   // prim bounds check counters
// build 064: CPU shadow copies of the game's DYNAMIC vertex/index buffers, filled at the game's own Unlock, so the prim bounds
// check reads normal memory instead of Lock()ing a buffer the GPU may be using (each such Lock stalled ~0.3 ms: ~4 ms a frame)
struct BufShadow { std::vector<uint8_t> data; UINT lockOff = 0, lockSize = 0; void* lockPtr = nullptr; bool locked = false; D3DFORMAT fmt = D3DFMT_UNKNOWN;
    CRITICAL_SECTION cs; BufShadow() { InitializeCriticalSectionAndSpinCount(&cs, 4000); } };
// build 066: the map is read under a shared SRW lock (writes only at buffer creation / first snapshot); each shadow has its own
// lock, so the game's other threads copying into their buffers never block the render thread's bounds checks
static std::unordered_map<void*, BufShadow*> g_shadow; static SRWLOCK g_shMap = SRWLOCK_INIT; static size_t g_shBytes; static long g_shCount;
static int g_useShadow = 1;
static BufShadow* ShGet(void* b) { AcquireSRWLockShared(&g_shMap); auto it = g_shadow.find(b); BufShadow* r = it == g_shadow.end() ? nullptr : it->second; ReleaseSRWLockShared(&g_shMap); return r; }
static void ShRegister(void* b, UINT len, bool want, D3DFORMAT fmt) {
    AcquireSRWLockExclusive(&g_shMap);
    auto it = g_shadow.find(b);
    if (it != g_shadow.end()) { g_shBytes -= it->second->data.size(); g_shadow.erase(it); g_shCount--; }   // old shadow leaked on purpose (another thread may hold it)
    if (want && len <= (16u << 20) && g_shBytes + len <= (256u << 20)) {
        BufShadow* sh = new BufShadow(); sh->data.assign(len, 0); sh->fmt = fmt; g_shadow[b] = sh; g_shBytes += len; g_shCount++;
        LOG_FIRST(8, "shadow: dynamic %s buffer %p, %u bytes (total %ld buffers, %.1f MB)", fmt == D3DFMT_UNKNOWN ? "vertex" : "index", b, len, g_shCount, g_shBytes / 1048576.0);
    }
    ReleaseSRWLockExclusive(&g_shMap);
}
// build 072: the game writes straight into our shadow (normal memory) and the bytes go to the real buffer at Unlock.
// Reading the written bytes back from the lock pointer (build 064-071) is cheap on Windows' D3D9 but very slow on DXVK,
// whose dynamic buffers are mapped GPU memory (uncached reads: ~30 ms a frame).
static void* ShLocked(void* b, UINT off, UINT size, void* ptr, DWORD flags) {
    if ((flags & D3DLOCK_READONLY) || !ptr) return ptr;
    BufShadow* sh = ShGet(b); if (!sh) return ptr;
    void* give = ptr;
    EnterCriticalSection(&sh->cs);
    UINT n = (UINT)sh->data.size();
    if (off < n && !sh->locked) {
        sh->locked = true; sh->lockOff = off; sh->lockSize = size ? std::min(size, n - off) : n - off; sh->lockPtr = ptr;
        give = sh->data.data() + off;
    }
    LeaveCriticalSection(&sh->cs);
    return give;
}
static void ShUnlocking(void* b) {
    BufShadow* sh = ShGet(b); if (!sh) return;
    EnterCriticalSection(&sh->cs);
    if (sh->locked) { memcpy(sh->lockPtr, sh->data.data() + sh->lockOff, sh->lockSize); sh->locked = false; }
    LeaveCriticalSection(&sh->cs);
}
// a buffer used by a prim draw that has no shadow yet: read it once (one stall), later writes keep it current
static long g_shSnaps;
static BufShadow* ShSnapshot(void* b, bool vb) {
    if (!b) return nullptr; if (BufShadow* e = ShGet(b)) return e;
    UINT len = 0; D3DFORMAT fmt = D3DFMT_UNKNOWN;
    DWORD usage = 0, pool = 0;
    if (vb) { D3DVERTEXBUFFER_DESC dd; if (FAILED(((IDirect3DVertexBuffer9*)b)->GetDesc(&dd))) return nullptr; len = dd.Size; usage = dd.Usage; pool = dd.Pool; }
    else { D3DINDEXBUFFER_DESC dd; if (FAILED(((IDirect3DIndexBuffer9*)b)->GetDesc(&dd))) return nullptr; len = dd.Size; fmt = dd.Format == D3DFMT_INDEX32 ? D3DFMT_INDEX32 : D3DFMT_INDEX16; usage = dd.Usage; pool = dd.Pool; }
    static void* failed[64]; static int nf;
    for (int i = 0; i < nf; i++) if (failed[i] == b) return nullptr;
    if (!len || len > (16u << 20) || g_shBytes + len > (256u << 20)) {
        if (nf < 64) failed[nf++] = b;
        LOG_FIRST(8, "shadow: %s buffer %p (%u bytes, usage 0x%lx, pool %lu) too big to snapshot", vb ? "vertex" : "index", b, len, (unsigned long)usage, (unsigned long)pool);
        return nullptr;
    }
    void* p = nullptr;
    HRESULT hr = vb ? ((IDirect3DVertexBuffer9*)b)->Lock(0, 0, &p, D3DLOCK_READONLY) : ((IDirect3DIndexBuffer9*)b)->Lock(0, 0, &p, D3DLOCK_READONLY);
    if (FAILED(hr) || !p) { if (nf < 64) failed[nf++] = b; return nullptr; }
    BufShadow* sh = new BufShadow(); sh->data.assign((const uint8_t*)p, (const uint8_t*)p + len); sh->fmt = fmt;
    if (vb) ((IDirect3DVertexBuffer9*)b)->Unlock(); else ((IDirect3DIndexBuffer9*)b)->Unlock();
    AcquireSRWLockExclusive(&g_shMap);
    if (g_shadow.find(b) == g_shadow.end()) { g_shadow[b] = sh; g_shBytes += len; g_shCount++; g_shSnaps++; } else { BufShadow* e = g_shadow[b]; ReleaseSRWLockExclusive(&g_shMap); delete sh; return e; }
    ReleaseSRWLockExclusive(&g_shMap);
    LOG_FIRST(16, "shadow: %s buffer %p snapshotted for prim bounds, %u bytes (usage 0x%lx, pool %lu)", vb ? "vertex" : "index", b, len, (unsigned long)usage, (unsigned long)pool);
    return sh;
}
// bounds from the shadows; false = no shadow for these buffers (caller falls back to the old Lock path)
static bool ShBounds(IDirect3DIndexBuffer9* ib, D3DPRIMITIVETYPE t, INT base, UINT minIdx, UINT numV, UINT si, UINT ni, float bb[4], bool* found) {
    *found = false; if (!g_useShadow) return false;
    BufShadow* si_ = ShSnapshot(ib, false); BufShadow* sv = ShSnapshot(g_vb0, true);
    if (!si_ || !sv) return false;
    *found = true; bool ok = false;
    EnterCriticalSection(&si_->cs); EnterCriticalSection(&sv->cs);
    UINT isz = si_->fmt == D3DFMT_INDEX32 ? 4 : 2; size_t ib0 = (size_t)si * isz, vb0 = (size_t)g_vbOff + (size_t)(base + (INT)minIdx) * g_vbStride;
    if (ib0 + (size_t)ni * isz <= si_->data.size() && vb0 + (size_t)numV * g_vbStride <= sv->data.size()) {
        const uint8_t* ip = si_->data.data() + ib0; const uint8_t* vp = sv->data.data() + vb0;
        bb[0] = bb[1] = 1e30f; bb[2] = bb[3] = -1e30f;
        for (UINT k = 0; k < ni; k++) {
            UINT ix = isz == 4 ? ((const uint32_t*)ip)[k] : ((const uint16_t*)ip)[k];
            if (ix < minIdx || ix - minIdx >= numV) continue;
            const float* f = (const float*)(vp + (size_t)(ix - minIdx) * g_vbStride);
            bb[0] = fminf(bb[0], f[0]); bb[1] = fminf(bb[1], f[1]); bb[2] = fmaxf(bb[2], f[0]); bb[3] = fmaxf(bb[3], f[1]); ok = true;
        }
    }
    LeaveCriticalSection(&sv->cs); LeaveCriticalSection(&si_->cs);
    return ok;
}
// build 063: DipBounds timing (g_dbCalls / g_dbMs declared with the CPU stats)
static bool DipBoundsImpl(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT base, UINT minIdx, UINT numV, UINT si, UINT prims, float bb[4]);
static bool DipBounds(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT base, UINT minIdx, UINT numV, UINT si, UINT prims, float bb[4]) {
    double t0 = NowMs(); bool r = DipBoundsImpl(d, t, base, minIdx, numV, si, prims, bb); g_dbMs += NowMs() - t0; g_dbCalls++; return r;
}
static bool DipBoundsImpl(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT base, UINT minIdx, UINT numV, UINT si, UINT prims, float bb[4]) {
    RefreshStream(d);
    if (!g_vb0 || g_vbStride < 8 || !numV || numV > 65536 || prims > 256) return false;
    IDirect3DIndexBuffer9* ib = nullptr; if (FAILED(d->GetIndices(&ib)) || !ib) return false;
    bool ok = false; D3DINDEXBUFFER_DESC idd; void* ip = nullptr; void* vp = nullptr; UINT ni = VertsFor(t, prims);
    { bool found = false; ok = ShBounds(ib, t, base, minIdx, numV, si, ni, bb, &found);
      if (found) { g_shHit++; ib->Release(); return ok; } g_shMiss++; }
    if (SUCCEEDED(ib->GetDesc(&idd))) {
        UINT isz = idd.Format == D3DFMT_INDEX32 ? 4 : 2;
        if (SUCCEEDED(ib->Lock(si * isz, ni * isz, &ip, D3DLOCK_READONLY)) && ip) {
            if (SUCCEEDED(g_vb0->Lock(g_vbOff + (UINT)(base + (INT)minIdx) * g_vbStride, numV * g_vbStride, &vp, D3DLOCK_READONLY)) && vp) {
                bb[0] = bb[1] = 1e30f; bb[2] = bb[3] = -1e30f;
                for (UINT k = 0; k < ni; k++) {
                    UINT ix = isz == 4 ? ((const uint32_t*)ip)[k] : ((const uint16_t*)ip)[k];
                    if (ix < minIdx || ix - minIdx >= numV) continue;
                    const float* f = (const float*)((const uint8_t*)vp + (size_t)(ix - minIdx) * g_vbStride);
                    bb[0] = fminf(bb[0], f[0]); bb[1] = fminf(bb[1], f[1]); bb[2] = fmaxf(bb[2], f[0]); bb[3] = fmaxf(bb[3], f[1]); ok = true;
                }
                g_vb0->Unlock();
            }
            ib->Unlock();
        }
    }
    ib->Release(); return ok;
}
// returns true if c1 was changed (restore with PrimRestore)
static float g_primC1[4]; static bool g_primShifted;
static bool PrimPre(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT base, UINT minIdx, UINT numV, UINT si, UINT prims) {
    g_primShifted = false;
    if (!(g_curVSF & VSF_PRIM) || !g_uiW || g_uiH <= g_uiW * 9 / 16) return false;
    if (!g_hideBandOverlay && !g_primShift) return false;   // build 063: nothing to decide -> no vertex read-back
    float bb[4]; if (prims > 64 || !DipBounds(d, t, base, minIdx, numV, si, prims, bb)) return false;
    // build 034: a pixel-space rectangle covering exactly the 16:9 band and the full width = the low-health screen overlay
    // (drawn into the half-res effect buffer, then composited into the scene): skip it
    float top = (float)(g_uiH - g_uiW * 9 / 16) * 0.5f;
    // build 035: generalised - any screen-centred rectangle at least as wide as the screen whose height is between the 16:9 band
    // and 1.5x the band (the low-health overlay, the pulsing cinematic overlay in events); full-screen flashes (much larger) stay
    float cx = (bb[0] + bb[2]) * 0.5f, cy = (bb[1] + bb[3]) * 0.5f, bh = bb[3] - bb[1], band = (float)g_uiH - 2.0f * top;
    // build 036: no upper size limit (the cinematic overlay animates its size; above 1.5x it flickered back in), instead the
    // overlay must be wider than tall (>= 1.3:1) - the square full-screen flashes (-4608..7680) still pass
    float bw = bb[2] - bb[0];
    bool centred = fabsf(cx - g_uiW * 0.5f) < 64.0f && fabsf(cy - g_uiH * 0.5f) < 64.0f;
    // build 037: any small pixel-space prim (<= 4 triangles) that covers the whole 16:9 screen area: the low-health overlay, the
    // cinematic overlay, the drifting sun-glare sheet (11059x9216, moves with the sun) and the full-screen flashes
    // build 073: not on the map screen (its parchment / map image are screen-covering prims too), and never a quad that is
    // exactly the render target (the map composite); the hurt / cinematic overlays are band-shaped or larger than the screen
    bool exactTarget = fabsf(bb[0]) < 0.5f && fabsf(bb[1]) < 0.5f && fabsf(bb[2] - g_uiW) < 0.5f && fabsf(bb[3] - g_uiH) < 0.5f;
    if (g_hideBandOverlay && !g_mapActive && !g_mapSeen && !exactTarget && prims <= 16 && bb[0] <= 1.0f && bb[2] >= g_uiW - 1.0f && bb[1] <= top + 2.0f && bb[3] >= g_uiH - top - 2.0f) {
        if (g_bandOvlN++ < 6) Log("overlay: screen overlay %.0f,%.0f-%.0f,%.0f skipped (low-health / cinematic effect)", bb[0], bb[1], bb[2], bb[3]);
        return true;
    }
    if (g_hideBandOverlay && centred && bw > 1000.0f) {   // a centred screen-sized prim that was kept: log the first ones
        static int nk; if (nk++ < 12) Log("overlay: centred screen prim %.0f,%.0f-%.0f,%.0f kept", bb[0], bb[1], bb[2], bb[3]);
    }
    if (!g_primShift || !(g_curVSF & VSF_PRIMSHIFT) || g_rtW != g_uiW || g_rtH != g_uiH) return false;
    if (bb[0] <= 0.5f && bb[1] <= 0.5f && bb[2] >= g_rtW - 0.5f && bb[3] >= g_rtH - 0.5f) {   // full-target background / composite
        // the map screen draws its parchment and map into the scene before any GUI constants: start the UI layer here
        // (its map tiles go to an offscreen target first: those are not full-target, so they never arm the layer)
        if (g_pixelUi && !g_uiArmed && g_curRT0 && g_curDS && !g_uiActive) { g_uiArmed = true; LOG_FIRST(3, "ui: armed by a full-screen pixel-space prim draw (map screen)"); }
        return false;
    }
    float shift = (float)(g_rtH - g_rtW * 9 / 16) * 0.5f;
    memcpy(g_primC1, g_vsShadow[1], 16); float c[4]; memcpy(c, g_primC1, 16); c[1] -= 2.0f * shift / (float)g_rtH;
    oVSC(d, 1, c, 1); g_primShifted = true;
    LOG_FIRST(4, "prim: pixel-space draw %.0f,%.0f-%.0f,%.0f shifted down %.0f px", bb[0], bb[1], bb[2], bb[3], shift);
    return false;
}
static inline void PrimPost(IDirect3DDevice9* d) { if (g_primShifted) { oVSC(d, 1, g_primC1, 1); g_primShifted = false; } }
typedef HRESULT(STDMETHODCALLTYPE* CreateVS_t)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
static CreateVS_t oCreateVS;
static HRESULT STDMETHODCALLTYPE hkCreateVS(IDirect3DDevice9* d, const DWORD* fn, IDirect3DVertexShader9** out) {
    HRESULT hr = oCreateVS(d, fn, out);
    if (SUCCEEDED(hr) && out && *out && fn) {
        void* p = *out; size_t n = 0; while (n < 65536 && fn[n] != 0x0000FFFF) n++;
        const char* b = (const char*)fn; size_t len = n * 4; static const char key[] = "fPrimInvViewportSize"; bool pr = false, proj = false;
        for (size_t i = 0; i + sizeof key - 1 <= len && !pr; i++) if (b[i] == 'f' && !memcmp(b + i, key, sizeof key - 1)) pr = true;
        for (size_t i = 0; i + 5 <= len && pr && !proj; i++) if (!memcmp(b + i, "fProj", 5) || !memcmp(b + i, "ViewProj", 8)) proj = true;
        { static const char fk[] = "LensFlare"; bool fl = false;
          for (size_t i = 0; i + sizeof fk - 1 <= len && !fl; i++) if (b[i] == 'L' && !memcmp(b + i, fk, sizeof fk - 1)) fl = true;
          for (int i = 0; i < g_flareN; i++) if (g_flareVS[i] == p) { g_flareVS[i] = g_flareVS[--g_flareN]; break; }
          if (fl && g_flareN < 16) { g_flareVS[g_flareN++] = p; Log("flare: lens flare vertex shader %p", p); } }
        {   // build 081: the GUI vertex shader (constant table fGUIMatrix)
            extern void NoteGuiVS(void* p, bool gui); static const char gk[] = "fGUIMatrix"; bool g = false;
            for (size_t i = 0; i + sizeof gk - 1 <= len && !g; i++) if (b[i] == 'f' && !memcmp(b + i, gk, sizeof gk - 1)) g = true;
            NoteGuiVS(p, g);
        }
        for (int i = 0; i < g_primN; i++) if (g_primVS[i] == p) { --g_primN; g_primVS[i] = g_primVS[g_primN]; g_primLen[i] = g_primLen[g_primN]; break; }
        if (pr && !proj && len < 1024 && g_primN < 16) { g_primLen[g_primN] = (uint16_t)len; g_primVS[g_primN++] = p; Log("prim: pixel-space 2D vertex shader %p (%u bytes)", p, (unsigned)len); }
    }
    return hr;
}
// build 035: the game parks an unused status icon at HUD (0,0): in flat only a quarter of it shows in the screen corner, in VR it
// is the "tiny dark dot" (UI clipped to the band leaves that quarter). Skip small centred GUI quads placed exactly at (0,0).
static int g_hideParked = 1; static long g_parkedN;
static bool ParkedIcon(D3DPRIMITIVETYPE t, UINT a, UINT prims) {
    if (!g_hideParked || prims != 2 || g_vsShadow[4][0] != 0.0f || g_vsShadow[4][1] != 0.0f || fabsf(g_vsShadow[10][0] - 0.0015625f) > 1e-6f) return false;
    if (!g_vb0 || g_vbStride < 8) return false;
    UINT n = VertsFor(t, prims); void* p = nullptr; bool hit = false;
    if (SUCCEEDED(g_vb0->Lock(g_vbOff + a * g_vbStride, n * g_vbStride, &p, D3DLOCK_READONLY)) && p) {
        float mnx = 1e30f, mxx = -1e30f, mny = 1e30f, mxy = -1e30f;
        for (UINT i = 0; i < n; i++) { const float* v = (const float*)((uint8_t*)p + i * g_vbStride); mnx = fminf(mnx, v[0]); mxx = fmaxf(mxx, v[0]); mny = fminf(mny, v[1]); mxy = fmaxf(mxy, v[1]); }
        g_vb0->Unlock();
        float sx = fabsf(g_vsShadow[1][0]) + fabsf(g_vsShadow[2][0]), sy = fabsf(g_vsShadow[1][1]) + fabsf(g_vsShadow[2][1]);   // GUI matrix
        hit = mnx < 0 && mxx > 0 && mny < 0 && mxy > 0 && (mxx - mnx) * sx <= 40 && (mxy - mny) * sy <= 40 && (mxx - mnx) * sx >= 8;
        if (hit && g_parkedN++ < 3) Log("ui: parked icon at HUD (0,0) %.0f,%.0f-%.0f,%.0f skipped", mnx, mny, mxx, mxy);
    }
    return hit;
}
// ---------------------------------------------------------------- build 081: three UI adjustments (GUI vertex shader draws)
// GUI VS: pos = v.x*c1 + v.y*c2 + v.z*c3 + c4 (UI units), clip = pos * c10.xy + c10.zw. GUI PS: colour = tex * vertex colour * c4 + c5.
//  1. cutscene subtitles: drawn into the scene in a 1280x720 space spread over the whole eye height (c10 = 2/1280, -2/720, -1, 1),
//     so they sat at ~88% of the view height; text placed low (c4.y >= 480) is raised by SubtitleRaise UI units.
//  2. NPC conversation box (frame pieces at c4 = 246/396/546, 599 in the HUD band space): the box, its text and the name tag are
//     scaled by DialogueScale around the box's bottom-left corner (96, 749).
//  3. bow / aiming reticle (GUI quads at the screen centre 640,360 with the 512x512 HUD atlas, while the game view is zoomed):
//     drawn opaque white, with a dark offset copy underneath for contrast (ReticleStyle 2; 1 = white only, 0 = the game's).
static void* g_guiVS[16]; static int g_guiN;
bool IsGuiVS(void* p) { for (int i = 0; i < g_guiN; i++) if (g_guiVS[i] == p) return true; return false; }
void NoteGuiVS(void* p, bool gui) {
    for (int i = 0; i < g_guiN; i++) if (g_guiVS[i] == p) { g_guiVS[i] = g_guiVS[--g_guiN]; break; }
    if (gui && g_guiN < 16) { g_guiVS[g_guiN++] = p; Log("ui: GUI vertex shader %p", p); }
}
static float g_subRaise = 190.0f, g_subDepth = 2.0f, g_dlgScale = 1.3f; static int g_reticle = 2;
enum { TW_NONE = 0, TW_VS = 1, TW_RETICLE = 2, TW_WORLD = 8 };
struct Tweak { int kind; float vs[11][4]; float ps[2][4]; bool rt, scEn, z; DWORD scOld, zOld; D3DVIEWPORT9 vp; };
static inline bool Eq(float a, float b) { return fabsf(a - b) < 0.01f; }
// build 088/089: which GUI draws are world-anchored (089: decided by a fractional screen position, see IsWorldEl). HUD layers use whole-number depths (0, 16, 32, 230 seen: the 230 ones are HUD
// pieces that 087 wrongly moved); a world element's depth is a measured distance. A depth within 0.01 of a whole number counts as
// HUD unless an element at nearly the same place and depth was world-anchored last frame (a real distance can land close to a
// whole number now and then). The right replay repeats the left replay's decisions for the same constants.
static bool IsWorldEl(const float* c4) {
    if (c4[2] <= 32.5f || c4[3] != 1.0f) return false;
    if (g_replay == 1) { for (int i = 0; i < g_wCurN; i++) if (g_wCur[i].x == c4[0] && g_wCur[i].y == c4[1] && g_wCur[i].z == c4[2]) return true; return false; }
    // build 091: a menu (vocation selection, Save/Quit dialogs) puts many pieces at ONE depth spread over the screen; a world element's
    // pieces (name, bar, icon) stay within a few dozen units. A depth whose pieces spread over 120 units this or last frame = HUD.
    {   WGrp* g = nullptr; for (int i = 0; i < g_gCurN; i++) if (g_gCur[i].z == c4[2]) { g = &g_gCur[i]; break; }
        if (!g && g_gCurN < 64) { g = &g_gCur[g_gCurN++]; *g = { c4[2], c4[0], c4[0], c4[1], c4[1] }; }
        if (g) { g->x0 = std::min(g->x0, c4[0]); g->x1 = std::max(g->x1, c4[0]); g->y0 = std::min(g->y0, c4[1]); g->y1 = std::max(g->y1, c4[1]);
                 if (g->x1 - g->x0 > 120.0f || g->y1 - g->y0 > 120.0f) return false; }
        for (int i = 0; i < g_gPrevN; i++) if (g_gPrev[i].z == c4[2] && (g_gPrev[i].x1 - g_gPrev[i].x0 > 120.0f || g_gPrev[i].y1 - g_gPrev[i].y0 > 120.0f)) return false;
    }
    // build 089: menus (class selection, Options, Save/Quit) put their pieces at whole-number screen positions with a fixed depth
    // that is not a whole number (207.854, 227.972, 228.411); world elements are projected, so their screen position is fractional
    bool w = fabsf(c4[0] - roundf(c4[0])) > 0.01f || fabsf(c4[1] - roundf(c4[1])) > 0.01f;
    if (!w) for (int i = 0; i < g_wPrevN && !w; i++) w = fabsf(g_wPrev[i].x - c4[0]) < 40.0f && fabsf(g_wPrev[i].y - c4[1]) < 40.0f && fabsf(g_wPrev[i].z - c4[2]) < 3.0f;
    if (w && g_wCurN < 64) { bool dup = false; for (int i = 0; i < g_wCurN; i++) if (g_wCur[i].x == c4[0] && g_wCur[i].y == c4[1] && g_wCur[i].z == c4[2]) dup = true; if (!dup) g_wCur[g_wCurN++] = { c4[0], c4[1], c4[2] }; }
    return w;
}
static bool WorldGuiNow() {   // build 083: the right replay draws world-anchored GUI into its own picture
    if (!g_worldGui || !(g_curVSF & VSF_GUI) || dxr::Flat() || !g_uiW) return false;
    const float* c10 = g_vsShadow[10]; const float* c4 = g_vsShadow[4];
    return fabsf(c10[0] - 0.0015625f) < 1e-6f && fabsf(c10[1] + 2.0f / (1280.0f * g_uiH / g_uiW)) < 2e-6f && IsWorldEl(c4);
}
static int UiTweakPre(IDirect3DDevice9* d, Tweak& tw) {
    tw.kind = TW_NONE;
    if (!(g_curVSF & VSF_GUI) || dxr::Flat()) return TW_NONE;
    const float* c10 = g_vsShadow[10]; const float* c4 = g_vsShadow[4];
    if (fabsf(c10[0] - 0.0015625f) > 1e-6f) return TW_NONE;
    // 1. cutscene subtitles
    if (g_subRaise != 0.0f && fabsf(c10[1] + 2.0f / 720.0f) < 1e-6f && Eq(c10[3], 1.0f) && c4[1] >= 480.0f) {
        memcpy(tw.vs[10], c10, 16); float n[4]; memcpy(n, c10, 16); n[3] += g_subRaise * 2.0f / 720.0f;
        // build 082: the text is drawn into both eye images at the same place = infinitely far, so it doubled over nearer scenery;
        // shift it per eye so it sits at SubtitleDepth metres (left eye right, right eye left)
        if (g_replay >= 0) n[2] += (g_replay == 0 ? 1.0f : -1.0f) * dxr::StereoClipShift(g_subDepth);
        oVSC(d, 10, n, 1); tw.kind = TW_VS; LOG_FIRST(2, "ui: cutscene subtitle raised by %.0f units", g_subRaise); return tw.kind;
    }
    if (!g_uiW || fabsf(c10[1] + 2.0f / (1280.0f * g_uiH / g_uiW)) > 2e-6f) return TW_NONE;   // HUD band space only below
    // build 083: world-anchored GUI (name tags, quest markers, enemy bars: c4.z = view depth beyond the GUI near plane 32) is drawn
    // into each eye's picture at its real place instead of the flat UI panel (where it sat at the panel's distance, pulled
    // towards the centre)
    {   // build 085: placement study - End toggles a log line per frame for the first world-anchored element
        static float seen[16][3]; static int nSeen; static long seenFrame = -1; extern bool g_wlog; extern long g_wlogFrame; extern long g_wlogLines; long& lines = g_wlogLines;
        if (g_wlog && g_replay == 0 && c4[2] > 32.5f && c4[3] == 1.0f && lines < 6000) {
            if (seenFrame != g_wlogFrame) { seenFrame = g_wlogFrame; nSeen = 0; }
            bool dup = false; for (int i = 0; i < nSeen; i++) if (seen[i][0] == c4[0] && seen[i][1] == c4[1] && seen[i][2] == c4[2]) dup = true;
            if (!dup && nSeen < 16) { memcpy(seen[nSeen++], c4, 12); lines++; dxr::WorldGuiLog(c4, g_wlogFrame, IsWorldEl(c4) ? 1 : 0); }
        }
    }
    if (g_worldGui && g_replay >= 0 && g_uiScene && IsWorldEl(c4)) {
        float X, Y, k, rs = 0, rc = 1;
        if (dxr::WorldGuiToEye(g_worldGui, c4[0], c4[1], c4[2], &X, &Y, &k, &rs, &rc)) {
            memcpy(tw.vs[1], g_vsShadow[1], 32); memcpy(tw.vs[4], c4, 16); memcpy(tw.vs[10], c10, 16);
            k *= g_wmScale;
            float m[2][4]; memcpy(m, g_vsShadow[1], 32);
            for (int r = 0; r < 2; r++) {   // scale, then turn by the head's roll (screen y points down: R = [c -s; s c])
                const float ax = m[r][0] * k, ay = m[r][1] * k;
                m[r][0] = rc * ax - rs * ay; m[r][1] = rs * ax + rc * ay;
            }
            {   // build 093: the pieces of one element (name, bar, icon: same depth) keep their layout around the first piece drawn,
                // scaled and turned with it; each piece placed on its own drifted when the head rolled
                struct GA { int rep; float z, ax, ay, X, Y; }; static GA ga[32]; static int gn; static long gFrame = -1;
                extern long g_wlogFrame; if (gFrame != g_wlogFrame) { gFrame = g_wlogFrame; gn = 0; }
                GA* a = nullptr; for (int i = 0; i < gn; i++) if (ga[i].rep == g_replay && ga[i].z == c4[2]) { a = &ga[i]; break; }
                if (!a) { if (gn < 32) ga[gn++] = { g_replay, c4[2], c4[0], c4[1], X, Y }; }
                else { const float dx = (c4[0] - a->ax) * k, dy = (c4[1] - a->ay) * k; X = a->X + rc * dx - rs * dy; Y = a->Y + rs * dx + rc * dy; }
            }
            const float lift = g_wmLift * k;   // along the world's up
            float p4[4] = { X + rs * lift, Y - rc * lift, c4[2], 1.0f }, p10[4] = { 2.0f / 1280.0f, -2.0f / 1360.0f, -1.0f, 1.0f };
            oVSC(d, 1, m[0], 2); oVSC(d, 4, p4, 1); oVSC(d, 10, p10, 1);
            tw.rt = g_replay == 0 && g_curRT0 == g_uiScene;   // left replay: the UI layer is bound in place of the scene target
            if (tw.rt) { d->GetViewport(&tw.vp); oSetRT(d, 0, g_uiScene); oSetVP(d, &tw.vp); }
            d->GetRenderState(D3DRS_SCISSORTESTENABLE, &tw.scOld); tw.scEn = tw.scOld != 0; if (tw.scEn) oSetRS(d, D3DRS_SCISSORTESTENABLE, FALSE);
            d->GetRenderState(D3DRS_ZENABLE, &tw.zOld); tw.z = tw.zOld != 0; if (tw.z) oSetRS(d, D3DRS_ZENABLE, FALSE);
            LOG_FIRST(1, "ui: world-anchored GUI drawn into the eye pictures (z %.0f -> %.0f,%.0f x%.2f)", c4[2], X, Y, k);
            tw.kind = TW_WORLD; return tw.kind;
        }
    }
    // 2. NPC conversation box
    if (Eq(c4[1], 599.0f) && (Eq(c4[0], 246.0f) || Eq(c4[0], 396.0f) || Eq(c4[0], 546.0f)) && Eq(g_vsShadow[1][0], 1.0f)) {
        if (!g_talkSeen && !g_talkActive) Log("ui: NPC conversation box - scaled x%.2f", g_dlgScale);
        g_talkSeen = true;
    }
    if ((g_talkSeen || g_talkActive) && g_dlgScale != 1.0f && c4[0] >= 80.0f && c4[0] <= 720.0f && c4[1] >= 480.0f && c4[1] <= 780.0f) {
        const float ax = 96.0f, ay = 749.0f, S = g_dlgScale;
        memcpy(tw.vs[1], g_vsShadow[1], 16); memcpy(tw.vs[2], g_vsShadow[2], 16); memcpy(tw.vs[4], c4, 16);
        float m[3][4]; memcpy(m[0], g_vsShadow[1], 16); memcpy(m[1], g_vsShadow[2], 16); memcpy(m[2], c4, 16);
        for (int k = 0; k < 2; k++) { m[0][k] *= S; m[1][k] *= S; }
        m[2][0] = ax + (c4[0] - ax) * S; m[2][1] = ay + (c4[1] - ay) * S;
        oVSC(d, 1, m[0], 2); oVSC(d, 4, m[2], 1);
        tw.kind = TW_VS | 4; return tw.kind;
    }
    // 3. reticle
    if (g_reticle && Eq(c4[0], 640.0f) && Eq(c4[1], 360.0f) && c4[2] == 0.0f && dxr::GameViewZoomed()) {
        IDirect3DBaseTexture9* bt = nullptr; bool ok = false;
        if (SUCCEEDED(d->GetTexture(0, &bt)) && bt) {
            if (bt->GetType() == D3DRTYPE_TEXTURE) { D3DSURFACE_DESC td; if (SUCCEEDED(((IDirect3DTexture9*)bt)->GetLevelDesc(0, &td))) ok = td.Width == 512 && td.Height == 512; }
            bt->Release();
        }
        if (!ok || FAILED(d->GetPixelShaderConstantF(4, tw.ps[0], 2))) return TW_NONE;
        if (tw.ps[0][3] <= 0.0f) return TW_NONE;   // parts the game hides (alpha 0) stay hidden
        memcpy(tw.vs[4], c4, 16);
        LOG_FIRST(1, "ui: aiming reticle drawn white (ReticleStyle %d)", g_reticle);
        tw.kind = TW_RETICLE; return tw.kind;
    }
    return TW_NONE;
}
static void UiTweakPost(IDirect3DDevice9* d, const Tweak& tw) {
    if (tw.kind == TW_VS) oVSC(d, 10, tw.vs[10], 1);
    else if (tw.kind == (TW_VS | 4)) { oVSC(d, 1, tw.vs[1], 2); oVSC(d, 4, tw.vs[4], 1); }
    else if (tw.kind == TW_RETICLE) { oVSC(d, 4, tw.vs[4], 1); oPSC(d, 4, tw.ps[0], 2); }
    else if (tw.kind == TW_WORLD) {
        oVSC(d, 1, tw.vs[1], 2); oVSC(d, 4, tw.vs[4], 1); oVSC(d, 10, tw.vs[10], 1);
        if (tw.rt) { oSetRT(d, 0, g_uiSurf); oSetVP(d, &tw.vp); }
        if (tw.scEn) oSetRS(d, D3DRS_SCISSORTESTENABLE, tw.scOld);
        if (tw.z) oSetRS(d, D3DRS_ZENABLE, tw.zOld);
    }
}
// the reticle's draws: an optional dark copy offset by 1 UI unit, then the white one
template <class F> static void ReticleDraws(IDirect3DDevice9* d, const Tweak& tw, F draw) {
    const float a = tw.ps[0][3];
    if (g_reticle >= 2) {
        float c4[4]; memcpy(c4, tw.vs[4], 16); c4[0] += 1.0f; c4[1] += 1.0f; oVSC(d, 4, c4, 1);
        float pc[2][4] = { { 0, 0, 0, a * 1.6f }, { 0, 0, 0, 0 } }; oPSC(d, 4, pc[0], 2); draw();
        oVSC(d, 4, tw.vs[4], 1);
    }
    float pc[2][4] = { { 0, 0, 0, a * 2.5f }, { 1, 1, 1, 0 } }; oPSC(d, 4, pc[0], 2);
}
static HRESULT STDMETHODCALLTYPE hkDP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT a, UINT b) { R1NoteIfEye(); HPScope _hp(HP_DP); g_fDraws++; if (g_diagOn || CAPON) RefreshTexState(d); if (g_diagOn) DiagDraw(); if (g_inBandRT) return D3D_OK; if (g_movieHold > 0 && (RefreshStream(d), MovieBar(t, a, b))) return D3D_OK; OVERLAY_SKIP(); bool ui = UiPreDraw(d); UI_SKIP(); if (ui && (RefreshStream(d), ParkedIcon(t, a, b))) { UiPost(d, ui); return D3D_OK; } if (CAPON) { g_da = { 1, t, a, 0, b, nullptr, 0, 0, b, d }; CapDraw(RA, b); PostRec(RA, b); } Tweak tw; if (UiTweakPre(d, tw) == TW_RETICLE) ReticleDraws(d, tw, [&] { oDP(d, t, a, b); }); HRESULT hr = oDP(d, t, a, b); if (tw.kind) UiTweakPost(d, tw); UiPost(d, ui); return hr; }
static HRESULT STDMETHODCALLTYPE hkDIP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT a, UINT b, UINT c, UINT e, UINT f) { R1NoteIfEye(); for (int k = 0; k < g_intzN; k++) if (g_curDS == g_intz[k].surf) { g_intz[k].draws++; break; } HPScope _hp(HP_DIP); g_fDraws++; if (g_diagOn || CAPON) RefreshTexState(d); if (g_diagOn) DiagDraw(); if (g_inBandRT) return D3D_OK; OVERLAY_SKIP(); if (PrimPre(d, t, a, b, c, e, f)) return D3D_OK; bool ui = UiPreDraw(d); UI_SKIP(); if (CAPON) { g_da = { 2, t, (UINT)a, b, c, nullptr, 0, e, f, d }; CapDraw(RA, f); PostRec(RA, f); } static unsigned dipN; bool timed = g_prof && (++dipN & 15) == 0; double t0 = timed ? NowMs() : 0; Tweak tw; if (UiTweakPre(d, tw) == TW_RETICLE) ReticleDraws(d, tw, [&] { oDIP(d, t, a, b, c, e, f); }); HRESULT hr = oDIP(d, t, a, b, c, e, f); if (tw.kind) UiTweakPost(d, tw); if (timed) g_s.drawMs += (NowMs() - t0) * 16.0; UiPost(d, ui); PrimPost(d); return hr; }
static HRESULT STDMETHODCALLTYPE hkDPUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT a, const void* p, UINT s) { R1NoteIfEye(); g_fDraws++; if (g_diagOn || CAPON) RefreshTexState(d); if (g_diagOn) DiagDraw(); g_s.drawsUP++; OVERLAY_SKIP(); bool ui = UiPreDraw(d); UI_SKIP(); if (CAPON) { g_da = { 3, t, 0, 0, a, p, s, 0, a, d }; CapDraw(RA, a); PostRec(RA, a); } HRESULT hr = oDPUP(d, t, a, p, s); UiPost(d, ui); return hr; }
static HRESULT STDMETHODCALLTYPE hkDIPUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT a, UINT b, UINT c, const void* i, D3DFORMAT f, const void* v, UINT s) { R1NoteIfEye();
    g_fDraws++; if (g_diagOn || CAPON) RefreshTexState(d); if (g_diagOn) DiagDraw(); g_s.drawsUP++; OVERLAY_SKIP(); bool ui = UiPreDraw(d); UI_SKIP(); if (CAPON) { g_da = { 4, t, a, b, c, v, s, 0, c, d }; CapDraw(RA, c); PostRec(RA, c); } HRESULT hr = oDIPUP(d, t, a, b, c, i, f, v, s); UiPost(d, ui); return hr;
}
// build 031: remember which pixel shaders are the image-plane filter (by their constant table)
typedef HRESULT(STDMETHODCALLTYPE* CreatePS_t)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
static CreatePS_t oCreatePS;
static HRESULT STDMETHODCALLTYPE hkCreatePS(IDirect3DDevice9* d, const DWORD* fn, IDirect3DPixelShader9** out) {
    HRESULT hr = oCreatePS(d, fn, out);
    if (SUCCEEDED(hr) && out && *out && fn) {
        void* p = *out; bool ip = false;
        // walk the token stream to the end token (0x0000FFFF); the CTAB comment holds the constant names
        size_t n = 0; while (n < 65536 && fn[n] != 0x0000FFFF) n++;
        const char* b = (const char*)fn; size_t len = n * 4; static const char key[] = "fImagePlaneColor";
        for (size_t i = 0; i + sizeof key - 1 <= len && !ip; i++) if (b[i] == 'f' && !memcmp(b + i, key, sizeof key - 1)) ip = true;
        for (int i = 0; i < g_ipN; i++) if (g_ipPS[i] == p) { g_ipPS[i] = g_ipPS[--g_ipN]; break; }   // pointer reused by a new shader
        for (int i = 0; i < g_dofN; i++) if (g_dofPS[i] == p) { g_dofPS[i] = g_dofPS[--g_dofN]; break; }
        static const char gk[] = "fGodRaysColor"; bool gr = false;
        for (size_t i = 0; i + sizeof gk - 1 <= len && !gr; i++) if (b[i] == 'f' && !memcmp(b + i, gk, sizeof gk - 1)) gr = true;
        for (int i = 0; i < g_grN; i++) if (g_grPS[i] == p) { g_grPS[i] = g_grPS[--g_grN]; break; }
        if (gr && g_grN < 16) { g_grPS[g_grN++] = p; Log("godrays: pixel shader %p", p); }
        static const char dk[] = "tDOFMap"; bool dof = false;
        for (size_t i = 0; i + sizeof dk - 1 <= len && !dof; i++) if (b[i] == 't' && !memcmp(b + i, dk, sizeof dk - 1)) dof = true;
        if (dof && g_dofN < 32) { g_dofPS[g_dofN++] = p; Log("dof: depth-of-field pixel shader %p (%u bytes)", p, (unsigned)len); }
        if (ip && g_ipN < 64) { g_ipPS[g_ipN++] = p; Log("overlay: image-plane pixel shader %p (%u bytes)%s", p, (unsigned)len, g_hideOverlay ? " - its draws are skipped" : ""); }
    }
    return hr;
}
static long g_capVsc;
static HRESULT STDMETHODCALLTYPE hkVSC(IDirect3DDevice9* d, UINT r, const float* f, UINT n) { HPScope _hp(HP_VSC);
    g_fVsc++;
    if (f) for (UINT i = 0; i < n && r + i < 16; i++) memcpy(g_vsShadow[r + i], f + i * 4, 16);
    if (g_replay < 0) dxr::NoteVsConstants(f, n);   // build 056: the synchronous replays know their camera
    if (f && r <= 10 && r + n > 10 && g_uiW) {   // UI screen constants: c10 = (2/1280, -2/UIheight, ...)
        const float* c = f + (10 - r) * 4; float want = -2.0f / (1280.0f * g_uiH / g_uiW);
        if (fabsf(c[0] - 0.0015625f) < 1e-6f && fabsf(c[1] - want) < 2e-6f && !g_uiArmed) g_uiArmed = true;
        // build 031: the map screen draws its GUI in pixels of the buffer: c10 = (2/W, -2/H)
        if (fabsf(c[0] - 2.0f / g_uiW) < 1e-7f && fabsf(c[1] + 2.0f / g_uiH) < 1e-7f) g_mapSeen = true;   // build 073: map screen this frame
        if (g_pixelUi && !g_uiArmed && fabsf(c[0] - 2.0f / g_uiW) < 1e-7f && fabsf(c[1] + 2.0f / g_uiH) < 1e-7f) { g_uiArmed = true; LOG_FIRST(3, "ui: armed by pixel-space GUI constants (map screen)"); }
    }
    if (f && !g_uiW) { IDirect3DSurface9* u = dxr::UiSurface(); if (u) { D3DSURFACE_DESC sd; u->GetDesc(&sd); g_uiW = sd.Width; g_uiH = sd.Height; g_uiSurf = u; } }
    if (g_replay == 0 && f && dxr::SyncNeedCam()) dxr::SyncVS((float*)f, n);   // replay 0 only identifies its camera (never writes)
    if (f && n <= 256 && (g_uiArmed || (g_replay == 1 && (g_diagOn || dxr::SyncMaybe(f, n, false))))) {   // build 068: copy only when needed
        float buf[1024]; memcpy(buf, f, n * 16); bool ch = false;
        if (g_replay == 1 && dxr::SyncVS(buf, n)) ch = true;   // build 042: right eye's view-projection
        if (g_uiArmed) for (UINT i = 0; i < n; i++) if (dxr::FixUiProjRow(buf + i * 4)) ch = true;   // UI: the game's own projection
        if (ch) { if (r + n > 0) for (UINT i = 0; i < n && r + i < 16; i++) memcpy(g_vsShadow[r + i], buf + i * 4, 16); if (g_diagOn) DiagConst(false, r, f, buf, n); return oVSC(d, r, buf, n); }
    }
    if (g_diagOn && f) DiagConst(false, r, f, f, n);
    if (CAPON && n >= 4 && g_capVsc < 400) {
        g_capVsc++;
        CapF("    VSC c%u x%u from %p: %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f", r, n, RA,
             f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
    }
    return oVSC(d, r, f, n);
}
static HRESULT STDMETHODCALLTYPE hkClear(IDirect3DDevice9* d, DWORD n, const D3DRECT* r, DWORD f, D3DCOLOR c, float z, DWORD s) { R1NoteIfEye(); HPScope _hp(HP_Clear);
    g_s.clears++;
    if (R1Skip()) return D3D_OK;
    if (CAPON) { CapFlushRun(); CapF("Clear rects %lu flags 0x%lx color %08lx z %.3f from %p", n, f, c, z, RA); }
    return oClear(d, n, r, f, c, z, s);
}
typedef HRESULT(STDMETHODCALLTYPE* SetDS_t)(IDirect3DDevice9*, IDirect3DSurface9*);
typedef HRESULT(STDMETHODCALLTYPE* SetSc_t)(IDirect3DDevice9*, const RECT*);
typedef HRESULT(STDMETHODCALLTYPE* SR_t)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
static SetDS_t oSetDS; static SetSc_t oSetSc; static SR_t oSR;
static HRESULT STDMETHODCALLTYPE hkSetDS(IDirect3DDevice9* d, IDirect3DSurface9* s) { HPScope _hp(HP_SetDS);
    g_curDS = s;
    if (CAPON) { CapFlushRun(); char b[96]; SurfDesc(s, b, sizeof b); CapF("DS = %s  from %p", b, RA); }
    return oSetDS(d, s);
}
// build 007: where does the 16:9 scene viewport come from?
static bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery(p, &m, sizeof m) || m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return (uint8_t*)p + n <= (uint8_t*)m.BaseAddress + m.RegionSize;
}
static void ProbeLetterbox(const D3DVIEWPORT9* v) {
    static int done; if (done++) return;
    char b[1024]; int o = 0;
    uintptr_t* sp = (uintptr_t*)__builtin_frame_address(0);
    for (int i = 0, n = 0; i < 400 && n < 24; i++) {
        if (!Readable(sp + i, 4)) break;
        uintptr_t x = sp[i];
        if (x > 0x401000 && x < 0x1390000) { o += snprintf(b + o, sizeof b - o, " %x", (unsigned)x); n++; }
    }
    Log("letterbox: viewport %lu,%lu %lux%lu; stack code addresses:%s", v->X, v->Y, v->Width, v->Height, b);
    uint8_t* r = g_game && Readable((void*)SRENDER_PTR, 4) ? *(uint8_t**)SRENDER_PTR : nullptr;
    Log("letterbox: render object %p", r);
    if (!r) return;
    int found = 0;
    for (uint32_t off = 0; off < 0x441000 && found < 60; off += 4) {
        uintptr_t pa = (uintptr_t)(r + off) & 0xfff;
        if ((off == 0 || pa == 0 || pa >= 0xff8) && !Readable(r + off, 8)) continue;
        uint32_t x = *(uint32_t*)(r + off), nx = *(uint32_t*)(r + off + 4);
        float f, nf; memcpy(&f, &x, 4); memcpy(&nf, &nx, 4);
        if (x == v->Y || x == v->Height || f == (float)v->Y || f == (float)v->Height) {
            Log("letterbox:   +0x%x = %u / %.1f  (next %u / %.3f)", off, x, f, nx, nf); found++;
        }
    }
}
static HRESULT STDMETHODCALLTYPE hkSetVP(IDirect3DDevice9* d, const D3DVIEWPORT9* v) { HPScope _hp(HP_SetVP);
    D3DVIEWPORT9 fv;
    if (v && v->X == 0 && v->Y > 0 && g_rtW && g_rtW < 1024 && v->Width == g_rtW && v->Height < g_rtH) {
        static int n; if (n++ < 6) {
            char b[1024]; int o = 0; uintptr_t* sp = (uintptr_t*)__builtin_frame_address(0);
            for (int i = 0, k = 0; i < 600 && k < 24; i++) { if (!Readable(sp + i, 4)) break; uintptr_t x = sp[i]; if (x > 0x401000 && x < 0x1390000) { o += snprintf(b + o, sizeof b - o, " %x", (unsigned)x); k++; } }
            Log("bandprobe: viewport %lu,%lu %lux%lu on %ux%u target; stack:%s", v->X, v->Y, v->Width, v->Height, g_rtW, g_rtH, b);
        }
    }
    if (v && g_killBand && g_fullRect && g_rtW && g_rtW < 1024 && v->X == 0 && v->Width == g_rtW && v->Y > 0 && v->Height < g_rtH &&
        abs((int)v->Height - (int)(g_rtW * 9 / 16)) <= 2) {
        if (!g_inBandRT) { g_inBandRT = true; HRESULT r = oSetVP(d, v); D3DVIEWPORT9 all = { 0, 0, g_rtW, g_rtH, 0, 1 }; oSetVP(d, &all);
            oClear(d, 0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0); LOG_FIRST(4, "killband: cleared %ux%u band target", g_rtW, g_rtH); oSetVP(d, v); return r; }
    }
    if (g_widen && g_fullRect && v && v->X == 0 && g_rtW && v->Width == g_rtW && v->Height < g_rtH) {
        int h169 = (int)(g_rtW * 9 / 16);
        if (abs((int)v->Height - h169) <= 2 && abs((int)v->Y - (int)(g_rtH - v->Height) / 2) <= 2) {
            fv = *v; fv.Y = 0; fv.Height = g_rtH; v = &fv;
            LOG_FIRST(8, "fullrect: widened viewport 0,%d %dx%d on %ux%u target", (int)(g_rtH - h169) / 2, g_rtW, h169, g_rtW, g_rtH);
        }
    }
    if (v && v->Y > 0 && v->Width >= 1024 && v->X == 0) ProbeLetterbox(v);
    if (CAPON && v) { CapFlushRun(); CapF("Viewport %lu,%lu %lux%lu z %.2f-%.2f  from %p", v->X, v->Y, v->Width, v->Height, v->MinZ, v->MaxZ, RA); }
    return oSetVP(d, v);
}
static HRESULT STDMETHODCALLTYPE hkSetSc(IDirect3DDevice9* d, const RECT* r) { HPScope _hp(HP_SetSc);
    if (r) g_scGame = *r;
    if (CAPON && r) { CapFlushRun(); CapF("Scissor %ld,%ld-%ld,%ld  from %p", r->left, r->top, r->right, r->bottom, RA); }
    return oSetSc(d, r);
}
static HRESULT STDMETHODCALLTYPE hkSR(IDirect3DDevice9* d, IDirect3DSurface9* a, const RECT* ra, IDirect3DSurface9* b, const RECT* rb, D3DTEXTUREFILTERTYPE f) { if (g_replay == 0 && g_r1Start < 0 && b && g_uiW) { D3DSURFACE_DESC dd; if (SUCCEEDED(b->GetDesc(&dd))) for (int k = 0; k < 6; k++) if (dd.Width == (g_uiW >> k) && dd.Height == (g_uiH >> k)) { R1NoteEyeWrite(); break; } }
    if (CAPON) {
        CapFlushRun(); char x[96], y[96]; SurfDesc(a, x, sizeof x); SurfDesc(b, y, sizeof y);
        CapF("StretchRect %s [%ld,%ld-%ld,%ld] -> %s [%ld,%ld-%ld,%ld] from %p", x, ra ? ra->left : -1, ra ? ra->top : -1, ra ? ra->right : -1, ra ? ra->bottom : -1,
             y, rb ? rb->left : -1, rb ? rb->top : -1, rb ? rb->right : -1, rb ? rb->bottom : -1, RA);
    }
    // build 053: replay 1 skips copies INTO non-eye targets too. The point-light (torch) cube shadow atlas 192x128 is
    // rebuilt each frame from a cached static copy (StretchRect static -> working, then dynamic casters + blur drawn into it);
    // replaying only the copy reset the atlas to the unfiltered static map for the right eye -> bars / lines on the ground.
    if (g_replay == 1 && b && g_uiW) {
        D3DSURFACE_DESC sd; if (SUCCEEDED(b->GetDesc(&sd)) && !EyeSized(sd.Width, sd.Height)) { g_r1SrSkipped++; LOG_FIRST(3, "stereo: replay 1 copy into %ux%u skipped", sd.Width, sd.Height); return D3D_OK; }
    }
    return oSR(d, a, ra, b, rb, f);
}
// build 008: render object +0x10488 looks like the scene rect {0, 768, 3072, 2496}. F9 toggles forcing it to the full buffer.
static void FullRectTick() {
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    static bool prev; bool k = g_devKeys && ctrl && (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    static bool prev10; bool k10 = g_devKeys && ctrl && (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    static bool prev9; bool k9 = g_devKeys && !ctrl && (GetAsyncKeyState(VK_F9) & 0x8000) != 0;   // build 031: UI hide test (tiny dot)
    if (k9 && !prev9) { static const char* nm[] = { "off", "hide depth-tested UI (world markers)", "hide world-anchored UI (c4.z)" }; g_uiHideMode = (g_uiHideMode + 1) % 3; Log("ui hide test (F9): %d = %s", g_uiHideMode, nm[g_uiHideMode]); }
    prev9 = k9;
    static bool prevF10; bool kf10 = !ctrl && (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (kf10 && !prevF10) { g_killGodRays = !g_killGodRays; Log("godrays (F10): %s", g_killGodRays ? "OFF (skipped)" : "ON"); }
    prevF10 = kf10;
    if (k10 && !prev10) { g_killBand = !g_killBand; Log("killband: %s", g_killBand ? "ON" : "OFF"); }
    prev10 = k10;
    if (k && !prev) { g_fullRect = !g_fullRect; Log("fullrect: %s", g_fullRect ? "ON" : "OFF"); if (g_fullRect) g_capCountdown = 60; }
    if (g_capCountdown > 0 && --g_capCountdown == 0 && g_capState == 0) { g_capState = 2; Log("capture: 1 s after full rect ON"); }
    prev = k;
    int32_t* r = g_game && Readable((void*)SRENDER_PTR, 4) ? *(int32_t**)SRENDER_PTR : nullptr;
    if (!r || !Readable((uint8_t*)r + 0x10480, 0x20)) return;
    int32_t* q = (int32_t*)((uint8_t*)r + 0x10488);
    int W = *(int32_t*)((uint8_t*)r + 0xb8), H = *(int32_t*)((uint8_t*)r + 0xbc);
    static int n; if (n++ % 600 == 0) Log("fullrect: render %dx%d, +0x10480: %d %d | %d %d %d %d | %d %d", W, H, q[-2], q[-1], q[0], q[1], q[2], q[3], q[4], q[5]);

}

// build 012: camera setup BbsRpg+0xdc1f80 (thiscall, 1 arg) reads its viewport rect from cam+0x18..0x24 (or, if byte cam+0x13 == 10,
// from [0x18d0b6c]+0xcb0). F9 forces the rect to the full render size before the camera is set up.
typedef char(__fastcall* CamSetup_t)(void* cam, void* edx, void* arg);
static CamSetup_t oCamSetup;
static int g_keepVrCam;
static char __fastcall hkCamSetup(void* cam, void* edx, void* arg) {
    int32_t* rc = (int32_t*)((uint8_t*)cam + 0x18);
    uint8_t mode = *((uint8_t*)cam + 0x13);
    static void* seen[16]; static int nSeen;
    bool known = false; for (int i = 0; i < nSeen; i++) if (seen[i] == cam) known = true;
    if (!known && nSeen < 16) {
        seen[nSeen++] = cam;
        Log("camsetup: camera %p (vtable %p) mode %u rect %d,%d-%d,%d active %u", cam, *(void**)cam, mode, rc[0], rc[1], rc[2], rc[3], *((uint8_t*)cam + 0x10));
    }
    if (g_fullRect) {
        int32_t* r = *(int32_t**)SRENDER_PTR;
        int W = *(int32_t*)((uint8_t*)r + 0xb8), H = *(int32_t*)((uint8_t*)r + 0xbc);
        if (W > 0 && H > 0 && rc[2] - rc[0] == W && rc[3] - rc[1] < H) { rc[0] = 0; rc[1] = 0; rc[2] = W; rc[3] = H; }
        if (mode == 10) {
            int32_t* g = (int32_t*)(*(uint8_t**)CAMRECT_PTR + 0xcb0);
            if (W > 0 && g[2] - g[0] == W && g[3] - g[1] < H) { g[0] = 0; g[1] = 0; g[2] = W; g[3] = H; }
        }
    }
    // build 024: head tracking - the main (full-size) camera's view and projection replaced for this setup only
    {   int32_t* r = *(int32_t**)SRENDER_PTR;
        int W = r ? *(int32_t*)((uint8_t*)r + 0xb8) : 0, H = r ? *(int32_t*)((uint8_t*)r + 0xbc) : 0;
        int32_t* g = mode == 10 ? (int32_t*)(*(uint8_t**)CAMRECT_PTR + 0xcb0) : rc;
        if (W > 0 && g[2] - g[0] == W && g[3] - g[1] == H) {
            float* V = (float*)((uint8_t*)cam + 0x90); float* P = (float*)((uint8_t*)cam + 0xd0);
            // build 069: if the game did not rewrite a matrix we left in the camera last frame, start from the game's own one again
            static float ourV[16], ourP[16], gameV[16], gameP[16]; static bool haveOur;
            if (haveOur && g_keepVrCam >= 1 && !memcmp(P, ourP, 64)) memcpy(P, gameP, 64);
            if (haveOur && g_keepVrCam >= 2 && !memcmp(V, ourV, 64)) memcpy(V, gameV, 64);
            float sv[16], sp[16]; memcpy(sv, V, 64); memcpy(sp, P, 64);
            if (dxr::ApplyToCamera(V, P)) {
                memcpy(ourV, V, 64); memcpy(ourP, P, 64); memcpy(gameV, sv, 64); memcpy(gameP, sp, 64); haveOur = true;
                char ret = oCamSetup(cam, edx, arg);
                // build 069 test: torch lights / NPCs switch off at the edge of the game's own view - something later in the frame
                // culls with the camera's matrices after we put the game's ones back. KeepVrCamera: 1 = leave our wide projection
                // in the camera, 2 = also leave our head-tracked view.
                if (g_keepVrCam < 2) memcpy(V, sv, 64);
                if (g_keepVrCam < 1) memcpy(P, sp, 64);
                return ret;
            }
        }
    }
    return oCamSetup(cam, edx, arg);
}
// build 009: hardware write watchpoint on render+0x1048c to find the code that computes the scene rect
#include <tlhelp32.h>
static volatile LONG g_wpHits; static uintptr_t g_wpAddr;
static void SetDrAll(uintptr_t addr) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = { sizeof te }; DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId(); int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID); if (!h) continue;
        SuspendThread(h);
        CONTEXT c = {}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(h, &c)) {
            c.Dr0 = addr; c.Dr7 = addr ? ((c.Dr7 & ~0xF0003ull) | 0x1 | (0x1 << 16) | (0x3 << 18)) : (c.Dr7 & ~0xF0003ull);  // L0, write, 4 bytes
            if (SetThreadContext(h, &c)) n++;
        }
        ResumeThread(h); CloseHandle(h);
    }
    CloseHandle(snap);
    Log("watch: debug registers %s on %d threads (addr %p)", addr ? "set" : "cleared", n, (void*)addr);
}
static LONG CALLBACK WatchVeh(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    LONG n = InterlockedIncrement(&g_wpHits);
    if (n <= 6) {
        CONTEXT* c = e->ContextRecord; char b[1024]; int o = 0;
        uintptr_t* sp = (uintptr_t*)c->Esp;
        for (int i = 0, k = 0; i < 800 && k < 28; i++) { if (!Readable(sp + i, 4)) break; uintptr_t x = sp[i]; if (x > 0x401000 && x < 0x1390000) { o += snprintf(b + o, sizeof b - o, " %x", (unsigned)x); k++; } }
        Log("watch: write #%ld by eip %08lx (thread %lu) value now %d | eax %lx ebx %lx ecx %lx edx %lx esi %lx edi %lx ebp %lx | stack:%s", n, c->Eip, GetCurrentThreadId(),
            *(int*)g_wpAddr, c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, b);
    }
    e->ContextRecord->Dr6 = 0;
    if (n == 6) { e->ContextRecord->Dr7 &= ~0xF0003ull; }   // this thread stops; others cleared from Present
    return EXCEPTION_CONTINUE_EXECUTION;
}
static DWORD WINAPI WpSetThread(LPVOID) { SetDrAll(g_wpAddr); return 0; }
static DWORD WINAPI WpClearThread(LPVOID) { SetDrAll(0); LogFlush(); return 0; }
static void WatchTick() {
    static int state; if (state == 2) return;
    if (state == 0 && g_bandAddr) {
        g_wpAddr = g_bandAddr;
        AddVectoredExceptionHandler(1, WatchVeh);
        HANDLE t = CreateThread(nullptr, 0, WpSetThread, nullptr, 0, nullptr); if (t) CloseHandle(t);
        state = 1;
    } else if (state == 1 && g_wpHits >= 6) {
        HANDLE t = CreateThread(nullptr, 0, WpClearThread, nullptr, 0, nullptr); if (t) CloseHandle(t);
        state = 2;
    }
}
static void CaptureTick(long lastDraws) {   // called at Present, before the next frame starts
    if (g_capState == 2) {
        CapFlushRun();
        static int nCap; wchar_t path[MAX_PATH]; wcscpy(path, g_ini);
        wchar_t* sl = wcsrchr(path, L'\\'); swprintf(sl ? sl + 1 : path, 64, L"ddda_frame_%d.txt", ++nCap);
        HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD w = 0; if (h != INVALID_HANDLE_VALUE) { WriteFile(h, g_cap.data(), (DWORD)g_cap.size(), &w, nullptr); CloseHandle(h); }
        Log("capture: %ld events written to ddda_frame_%d.txt (%lu bytes)", g_capEvents, nCap, w);
        UiDump(nCap);
        if (g_dumpPost) PostDump(nCap); else g_postN = 0;
        LogFlush();
        g_cap.clear(); g_capEvents = 0; g_capVsc = 0; g_capState = 0;
    }
    static bool autoDone; static bool f8Prev;
    bool f8 = g_devKeys && (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f8 && !f8Prev) { g_capState = 2; Log("capture: F8"); }
    f8Prev = f8;
        if (g_devKeys && !autoDone && g_capState == 0 && lastDraws > 3000 && NowMs() - g_startMs > 20000) { autoDone = true; g_capState = 2; Log("capture: automatic (%ld draws last frame)", lastDraws); }
}

static void Hook(void* target, void* det, void** orig, const char* what) {
    MH_STATUS s = MH_CreateHook(target, det, orig);
    if (s == MH_OK) s = MH_EnableHook(target);
    Log("hook %s : %s", what, MH_StatusToString(s));
}

// ---------------------------------------------------------------- build 018: D3D9Ex has no managed pool
// Managed textures / buffers become default-pool dynamic resources (lockable like managed ones).
typedef HRESULT(STDMETHODCALLTYPE* CreateTex_t)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* CreateVol_t)(IDirect3DDevice9*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* CreateCube_t)(IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* CreateVB_t)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* CreateIB_t)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*);
static CreateTex_t oCreateTex; static CreateVol_t oCreateVol; static CreateCube_t oCreateCube; static CreateVB_t oCreateVB; static CreateIB_t oCreateIB;
static inline void ExPool(D3DPOOL& pool, DWORD& usage) {
    if (!g_isEx || pool != D3DPOOL_MANAGED) return;
    pool = D3DPOOL_DEFAULT;
    if (!(usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) usage |= D3DUSAGE_DYNAMIC;
}
// ---------------------------------------------------------------- build 020: vignette removal
// The vignette is the overlay texture scr\sky\camera_shadow_NOMIP_BM.tex in every stage archive: 512x256 DXT5, one
// level, black with alpha rising towards the corners. (The Nexus "Vignette Remover" replaces it with all-transparent
// blocks inside the archives.) Here, any 512x256 DXT5 / A8R8G8B8 texture is a candidate; when the game unlocks level 0
// we check that the data looks like a vignette (dark colour, transparent centre, opaque corners) and overwrite it with
// transparent black. The original is kept so F11 can toggle it back for comparison. ini: RemoveVignette=1.
static int g_noVignette = 1; static bool g_vigShown;      // g_vigShown: F11 state (true = original restored)
typedef HRESULT(STDMETHODCALLTYPE* TexLock_t)(IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* TexUnlock_t)(IDirect3DTexture9*, UINT);
static TexLock_t oTexLock; static TexUnlock_t oTexUnlock;
struct VigCand { IDirect3DTexture9* t; D3DFORMAT f; UINT w, h; void* bits; INT pitch; bool isVig; std::string orig; };
enum { VIGN = 32 };
static VigCand g_vig[VIGN]; static CRITICAL_SECTION g_vigCs; static bool g_vigCsInit;
static const uint8_t kClearDXT5[16] = { 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0x01, 0, 0, 0, 0x55, 0x55, 0x55, 0x55 };   // the Nexus mod's block
static VigCand* VigFind(IDirect3DTexture9* t) { for (auto& c : g_vig) if (c.t == t) return &c; return nullptr; }
static float Dxt5BlockAlpha(const uint8_t* b) {
    int a0 = b[0], a1 = b[1], pal[8] = { a0, a1 };
    if (a0 > a1) for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    else { for (int i = 1; i < 5; i++) pal[i + 1] = ((5 - i) * a0 + i * a1) / 5; pal[6] = 0; pal[7] = 255; }
    uint64_t idx = 0; for (int i = 0; i < 6; i++) idx |= (uint64_t)b[2 + i] << (8 * i);
    int s = 0; for (int i = 0; i < 16; i++) s += pal[(idx >> (3 * i)) & 7];
    return s / 16.0f;
}
static bool Dxt5Dark(const uint8_t* b) {
    for (int k = 0; k < 2; k++) { uint16_t c = b[8 + 2 * k] | (b[9 + 2 * k] << 8); if ((c >> 11) > 6 || ((c >> 5) & 63) > 12 || (c & 31) > 6) return false; }
    return true;
}
// alpha at block/pixel (x, y) in the locked data; dark = colour near black
static float VigAlpha(const VigCand& c, UINT x, UINT y, bool* dark) {
    const uint8_t* row = (const uint8_t*)c.bits + (size_t)c.pitch * (c.f == D3DFMT_DXT5 ? y / 4 : y);
    if (c.f == D3DFMT_DXT5) { const uint8_t* b = row + (x / 4) * 16; if (dark) *dark = Dxt5Dark(b); return Dxt5BlockAlpha(b); }
    const uint8_t* p = row + x * 4; if (dark) *dark = p[0] < 48 && p[1] < 48 && p[2] < 48; return p[3];
}
static void VigWrite(VigCand& c, bool clear) {
    size_t rows = c.f == D3DFMT_DXT5 ? c.h / 4 : c.h, rowBytes = c.f == D3DFMT_DXT5 ? (c.w / 4) * 16 : c.w * 4;
    uint8_t* d = (uint8_t*)c.bits;
    for (size_t y = 0; y < rows; y++) {
        uint8_t* r = d + (size_t)c.pitch * y;
        if (!clear) { memcpy(r, c.orig.data() + y * rowBytes, rowBytes); continue; }
        if (c.f == D3DFMT_DXT5) for (size_t x = 0; x < rowBytes; x += 16) memcpy(r + x, kClearDXT5, 16);
        else memset(r, 0, rowBytes);
    }
}
static HRESULT STDMETHODCALLTYPE hkTexLock(IDirect3DTexture9* t, UINT lv, D3DLOCKED_RECT* lr, const RECT* rc, DWORD fl) {
    HRESULT hr = oTexLock(t, lv, lr, rc, fl);
    if (SUCCEEDED(hr) && lv == 0 && !rc && lr && !(fl & D3DLOCK_READONLY)) {
        EnterCriticalSection(&g_vigCs);
        if (VigCand* c = VigFind(t)) { c->bits = lr->pBits; c->pitch = lr->Pitch; }
        LeaveCriticalSection(&g_vigCs);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE hkTexUnlock(IDirect3DTexture9* t, UINT lv) {
    if (lv == 0) {
        EnterCriticalSection(&g_vigCs);
        VigCand* c = VigFind(t);
        if (c && c->bits) {
            bool dark = true, d1; float mid = VigAlpha(*c, c->w / 2, c->h / 2, &d1); dark &= d1;
            float cn[4] = { VigAlpha(*c, 0, 0, &d1), 0, 0, 0 }; dark &= d1;
            cn[1] = VigAlpha(*c, c->w - 1, 0, &d1); dark &= d1; cn[2] = VigAlpha(*c, 0, c->h - 1, &d1); dark &= d1; cn[3] = VigAlpha(*c, c->w - 1, c->h - 1, &d1); dark &= d1;
            float cmin = cn[0]; for (float v : cn) if (v < cmin) cmin = v;
            bool vig = dark && mid < 24 && cmin > 40 && cmin > mid + 32;
            static int nNo; if (vig || nNo++ < 3)
                Log("vignette: candidate %p %ux%u fmt %d: centre alpha %.0f, corners %.0f %.0f %.0f %.0f, dark %d -> %s",
                    (void*)t, c->w, c->h, (int)c->f, mid, cn[0], cn[1], cn[2], cn[3], (int)dark, vig ? (g_noVignette ? "VIGNETTE, removed" : "VIGNETTE (kept, RemoveVignette=0)") : "not a vignette");
            if (vig) {
                size_t rows = c->f == D3DFMT_DXT5 ? c->h / 4 : c->h, rowBytes = c->f == D3DFMT_DXT5 ? (c->w / 4) * 16 : c->w * 4;
                c->orig.resize(rows * rowBytes);
                for (size_t y = 0; y < rows; y++) memcpy(&c->orig[y * rowBytes], (uint8_t*)c->bits + (size_t)c->pitch * y, rowBytes);
                c->isVig = true;
                if (g_noVignette && !g_vigShown) VigWrite(*c, true);
            }
            c->bits = nullptr;
        }
        LeaveCriticalSection(&g_vigCs);
    }
    return oTexUnlock(t, lv);
}
static void VignetteTick() {          // at Present: F11 toggles original / removed on every known vignette texture
    static bool prev; bool k = g_devKeys && (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (k && !prev && g_vigCsInit) {
        g_vigShown = !g_vigShown; int n = 0;
        EnterCriticalSection(&g_vigCs);
        for (auto& c : g_vig) if (c.t && c.isVig && !c.orig.empty()) {
            D3DLOCKED_RECT lr; if (SUCCEEDED(oTexLock(c.t, 0, &lr, nullptr, 0))) { c.bits = lr.pBits; c.pitch = lr.Pitch; VigWrite(c, !g_vigShown); c.bits = nullptr; oTexUnlock(c.t, 0); n++; }
        }
        LeaveCriticalSection(&g_vigCs);
        Log("vignette: F11 -> %s (%d textures)", g_vigShown ? "original vignette shown" : "removed", n);
    }
    prev = k;
}
static void VigTrack(IDirect3DTexture9* t, UINT w, UINT h, D3DFORMAT f, DWORD usage) {
    if (!t) return;
    if (!g_vigCsInit) { InitializeCriticalSection(&g_vigCs); g_vigCsInit = true; }
    static bool hooked;
    if (!hooked) { hooked = true; void** vt = *(void***)t;
        Hook(vt[19], (void*)hkTexLock, (void**)&oTexLock, "IDirect3DTexture9::LockRect");
        Hook(vt[20], (void*)hkTexUnlock, (void**)&oTexUnlock, "IDirect3DTexture9::UnlockRect"); }
    EnterCriticalSection(&g_vigCs);
    if (VigCand* old = VigFind(t)) { old->t = nullptr; old->orig.clear(); old->isVig = false; }   // pointer reused by a new texture
    bool cand = w == 512 && h == 256 && (f == D3DFMT_DXT5 || f == D3DFMT_A8R8G8B8) && !(usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL));
    if (cand) {
        VigCand* slot = nullptr; for (auto& c : g_vig) if (!c.t) { slot = &c; break; }
        if (!slot) { static int rr; slot = &g_vig[rr++ % VIGN]; }       // oldest-ish slot; stale ones are harmless
        slot->t = t; slot->f = f; slot->w = w; slot->h = h; slot->bits = nullptr; slot->isVig = false; slot->orig.clear();
    }
    LeaveCriticalSection(&g_vigCs);
}
static HRESULT STDMETHODCALLTYPE hkCreateTex(IDirect3DDevice9* d, UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9** o, HANDLE* sh) {
    bool m = p == D3DPOOL_MANAGED; DWORD u0 = u; ExPool(p, u);
    HRESULT hr = oCreateTex(d, w, h, l, u, f, p, o, sh);
    if (m && g_isEx) { InterlockedIncrement(&g_convTex);
        if (FAILED(hr)) { LOG_FIRST(10, "ex: managed texture %ux%u levels %u fmt %d usage 0x%lx -> default/dynamic failed 0x%08lx; retrying without dynamic", w, h, l, (int)f, u0, (unsigned long)hr);
            hr = oCreateTex(d, w, h, l, u0, f, D3DPOOL_DEFAULT, o, sh); } }
    if (SUCCEEDED(hr) && o) VigTrack(*o, w, h, f, u0);
    if ((u0 & D3DUSAGE_DEPTHSTENCIL)) LOG_FIRST(16, "depth-stencil texture %ux%u format %d", w, h, (int)f);
    return hr;
}
// ---------------------------------------------------------------- build 075: scene depth for DLSS
// The scene depth-stencil (back-buffer size, D24S8) is created as an INTZ texture instead (same 24-bit depth + 8-bit
// stencil, but readable by a shader). After each eye's replay one full-screen D3D9 pass packs the depth into the eye's
// shared BGRA texture (24 bits in R, G, B), which the D3D11 side unpacks for DLSS.
typedef HRESULT(STDMETHODCALLTYPE* CreateDS_t)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
static CreateDS_t oCreateDS;
static bool DepthWanted(UINT w, UINT h, D3DFORMAT f) { return ddlss::Enabled() && (int)w == g_vrW && (int)h == g_vrH && (f == D3DFMT_D24S8 || f == D3DFMT_D24X8); }
static bool MakeIntz(IDirect3DDevice9* d, UINT w, UINT h, IDirect3DTexture9** t, const char* via) {
    const D3DFORMAT INTZ = (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z');
    HRESULT hr = oCreateTex(d, w, h, 1, D3DUSAGE_DEPTHSTENCIL, INTZ, D3DPOOL_DEFAULT, t, nullptr);
    Log("dlss: scene depth %ux%u (%s) -> INTZ texture 0x%08lx", w, h, via, (unsigned long)hr);
    if (FAILED(hr)) { *t = nullptr; return false; }
    if (g_intzN == 4) { g_intz[0].surf->Release(); g_intz[0].tex->Release(); memmove(g_intz, g_intz + 1, 3 * sizeof g_intz[0]); g_intzN = 3; }   // keep the newest 4
    IntzDS& z = g_intz[g_intzN]; z.tex = *t; z.tex->AddRef(); z.tex->GetSurfaceLevel(0, &z.surf); z.draws = 0; g_intzN++;
    return true;
}
static HRESULT STDMETHODCALLTYPE hkCreateDS(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL disc, IDirect3DSurface9** o, HANDLE* sh) {
    LOG_FIRST(16, "depth-stencil surface %ux%u format %d msaa %d discard %d", w, h, (int)f, (int)ms, disc);
    if (o && !sh && ms == D3DMULTISAMPLE_NONE && DepthWanted(w, h, f)) {
        IDirect3DTexture9* t = nullptr;
        if (MakeIntz(d, w, h, &t, "CreateDepthStencilSurface")) { t->GetSurfaceLevel(0, o); t->Release(); return D3D_OK; }   // the surface keeps the texture alive
    }
    return oCreateDS(d, w, h, f, ms, q, disc, o, sh);
}
static IDirect3DPixelShader9* g_encPS; static IDirect3DStateBlock9* g_encSB; static bool g_encBad;
static bool EncInit(IDirect3DDevice9* d) {
    if (g_encPS && g_encSB) return true;
    if (g_encBad) return false;
    g_encBad = true;
    if (!oSetRT || !oSetDS || !oSetVS || !oSetPS || !oSetRS || !oDPUP || !oBegin || !oEnd) { Log("dlss: depth pack pass - a device hook is missing"); return false; }
    static const char hlsl[] =
        "sampler2D s : register(s0);\n"
        "float4 main(float2 uv : TEXCOORD0) : COLOR0 {\n"
        "  float v = floor(tex2D(s, uv).r * 16777215.0 + 0.5);\n"
        "  float r = floor(v / 65536.0); v -= r * 65536.0;\n"
        "  float g = floor(v / 256.0); float b = v - g * 256.0;\n"
        "  return float4(r, g, b, 255.0) / 255.0;\n"
        "}\n";
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    pD3DCompile comp = dc ? (pD3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    ID3DBlob *b = nullptr, *err = nullptr;
    if (!comp || FAILED(comp(hlsl, sizeof hlsl - 1, "ddda_depth_pack", nullptr, nullptr, "main", "ps_2_0", 0, 0, &b, &err))) {
        Log("dlss: depth pack shader: %s", !comp ? "d3dcompiler_47.dll not available" : err ? (const char*)err->GetBufferPointer() : "compile failed");
        if (err) err->Release(); return false;
    }
    HRESULT hr = d->CreatePixelShader((const DWORD*)b->GetBufferPointer(), &g_encPS); b->Release();
    if (FAILED(hr) || FAILED(d->CreateStateBlock(D3DSBT_ALL, &g_encSB))) { Log("dlss: depth pack pass setup failed 0x%08lx", (unsigned long)hr); return false; }
    g_encBad = false;
    Log("dlss: depth pack pass ready");
    return true;
}
static void DepthEncode(IDirect3DDevice9* d, int eye) {
    IDirect3DSurface9* dst = dxr::DepthSurface(eye);
    int best = -1; for (int k = 0; k < g_intzN; k++) if (g_intz[k].draws > 0 && (best < 0 || g_intz[k].draws > g_intz[best].draws)) best = k;
    { static long n; if (dst && (n++ % 900) == 0) { char b[160] = ""; int o = 0; for (int k = 0; k < g_intzN; k++) o += snprintf(b + o, sizeof b - o, " #%d %ld", k, g_intz[k].draws);
        Log("dlss: eye %d draws per INTZ depth this replay:%s -> %d", eye, b, best); } }
    for (int k = 0; k < g_intzN; k++) g_intz[k].draws = 0;
    if (!dst || best < 0 || !EncInit(d)) return;
    IDirect3DTexture9* depthTex = g_intz[best].tex;
    D3DSURFACE_DESC dd; dst->GetDesc(&dd);
    { D3DSURFACE_DESC zd; g_intz[best].surf->GetDesc(&zd); if (zd.Width != dd.Width || zd.Height != dd.Height) { LOG_FIRST(3, "dlss: depth %ux%u differs from the eye picture %ux%u", zd.Width, zd.Height, dd.Width, dd.Height); return; } }
    IDirect3DSurface9* rt[4] = {}; for (DWORD i = 0; i < 4; i++) if (FAILED(d->GetRenderTarget(i, &rt[i]))) rt[i] = nullptr;
    IDirect3DSurface9* ds = nullptr; if (FAILED(d->GetDepthStencilSurface(&ds))) ds = nullptr;
    g_encSB->Capture();
    oSetRT(d, 0, dst); for (DWORD i = 1; i < 4; i++) if (rt[i]) oSetRT(d, i, nullptr);
    oSetDS(d, nullptr);
    oSetVS(d, nullptr); oSetPS(d, g_encPS); d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    d->SetTexture(0, depthTex);   // SetTexture is not hooked (056): straight to the device
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT); d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT); d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    static const D3DRENDERSTATETYPE off[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE,
                                              D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_TWOSIDEDSTENCILMODE };
    for (auto r : off) oSetRS(d, r, FALSE);
    oSetRS(d, D3DRS_CULLMODE, D3DCULL_NONE); oSetRS(d, D3DRS_FILLMODE, D3DFILL_SOLID); oSetRS(d, D3DRS_COLORWRITEENABLE, 0xF);
    const float W = (float)dd.Width - 0.5f, H = (float)dd.Height - 0.5f;
    const float v[4][6] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { W, -0.5f, 0, 1, 1, 0 }, { -0.5f, H, 0, 1, 0, 1 }, { W, H, 0, 1, 1, 1 } };
    bool scene = !g_inScene; if (scene) oBegin(d);
    HRESULT hr = oDPUP(d, D3DPT_TRIANGLESTRIP, 2, v, sizeof v[0]);
    if (scene) oEnd(d);
    LOG_FIRST(2, "dlss: eye %d depth packed -> 0x%08lx%s", eye, (unsigned long)hr, scene ? " (own scene)" : "");
    oSetRT(d, 0, rt[0]); for (DWORD i = 1; i < 4; i++) if (rt[i]) oSetRT(d, i, rt[i]);
    oSetDS(d, ds);
    g_encSB->Apply();
    for (auto x : rt) if (x) x->Release(); if (ds) ds->Release();
}
static HRESULT STDMETHODCALLTYPE hkCreateVol(IDirect3DDevice9* d, UINT w, UINT h, UINT dp, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DVolumeTexture9** o, HANDLE* sh) {
    bool m = p == D3DPOOL_MANAGED; ExPool(p, u);
    HRESULT hr = oCreateVol(d, w, h, dp, l, u, f, p, o, sh);
    if (m && g_isEx) { InterlockedIncrement(&g_convVol); if (FAILED(hr)) LOG_FIRST(5, "ex: managed volume texture %ux%ux%u fmt %d failed 0x%08lx", w, h, dp, (int)f, (unsigned long)hr); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE hkCreateCube(IDirect3DDevice9* d, UINT e, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DCubeTexture9** o, HANDLE* sh) {
    bool m = p == D3DPOOL_MANAGED; ExPool(p, u);
    HRESULT hr = oCreateCube(d, e, l, u, f, p, o, sh);
    if (m && g_isEx) { InterlockedIncrement(&g_convCube); if (FAILED(hr)) LOG_FIRST(5, "ex: managed cube texture %u fmt %d failed 0x%08lx", e, (int)f, (unsigned long)hr); }
    return hr;
}
typedef HRESULT(STDMETHODCALLTYPE* BufLock_t)(void*, UINT, UINT, void**, DWORD); typedef HRESULT(STDMETHODCALLTYPE* BufUnlock_t)(void*);
static BufLock_t oVBLock, oIBLock; static BufUnlock_t oVBUnlock, oIBUnlock;
static HRESULT STDMETHODCALLTYPE hkVBLock(void* b, UINT off, UINT size, void** pp, DWORD f) { HRESULT hr = oVBLock(b, off, size, pp, f); if (SUCCEEDED(hr) && pp) *pp = ShLocked(b, off, size, *pp, f); return hr; }
static HRESULT STDMETHODCALLTYPE hkVBUnlock(void* b) { ShUnlocking(b); return oVBUnlock(b); }
static HRESULT STDMETHODCALLTYPE hkIBLock(void* b, UINT off, UINT size, void** pp, DWORD f) { HRESULT hr = oIBLock(b, off, size, pp, f); if (SUCCEEDED(hr) && pp) *pp = ShLocked(b, off, size, *pp, f); return hr; }
static HRESULT STDMETHODCALLTYPE hkIBUnlock(void* b) { ShUnlocking(b); return oIBUnlock(b); }
static void ShHookBuf(void* obj, bool vb) {   // Lock = vtable 11, Unlock = 12 (IDirect3DVertexBuffer9 / IDirect3DIndexBuffer9)
    static bool doneV, doneI; if (!obj || !g_useShadow || (vb ? doneV : doneI)) return; (vb ? doneV : doneI) = true;
    void** vt = *(void***)obj;
    if (vb) { Hook(vt[11], (void*)hkVBLock, (void**)&oVBLock, "IDirect3DVertexBuffer9::Lock"); Hook(vt[12], (void*)hkVBUnlock, (void**)&oVBUnlock, "IDirect3DVertexBuffer9::Unlock"); }
    else { Hook(vt[11], (void*)hkIBLock, (void**)&oIBLock, "IDirect3DIndexBuffer9::Lock"); Hook(vt[12], (void*)hkIBUnlock, (void**)&oIBUnlock, "IDirect3DIndexBuffer9::Unlock"); }
}
static HRESULT STDMETHODCALLTYPE hkCreateVB(IDirect3DDevice9* d, UINT len, DWORD u, DWORD fvf, D3DPOOL p, IDirect3DVertexBuffer9** o, HANDLE* sh) {
    bool m = p == D3DPOOL_MANAGED; bool dyn = (u & D3DUSAGE_DYNAMIC) != 0; ExPool(p, u);
    HRESULT hr = oCreateVB(d, len, u, fvf, p, o, sh);
    if (SUCCEEDED(hr) && o && *o) { ShRegister(*o, len, dyn, D3DFMT_UNKNOWN); ShHookBuf(*o, true); }
    if (m && g_isEx) { InterlockedIncrement(&g_convVB); if (FAILED(hr)) LOG_FIRST(5, "ex: managed vertex buffer %u failed 0x%08lx", len, (unsigned long)hr); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE hkCreateIB(IDirect3DDevice9* d, UINT len, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DIndexBuffer9** o, HANDLE* sh) {
    bool m = p == D3DPOOL_MANAGED; bool dyn = (u & D3DUSAGE_DYNAMIC) != 0; ExPool(p, u);
    HRESULT hr = oCreateIB(d, len, u, f, p, o, sh);
    if (SUCCEEDED(hr) && o && *o) { ShRegister(*o, len, dyn, f == D3DFMT_INDEX32 ? D3DFMT_INDEX32 : D3DFMT_INDEX16); ShHookBuf(*o, false); }
    if (m && g_isEx) { InterlockedIncrement(&g_convIB); if (FAILED(hr)) LOG_FIRST(5, "ex: managed index buffer %u failed 0x%08lx", len, (unsigned long)hr); }
    return hr;
}
// build 055: timing thunks around the real device calls
static VSC_t rVSC; static HRESULT STDMETHODCALLTYPE tVSC(IDirect3DDevice9* d, UINT a, const float* b, UINT c) { HPReal _r(HP_VSC); return rVSC(d, a, b, c); }
static VSC_t rPSC; static HRESULT STDMETHODCALLTYPE tPSC(IDirect3DDevice9* d, UINT a, const float* b, UINT c) { HPReal _r(HP_PSC); return rPSC(d, a, b, c); }
static DIP_t rDIP; static HRESULT STDMETHODCALLTYPE tDIP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT a, UINT b, UINT c, UINT e, UINT f) { HPReal _r(HP_DIP); return rDIP(d, t, a, b, c, e, f); }
static DP_t rDP; static HRESULT STDMETHODCALLTYPE tDP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT a, UINT b) { HPReal _r(HP_DP); return rDP(d, t, a, b); }
static SetRT_t rSetRT; static HRESULT STDMETHODCALLTYPE tSetRT(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s) { HPReal _r(HP_SetRT); return rSetRT(d, i, s); }
static SetTex_t rSetTex; static HRESULT STDMETHODCALLTYPE tSetTex(IDirect3DDevice9* d, DWORD a, IDirect3DBaseTexture9* t) { HPReal _r(HP_SetTex); return rSetTex(d, a, t); }
static SetRS_t rSetRS; static HRESULT STDMETHODCALLTYPE tSetRS(IDirect3DDevice9* d, D3DRENDERSTATETYPE a, DWORD v) { HPReal _r(HP_SetRS); return rSetRS(d, a, v); }
static SetSS_t rSetSS; static HRESULT STDMETHODCALLTYPE tSetSS(IDirect3DDevice9* d, UINT n, IDirect3DVertexBuffer9* vb, UINT o, UINT st) { HPReal _r(HP_SetSS); return rSetSS(d, n, vb, o, st); }
static SetVS_t rSetVS; static HRESULT STDMETHODCALLTYPE tSetVS(IDirect3DDevice9* d, IDirect3DVertexShader9* p) { HPReal _r(HP_SetVS); return rSetVS(d, p); }
static SetPS_t rSetPS; static HRESULT STDMETHODCALLTYPE tSetPS(IDirect3DDevice9* d, IDirect3DPixelShader9* p) { HPReal _r(HP_SetPS); return rSetPS(d, p); }
static Clear_t rClear; static HRESULT STDMETHODCALLTYPE tClear(IDirect3DDevice9* d, DWORD n, const D3DRECT* r, DWORD f, D3DCOLOR c, float z, DWORD st) { HPReal _r(HP_Clear); return rClear(d, n, r, f, c, z, st); }
static SetVP_t rSetVP; static HRESULT STDMETHODCALLTYPE tSetVP(IDirect3DDevice9* d, const D3DVIEWPORT9* v) { HPReal _r(HP_SetVP); return rSetVP(d, v); }
static SetSc_t rSetSc; static HRESULT STDMETHODCALLTYPE tSetSc(IDirect3DDevice9* d, const RECT* r) { HPReal _r(HP_SetSc); return rSetSc(d, r); }
static SetDS_t rSetDS; static HRESULT STDMETHODCALLTYPE tSetDS(IDirect3DDevice9* d, IDirect3DSurface9* sf) { HPReal _r(HP_SetDS); return rSetDS(d, sf); }
static void HookDevice(IDirect3DDevice9* d) {
    static bool done; if (done) return; done = true;
    void** vt = *(void***)d;
    Hook(vt[16], (void*)hkReset, (void**)&oReset, "Reset");
    Hook(vt[23], (void*)hkCreateTex, (void**)&oCreateTex, "CreateTexture");
    if (ddlss::Enabled()) Hook(vt[29], (void*)hkCreateDS, (void**)&oCreateDS, "CreateDepthStencilSurface");   // build 075
    Hook(vt[24], (void*)hkCreateVol, (void**)&oCreateVol, "CreateVolumeTexture");
    Hook(vt[25], (void*)hkCreateCube, (void**)&oCreateCube, "CreateCubeTexture");
    Hook(vt[26], (void*)hkCreateVB, (void**)&oCreateVB, "CreateVertexBuffer");
    Hook(vt[27], (void*)hkCreateIB, (void**)&oCreateIB, "CreateIndexBuffer");
    Hook(vt[17], (void*)hkPresent, (void**)&oPresent, "Present");
    if (g_game) {   // build 094: the code is checked here (the Steam exe is only decrypted by its SteamStub after our DllMain)
        static const uint8_t pro[3][6] = { { 0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0 }, { 0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0 }, { 0x56, 0x8b, 0x74, 0x24, 0x08, 0x57 } };
        const uint32_t at[3] = { g_game->camSetup, g_game->draw, g_game->ctx }; bool ok = true;
        for (int i = 0; i < 3; i++) ok = ok && !memcmp((void*)(uintptr_t)at[i], pro[i], 6);
        if (!ok) { Log("game: %s version, but its camera / draw code is not where expected - camera and stereo hooks OFF", g_game->name); g_game = nullptr; }
    }
    if (g_game) {   // build 094: per game version
        Hook((void*)(uintptr_t)g_game->camSetup, (void*)hkCamSetup, (void**)&oCamSetup, "game camera setup");
        Hook((void*)(uintptr_t)g_game->draw, (void*)hkDraw, (void**)&oDraw, "game render-thread draw (stereo replay)");
        if (GetPrivateProfileIntW(L"VR", L"R1SkipPrefix", 0, g_ini)) Hook((void*)(uintptr_t)g_game->ctx, (void*)hkCtx, (void**)&oCtx, "game command walker setup (replay 1 prefix skip)");
        {   wchar_t b[32]; GetPrivateProfileStringW(L"VR", L"DrawDistance", L"1", b, 32, g_ini);   // build 095
            float m = (float)_wtof(b); if (!(m >= 1.0f)) m = 1.0f; if (m > 3.0f) m = 3.0f; g_ddViewRange = GetPrivateProfileIntW(L"VR", L"ViewRangeScale", 0, g_ini); DdSet(m);
            DdInstall(g_game->ts); }
    }
    Hook(vt[34], (void*)hkSR, (void**)&oSR, "StretchRect");
    Hook(vt[37], (void*)hkSetRT, (void**)&oSetRT, "SetRenderTarget");
    Hook(vt[39], (void*)hkSetDS, (void**)&oSetDS, "SetDepthStencilSurface");
    Hook(vt[47], (void*)hkSetVP, (void**)&oSetVP, "SetViewport");
    Hook(vt[75], (void*)hkSetSc, (void**)&oSetSc, "SetScissorRect"); oSetScF = oSetSc;
    Hook(vt[91], (void*)hkCreateVS, (void**)&oCreateVS, "CreateVertexShader");
    Hook(vt[41], (void*)hkBegin, (void**)&oBegin, "BeginScene");
    Hook(vt[42], (void*)hkEnd, (void**)&oEnd, "EndScene");
    Hook(vt[43], (void*)hkClear, (void**)&oClear, "Clear");
    Hook(vt[81], (void*)hkDP, (void**)&oDP, "DrawPrimitive");
    Hook(vt[82], (void*)hkDIP, (void**)&oDIP, "DrawIndexedPrimitive");
    Hook(vt[83], (void*)hkDPUP, (void**)&oDPUP, "DrawPrimitiveUP");
    Hook(vt[84], (void*)hkDIPUP, (void**)&oDIPUP, "DrawIndexedPrimitiveUP");
    Hook(vt[94], (void*)hkVSC, (void**)&oVSC, "SetVertexShaderConstantF");
    Hook(vt[57], (void*)hkSetRS, (void**)&oSetRS, "SetRenderState");
    // build 056: SetTexture / SetStreamSource are no longer hooked (about 30000 calls a frame); the few places that need the
    // current textures or vertex stream read them from the device (RefreshTexState / RefreshStream)
    Hook(vt[92], (void*)hkSetVS, (void**)&oSetVS, "SetVertexShader");
    Hook(vt[107], (void*)hkSetPS, (void**)&oSetPS, "SetPixelShader");
    Hook(vt[106], (void*)hkCreatePS, (void**)&oCreatePS, "CreatePixelShader");
    Hook(vt[109], (void*)hkPSC, (void**)&oPSC, "SetPixelShaderConstantF");
    // build 055: route the real calls through timing thunks (ini ProfileHooks=1 only)
    if (g_prof) {
    rVSC = oVSC; oVSC = tVSC;
    rPSC = oPSC; oPSC = tPSC;
    rDIP = oDIP; oDIP = tDIP;
    rDP = oDP; oDP = tDP;
    rSetRT = oSetRT; oSetRT = tSetRT;
    rSetRS = oSetRS; oSetRS = tSetRS;
    rSetVS = oSetVS; oSetVS = tSetVS;
    rSetPS = oSetPS; oSetPS = tSetPS;
    rClear = oClear; oClear = tClear;
    rSetVP = oSetVP; oSetVP = tSetVP;
    rSetSc = oSetSc; oSetSc = tSetSc;
    rSetDS = oSetDS; oSetDS = tSetDS;
    }
}

static void DumpExe();
// ---------------------------------------------------------------- IDirect3D9::CreateDevice
typedef HRESULT(STDMETHODCALLTYPE* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
static CreateDevice_t oCreateDevice;
static HRESULT STDMETHODCALLTYPE hkCreateDevice(IDirect3D9* self, UINT ad, D3DDEVTYPE t, HWND w, DWORD flags, D3DPRESENT_PARAMETERS* p, IDirect3DDevice9** out) {
    g_devTid = GetCurrentThreadId();
    D3DADAPTER_IDENTIFIER9 id = {};
    self->GetAdapterIdentifier(ad, 0, &id);
    Log("CreateDevice: adapter %u '%s', type %d, window %p, behaviour flags 0x%lx%s%s%s", ad, id.Description, (int)t, w, flags,
        (flags & D3DCREATE_MULTITHREADED) ? " MULTITHREADED" : "", (flags & D3DCREATE_HARDWARE_VERTEXPROCESSING) ? " HWVP" : "",
        (flags & D3DCREATE_PUREDEVICE) ? " PURE" : "");
    // build 054 perf test (ini NoMultithreaded=1): drop D3DCREATE_MULTITHREADED - saves the runtime's lock on every device call,
    // but only safe if the game never calls the device from two threads at once
    if (g_noMt && (flags & D3DCREATE_MULTITHREADED)) { flags &= ~(DWORD)D3DCREATE_MULTITHREADED; Log("CreateDevice: MULTITHREADED removed (NoMultithreaded=1, perf test)"); }
    if (p) { LogPP("CreateDevice (game)", p); ApplyOverrides(p); if (g_forceW || g_forceWindowed || g_noVsync) LogPP("CreateDevice (used)", p); }
    RECT rc; if (w && GetClientRect(w, &rc)) Log("window client %ldx%ld", rc.right - rc.left, rc.bottom - rc.top);
    HRESULT hr;
    if (g_isEx) {
        D3DDISPLAYMODEEX fm = { sizeof fm };
        if (p && !p->Windowed) { fm.Width = p->BackBufferWidth; fm.Height = p->BackBufferHeight; fm.RefreshRate = p->FullScreen_RefreshRateInHz; fm.Format = p->BackBufferFormat; fm.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE; }
        hr = ((IDirect3D9Ex*)self)->CreateDeviceEx(ad, t, w, flags, p, (p && !p->Windowed) ? &fm : nullptr, (IDirect3DDevice9Ex**)out);
        Log("CreateDeviceEx (the game asked for CreateDevice) -> 0x%08lx", (unsigned long)hr);
    } else hr = oCreateDevice(self, ad, t, w, flags, p, out);
    Log("CreateDevice -> 0x%08lx device %p", (unsigned long)hr, (out && SUCCEEDED(hr)) ? (void*)*out : nullptr);
    if (SUCCEEDED(hr) && out && *out) HookDevice(*out);
    DumpExe();
    LogFlush();
    return hr;
}
// ---------------------------------------------------------------- build 003: extra display mode for the headset eye size
typedef UINT(STDMETHODCALLTYPE* ModeCount_t)(IDirect3D9*, UINT, D3DFORMAT);
typedef HRESULT(STDMETHODCALLTYPE* EnumModes_t)(IDirect3D9*, UINT, D3DFORMAT, UINT, D3DDISPLAYMODE*);
static ModeCount_t oModeCount; static EnumModes_t oEnumModes;
static UINT STDMETHODCALLTYPE hkModeCount(IDirect3D9* s, UINT a, D3DFORMAT f) {
    UINT n = oModeCount(s, a, f);
    return (g_vrW > 0 && a == 0) ? n + 1 : n;
}
static HRESULT STDMETHODCALLTYPE hkEnumModes(IDirect3D9* s, UINT a, D3DFORMAT f, UINT m, D3DDISPLAYMODE* out) {
    UINT n = oModeCount(s, a, f);
    if (g_vrW > 0 && a == 0 && m == n && out) {
        out->Width = g_vrW; out->Height = g_vrH; out->RefreshRate = 60; out->Format = f;
        LOG_FIRST(3, "display modes: added %dx%d (format %d)", g_vrW, g_vrH, (int)f);
        return D3D_OK;
    }
    return oEnumModes(s, a, f, m, out);
}


// ---------------------------------------------------------------- build 004: desktop size as the game sees it
// Windowed mode seems to clamp the resolution to the desktop; report a desktop at least as large as the VR mode.
typedef int(WINAPI* GSM_t)(int);
typedef BOOL(WINAPI* SPI_t)(UINT, UINT, PVOID, UINT);
static GSM_t oGSM; static SPI_t oSPI;
static int WINAPI hkGSM(int i) {
    int v = oGSM(i);
    int nv = v;
    if (g_vrW > 0) {
        if ((i == SM_CXSCREEN || i == SM_CXFULLSCREEN || i == SM_CXVIRTUALSCREEN || i == SM_CXMAXIMIZED || i == SM_CXMAXTRACK) && v < g_vrW) nv = g_vrW + (i == SM_CXMAXTRACK ? 64 : 0);
        if ((i == SM_CYSCREEN || i == SM_CYFULLSCREEN || i == SM_CYVIRTUALSCREEN || i == SM_CYMAXIMIZED || i == SM_CYMAXTRACK) && v < g_vrH) nv = g_vrH + (i == SM_CYMAXTRACK ? 64 : 0);
    }
    static int logged[100]; if (i >= 0 && i < 100 && logged[i]++ < 2) Log("GetSystemMetrics(%d) = %d%s%d", i, v, nv != v ? " -> reported " : "", nv != v ? nv : 0);
    return nv;
}
static BOOL WINAPI hkSPI(UINT a, UINT p, PVOID v, UINT w) {
    BOOL r = oSPI(a, p, v, w);
    if (r && a == SPI_GETWORKAREA && v && g_vrW > 0) {
        RECT* rc = (RECT*)v;
        RECT old = *rc;
        if (rc->right - rc->left < g_vrW) rc->right = rc->left + g_vrW;
        if (rc->bottom - rc->top < g_vrH) rc->bottom = rc->top + g_vrH;
        LOG_FIRST(3, "SystemParametersInfoA(SPI_GETWORKAREA) %ld,%ld-%ld,%ld -> %ld,%ld-%ld,%ld", old.left, old.top, old.right, old.bottom, rc->left, rc->top, rc->right, rc->bottom);
    } else LOG_FIRST(10, "SystemParametersInfoA(0x%x)", a);
    return r;
}
// build 005: the game sizes its back buffer from GetClientRect(main window) at startup (BbsRpg+0xdb1629).
typedef BOOL(WINAPI* GCR_t)(HWND, LPRECT);
static GCR_t oGCR; static int g_clientVR = 1;
static bool IsGameTopWindow(HWND h) {
    if (!h || GetAncestor(h, GA_ROOT) != h || GetWindow(h, GW_OWNER)) return false;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    return pid == GetCurrentProcessId();
}
static BOOL WINAPI hkGCR(HWND h, LPRECT rc) {
    BOOL r = oGCR(h, rc);
    if (r && rc && g_clientVR && g_vrW > 0 && IsGameTopWindow(h)) {
        RECT old = *rc;
        rc->left = 0; rc->top = 0; rc->right = g_vrW; rc->bottom = g_vrH;
        LOG_FIRST(12, "GetClientRect(%p) from %p: %ldx%ld -> reported %dx%d", h, __builtin_return_address(0), old.right - old.left, old.bottom - old.top, g_vrW, g_vrH);
    }
    return r;
}
// build 039: glow / glare reduction without touching the game files: when NoGlow=1 the game's opening of
// nativePC\rom\bbsrpg_core.arc is redirected to ddda_vr_mods\bbsrpg_core_noglow(_gog).arc, a copy of the player's own
// archive in which the launcher has dimmed the two effect textures t_cm000_00_GM / t_cm040_00_GM (launcher game.cpp
// BuildNoGlow; same algorithm in mod/tools/make_noglow2.py). Missing copy = the game's own glare.
static wchar_t g_glowArcW[MAX_PATH]; static char g_glowArcA[MAX_PATH]; static bool g_glowOk;
typedef HANDLE(WINAPI* CFW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI* CFA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CFW_t oCFW; static CFA_t oCFA; static long g_arcOpens;
static bool EndsWithI(const char* s, const char* t) { size_t a = strlen(s), b = strlen(t); return a >= b && !_stricmp(s + a - b, t); }
static bool EndsWithIW(const wchar_t* s, const wchar_t* t) { size_t a = wcslen(s), b = wcslen(t); return a >= b && !_wcsicmp(s + a - b, t); }
static HANDLE WINAPI hkCFW(LPCWSTR n, DWORD a, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD cd, DWORD fl, HANDLE t) {
    if (n && g_glowOk && EndsWithIW(n, L"bbsrpg_core.arc") && (EndsWithIW(n, L"\\rom\\bbsrpg_core.arc") || EndsWithIW(n, L"/rom/bbsrpg_core.arc"))) {
        Log("noglow: %ls -> %ls", n, g_glowArcW); n = g_glowArcW;
    } else if (n && g_arcOpens < 6 && EndsWithIW(n, L".arc")) { g_arcOpens++; Log("file: CreateFileW %ls", n); }
    return oCFW(n, a, sh, sa, cd, fl, t);
}
static HANDLE WINAPI hkCFA(LPCSTR n, DWORD a, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD cd, DWORD fl, HANDLE t) {
    if (n && g_glowOk && (EndsWithI(n, "\\rom\\bbsrpg_core.arc") || EndsWithI(n, "/rom/bbsrpg_core.arc"))) {
        Log("noglow: %s -> %s", n, g_glowArcA); n = g_glowArcA;
    } else if (n && g_arcOpens < 6 && EndsWithI(n, ".arc")) { g_arcOpens++; Log("file: CreateFileA %s", n); }
    return oCFA(n, a, sh, sa, cd, fl, t);
}
static void HookFiles() {
    wchar_t dir[MAX_PATH]; wcscpy(dir, g_ini); wchar_t* sl = wcsrchr(dir, L'\\'); if (sl) sl[1] = 0;
    swprintf(g_glowArcW, MAX_PATH, L"%lsddda_vr_mods\\%ls", dir, g_game ? g_game->noGlowArc : L"bbsrpg_core_noglow.arc");   // build 094: per version
    WideCharToMultiByte(CP_ACP, 0, g_glowArcW, -1, g_glowArcA, MAX_PATH, nullptr, nullptr);
    g_glowOk = g_noGlow && GetFileAttributesW(g_glowArcW) != INVALID_FILE_ATTRIBUTES;
    Log("noglow: NoGlow=%d, replacement archive %ls", g_noGlow, g_glowOk ? L"found" : L"not used");
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    Hook((void*)GetProcAddress(k, "CreateFileW"), (void*)hkCFW, (void**)&oCFW, "kernel32!CreateFileW");
    Hook((void*)GetProcAddress(k, "CreateFileA"), (void*)hkCFA, (void**)&oCFA, "kernel32!CreateFileA");
}
static void HookDesktop() {
    HMODULE u = GetModuleHandleW(L"user32.dll"); if (!u) u = LoadLibraryW(L"user32.dll");
    g_clientVR = GetPrivateProfileIntW(L"VR", L"ClientRectVR", 1, g_ini);
    if (g_clientVR) Hook((void*)GetProcAddress(u, "GetClientRect"), (void*)hkGCR, (void**)&oGCR, "user32!GetClientRect");
    if (!GetPrivateProfileIntW(L"VR", L"BigDesktop", 0, g_ini)) return;
    Hook((void*)GetProcAddress(u, "GetSystemMetrics"), (void*)hkGSM, (void**)&oGSM, "user32!GetSystemMetrics");
    Hook((void*)GetProcAddress(u, "SystemParametersInfoA"), (void*)hkSPI, (void**)&oSPI, "user32!SystemParametersInfoA");
}

// ---------------------------------------------------------------- build 003: one-time image of the unpacked game executable
static void DumpExe() {
    static bool done; if (done) return; done = true;
    if (!GetPrivateProfileIntW(L"VR", L"DumpExe", 0, g_ini)) return;
    uint8_t* base = (uint8_t*)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS32*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    DWORD size = nt->OptionalHeader.SizeOfImage;
    wchar_t path[MAX_PATH]; wcscpy(path, g_ini); wchar_t* sl = wcsrchr(path, L'\\'); if (sl) sl[1] = 0; wcscat(path, L"ddda_dump.bin");
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { Log("dump: cannot create the file"); return; }
    DWORD written = 0, total = 0;
    static uint8_t zero[4096];
    for (DWORD off = 0; off < size; off += 4096) {   // page by page: unreadable pages become zeros
        MEMORY_BASIC_INFORMATION mb; VirtualQuery(base + off, &mb, sizeof mb);
        bool ok = mb.State == MEM_COMMIT && !(mb.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        WriteFile(f, ok ? base + off : zero, 4096, &written, nullptr); total += written;
    }
    CloseHandle(f);
    Log("dump: game image %p size 0x%lx written to ddda_dump.bin (%lu bytes)", base, size, total);
}

static void HookD3D(IDirect3D9* d3d) {
    static bool done; if (done || !d3d) return; done = true;
    void** vt = *(void***)d3d;
    Hook(vt[6], (void*)hkModeCount, (void**)&oModeCount, "IDirect3D9::GetAdapterModeCount");
    Hook(vt[7], (void*)hkEnumModes, (void**)&oEnumModes, "IDirect3D9::EnumAdapterModes");
    Hook(vt[16], (void*)hkCreateDevice, (void**)&oCreateDevice, "IDirect3D9::CreateDevice");
}

// ---------------------------------------------------------------- exports
extern "C" {
IDirect3D9* WINAPI Direct3DCreate9(UINT sdk) {
    auto f = Real<IDirect3D9*(WINAPI*)(UINT)>("Direct3DCreate9");
    IDirect3D9* d = nullptr;
    if (g_useEx) {   // build 018: hand the game a D3D9Ex object (it is an IDirect3D9 too)
        auto fx = Real<HRESULT(WINAPI*)(UINT, IDirect3D9Ex**)>("Direct3DCreate9Ex");
        IDirect3D9Ex* ex = nullptr;
        HRESULT hr = fx ? fx(sdk, &ex) : E_FAIL;
        if (SUCCEEDED(hr) && ex) { d = ex; g_isEx = true; }
        Log("Direct3DCreate9(%u): using Direct3DCreate9Ex -> 0x%08lx %p", sdk, (unsigned long)hr, (void*)ex);
    }
    if (!d) d = f ? f(sdk) : nullptr;
    Log("Direct3DCreate9(%u) -> %p%s", sdk, d, g_isEx ? " (D3D9Ex)" : "");
    HookD3D(d);
    return d;
}
HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out) {
    auto f = Real<HRESULT(WINAPI*)(UINT, IDirect3D9Ex**)>("Direct3DCreate9Ex");
    HRESULT hr = f ? f(sdk, out) : E_FAIL;
    Log("Direct3DCreate9Ex(%u) -> 0x%08lx", sdk, (unsigned long)hr);
    if (SUCCEEDED(hr) && out) HookD3D(*out);
    return hr;
}
int WINAPI D3DPERF_BeginEvent(D3DCOLOR c, LPCWSTR n) { auto f = Real<int(WINAPI*)(D3DCOLOR, LPCWSTR)>("D3DPERF_BeginEvent"); return f ? f(c, n) : 0; }
int WINAPI D3DPERF_EndEvent() { auto f = Real<int(WINAPI*)()>("D3DPERF_EndEvent"); return f ? f() : 0; }
DWORD WINAPI D3DPERF_GetStatus() { auto f = Real<DWORD(WINAPI*)()>("D3DPERF_GetStatus"); return f ? f() : 0; }
BOOL WINAPI D3DPERF_QueryRepeatFrame() { auto f = Real<BOOL(WINAPI*)()>("D3DPERF_QueryRepeatFrame"); return f ? f() : FALSE; }
void WINAPI D3DPERF_SetMarker(D3DCOLOR c, LPCWSTR n) { auto f = Real<void(WINAPI*)(D3DCOLOR, LPCWSTR)>("D3DPERF_SetMarker"); if (f) f(c, n); }
void WINAPI D3DPERF_SetOptions(DWORD o) { auto f = Real<void(WINAPI*)(DWORD)>("D3DPERF_SetOptions"); if (f) f(o); }
void WINAPI D3DPERF_SetRegion(D3DCOLOR c, LPCWSTR n) { auto f = Real<void(WINAPI*)(D3DCOLOR, LPCWSTR)>("D3DPERF_SetRegion"); if (f) f(c, n); }
void* WINAPI Direct3DShaderValidatorCreate9() { auto f = Real<void*(WINAPI*)()>("Direct3DShaderValidatorCreate9"); return f ? f() : nullptr; }
void WINAPI DebugSetMute() { auto f = Real<void(WINAPI*)()>("DebugSetMute"); if (f) f(); }
void WINAPI PSGPError() { auto f = Real<void(WINAPI*)()>("PSGPError"); if (f) f(); }
void WINAPI PSGPSampleTexture() { auto f = Real<void(WINAPI*)()>("PSGPSampleTexture"); if (f) f(); }
void WINAPI Direct3D9EnableMaximizedWindowedModeShim(UINT a) { auto f = Real<void(WINAPI*)(UINT)>("Direct3D9EnableMaximizedWindowedModeShim"); if (f) f(a); }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(h);
    wchar_t dir[MAX_PATH]; GetModuleFileNameW(h, dir, MAX_PATH);
    wchar_t* sl = wcsrchr(dir, L'\\'); if (sl) sl[1] = 0;
    wchar_t logp[MAX_PATH]; swprintf(logp, MAX_PATH, L"%lsddda_vr.log", dir);
    swprintf(g_ini, MAX_PATH, L"%lsddda_vr.ini", dir);
    LogInit(logp);
    QueryPerformanceFrequency(&g_qpf);
    g_startMs = NowMs();
    g_fullRect = GetPrivateProfileIntW(L"VR", L"FullRect", 1, g_ini);
    g_killBand = GetPrivateProfileIntW(L"VR", L"KillBandEffect", 1, g_ini);
    g_noVignette = GetPrivateProfileIntW(L"VR", L"RemoveVignette", 1, g_ini);
    g_dumpPost = GetPrivateProfileIntW(L"VR", L"DumpPostPasses", 0, g_ini);
    g_hideOverlay = GetPrivateProfileIntW(L"VR", L"HideScreenOverlay", 0, g_ini);
    g_pixelUi = GetPrivateProfileIntW(L"VR", L"PixelSpaceUi", 1, g_ini);
    g_killDof = GetPrivateProfileIntW(L"VR", L"KillDOF", 1, g_ini);
    g_killGodRays = GetPrivateProfileIntW(L"VR", L"KillGodRays", 1, g_ini);
    g_shadowSync = GetPrivateProfileIntW(L"VR", L"StereoShadowSync", 0, g_ini);
    { wchar_t b[32]; GetPrivateProfileStringW(L"VR", L"CamPosTolerance", L"16", b, 32, g_ini); float t = (float)_wtof(b); if (t > 0 && t <= 32) dxr::g_camTol = t; }
    g_hideFlare = GetPrivateProfileIntW(L"VR", L"HideLensFlare", 1, g_ini);
    g_noGlow = GetPrivateProfileIntW(L"VR", L"NoGlow", 1, g_ini);
    g_hideBandOverlay = GetPrivateProfileIntW(L"VR", L"HideHurtOverlay", 1, g_ini);
    g_hideParked = GetPrivateProfileIntW(L"VR", L"HideParkedIcon", 1, g_ini);
    g_primShift = GetPrivateProfileIntW(L"VR", L"ShiftPixelPrims", 1, g_ini);
    g_uiClip = GetPrivateProfileIntW(L"VR", L"UiClipBand", 1, g_ini);
    g_postAll = GetPrivateProfileIntW(L"VR", L"DumpWholeFrame", 0, g_ini);
    dxr::Configure(g_ini); dxr::ConfigureTracking(g_ini);
    g_useEx = GetPrivateProfileIntW(L"VR", L"UseD3D9Ex", 1, g_ini);
    g_forceW = GetPrivateProfileIntW(L"VR", L"ForceWidth", 0, g_ini);
    g_forceH = GetPrivateProfileIntW(L"VR", L"ForceHeight", 0, g_ini);
    g_forceWindowed = GetPrivateProfileIntW(L"VR", L"ForceWindowed", 0, g_ini);
    g_noVsync = GetPrivateProfileIntW(L"VR", L"NoVsync", 1, g_ini);
    g_noMt = GetPrivateProfileIntW(L"VR", L"NoMultithreaded", 0, g_ini);
    g_prof = GetPrivateProfileIntW(L"VR", L"ProfileHooks", 0, g_ini);
    g_devKeys = GetPrivateProfileIntW(L"VR", L"DevKeys", 0, g_ini); g_perfStats = GetPrivateProfileIntW(L"VR", L"PerfStats", 0, g_ini);   // build 074
    g_useShadow = GetPrivateProfileIntW(L"VR", L"ShadowPrimBuffers", 1, g_ini);   // build 064
    g_r1Prefix = GetPrivateProfileIntW(L"VR", L"R1SkipPrefix", 0, g_ini);   // build 067
    g_keepVrCam = GetPrivateProfileIntW(L"VR", L"KeepVrCamera", 2, g_ini);
    {   // build 081: UI adjustments
        wchar_t b[32];
        GetPrivateProfileStringW(L"VR", L"SubtitleRaise", L"190", b, 32, g_ini); g_subRaise = (float)_wtof(b);
        GetPrivateProfileStringW(L"VR", L"SubtitleDepth", L"2.0", b, 32, g_ini); g_subDepth = (float)_wtof(b);
        GetPrivateProfileStringW(L"VR", L"DialogueScale", L"1.3", b, 32, g_ini); g_dlgScale = (float)_wtof(b); if (g_dlgScale < 0.5f || g_dlgScale > 2.0f) g_dlgScale = 1.3f;
        g_reticle = GetPrivateProfileIntW(L"VR", L"ReticleStyle", 2, g_ini);
        g_worldGui = GetPrivateProfileIntW(L"VR", L"WorldMarkers", 1, g_ini) ? 1 : 0;   // build 087
        GetPrivateProfileStringW(L"VR", L"WorldMarkerScale", L"1.0", b, 32, g_ini); g_wmScale = (float)_wtof(b); if (g_wmScale < 0.2f || g_wmScale > 4.0f) g_wmScale = 1.0f;
        GetPrivateProfileStringW(L"VR", L"WorldMarkerLift", L"0", b, 32, g_ini); g_wmLift = (float)_wtof(b);
        Log("ui: SubtitleRaise %.0f, SubtitleDepth %.1f m, DialogueScale %.2f, ReticleStyle %d", g_subRaise, g_subDepth, g_dlgScale, g_reticle);
    }   // build 069 culling test
    Log("camera: KeepVrCamera %d", g_keepVrCam);
    g_vrW = GetPrivateProfileIntW(L"VR", L"VRModeWidth", 3072, g_ini); g_vrH = GetPrivateProfileIntW(L"VR", L"VRModeHeight", 3264, g_ini);
    ddlss::Configure(g_ini);   // build 075: DLSS upscaling renders the game smaller; the eyes are shown at VRModeWidth x VRModeHeight
    if (ddlss::RenderRatio() < 1.0f && g_vrW > 0) {
        int ow = g_vrW, oh = g_vrH; float r = ddlss::RenderRatio();
        g_vrW = ((int)lroundf(ow * r) + 1) & ~1; g_vrH = ((int)lroundf(oh * r) + 1) & ~1;
        dxr::SetOutputSize(ow, oh);
        Log("dlss: %s - game renders %dx%d per eye, shown at %dx%d", ddlss::QualityName(ddlss::Quality()), g_vrW, g_vrH, ow, oh);
    }
    Log("DDDAVR build " DDDAVR_BUILD " - ForceWidth %d ForceHeight %d ForceWindowed %d NoVsync %d", g_forceW, g_forceH, g_forceWindowed, g_noVsync);
    // build 057: D3D9Dll=<file in the game folder> loads another d3d9 (e.g. dgVoodoo2's, renamed d3d9_dgvoodoo.dll) instead of the system one
    wchar_t alt[MAX_PATH] = L""; GetPrivateProfileStringW(L"VR", L"D3D9Dll", L"", alt, MAX_PATH, g_ini);
    if (alt[0]) {
        wchar_t ap[MAX_PATH]; wcscpy(ap, g_ini); wchar_t* sl = wcsrchr(ap, L'\\'); if (sl) sl[1] = 0; else ap[0] = 0;
        wcscat(ap, alt);
        g_real = LoadLibraryW(ap);
        Log("alternate d3d9 (D3D9Dll): %ls -> %p%s", ap, g_real, g_real ? "" : " - FAILED, falling back to the system d3d9");
    }
    if (!g_real) {
        wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);   // SysWOW64 for this 32-bit process
        wcscat(sys, L"\\d3d9.dll");
        g_real = LoadLibraryW(sys);
        Log("system d3d9: %ls -> %p", sys, g_real);
    }
    MH_STATUS s = MH_Initialize();
    Log("MinHook: %s", MH_StatusToString(s));
    HookDesktop();
    DetectGame();   // build 094
    HookFiles();
    LogFlush();
    return TRUE;
}
