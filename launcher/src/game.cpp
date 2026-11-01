// game.cpp - see game.h
#include "game.h"
#include "zip.h"
#include "payload_ids.h"
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#include "miniz/miniz.h"
extern "C" {
#include "lzma/LzmaDec.h"
}
#include <map>
#include <mutex>
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <cstring>
#include <algorithm>

namespace ddda {

const char* kModVersion = DDDAVR_VERSION;
static const uint32_t kTsSteam = 0x5a314c31, kTsGog = 0x5936abab;

// ---------------------------------------------------------------- strings, logging
std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0'); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr); return s;
}
std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n); return w;
}
static std::wstring Lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }
static bool Exists(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
static std::wstring Join(const std::wstring& a, const std::wstring& b) { return a.empty() ? b : (a.back() == L'\\' || a.back() == L'/') ? a + b : a + L"\\" + b; }
const char* StoreName(Store s) { return s == StoreSteam ? "Steam" : s == StoreGog ? "GOG" : "Other"; }

std::wstring PrefsDir() {
    wchar_t* p = nullptr; std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) d = Join(p, L"DragonsDogmaVR");
    CoTaskMemFree(p);
    if (!d.empty()) CreateDirectoryW(d.c_str(), nullptr);
    return d;
}
void Log(const char* fmt, ...) {
    static std::wstring path = Join(PrefsDir(), L"launcher.log");
    char buf[2048]; va_list a; va_start(a, fmt); vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    SYSTEMTIME st; GetLocalTime(&st);
    char line[2200]; int n = snprintf(line, sizeof line, "[%04d-%02d-%02d %02d:%02d:%02d] %s\r\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, buf);
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, line, (DWORD)n, &w, nullptr); CloseHandle(h); }
}

// ---------------------------------------------------------------- preferences / ini
std::wstring GetPref(const wchar_t* section, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[1024]; GetPrivateProfileStringW(section, key, def, buf, 1024, Join(PrefsDir(), L"launcher.ini").c_str()); return buf;
}
void SetPref(const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(section, key, value.c_str(), Join(PrefsDir(), L"launcher.ini").c_str());
}
std::wstring IniPath(const Game& g) { return Join(g.root, L"ddda_vr.ini"); }
std::wstring IniGet(const Game& g, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[1024]; GetPrivateProfileStringW(L"VR", key, def, buf, 1024, IniPath(g).c_str());
    std::wstring s = buf; size_t c = s.find(L';'); if (c != std::wstring::npos) s = s.substr(0, c);
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    return s;
}
void IniSet(const Game& g, const wchar_t* key, const std::wstring& value) { WritePrivateProfileStringW(L"VR", key, value.c_str(), IniPath(g).c_str()); }

// ---------------------------------------------------------------- payload (LZMA "alone" resources, unpacked on first use)
static void* LzAlloc(ISzAllocPtr, size_t n) { return malloc(n); }
static void LzFree(ISzAllocPtr, void* p) { free(p); }
Blob Payload(int id) {
    static std::mutex mx; static std::map<int, std::string> cache;
    std::lock_guard<std::mutex> lock(mx);
    auto it = cache.find(id); if (it != cache.end()) return { it->second.data(), it->second.size() };
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    if (!r) return {};
    HGLOBAL g = LoadResource(nullptr, r); const uint8_t* src = g ? (const uint8_t*)LockResource(g) : nullptr; size_t srcLen = g ? SizeofResource(nullptr, r) : 0;
    if (!src || srcLen < 13) return {};
    uint64_t size = 0; memcpy(&size, src + 5, 8);
    if (size > 512ull * 1024 * 1024) return {};
    std::string out((size_t)size, '\0');
    SizeT dl = (SizeT)size, sl = srcLen - 13; ELzmaStatus st; ISzAlloc al = { LzAlloc, LzFree };
    SRes res = LzmaDecode((Byte*)&out[0], &dl, src + 13, &sl, src, LZMA_PROPS_SIZE, LZMA_FINISH_ANY, &st, &al);
    if (res != SZ_OK || dl != size) { Log("payload %d: unpack failed (%d)", id, (int)res); return {}; }
    auto& slot = cache[id]; slot.swap(out);
    return { slot.data(), slot.size() };
}

// ---------------------------------------------------------------- files
static bool ReadFileAll(const std::wstring& p, std::string& out, size_t maxBytes = 200u * 1024 * 1024) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    if ((uint64_t)sz.QuadPart > maxBytes) { CloseHandle(h); return false; }
    out.resize((size_t)sz.QuadPart); DWORD rd = 0;
    bool ok = sz.QuadPart == 0 || (ReadFile(h, &out[0], (DWORD)sz.QuadPart, &rd, nullptr) && rd == sz.QuadPart);
    CloseHandle(h); return ok;
}
static DWORD WriteAtomic(const std::wstring& p, const void* data, size_t n) {
    std::wstring tmp = p + L".ddda-vr-tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD w = 0; BOOL ok = WriteFile(h, data, (DWORD)n, &w, nullptr) && w == n; FlushFileBuffers(h); CloseHandle(h);
    if (!ok) { DWORD e = GetLastError(); DeleteFileW(tmp.c_str()); return e ? e : ERROR_WRITE_FAULT; }
    if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DWORD e = GetLastError(); DeleteFileW(tmp.c_str()); return e; }
    return 0;
}
static bool SameAsBlob(const std::wstring& p, const Blob& b) {
    std::string s; return b.data && ReadFileAll(p, s) && s.size() == b.size && memcmp(s.data(), b.data, b.size) == 0;
}
static bool CanWrite(const std::wstring& dir, bool& denied) {
    std::wstring p = Join(dir, L".ddda-vr-write-probe.tmp");
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) { denied = GetLastError() == ERROR_ACCESS_DENIED; return false; }
    CloseHandle(h); denied = false; return true;
}
static bool IsElevated() {
    HANDLE t; TOKEN_ELEVATION e{}; DWORD n = 0; bool r = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) { if (GetTokenInformation(t, TokenElevation, &e, sizeof e, &n)) r = e.TokenIsElevated != 0; CloseHandle(t); }
    return r;
}

// ---------------------------------------------------------------- discovery
static std::wstring RegString(HKEY root, const wchar_t* path, const wchar_t* value, REGSAM view) {
    HKEY k; if (RegOpenKeyExW(root, path, 0, KEY_READ | view, &k) != ERROR_SUCCESS) return {};
    wchar_t buf[1024]; DWORD n = sizeof buf, type = 0; std::wstring r;
    if (RegQueryValueExW(k, value, nullptr, &type, (BYTE*)buf, &n) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) { buf[std::min<DWORD>(n / 2, 1023)] = 0; r = buf; }
    RegCloseKey(k); return r;
}
static Build ExeBuild(const std::wstring& root) {
    HANDLE h = CreateFileW(Join(root, L"DDDA.exe").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return BuildUnknown;
    uint8_t hdr[0x400]; DWORD rd = 0; ReadFile(h, hdr, sizeof hdr, &rd, nullptr); CloseHandle(h);
    if (rd < 0x100) return BuildUnknown;
    uint32_t pe = *(uint32_t*)(hdr + 0x3C); if (pe + 0x60 > rd) return BuildUnknown;
    uint32_t ts = *(uint32_t*)(hdr + pe + 8);
    return ts == kTsSteam ? BuildSteam : ts == kTsGog ? BuildGog : BuildUnknown;
}
Game Inspect(const std::wstring& dirIn) {
    Game g; std::wstring d = dirIn;
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    g.root = d;
    g.valid = Exists(Join(d, L"DDDA.exe")) && Exists(Join(d, L"nativePC\\rom\\bbsrpg_core.arc"));
    if (!g.valid) return g;
    g.build = ExeBuild(d);
    std::wstring l = Lower(d);
    WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW(Join(d, L"goggame-*.info").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) { g.store = StoreGog; FindClose(f); }
    else if (l.find(L"steamapps\\common") != std::wstring::npos) g.store = StoreSteam;
    else g.store = g.build == BuildSteam ? StoreSteam : g.build == BuildGog ? StoreGog : StoreUnknown;
    return g;
}
std::vector<Game> FindGames() {
    std::vector<std::wstring> dirs;
    std::vector<std::wstring> steam;
    for (REGSAM v : { KEY_WOW64_32KEY, KEY_WOW64_64KEY }) {
        std::wstring a = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", v); if (!a.empty()) steam.push_back(a);
        std::wstring b = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", v); if (!b.empty()) steam.push_back(b);
    }
    std::vector<std::wstring> libs;
    for (auto& s : steam) {
        std::wstring p = s; for (auto& c : p) if (c == L'/') c = L'\\';
        libs.push_back(p);
        std::string vdf; if (!ReadFileAll(Join(p, L"steamapps\\libraryfolders.vdf"), vdf)) continue;
        size_t pos = 0;
        while ((pos = vdf.find("\"path\"", pos)) != std::string::npos) {
            size_t a = vdf.find('"', pos + 6); if (a == std::string::npos) break;
            size_t b = vdf.find('"', a + 1); if (b == std::string::npos) break;
            std::string v = vdf.substr(a + 1, b - a - 1), u;
            for (size_t i = 0; i < v.size(); ++i) { if (v[i] == '\\' && i + 1 < v.size() && v[i + 1] == '\\') ++i; u += v[i]; }
            libs.push_back(Widen(u)); pos = b + 1;
        }
    }
    for (auto& l : libs) {
        std::wstring dir = Join(l, L"steamapps\\common\\DDDA");
        std::string acf;
        if (ReadFileAll(Join(l, L"steamapps\\appmanifest_367500.acf"), acf)) {
            size_t p = acf.find("\"installdir\"");
            if (p != std::string::npos) { size_t a = acf.find('"', p + 12), b = a == std::string::npos ? a : acf.find('"', a + 1); if (b != std::string::npos) dir = Join(l, L"steamapps\\common\\" + Widen(acf.substr(a + 1, b - a - 1))); }
        }
        dirs.push_back(dir);
    }
    for (REGSAM v : { KEY_WOW64_32KEY, KEY_WOW64_64KEY })
        for (HKEY root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER }) {
            HKEY k; if (RegOpenKeyExW(root, L"SOFTWARE\\GOG.com\\Games", 0, KEY_READ | v, &k) != ERROR_SUCCESS) continue;
            wchar_t name[256];
            for (DWORD i = 0; ; ++i) {
                DWORD n = 256; if (RegEnumKeyExW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                std::wstring sub = std::wstring(L"SOFTWARE\\GOG.com\\Games\\") + name;
                std::wstring gn = Lower(RegString(root, sub.c_str(), L"gameName", v));
                if (gn.find(L"dragon") != std::wstring::npos && gn.find(L"dogma") != std::wstring::npos) { std::wstring p = RegString(root, sub.c_str(), L"path", v); if (!p.empty()) dirs.push_back(p); }
            }
            RegCloseKey(k);
        }
    for (REGSAM v : { KEY_WOW64_32KEY, KEY_WOW64_64KEY }) {
        HKEY k; if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_READ | v, &k) != ERROR_SUCCESS) continue;
        wchar_t name[256];
        for (DWORD i = 0; ; ++i) {
            DWORD n = 256; if (RegEnumKeyExW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            std::wstring sub = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\") + name;
            std::wstring dn = Lower(RegString(HKEY_LOCAL_MACHINE, sub.c_str(), L"DisplayName", v));
            if (dn.find(L"dragon") != std::wstring::npos && dn.find(L"dogma") != std::wstring::npos) { std::wstring p = RegString(HKEY_LOCAL_MACHINE, sub.c_str(), L"InstallLocation", v); if (!p.empty()) dirs.push_back(p); }
        }
        RegCloseKey(k);
    }
    // folders the player added with Browse
    for (int i = 0; i < 8; ++i) { std::wstring k = L"Dir" + std::to_wstring(i); std::wstring p = GetPref(L"Games", k.c_str(), L""); if (!p.empty()) dirs.push_back(p); }
    std::vector<Game> out;
    for (auto& d : dirs) {
        Game g = Inspect(d); if (!g.valid) continue;
        bool dup = false; for (auto& o : out) dup |= Lower(o.root) == Lower(g.root);
        if (!dup) out.push_back(g);
    }
    return out;
}

// ---------------------------------------------------------------- state
static const wchar_t* kRecord = L"ddda_vr_install.txt";
static const wchar_t* kBackup = L"d3d9.dll.ddda-vr-backup";
static const wchar_t* NoGlowName(const Game& g) { return g.build == BuildGog ? L"ddda_vr_mods\\bbsrpg_core_noglow_gog.arc" : L"ddda_vr_mods\\bbsrpg_core_noglow.arc"; }
State Check(const Game& g) {
    State s;
    if (!g.valid) return s;
    std::string rec; bool hasRec = ReadFileAll(Join(g.root, kRecord), rec);
    if (hasRec) { size_t p = rec.find("version="); if (p != std::string::npos) { size_t e = rec.find_first_of("\r\n", p); s.installedVersion = rec.substr(p + 8, e == std::string::npos ? std::string::npos : e - p - 8); } }
    bool dll = Exists(Join(g.root, L"d3d9.dll"));
    bool ours = dll && (hasRec || SameAsBlob(Join(g.root, L"d3d9.dll"), Payload(IDR_D3D9)) || Exists(Join(g.root, L"ddda_vr.log")));
    s.installed = ours;
    s.outdated = ours && hasRec && !SameAsBlob(Join(g.root, L"d3d9.dll"), Payload(IDR_D3D9));
    s.foreignD3d9 = dll && !ours;
    s.backupPresent = Exists(Join(g.root, kBackup));
    s.iniExists = Exists(IniPath(g));
    s.noGlowArc = Exists(Join(g.root, NoGlowName(g)));
    bool denied = false; if (!CanWrite(g.root, denied) && denied && !IsElevated()) s.needsElevation = true;
    return s;
}
bool GameRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{ sizeof pe }; bool r = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) if (!_wcsicmp(pe.szExeFile, L"DDDA.exe")) { r = true; break; }
    CloseHandle(snap); return r;
}

// ---------------------------------------------------------------- OpenXR runtime
Runtime ActiveRuntime() {
    Runtime r;
    std::wstring m = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", KEY_WOW64_64KEY);
    if (m.empty()) m = RegString(HKEY_CURRENT_USER, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", KEY_WOW64_64KEY);
    if (m.empty()) return r;
    r.manifest = Narrow(m);
    std::string j; if (ReadFileAll(m, j, 1 << 20)) {
        size_t p = j.find("\"name\"");
        if (p != std::string::npos) { size_t a = j.find('"', j.find(':', p) + 1), b = a == std::string::npos ? a : j.find('"', a + 1); if (b != std::string::npos) r.name = j.substr(a + 1, b - a - 1); }
    }
    if (r.name.empty()) {   // fall back to a readable name from the manifest path
        std::string l = r.manifest; for (auto& c : l) c = (char)tolower(c);
        if (l.find("virtualdesktop") != std::string::npos) r.name = "Virtual Desktop (VDXR)";
        else if (l.find("steamvr") != std::string::npos) r.name = "SteamVR";
        else if (l.find("oculus") != std::string::npos || l.find("meta") != std::string::npos) r.name = "Meta Quest Link";
        else if (l.find("windowsmixedreality") != std::string::npos || l.find("mixedreality") != std::string::npos) r.name = "Windows Mixed Reality";
        else if (l.find("pico") != std::string::npos) r.name = "PICO Connect";
        else { size_t s = r.manifest.find_last_of("\\/"); r.name = s == std::string::npos ? r.manifest : r.manifest.substr(s + 1); }
    }
    r.found = Exists(m);
    return r;
}

// ---------------------------------------------------------------- install / uninstall
struct PayloadFile { int id; const wchar_t* rel; };
static const PayloadFile kFiles[] = {
    { IDR_D3D9, L"d3d9.dll" }, { IDR_OPENXR, L"openxr_loader.dll" }, { IDR_DXVK, L"d3d9_dxvk.dll" }, { IDR_DXVK_LIC, L"ddda_vr_mods\\dxvk_license.txt" },
    { IDR_DLSS_HOST, L"ddda_vr_dlss\\ddda_vr_dlss64.exe" }, { IDR_NGX_DLSS, L"ddda_vr_dlss\\nvngx_dlss.dll" },
};
static void Add(Report& r, char kind, const std::string& text) { r.lines.push_back(std::string(1, kind) + " " + text); Log("%c %s", kind, text.c_str()); if (kind == 'x') r.ok = false; }
static std::string ErrText(DWORD e) {
    wchar_t* m = nullptr; FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, (LPWSTR)&m, 0, nullptr);
    std::string s = m ? Narrow(m) : "error " + std::to_string(e); LocalFree(m);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '.')) s.pop_back();
    return s;
}
// the glow fix: a copy of the player's own nativePC\rom\bbsrpg_core.arc in which the two sun-glow effect textures
// (effect\tex\cm\t_cm000_00_GM, t_cm040_00_GM: MT TEX v0x99, BC3) are dimmed block by block - colour endpoints and alpha
// multiplied by a factor that depends on the place in the effect atlas (starburst / glint sprites removed). Nothing of the
// game or of other mods is shipped; the game's archive is not changed (the mod redirects the game to the copy).
static double GlowRule000(float u, float v) {
    int cx = (int)(u * 8), cy = (int)(v * 8);
    if ((cy == 2 || cy == 3) && cx <= 5) return 0.0;
    if (cy == 1 && (cx == 0 || cx == 1 || cx == 3)) return 1.0;
    return 0.4;
}
static double GlowRule040(float, float) { return 0.35; }
static void DimAlphaBlock(uint8_t* b, double k) {
    int a0 = b[0], a1 = b[1], pal[8]; uint64_t bits = 0; for (int i = 0; i < 6; ++i) bits |= (uint64_t)b[2 + i] << (8 * i);
    pal[0] = a0; pal[1] = a1;
    if (a0 > a1) for (int i = 1; i < 7; ++i) pal[1 + i] = ((7 - i) * a0 + i * a1) / 7;
    else { for (int i = 1; i < 5; ++i) pal[1 + i] = ((5 - i) * a0 + i * a1) / 5; pal[6] = 0; pal[7] = 255; }
    int v[16], hi = 0, lo = 255;
    for (int i = 0; i < 16; ++i) { v[i] = (int)(pal[(bits >> (3 * i)) & 7] * k + 0.5); if (v[i] > hi) hi = v[i]; if (v[i] < lo) lo = v[i]; }
    b[0] = (uint8_t)hi; b[1] = (uint8_t)lo;
    if (hi == lo) { memset(b + 2, 0, 6); return; }
    int p2[8] = { hi, lo }; for (int i = 1; i < 7; ++i) p2[1 + i] = ((7 - i) * hi + i * lo) / 7;
    bits = 0;
    for (int i = 0; i < 16; ++i) { int best = 0; for (int j = 1; j < 8; ++j) if (abs(p2[j] - v[i]) < abs(p2[best] - v[i])) best = j; bits |= (uint64_t)best << (3 * i); }
    for (int i = 0; i < 6; ++i) b[2 + i] = (uint8_t)(bits >> (8 * i));
}
static uint16_t DimColour(uint16_t c, double k) {
    int r = c >> 11, g = (c >> 5) & 63, b = c & 31;
    return (uint16_t)(((int)(r * k + 0.5) << 11) | ((int)(g * k + 0.5) << 5) | (int)(b * k + 0.5));
}
static bool DimTexture(std::string& t, double (*rule)(float, float)) {
    if (t.size() < 0x20 || memcmp(t.data(), "TEX\0", 4)) return false;
    uint32_t p2; memcpy(&p2, &t[8], 4); int mips = p2 & 0x3f, W = (p2 >> 6) & 0x1fff, H = (p2 >> 19) & 0x1fff;
    if (!mips || 16 + 4 * mips > (int)t.size()) return false;
    for (int m = 0; m < mips; ++m) {
        uint32_t o; memcpy(&o, &t[16 + 4 * m], 4);
        int w = std::max(1, W >> m), h = std::max(1, H >> m), bw = (w + 3) / 4, bh = (h + 3) / 4;
        if ((uint64_t)o + (uint64_t)bw * bh * 16 > t.size()) return false;
        for (int by = 0; by < bh; ++by) for (int bx = 0; bx < bw; ++bx, o += 16) {
            double k = rule((bx * 4 + 2) / (float)w, (by * 4 + 2) / (float)h);
            if (k == 1.0) continue;
            uint8_t* b = (uint8_t*)&t[o]; DimAlphaBlock(b, k);
            uint16_t c0, c1; memcpy(&c0, b + 8, 2); memcpy(&c1, b + 10, 2); c0 = DimColour(c0, k); c1 = DimColour(c1, k); memcpy(b + 8, &c0, 2); memcpy(b + 10, &c1, 2);
        }
    }
    return true;
}
static DWORD BuildNoGlow(const Game& g, std::string& why) {
    std::string arc; if (!ReadFileAll(Join(g.root, L"nativePC\\rom\\bbsrpg_core.arc"), arc)) { why = "nativePC\\rom\\bbsrpg_core.arc couldn't be read"; return ERROR_FILE_NOT_FOUND; }
    if (arc.size() < 0x8000 || memcmp(arc.data(), "ARC\0", 4)) { why = "bbsrpg_core.arc isn't in the expected format"; return ERROR_BAD_FORMAT; }
    const uint16_t n = *(const uint16_t*)(arc.data() + 6);
    if (8 + 80u * n > 0x8000) { why = "bbsrpg_core.arc has an unexpected table"; return ERROR_BAD_FORMAT; }
    std::string out = arc.substr(0, 8), data; int done = 0;
    for (uint16_t i = 0; i < n; ++i) {
        const char* e = arc.data() + 8 + 80 * i;
        uint32_t type, cs, ds, off; memcpy(&type, e + 64, 4); memcpy(&cs, e + 68, 4); memcpy(&ds, e + 72, 4); memcpy(&off, e + 76, 4);
        if ((uint64_t)off + cs > arc.size()) { why = "bbsrpg_core.arc is damaged"; return ERROR_BAD_FORMAT; }
        std::string body(arc.data() + off, cs);
        double (*rule)(float, float) = !strncmp(e, "effect\\tex\\cm\\t_cm000_00_GM", 64) ? GlowRule000 : !strncmp(e, "effect\\tex\\cm\\t_cm040_00_GM", 64) ? GlowRule040 : nullptr;
        if (rule) {
            std::string t(ds & 0x1FFFFFFF, '\0'); mz_ulong tl = (mz_ulong)t.size();
            if (mz_uncompress((unsigned char*)&t[0], &tl, (const unsigned char*)body.data(), (mz_ulong)body.size()) != MZ_OK || tl != t.size() || !DimTexture(t, rule)) { why = "a glow texture couldn't be read"; return ERROR_INVALID_DATA; }
            mz_ulong cl = mz_compressBound((mz_ulong)t.size()); std::string c(cl, '\0');
            if (mz_compress2((unsigned char*)&c[0], &cl, (const unsigned char*)t.data(), (mz_ulong)t.size(), 9) != MZ_OK) { why = "compressing failed"; return ERROR_INVALID_DATA; }
            c.resize(cl); body.swap(c); cs = (uint32_t)body.size(); ds = (ds & 0xE0000000u) | (uint32_t)t.size(); ++done;
        }
        uint32_t no = 0x8000 + (uint32_t)data.size();
        out.append(e, 64); out.append((const char*)&type, 4); out.append((const char*)&cs, 4); out.append((const char*)&ds, 4); out.append((const char*)&no, 4);
        data += body;
    }
    if (done != 2) { why = "the glow textures weren't found in bbsrpg_core.arc"; return ERROR_NOT_FOUND; }
    out.resize(0x8000, '\0'); out += data;
    CreateDirectoryW(Join(g.root, L"ddda_vr_mods").c_str(), nullptr);
    return WriteAtomic(Join(g.root, NoGlowName(g)), out.data(), out.size());
}
static int MergeIniDefaults(const Game& g, const Blob& b) {
    std::string s((const char*)b.data, b.size); std::wstring section; int added = 0; size_t p = 0;
    while (p < s.size()) {
        size_t e = s.find('\n', p); if (e == std::string::npos) e = s.size();
        std::string line = s.substr(p, e - p); p = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[') { size_t c = line.find(']'); section = Widen(line.substr(1, c == std::string::npos ? std::string::npos : c - 1)); continue; }
        size_t eq = line.find('='); if (eq == std::string::npos || section.empty()) continue;
        std::wstring key = Widen(line.substr(0, eq)), val = Widen(line.substr(eq + 1));
        wchar_t buf[8]; GetPrivateProfileStringW(section.c_str(), key.c_str(), L"\x01", buf, 8, IniPath(g).c_str());
        if (buf[0] == 1) { WritePrivateProfileStringW(section.c_str(), key.c_str(), val.c_str(), IniPath(g).c_str()); ++added; }
    }
    return added;
}
Report Install(const Game& g) {
    Report r;
    Log("install %s (%s build) version %s", Narrow(g.root).c_str(), g.build == BuildSteam ? "Steam" : g.build == BuildGog ? "GOG" : "unknown", kModVersion);
    if (!g.valid) { Add(r, 'x', "Dragon's Dogma: Dark Arisen wasn't found in that folder (DDDA.exe and nativePC)."); return r; }
    if (GameRunning()) { Add(r, 'x', "Dragon's Dogma is running. Close the game first."); return r; }
    if (g.build == BuildUnknown) Add(r, '!', "This game version isn't one the mod was made for (the current Steam or GOG release). It may not work.");
    for (const auto& f : kFiles) if (!Payload(f.id).data) { Add(r, 'x', "The launcher is incomplete (missing " + Narrow(f.rel) + ")."); return r; }
    State s = Check(g);
    if (s.foreignD3d9) {
        DWORD e = MoveFileExW(Join(g.root, L"d3d9.dll").c_str(), Join(g.root, kBackup).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : GetLastError();
        if (e) { Add(r, 'x', "Couldn't move the existing d3d9.dll aside: " + ErrText(e)); r.accessDenied = e == ERROR_ACCESS_DENIED; return r; }
        Add(r, '+', "Another mod's d3d9.dll was kept as d3d9.dll.ddda-vr-backup (restored on uninstall)");
    }
    CreateDirectoryW(Join(g.root, L"ddda_vr_dlss").c_str(), nullptr);
    CreateDirectoryW(Join(g.root, L"ddda_vr_mods").c_str(), nullptr);
    for (const auto& f : kFiles) {
        Blob b = Payload(f.id);
        DWORD e = WriteAtomic(Join(g.root, f.rel), b.data, b.size);
        if (e) { Add(r, 'x', "Couldn't write " + Narrow(f.rel) + ": " + ErrText(e)); r.accessDenied |= e == ERROR_ACCESS_DENIED; return r; }
    }
    Add(r, '+', "Mod files installed (d3d9.dll, openxr_loader.dll, DXVK, DLSS)");
    { std::string why; DWORD e = BuildNoGlow(g, why);
      if (e) Add(r, '!', "The sun-glare fix couldn't be prepared (" + why + "); the game's own glare stays."); else Add(r, '+', "Sun-glare fix made from your own game files (ddda_vr_mods)"); }
    Blob ini = Payload(IDR_INI);
    if (!Exists(IniPath(g))) {
        DWORD e = WriteAtomic(IniPath(g), ini.data, ini.size);
        if (e) { Add(r, 'x', "Couldn't write ddda_vr.ini: " + ErrText(e)); return r; }
        Add(r, '+', "Settings file created (ddda_vr.ini)");
    } else {
        int n = MergeIniDefaults(g, ini);
        Add(r, '+', n ? "Your settings were kept; " + std::to_string(n) + " new option(s) added to ddda_vr.ini" : std::string("Your settings in ddda_vr.ini were kept"));
    }
    std::string rec = std::string("version=") + kModVersion + "\r\nbuild=" + (g.build == BuildSteam ? "steam" : g.build == BuildGog ? "gog" : "unknown") + "\r\n";
    for (const auto& f : kFiles) rec += "file=" + Narrow(f.rel) + "\r\n";
    rec += "file=" + Narrow(NoGlowName(g)) + "\r\n";
    WriteAtomic(Join(g.root, kRecord), rec.data(), rec.size());
    Add(r, '+', std::string("Dragon's Dogma VR ") + kModVersion + " is installed");
    return r;
}
Report Uninstall(const Game& g, bool deleteIni) {
    Report r;
    Log("uninstall %s (delete ini %d)", Narrow(g.root).c_str(), deleteIni);
    if (GameRunning()) { Add(r, 'x', "Dragon's Dogma is running. Close the game first."); return r; }
    State s = Check(g);
    auto del = [&](const std::wstring& rel) -> bool {
        std::wstring p = Join(g.root, rel);
        if (!Exists(p)) return true;
        if (DeleteFileW(p.c_str())) return true;
        DWORD e = GetLastError(); Add(r, 'x', "Couldn't remove " + Narrow(rel) + ": " + ErrText(e)); r.accessDenied |= e == ERROR_ACCESS_DENIED; return false;
    };
    if (s.installed || Exists(Join(g.root, kRecord))) { if (!del(L"d3d9.dll")) return r; }
    else if (Exists(Join(g.root, L"d3d9.dll"))) Add(r, '!', "d3d9.dll belongs to another mod and was left in place");
    for (const auto& f : kFiles) if (f.id != IDR_D3D9 && !del(f.rel)) return r;
    for (const wchar_t* f : { L"ddda_vr_mods\\bbsrpg_core_noglow.arc", L"ddda_vr_mods\\bbsrpg_core_noglow_gog.arc", L"ddda_vr_dlss\\ddda_vr_dlss64.log", L"ddda_vr.log", L"ddda_vr_hang.dmp", L"ddda_vr_crash.dmp" }) if (!del(f)) return r;
    RemoveDirectoryW(Join(g.root, L"ddda_vr_dlss").c_str()); RemoveDirectoryW(Join(g.root, L"ddda_vr_mods").c_str());   // only if empty
    Add(r, '+', "Mod files removed");
    if (s.backupPresent) {
        if (MoveFileExW(Join(g.root, kBackup).c_str(), Join(g.root, L"d3d9.dll").c_str(), MOVEFILE_REPLACE_EXISTING)) Add(r, '+', "The other mod's d3d9.dll was put back");
        else Add(r, '!', "Couldn't restore d3d9.dll.ddda-vr-backup: " + ErrText(GetLastError()));
    }
    if (deleteIni) { if (del(L"ddda_vr.ini")) Add(r, '+', "Settings file removed"); }
    else if (Exists(IniPath(g))) Add(r, '+', "ddda_vr.ini was kept (your settings for a later reinstall)");
    del(kRecord);
    Add(r, '+', "Dragon's Dogma VR is uninstalled. Your save games weren't touched.");
    return r;
}

static void WriteResult(const std::wstring& path, const Report& r) {
    std::string s = "ok=" + std::to_string(r.ok) + " denied=" + std::to_string(r.accessDenied) + "\n";
    for (auto& l : r.lines) s += l + "\n";
    WriteAtomic(path, s.data(), s.size());
}
static bool ReadResult(const std::wstring& path, Report& r) {
    std::string s; if (!ReadFileAll(path, s) || s.size() < 4) return false;
    r = Report{}; r.ok = s.find("ok=1") == 0; r.accessDenied = s.find("denied=1") != std::string::npos;
    size_t p = s.find('\n'); while (p != std::string::npos && p + 1 < s.size()) { size_t e = s.find('\n', p + 1); r.lines.push_back(s.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1)); p = e; }
    return true;
}
int ElevatedMain(int argc, wchar_t** argv) {
    std::wstring op, game, result; bool delIni = false;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--op") && i + 1 < argc) op = argv[++i];
        else if (!wcscmp(argv[i], L"--game") && i + 1 < argc) game = argv[++i];
        else if (!wcscmp(argv[i], L"--result") && i + 1 < argc) result = argv[++i];
        else if (!wcscmp(argv[i], L"--delete-ini")) delIni = true;
    }
    Game g = Inspect(game);
    Report r = op == L"install" ? Install(g) : op == L"uninstall" ? Uninstall(g, delIni) : Report{ false, false, { "x unknown operation" } };
    if (!result.empty()) WriteResult(result, r);
    return r.ok ? 0 : r.accessDenied ? 3 : 2;
}
Report RunOperation(const Game& g, const char* op, bool deleteIni) {
    State s = Check(g);
    if (!s.needsElevation) {
        Report r = !strcmp(op, "install") ? Install(g) : Uninstall(g, deleteIni);
        if (!(r.accessDenied && !IsElevated())) return r;
    }
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring result = Join(tmp, L"ddda-vr-launcher-result-" + std::to_wstring(GetCurrentProcessId()) + L".txt");
    DeleteFileW(result.c_str());
    std::wstring params = L"--op " + Widen(op) + L" --game \"" + g.root + L"\" --result \"" + result + L"\"" + (deleteIni ? L" --delete-ini" : L"");
    SHELLEXECUTEINFOW sei{ sizeof sei }; sei.fMask = SEE_MASK_NOCLOSEPROCESS; sei.lpVerb = L"runas"; sei.lpFile = self; sei.lpParameters = params.c_str(); sei.nShow = SW_HIDE;
    Log("asking for administrator rights: %s", Narrow(params).c_str());
    if (!ShellExecuteExW(&sei)) {
        Report r; r.ok = false;
        r.lines.push_back(GetLastError() == ERROR_CANCELLED ? "x Administrator rights were declined, so nothing was changed." : "x Couldn't start the installer with administrator rights: " + ErrText(GetLastError()));
        return r;
    }
    WaitForSingleObject(sei.hProcess, 10 * 60 * 1000); CloseHandle(sei.hProcess);
    Report r; if (!ReadResult(result, r)) { r.ok = false; r.lines.push_back("x The elevated installer didn't report back."); }
    DeleteFileW(result.c_str());
    return r;
}

// ---------------------------------------------------------------- launch
bool Launch(const Game& g, std::string& err) {
    std::wstring exe = Join(g.root, L"DDDA.exe");
    std::wstring cmd = L"\"" + exe + L"\"";
    STARTUPINFOW si{ sizeof si }; PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    Log("launch: %s", Narrow(cmd).c_str());
    if (!CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, g.root.c_str(), &si, &pi)) { err = "Couldn't start the game: " + ErrText(GetLastError()); Log("%s", err.c_str()); return false; }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
}

// ---------------------------------------------------------------- log collection
std::wstring CollectLogs(const Game& g, std::string& err) {
    wchar_t* desk = nullptr; std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk))) dir = desk;
    CoTaskMemFree(desk);
    if (dir.empty()) { err = "The desktop folder wasn't found."; return {}; }
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t name[96]; swprintf(name, 96, L"DDDAVR-Logs-%04d%02d%02d-%02d%02d%02d.zip", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring zipPath = Join(dir, name);
    ZipWriter z; if (!z.Open(zipPath)) { err = "Couldn't create " + Narrow(zipPath); return {}; }
    std::string info = "Dragon's Dogma VR launcher " + std::string(kModVersion) + "\r\n";
    { typedef LONG (WINAPI *RtlGetVersion_t)(OSVERSIONINFOW*); OSVERSIONINFOW v{ sizeof v };
      auto f = (RtlGetVersion_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
      if (f && !f(&v)) info += "Windows " + std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + " build " + std::to_string(v.dwBuildNumber) + "\r\n"; }
    { SYSTEM_INFO si; GetNativeSystemInfo(&si); info += "Logical processors: " + std::to_string(si.dwNumberOfProcessors) + "\r\n";
      MEMORYSTATUSEX ms{ sizeof ms }; GlobalMemoryStatusEx(&ms); info += "RAM: " + std::to_string((unsigned)(ms.ullTotalPhys >> 20)) + " MB\r\n"; }
    { IDXGIFactory1* f = nullptr;
      if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) {
          IDXGIAdapter1* a = nullptr;
          for (UINT i = 0; f->EnumAdapters1(i, &a) == S_OK; ++i) { DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d); info += "GPU " + std::to_string(i) + ": " + Narrow(d.Description) + " (" + std::to_string((unsigned)(d.DedicatedVideoMemory >> 20)) + " MB)\r\n"; a->Release(); }
          f->Release(); } }
    Runtime rt = ActiveRuntime(); info += "OpenXR runtime: " + rt.name + " (" + rt.manifest + ")\r\n";
    info += "Game: " + Narrow(g.root) + " (" + StoreName(g.store) + ", exe " + (g.build == BuildSteam ? "Steam" : g.build == BuildGog ? "GOG" : "unknown") + ")\r\n";
    State s = Check(g);
    info += "Mod installed: " + std::string(s.installed ? "yes " + s.installedVersion : "no") + ", other d3d9.dll: " + (s.foreignD3d9 ? "yes" : "no") + "\r\n";
    z.Add("system-info.txt", info.data(), info.size());
    auto addFile = [&](const std::wstring& path, const std::string& nm) { std::string d; if (ReadFileAll(path, d)) z.Add(nm, d.data(), d.size()); };
    if (g.valid) {
        for (const wchar_t* f : { L"ddda_vr.log", L"ddda_vr.ini", L"ddda_vr_install.txt", L"ddda_vr_hang.dmp", L"ddda_vr_crash.dmp" }) addFile(Join(g.root, f), Narrow(f));   // 0.8.1: freeze / crash dumps
        addFile(Join(g.root, L"ddda_vr_dlss\\ddda_vr_dlss64.log"), "ddda_vr_dlss64.log");
    }
    addFile(Join(PrefsDir(), L"launcher.log"), "launcher.log");
    if (!z.Close()) { err = "Writing the zip failed."; return {}; }
    return zipPath;
}

} // namespace ddda
