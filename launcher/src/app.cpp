// app.cpp - Dragon's Dogma VR launcher: window, Direct3D 11 + Dear ImGui loop, pages.
// Built on the Far Cry 2 VR launcher's code (itself modelled on Pizza Parker's Dishonored VR launcher, with permission);
// the look follows the user's concept: sidebar with the emblem, the banner art on top, a "Ready to play" card.
#include "game.h"
#include "theme.h"
#include "payload_ids.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <d3d11.h>
#include <thread>
#include <cmath>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace ddda { int ElevatedMain(int argc, wchar_t** argv); }
using namespace ddda;
using theme::g_s;

// ---------------------------------------------------------------- D3D
static ID3D11Device* g_dev = nullptr; static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr; static ID3D11RenderTargetView* g_rtv = nullptr;
static UINT g_resizeW = 0, g_resizeH = 0;
static float g_scale = 1.0f; static bool g_fontsDirty = false;
static int g_framesPending = 3;

static void CreateRT() { ID3D11Texture2D* bb = nullptr; g_swap->GetBuffer(0, IID_PPV_ARGS(&bb)); if (bb) { g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv); bb->Release(); } }
static bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 }; D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fl, 3, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) { sd.BufferCount = 1; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fl, 3, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx); }
    if (FAILED(hr)) hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, fl, 3, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) return false;
    CreateRT(); return true;
}
// the art (JPEG / PNG resources) decoded with WIC into D3D11 textures
static theme::Image LoadImageRes(int id) {
    theme::Image img;
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA); if (!r) return img;
    HGLOBAL hg = LoadResource(nullptr, r); BYTE* data = hg ? (BYTE*)LockResource(hg) : nullptr; DWORD n = hg ? SizeofResource(nullptr, r) : 0;
    if (!data) return img;
    IWICImagingFactory* f = nullptr; IWICStream* st = nullptr; IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) && SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory(data, n))
        && SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(f->CreateFormatConverter(&cv))
        && SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        UINT w = 0, h = 0; cv->GetSize(&w, &h);
        std::vector<BYTE> px((size_t)w * h * 4);
        if (SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data()))) {
            D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA sd{ px.data(), w * 4, 0 }; ID3D11Texture2D* t = nullptr; ID3D11ShaderResourceView* srv = nullptr;
            if (SUCCEEDED(g_dev->CreateTexture2D(&td, &sd, &t)) && SUCCEEDED(g_dev->CreateShaderResourceView(t, nullptr, &srv))) { img.tex = (ImTextureID)srv; img.w = (int)w; img.h = (int)h; }
            if (t) t->Release();
        }
    }
    if (cv) cv->Release(); if (fr) fr->Release(); if (dec) dec->Release(); if (st) st->Release(); if (f) f->Release();
    if (!img.tex) Log("art %d: decode failed", id);
    return img;
}

// ---------------------------------------------------------------- app state
enum Page { PageMain, PageSettings, PageOptional, PageAbout };
struct App {
    Page page = PageMain;
    std::vector<Game> games; int gameIdx = -1;
    Game game; State state; bool running = false; GameConfig cfg;   // 0.8.1: the game's own settings
    Runtime runtime;
    std::atomic<bool> busy{ false }; std::string busyText; std::mutex mx; bool haveReport = false; Report report; Report pending; bool pendingReady = false;
    bool confirmUninstall = false, deleteIni = false;
    std::string notice; ImU32 noticeCol = 0; double noticeAt = 0;
    float scrollReset = -1;
} g_app;

static void Notice(const std::string& t, ImU32 col) { g_app.notice = t; g_app.noticeCol = col; g_app.noticeAt = ImGui::GetTime(); }
static void SelectGame(int i) {
    g_app.gameIdx = i; g_app.game = i >= 0 && i < (int)g_app.games.size() ? g_app.games[i] : Game{};
    g_app.state = Check(g_app.game);
    if (g_app.game.valid) SetPref(L"Game", L"Directory", g_app.game.root);
}
static void Rescan() {
    g_app.games = FindGames();
    std::wstring last = GetPref(L"Game", L"Directory", L""); int sel = g_app.games.empty() ? -1 : 0;
    for (int i = 0; i < (int)g_app.games.size(); ++i) if (_wcsicmp(g_app.games[i].root.c_str(), last.c_str()) == 0) sel = i;
    SelectGame(sel);
    g_app.runtime = ActiveRuntime(); g_app.cfg = CheckGameConfig();
}
static void Refresh() { if (g_app.game.valid) g_app.state = Check(g_app.game); g_app.running = GameRunning(); g_app.runtime = ActiveRuntime(); g_app.cfg = CheckGameConfig(); }

static void StartOp(const char* op, bool deleteIni) {
    if (g_app.busy) return;
    g_app.busy = true; g_app.busyText = !strcmp(op, "install") ? "Installing Dragon's Dogma VR..." : "Removing Dragon's Dogma VR...";
    g_app.haveReport = false; g_app.notice.clear();
    Game g = g_app.game; std::string o = op;
    std::thread([g, o, deleteIni] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        Report r = RunOperation(g, o.c_str(), deleteIni);
        { std::lock_guard<std::mutex> l(g_app.mx); g_app.pending = r; g_app.pendingReady = true; }
        g_app.busy = false;
        CoUninitialize();
    }).detach();
}
static void PollWorker() {
    std::lock_guard<std::mutex> l(g_app.mx);
    if (!g_app.pendingReady) return;
    g_app.pendingReady = false; g_app.report = g_app.pending; g_app.haveReport = true;
    Refresh(); g_framesPending = 3;
}
static void BrowseForGame(HWND hwnd) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD o; d->GetOptions(&o); d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM); d->SetTitle(L"Select the Dragon's Dogma: Dark Arisen folder (the one with DDDA.exe)");
    if (SUCCEEDED(d->Show(hwnd))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                Game g = Inspect(p);
                if (g.valid) {
                    for (int i = 0; i < 8; ++i) { std::wstring k = L"Dir" + std::to_wstring(i); if (GetPref(L"Games", k.c_str(), L"").empty() || _wcsicmp(GetPref(L"Games", k.c_str(), L"").c_str(), g.root.c_str()) == 0) { SetPref(L"Games", k.c_str(), g.root); break; } }
                    SetPref(L"Game", L"Directory", g.root); Rescan(); Notice("Game folder added.", theme::Good());
                } else Notice("That folder doesn't contain Dragon's Dogma: Dark Arisen (DDDA.exe and nativePC). Choose the game's main folder.", theme::Bad());
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    d->Release();
}
static void DesktopShortcut() {
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    PWSTR desk = nullptr; if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk))) return;
    std::wstring lnk = std::wstring(desk) + L"\\Dragon's Dogma VR.lnk"; CoTaskMemFree(desk);
    IShellLinkW* sl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
        sl->SetPath(self); sl->SetIconLocation(self, 0); sl->SetDescription(L"Dragon's Dogma VR launcher");
        std::wstring dir = self; dir = dir.substr(0, dir.find_last_of(L'\\')); sl->SetWorkingDirectory(dir.c_str());
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) { bool ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE)); pf->Release(); Notice(ok ? "Desktop shortcut created." : "Couldn't create the shortcut.", ok ? theme::Good() : theme::Bad()); }
        sl->Release();
    }
}

// ---------------------------------------------------------------- settings rows (written to ddda_vr.ini straight away)
struct Opt { const char* label; const wchar_t* value; };
static const Opt kUpscaler[] = { { "Off", L"0" }, { "NVIDIA DLSS", L"1" } };
static const Opt kQuality[] = { { "DLAA (native resolution)", L"0" }, { "Ultra Quality", L"1" }, { "Quality", L"2" }, { "Balanced", L"3" }, { "Performance", L"4" }, { "Ultra Performance", L"5" } };
static const float kRatio[] = { 1.0f, 0.77f, 0.667f, 0.58f, 0.5f, 0.333f };
static const Opt kPreset[] = { { "Transformer K (recommended)", L"11" }, { "Transformer J", L"10" }, { "Transformer 2 M", L"13" }, { "Transformer 2 L", L"12" }, { "NVIDIA recommended per mode", L"16" } };
static const Opt kReticle[] = { { "Game's own", L"0" }, { "White", L"1" }, { "White with a dark edge", L"2" } };

static bool Installed() { return g_app.game.valid && g_app.state.iniExists; }
static std::wstring Get(const wchar_t* key, const wchar_t* def) { return Installed() ? IniGet(g_app.game, key, def) : std::wstring(def); }
static void Set(const wchar_t* key, const std::wstring& v) {
    if (!Installed()) return;
    IniSet(g_app.game, key, v); Notice("Saved. Takes effect the next time the game starts.", theme::Good());
}
static void RowLabel(const char* label, const char* help, float ctrlW) {
    ImGui::BeginGroup();
    ImGui::PushFont(theme::g_fonts.bold); ImGui::TextUnformatted(label); ImGui::PopFont();
    if (help) { ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ctrlW - 24 * g_s); theme::Wrapped(help, theme::Muted()); ImGui::PopTextWrapPos(); }
    ImGui::EndGroup();
}
static void ComboRow(const char* label, const char* help, const wchar_t* key, const wchar_t* def, const Opt* o, int n, bool enabled = true) {
    ImGui::PushID(label);
    float ctrlW = 300 * g_s, x0 = ImGui::GetCursorPosX(), avail = ImGui::GetContentRegionAvail().x;
    float y0 = ImGui::GetCursorPosY();
    ImGui::BeginDisabled(!enabled);
    RowLabel(label, help, ctrlW);
    float y1 = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(x0 + avail - ctrlW, y0)); ImGui::SetNextItemWidth(ctrlW);
    std::wstring v = Get(key, def); int cur = -1; for (int k = 0; k < n; ++k) if (v == o[k].value) cur = k;
    if (ImGui::BeginCombo("##c", cur >= 0 ? o[cur].label : "(custom value)")) {
        for (int k = 0; k < n; ++k) if (ImGui::Selectable(o[k].label, k == cur)) Set(key, o[k].value);
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::SetCursorPosY(std::max(y1, ImGui::GetCursorPosY()) + 6 * g_s);
    ImGui::PopID();
}
static void SliderRow(const char* label, const char* help, const wchar_t* key, const wchar_t* def, float mn, float mx, const char* fmt, float shown, bool enabled = true) {
    ImGui::PushID(label);
    float ctrlW = 300 * g_s, x0 = ImGui::GetCursorPosX(), avail = ImGui::GetContentRegionAvail().x, y0 = ImGui::GetCursorPosY();
    ImGui::BeginDisabled(!enabled);
    RowLabel(label, help, ctrlW);
    float y1 = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(x0 + avail - ctrlW, y0)); ImGui::SetNextItemWidth(ctrlW);
    // the value being dragged lives in ImGui's state storage; the ini is written when the slider is released
    ImGuiStorage* stg = ImGui::GetStateStorage(); ImGuiID vid = ImGui::GetID("##val"), aid = ImGui::GetID("##act");
    float& d = *stg->GetFloatRef(vid, 0.0f); bool& dragging = *stg->GetBoolRef(aid, false);
    if (!dragging) d = (float)_wtof(Get(key, def).c_str()) * shown;
    ImGui::SliderFloat("##s", &d, mn * shown, mx * shown, fmt);
    dragging = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) { wchar_t b[32]; swprintf(b, 32, L"%.2f", d / shown); std::wstring nv = b; while (nv.size() > 3 && nv.back() == L'0' && nv[nv.size() - 2] != L'.') nv.pop_back(); if (nv.size() > 3 && nv.substr(nv.size() - 2) == L".0" && shown == 1 && mx > 20) nv = nv.substr(0, nv.size() - 2); Set(key, nv); }
    ImGui::EndDisabled();
    ImGui::SetCursorPosY(std::max(y1, ImGui::GetCursorPosY()) + 6 * g_s);
    ImGui::PopID();
}
static bool ToggleKey(const char* label, const char* help, const wchar_t* key, const wchar_t* def, bool invert = false, bool enabled = true) {
    bool on = (Get(key, def) != L"0") != invert;
    if (theme::Toggle(label, &on, help, enabled && Installed())) { Set(key, (on != invert) ? L"1" : L"0"); return true; }
    return false;
}
static void NeedInstall() {
    if (Installed()) return;
    theme::BeginCard("needinstall");
    theme::Dot(theme::Warn()); theme::Wrapped(g_app.game.valid ? "Install the VR mod on the Main Menu first; these settings live in its ddda_vr.ini." : "Choose your game folder on the Main Menu first.", theme::Text());
    theme::EndCard();
}

// ---------------------------------------------------------------- Main Menu
static void ReadyCard() {
    const State& s = g_app.state; bool ok = g_app.game.valid && s.installed;
    theme::BeginCard("ready", 22);
    ImDrawList* dl = ImGui::GetWindowDrawList(); float W = ImGui::GetContentRegionAvail().x;
    {   const char* t = !g_app.game.valid ? "GAME NOT FOUND" : !s.installed ? "INSTALL REQUIRED" : g_app.running ? "NOW PLAYING" : "READY TO PLAY";
        ImFont* f = theme::g_fonts.capsLg; float sp = 2.0f * g_s, tw = theme::SpacedCapsWidth(f, t, sp);
        ImVec2 c = ImGui::GetCursorScreenPos(); theme::SpacedCaps(dl, f, ImVec2(c.x + (W - tw) * 0.5f, c.y), ok ? theme::Teal() : theme::Warn(), t, sp);
        ImGui::Dummy(ImVec2(W, f->FontSize + 10 * g_s)); }
    {   // progress-style bar: how ready the setup is (game found, mod installed, runtime present)
        int steps = (g_app.game.valid ? 1 : 0) + (s.installed && !s.outdated ? 1 : 0) + (g_app.runtime.found ? 1 : 0);
        float frac = steps / 3.0f; ImVec2 c = ImGui::GetCursorScreenPos(); float h = 6 * g_s;
        dl->AddRectFilled(c, ImVec2(c.x + W, c.y + h), IM_COL32(44, 41, 44, 255), h);
        if (frac > 0) dl->AddRectFilled(c, ImVec2(c.x + W * frac, c.y + h), theme::Gold(), h);
        ImGui::Dummy(ImVec2(W, h + 4 * g_s));
        char l[64]; snprintf(l, sizeof l, "%d%%", (int)std::round(frac * 100));
        ImGui::PushFont(theme::g_fonts.bold); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::Gold()), "%s", l); ImGui::PopFont();
        ImGui::SameLine(); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::Muted()), "%s", !g_app.game.valid ? "Game folder needed" : !s.installed ? "VR mod not installed" : s.outdated ? "Update available: Reinstall" : ("Dragon's Dogma VR " + std::string(kModVersion)).c_str());
        std::string rt = g_app.runtime.found ? "Runtime: " + g_app.runtime.name : "No OpenXR runtime";
        float rw = ImGui::CalcTextSize(rt.c_str()).x; ImGui::SameLine(ImGui::GetCursorPosX() + 0); ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - rw - 22 * g_s));
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::Muted()), "%s", rt.c_str());
    }
    {   // requirements strip with a gold edge
        ImVec2 c = ImGui::GetCursorScreenPos(); float h = ImGui::GetTextLineHeight() + 14 * g_s;
        dl->AddRectFilled(c, ImVec2(c.x + W - 22 * g_s, c.y + h), IM_COL32(24, 23, 26, 255), 4 * g_s); dl->AddRectFilled(c, ImVec2(c.x + 2.5f * g_s, c.y + h), theme::Gold());
        ImGui::PushFont(theme::g_fonts.small);
        dl->AddText(ImVec2(c.x + 14 * g_s, c.y + (h - ImGui::GetTextLineHeight()) * 0.5f), theme::Muted(), "Requires: a PC VR headset with an OpenXR runtime  \xe2\x80\xa2  DirectX 11 graphics card  \xe2\x80\xa2  NVIDIA RTX for DLSS (optional)");
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(W, h + 8 * g_s));
    }
    {   // 0.8.1: the game's own settings the VR mod needs
        const GameConfig& gc = g_app.cfg;
        if (gc.found && !gc.issues.empty()) {
            theme::Dot(theme::Warn()); ImGui::PushFont(theme::g_fonts.bold); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::Warn()), "Game settings need a change for VR"); ImGui::PopFont();
            for (const auto& i : gc.issues) theme::Bullet(i.c_str());
            if (theme::Button("Fix game settings", true, !g_app.running && !g_app.busy)) {
                std::string err;
                if (FixGameConfig(err)) { Notice("Game settings fixed: V-Sync off, windowed, variable frame rate. A backup of config.ini was kept.", theme::Good()); g_app.cfg = CheckGameConfig(); }
                else Notice(err, theme::Bad());
            }
            if (g_app.running) { ImGui::SameLine(); theme::Wrapped("Close the game to fix them.", theme::Muted()); }
            ImGui::Dummy(ImVec2(W, 6 * g_s));
        } else {
            ImGui::PushFont(theme::g_fonts.small);
            if (gc.found) { theme::Dot(theme::Good()); theme::Wrapped("Game settings OK: V-Sync off, windowed, variable frame rate, View Range Farthest.", theme::Muted()); }
            else { theme::Dot(theme::Muted()); theme::Wrapped("In the game's options set V-Sync Off, Windowed, Frame rate Variable. (The launcher checks them once the game has run.)", theme::Muted()); }
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(W, 4 * g_s));
        }
    }
    float bw = 170 * g_s; ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (W - bw) * 0.5f - 11 * g_s);
    bool can = ok && !g_app.busy && !g_app.running;
    if (theme::PlayButton(g_app.running ? "RUNNING" : "PLAY", can, bw)) {
        std::string err;
        if (Launch(g_app.game, err)) { Notice("Starting Dragon's Dogma VR... Put your headset on.", theme::Good()); g_app.running = true; }
        else Notice(err, theme::Bad());
    }
    theme::EndCard();
}
static void PageMainDraw(HWND hwnd) {
    theme::Banner("Dragon's Dogma: Dark Arisen VR", "A VIRTUAL REALITY MOD");
    ImGui::Indent(34 * g_s); ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 34 * g_s);
    float W = ImGui::GetContentRegionMax().x - 34 * g_s - ImGui::GetCursorPosX();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(IM_COL32(206, 199, 188, 255)));
    ImGui::TextWrapped("Full Native Stereo with DLSS, DXVK support, and optional effects and interface settings");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(1, 4 * g_s));
    {   // VR mod info grid (two columns, like the concept's publisher / genre block)
        const State& s = g_app.state;
        std::string ver = std::string(kModVersion), status = !g_app.game.valid ? "Game not found" : s.installed ? (s.outdated ? "Installed (update available)" : "Installed") : "Not installed";
        std::string edition = !g_app.game.valid ? "-" : std::string(StoreName(g_app.game.store)) + (g_app.game.build == BuildUnknown ? " (unrecognised version)" : "");
        ImGui::Columns(2, "##info", false);
        theme::InfoPair("VR MOD VERSION", ver.c_str()); theme::InfoPair("GAME EDITION", edition.c_str());
        ImGui::NextColumn();
        theme::InfoPair("STATUS", status.c_str(), s.installed && !s.outdated ? theme::Good() : theme::Warn()); theme::InfoPair("SUPPORTED", "Steam and GOG");
        ImGui::Columns(1);
    }
    // game folders
    theme::SectionTitle("Game Folders");
    theme::BeginCard("games");
    if (g_app.games.empty()) { theme::Dot(theme::Bad()); theme::Wrapped("Dragon's Dogma: Dark Arisen wasn't found automatically. Use Browse to pick the game folder (the one with DDDA.exe).", theme::Text()); }
    for (int i = 0; i < (int)g_app.games.size(); ++i) {
        const Game& g = g_app.games[i]; State st = i == g_app.gameIdx ? g_app.state : Check(g);
        ImGui::PushID(i);
        ImVec2 c = ImGui::GetCursorScreenPos(); float w = ImGui::GetContentRegionAvail().x, h = 50 * g_s;
        bool sel = i == g_app.gameIdx;
        if (ImGui::InvisibleButton("##g", ImVec2(w, h)) && !g_app.busy) { SelectGame(i); g_app.haveReport = false; g_app.confirmUninstall = false; }
        bool hov = ImGui::IsItemHovered(); if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(c, ImVec2(c.x + w, c.y + h), sel ? IM_COL32(40, 34, 27, 255) : hov ? theme::CardHi() : IM_COL32(32, 31, 35, 255), 6 * g_s);
        if (sel) dl->AddRect(c, ImVec2(c.x + w, c.y + h), theme::GoldDim(), 6 * g_s, 0, 1.2f * g_s);
        // radio
        ImVec2 rc(c.x + 20 * g_s, c.y + h * 0.5f); dl->AddCircle(rc, 7 * g_s, sel ? theme::Gold() : theme::Muted(), 20, 1.5f * g_s); if (sel) dl->AddCircleFilled(rc, 3.6f * g_s, theme::Gold(), 16);
        // store badge
        const char* sn = StoreName(g.store); ImFont* lf = theme::g_fonts.label; float bw = lf->CalcTextSizeA(lf->FontSize, 1e9f, 0, sn).x + 16 * g_s;
        ImVec2 b0(c.x + 40 * g_s, c.y + h * 0.5f - 10 * g_s); dl->AddRectFilled(b0, ImVec2(b0.x + bw, b0.y + 20 * g_s), g.store == StoreSteam ? IM_COL32(38, 60, 92, 255) : g.store == StoreGog ? IM_COL32(86, 46, 110, 255) : IM_COL32(60, 60, 60, 255), 4 * g_s);
        dl->AddText(lf, lf->FontSize, ImVec2(b0.x + 8 * g_s, b0.y + (20 * g_s - lf->FontSize) * 0.5f), IM_COL32(236, 236, 240, 255), sn);
        std::string path = Narrow(g.root);
        dl->PushClipRect(ImVec2(b0.x + bw + 12 * g_s, c.y), ImVec2(c.x + w - 170 * g_s, c.y + h), true);
        dl->AddText(theme::g_fonts.body, theme::g_fonts.body->FontSize, ImVec2(b0.x + bw + 12 * g_s, c.y + (h - theme::g_fonts.body->FontSize) * 0.5f), theme::Text(), path.c_str());
        dl->PopClipRect();
        const char* stt = st.installed ? (st.outdated ? "Update available" : "VR installed") : "VR not installed";
        ImU32 sc = st.installed ? (st.outdated ? theme::Warn() : theme::Good()) : theme::Muted();
        float sw = ImGui::CalcTextSize(stt).x; ImVec2 sp(c.x + w - sw - 16 * g_s, c.y + (h - ImGui::GetTextLineHeight()) * 0.5f);
        dl->AddCircleFilled(ImVec2(sp.x - 10 * g_s, sp.y + ImGui::GetTextLineHeight() * 0.55f), 3.5f * g_s, sc, 12); dl->AddText(sp, sc, stt);
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(1, 2 * g_s));
    if (theme::Button("Browse...", false, !g_app.busy)) BrowseForGame(hwnd);
    ImGui::SameLine(); if (theme::Button("Scan again", false, !g_app.busy)) { Rescan(); Notice(g_app.games.empty() ? "No game folder found." : "Found " + std::to_string(g_app.games.size()) + " game folder(s).", theme::Muted()); }
    if (g_app.game.valid) { ImGui::SameLine(); if (theme::Button("Open game folder")) ShellExecuteW(nullptr, L"open", g_app.game.root.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
    theme::EndCard();
    // install
    if (g_app.game.valid) {
        theme::SectionTitle("VR Mod");
        theme::BeginCard("mod");
        const State& s = g_app.state;
        if (s.installed) {
            theme::Dot(s.outdated ? theme::Warn() : theme::Good());
            std::string t = "Installed in this game folder" + (s.installedVersion.empty() ? std::string(".") : " (version " + s.installedVersion + ").");
            if (s.outdated) t += " The files differ from this launcher's: Reinstall to update (your settings are kept).";
            theme::Wrapped(t.c_str(), theme::Text());
        } else { theme::Dot(theme::Muted()); theme::Wrapped("Not installed in this game folder. Install adds the VR files next to DDDA.exe; the game's own files are not changed.", theme::Text()); }
        if (s.foreignD3d9) { theme::Dot(theme::Warn()); theme::Wrapped("Another d3d9.dll (e.g. ReShade) is in the game folder. Installing keeps it as a backup and puts it back on uninstall.", theme::Text()); }
        if (s.needsElevation) { theme::Dot(theme::Warn()); theme::Wrapped("The game folder needs administrator rights to change; Windows will ask once.", theme::Text()); }
        if (g_app.running) { theme::Dot(theme::Warn()); theme::Wrapped("The game is running. Close it to install or remove the mod.", theme::Text()); }
        ImGui::Dummy(ImVec2(1, 2 * g_s));
        bool can = !g_app.busy && !g_app.running;
        if (theme::Button(s.installed ? "Reinstall / update" : "Install VR mod", true, can)) { g_app.confirmUninstall = false; StartOp("install", false); }
        if (s.installed) { ImGui::SameLine(); if (theme::Button("Uninstall", false, can)) g_app.confirmUninstall = !g_app.confirmUninstall; }
        if (g_app.confirmUninstall && s.installed) {
            ImGui::Dummy(ImVec2(1, 2));
            theme::Wrapped("Remove Dragon's Dogma VR from this game folder? The game itself and your save games are not touched.", theme::Text());
            ImGui::Checkbox("Also delete my VR settings (ddda_vr.ini)", &g_app.deleteIni);
            if (theme::Button("Yes, uninstall", true, can)) { g_app.confirmUninstall = false; StartOp("uninstall", g_app.deleteIni); }
            ImGui::SameLine(); if (theme::Button("Cancel")) g_app.confirmUninstall = false;
        }
        if (g_app.haveReport) {
            ImGui::Dummy(ImVec2(1, 4 * g_s));
            for (auto& l : g_app.report.lines) if (l.size() > 2) { theme::Dot(l[0] == '+' ? theme::Good() : l[0] == '!' ? theme::Warn() : theme::Bad()); theme::Wrapped(l.c_str() + 2, theme::Text()); }
        }
        theme::EndCard();
    }
    // runtime
    theme::SectionTitle("VR Runtime");
    theme::BeginCard("runtime");
    const Runtime& rt = g_app.runtime;
    if (rt.found) {
        theme::Dot(theme::Good()); ImGui::PushFont(theme::g_fonts.bold); ImGui::TextUnformatted(rt.name.c_str()); ImGui::PopFont();
        theme::Wrapped("The active OpenXR runtime. The game will start in this runtime; switch it in the runtime's own app (Virtual Desktop, SteamVR, Meta Quest Link...).", theme::Muted());
        ImGui::PushFont(theme::g_fonts.small); theme::Wrapped(rt.manifest.c_str(), IM_COL32(110, 104, 96, 255)); ImGui::PopFont();
    } else {
        theme::Dot(theme::Bad()); ImGui::PushFont(theme::g_fonts.bold); ImGui::TextUnformatted(rt.manifest.empty() ? "No OpenXR runtime is active" : "The active OpenXR runtime is missing"); ImGui::PopFont();
        theme::Wrapped("Start your headset's PC app (Virtual Desktop, SteamVR or Meta Quest Link) and set it as the OpenXR runtime, then press Check again.", theme::Muted());
    }
    if (theme::Button("Check again")) { g_app.runtime = ActiveRuntime(); }
    theme::EndCard();
    // shortcut
    theme::SectionTitle("Desktop Shortcut");
    theme::BeginCard("shortcut");
    if (theme::Button("Create desktop shortcut")) DesktopShortcut();
    ImGui::SameLine(); ImGui::AlignTextToFramePadding(); theme::Wrapped("A shortcut to this launcher on your desktop.", theme::Muted());
    theme::EndCard();
    ImGui::Dummy(ImVec2(1, 10 * g_s));
    ReadyCard();
    ImGui::Dummy(ImVec2(1, 24 * g_s));
    (void)W;
    ImGui::PopTextWrapPos(); ImGui::Unindent(34 * g_s);
}

// ---------------------------------------------------------------- Settings
static void DistanceCards() {
    static const struct { const wchar_t* v; const char* name; const char* tag; ImU32 col; const char* help; } k[] = {
        { L"1", "Game Default", "No extra cost", IM_COL32(96, 200, 120, 255), "The game's own High (Farthest) draw distance." },
        { L"2", "2X", "Performance Impact", IM_COL32(232, 176, 70, 255), "One more ring of full-detail trees, rocks and cliffs around you." },
        { L"3", "3X", "Extreme Performance Impact", IM_COL32(222, 92, 72, 255), "Two more rings. Very demanding; for the fastest PCs." },
    };
    std::wstring cur = Get(L"DrawDistance", L"1");
    float gap = 12 * g_s, w = (ImGui::GetContentRegionAvail().x - gap * 2) / 3, h = 108 * g_s;
    ImGui::BeginDisabled(!Installed());
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine(0, gap);
        ImGui::PushID(i);
        ImVec2 c = ImGui::GetCursorScreenPos(); bool sel = cur == k[i].v || (i == 0 && cur != L"2" && cur != L"3");
        if (ImGui::InvisibleButton("##d", ImVec2(w, h)) && Installed()) Set(L"DrawDistance", k[i].v);
        bool hov = ImGui::IsItemHovered() && Installed(); if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(c, ImVec2(c.x + w, c.y + h), sel ? IM_COL32(42, 35, 27, 255) : hov ? theme::CardHi() : IM_COL32(32, 31, 35, 255), 8 * g_s);
        dl->AddRect(c, ImVec2(c.x + w, c.y + h), sel ? theme::Gold() : IM_COL32(52, 48, 52, 255), 8 * g_s, 0, sel ? 1.6f * g_s : 1.0f);
        ImFont* f = theme::g_fonts.displaySm; dl->AddText(f, f->FontSize, ImVec2(c.x + 16 * g_s, c.y + 14 * g_s), sel ? theme::Gold() : theme::Text(), k[i].name);
        dl->AddCircleFilled(ImVec2(c.x + 20 * g_s, c.y + 54 * g_s), 3.5f * g_s, k[i].col, 12);
        dl->AddText(theme::g_fonts.label, theme::g_fonts.label->FontSize, ImVec2(c.x + 30 * g_s, c.y + 54 * g_s - theme::g_fonts.label->FontSize * 0.55f), k[i].col, k[i].tag);
        ImFont* sf = theme::g_fonts.small;
        dl->AddText(sf, sf->FontSize, ImVec2(c.x + 16 * g_s, c.y + 70 * g_s), theme::Muted(), k[i].help, nullptr, w - 30 * g_s);
        ImGui::PopID();
    }
    ImGui::EndDisabled();
}
static void PageSettingsDraw() {
    theme::Banner("Settings", "PICTURE  \xe2\x80\xa2  DRAW DISTANCE  \xe2\x80\xa2  PERFORMANCE");
    ImGui::Indent(34 * g_s); ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 34 * g_s);
    NeedInstall();
    bool dlss = Get(L"Dlss", L"1") != L"0";
    theme::SectionTitle("DLSS Settings");
    theme::BeginCard("dlss");
    ComboRow("Upscaler", "NVIDIA DLSS needs an RTX graphics card. Off renders at the full headset resolution.", L"Dlss", L"1", kUpscaler, 2, Installed());
    ComboRow("Upscaler quality", nullptr, L"DlssQuality", L"0", kQuality, 6, Installed() && dlss);
    if (dlss) {
        int q = _wtoi(Get(L"DlssQuality", L"0").c_str()); if (q < 0 || q > 5) q = 0;
        int w = _wtoi(Get(L"VRModeWidth", L"3072").c_str()), h = _wtoi(Get(L"VRModeHeight", L"3264").c_str());
        char t[200];
        if (q == 0) snprintf(t, sizeof t, "DLAA renders each eye at %d x %d and smooths the edges: the sharpest picture.", w, h);
        else snprintf(t, sizeof t, "Renders each eye at %d x %d (%.0f%% of the pixels) and DLSS rebuilds it to %d x %d. Lower modes run faster and look softer.", (int)(w * kRatio[q]), (int)(h * kRatio[q]), kRatio[q] * kRatio[q] * 100, w, h);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 324 * g_s); theme::Wrapped(t, theme::Muted()); ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(1, 4 * g_s));
    }
    ComboRow("Presets", "The DLSS model. K is the tested default.", L"DlssPreset", L"11", kPreset, 5, Installed() && dlss);
    theme::EndCard();
    theme::SectionTitle("Distance Scaling");
    theme::Wrapped("How far around you the game keeps trees, rocks, cliffs and other scenery at full detail. Each step loads more of the world, which costs frame rate and memory.", theme::Muted());
    ImGui::Dummy(ImVec2(1, 2 * g_s));
    DistanceCards();
    theme::SectionTitle("Performance");
    theme::BeginCard("perf");
    bool dxvk = Installed() && !Get(L"D3D9Dll", L"").empty();
    {   bool v = dxvk;
        if (theme::Toggle("DXVK (Vulkan)", &v, "Runs the game's DirectX 9 on Vulkan. Much lighter on the CPU in busy towns and forests. The first time you visit an area it builds a shader cache, which can stutter briefly.", Installed()))
            Set(L"D3D9Dll", v ? L"d3d9_dxvk.dll" : L""); }
    ImGui::Dummy(ImVec2(1, 4 * g_s));
    ToggleKey("\"Building shader cache\" notice", "With DXVK: shows a small notice in the headset while shaders are being built, so short pauses aren't a surprise.", L"ShaderCacheNotice", L"1", false, dxvk);
    theme::EndCard();
    ImGui::Dummy(ImVec2(1, 24 * g_s));
    ImGui::PopTextWrapPos(); ImGui::Unindent(34 * g_s);
}

// ---------------------------------------------------------------- Optional
static void PageOptionalDraw() {
    theme::Banner("Optional", "EFFECTS  \xe2\x80\xa2  INTERFACE");
    ImGui::Indent(34 * g_s); ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 34 * g_s);
    NeedInstall();
    theme::SectionTitle("Effects");
    theme::BeginCard("fx");
    ToggleKey("Show Sun Shafts", "The game's screen-space god rays. Off by default: in VR they float in front of you rather than in the world.", L"KillGodRays", L"1", true);
    ImGui::Dummy(ImVec2(1, 4 * g_s));
    ToggleKey("Show Lens Flare", "Lens flare from the sun. Off by default: a camera effect that looks odd in VR.", L"HideLensFlare", L"1", true);
    theme::EndCard();
    theme::SectionTitle("Interface");
    theme::BeginCard("ui");
    ToggleKey("Markers in the world", "Quest markers, pawn names and enemy health bars stay above their characters in the world (off: they appear on the HUD panel).", L"WorldMarkers", L"1");
    ImGui::Dummy(ImVec2(1, 4 * g_s));
    SliderRow("Marker size", nullptr, L"WorldMarkerScale", L"1.0", 0.6f, 1.6f, "%.0f%%", 100, Installed() && Get(L"WorldMarkers", L"1") != L"0");
    SliderRow("Dialogue box size", "NPC conversation text and its box.", L"DialogueScale", L"1.3", 1.0f, 1.6f, "%.0f%%", 100, Installed());
    SliderRow("Subtitle height", "How far cutscene subtitles are raised from the bottom of the view.", L"SubtitleRaise", L"190", 0.0f, 300.0f, "%.0f", 1, Installed());
    SliderRow("Subtitle distance", "How far away cutscene subtitles appear.", L"SubtitleDepth", L"2.0", 1.0f, 5.0f, "%.1f m", 1, Installed());
    ComboRow("Bow reticle", "The aiming reticle while drawing a bow.", L"ReticleStyle", L"2", kReticle, 3, Installed());
    theme::EndCard();
    ImGui::Dummy(ImVec2(1, 24 * g_s));
    ImGui::PopTextWrapPos(); ImGui::Unindent(34 * g_s);
}

// ---------------------------------------------------------------- About
static void PageAboutDraw() {
    theme::Banner("About", "CREDITS  \xe2\x80\xa2  HELP");
    ImGui::Indent(34 * g_s); ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 34 * g_s);
    theme::SectionTitle("Dragon's Dogma VR");
    theme::BeginCard("about");
    ImGui::PushFont(theme::g_fonts.bold); ImGui::Text("Dragon's Dogma VR %s", kModVersion); ImGui::PopFont();
    theme::Wrapped("A virtual reality mod for Dragon's Dogma: Dark Arisen by Braxus. Full stereo 3D with head tracking, a comfortable HUD panel, world-placed markers and name tags, NVIDIA DLSS / DLAA, optional DXVK and extended draw distance.", theme::Muted());
    ImGui::Dummy(ImVec2(1, 4 * g_s));
    theme::Bullet("Head tracking and full stereo rendering of the game's own third-person view");
    theme::Bullet("NVIDIA DLSS and DLAA through a 64-bit helper (the game itself is 32-bit)");
    theme::Bullet("Supports the Steam and GOG releases");
    theme::EndCard();
    theme::SectionTitle("Need Help?");
    theme::BeginCard("help");
    theme::Wrapped("If the game crashes or something doesn't look right, collect the logs and send the zip with your report. It gathers the mod's log, settings and a short system summary (Windows, CPU, GPU, VR runtime) into one zip on your desktop. Nothing is uploaded.", theme::Muted());
    if (theme::Button("Collect logs", true)) {
        std::string err; std::wstring z = CollectLogs(g_app.game, err);
        if (!z.empty()) { Notice("Saved to your desktop: " + Narrow(z.substr(z.find_last_of(L'\\') + 1)), theme::Good()); std::wstring arg = L"/select,\"" + z + L"\""; ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL); }
        else Notice(err, theme::Bad());
    }
    ImGui::SameLine(); if (theme::Button("Open launcher log")) { std::wstring p = PrefsDir() + L"\\launcher.log"; ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
    if (Installed()) { ImGui::SameLine(); if (theme::Button("Open ddda_vr.ini")) ShellExecuteW(nullptr, L"open", IniPath(g_app.game).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
    theme::EndCard();
    theme::SectionTitle("Credits");
    theme::BeginCard("credits");
    theme::Bullet("Dragon's Dogma VR by Braxus.");
    theme::Bullet("DLSS helper: from the Dishonored VR mod by Pizza Parker (used with permission), based on the BioShock VR DLSS fork (MIT) and DLSS5-Feeder (MIT).");
    theme::Bullet("Launcher: built on the Far Cry 2 VR launcher, whose design follows Pizza Parker's Dishonored VR launcher (with permission).");
    theme::Bullet("DXVK by Philip Rebohle and contributors (zlib licence). miniz by Rich Geldreich (MIT).");
    theme::Bullet("Dear ImGui by Omar Cornut (MIT). OpenXR loader by the Khronos Group (Apache 2.0). NVIDIA DLSS (NVIDIA RTX SDK licence).");
    theme::Bullet("Typefaces: Chomsky by Fredrick R. Brennan and Cinzel by Natanael Gama (both SIL Open Font Licence).");
    ImGui::Dummy(ImVec2(1, 4 * g_s));
    theme::Wrapped("Dragon's Dogma is a trademark of CAPCOM. This is an unofficial fan mod, not affiliated with or endorsed by CAPCOM. Header artwork is AI-generated fan art.", IM_COL32(110, 104, 96, 255));
    theme::EndCard();
    ImGui::Dummy(ImVec2(1, 24 * g_s));
    ImGui::PopTextWrapPos(); ImGui::Unindent(34 * g_s);
}

// ---------------------------------------------------------------- frame
static void DrawUI(HWND hwnd) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos); ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float sideW = 214 * g_s, H = vp->Size.y;
    dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + H), theme::Bg());
    // sidebar
    dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + sideW, vp->Pos.y + H), theme::Side());
    dl->AddLine(ImVec2(vp->Pos.x + sideW, vp->Pos.y), ImVec2(vp->Pos.x + sideW, vp->Pos.y + H), IM_COL32(38, 35, 38, 255), 1.0f);
    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::BeginChild("##side", ImVec2(sideW, H), 0, ImGuiWindowFlags_NoScrollbar);
    ImGui::Dummy(ImVec2(1, 22 * g_s));
    theme::Emblem(76 * g_s);
    {   // the name under the emblem, on two centred lines (too wide for the sidebar on one)
        ImFont* f = theme::g_fonts.caps; float sp = 1.4f * g_s;
        ImGui::Dummy(ImVec2(1, 8 * g_s));
        for (const char* t : { "DRAGON'S DOGMA:", "DARK ARISEN VR" }) {
            float tw = theme::SpacedCapsWidth(f, t, sp); ImVec2 c = ImGui::GetCursorScreenPos();
            theme::SpacedCaps(ImGui::GetWindowDrawList(), f, ImVec2(c.x + (sideW - tw) * 0.5f, c.y), theme::Gold(), t, sp); ImGui::Dummy(ImVec2(1, f->FontSize + 2 * g_s));
        }
        ImGui::Dummy(ImVec2(1, 16 * g_s)); }
    static const char* names[] = { "Main Menu", "Settings", "Optional", "About" };
    for (int i = 0; i < 4; ++i) if (theme::NavItem(names[i], i == 0 ? 0 : i == 1 ? 1 : i == 2 ? 2 : 3, g_app.page == i)) { if (g_app.page != i) g_app.scrollReset = 0; g_app.page = (Page)i; }
    {   // bottom: the selected game, like the concept's account chip
        float y = H - 64 * g_s; ImGui::SetCursorPosY(y);
        ImVec2 c = ImGui::GetCursorScreenPos(); ImDrawList* sd = ImGui::GetWindowDrawList();
        sd->AddLine(ImVec2(c.x + 16 * g_s, c.y - 10 * g_s), ImVec2(c.x + sideW - 16 * g_s, c.y - 10 * g_s), IM_COL32(38, 35, 38, 255), 1.0f);
        ImVec2 m(c.x + 34 * g_s, c.y + 20 * g_s);
        bool ok = g_app.game.valid && g_app.state.installed;
        sd->AddCircleFilled(m, 14 * g_s, ok ? IM_COL32(150, 110, 52, 255) : IM_COL32(60, 56, 58, 255), 24);
        ImFont* lf = theme::g_fonts.label; const char* ini = g_app.game.store == StoreGog ? "G" : g_app.game.store == StoreSteam ? "S" : "?";
        ImVec2 isz = lf->CalcTextSizeA(lf->FontSize, 1e9f, 0, ini); sd->AddText(lf, lf->FontSize, ImVec2(m.x - isz.x * 0.5f, m.y - isz.y * 0.5f), IM_COL32(255, 246, 228, 255), ini);
        std::string l1 = g_app.game.valid ? std::string(StoreName(g_app.game.store)) + " edition" : "No game selected";
        sd->AddText(theme::g_fonts.bold, theme::g_fonts.bold->FontSize * 0.92f, ImVec2(m.x + 24 * g_s, m.y - 17 * g_s), theme::Text(), l1.c_str());
        sd->AddText(theme::g_fonts.small, theme::g_fonts.small->FontSize, ImVec2(m.x + 24 * g_s, m.y + 2 * g_s), ok ? theme::Good() : theme::Muted(), !g_app.game.valid ? "Choose a folder" : ok ? (g_app.running ? "Game running" : "VR installed") : "VR not installed");
    }
    ImGui::EndChild();
    // page
    ImGui::SetCursorPos(ImVec2(sideW + 1, 0));
    ImGui::BeginChild("##page", ImVec2(vp->Size.x - sideW - 1, H), 0, 0);
    if (g_app.scrollReset >= 0) { ImGui::SetScrollY(0); g_app.scrollReset = -1; }
    if (g_app.busy) {
        theme::Banner("Please wait", nullptr);
        ImGui::Indent(34 * g_s); ImGui::TextUnformatted(g_app.busyText.c_str());
        float t = (float)ImGui::GetTime(); ImVec2 c = ImGui::GetCursorScreenPos(); float r = 14 * g_s;
        ImGui::GetWindowDrawList()->PathArcTo(ImVec2(c.x + r, c.y + r + 8 * g_s), r, t * 5, t * 5 + 4.2f, 28); ImGui::GetWindowDrawList()->PathStroke(theme::Gold(), 0, 3 * g_s);
        ImGui::Unindent(34 * g_s); g_framesPending = 2;
    } else switch (g_app.page) {
        case PageMain: PageMainDraw(hwnd); break;
        case PageSettings: PageSettingsDraw(); break;
        case PageOptional: PageOptionalDraw(); break;
        case PageAbout: PageAboutDraw(); break;
    }
    ImGui::EndChild();
    // notice toast (bottom right of the page, fades after a few seconds)
    if (!g_app.notice.empty()) {
        double age = ImGui::GetTime() - g_app.noticeAt; float a = age < 5.0 ? 1.0f : age < 6.0 ? (float)(6.0 - age) : 0.0f;
        if (a <= 0) g_app.notice.clear();
        else {
            ImDrawList* fg = ImGui::GetForegroundDrawList(); ImFont* f = theme::g_fonts.body;
            float maxW = std::min(520 * g_s, vp->Size.x - sideW - 60 * g_s);
            ImVec2 ts = f->CalcTextSizeA(f->FontSize, 1e9f, maxW, g_app.notice.c_str());
            ImVec2 b(vp->Pos.x + vp->Size.x - 24 * g_s, vp->Pos.y + H - 22 * g_s), a0(b.x - ts.x - 40 * g_s, b.y - ts.y - 22 * g_s);
            fg->AddRectFilled(a0, b, IM_COL32(30, 28, 32, (int)(245 * a)), 8 * g_s); fg->AddRect(a0, b, IM_COL32(70, 64, 60, (int)(255 * a)), 8 * g_s);
            ImU32 col = g_app.noticeCol ? g_app.noticeCol : theme::Text();
            fg->AddCircleFilled(ImVec2(a0.x + 16 * g_s, a0.y + 11 * g_s + f->FontSize * 0.55f), 4 * g_s, (col & 0x00FFFFFF) | ((ImU32)(255 * a) << 24), 12);
            fg->AddText(f, f->FontSize, ImVec2(a0.x + 28 * g_s, a0.y + 11 * g_s), (theme::Text() & 0x00FFFFFF) | ((ImU32)(255 * a) << 24), g_app.notice.c_str(), nullptr, maxW);
            g_framesPending = 2;
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------- window
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) { g_framesPending = 3; return 1; }
    switch (msg) {
    case WM_SIZE: if (wp != SIZE_MINIMIZED) { g_resizeW = LOWORD(lp); g_resizeH = HIWORD(lp); } g_framesPending = 3; return 0;
    case WM_GETMINMAXINFO: { MINMAXINFO* m = (MINMAXINFO*)lp; m->ptMinTrackSize = { (LONG)(960 * g_scale), (LONG)(620 * g_scale) }; return 0; }
    case WM_DPICHANGED: { g_scale = HIWORD(wp) / 96.0f; g_fontsDirty = true; RECT* r = (RECT*)lp; SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE); return 0; }
    case WM_SYSCOMMAND: if ((wp & 0xfff0) == SC_KEYMENU) return 0; break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) g_framesPending = 3;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    int argc = 0; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) if (!wcscmp(argv[i], L"--op")) { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); int r = ElevatedMain(argc, argv); CoUninitialize(); return r; }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Log("launcher %s started", kModVersion);
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\DragonsDogmaVR.Launcher");
    if (GetLastError() == ERROR_ALREADY_EXISTS) { HWND o = FindWindowW(L"DragonsDogmaVRLauncher", nullptr); if (o) { ShowWindow(o, SW_RESTORE); SetForegroundWindow(o); } return 0; }

    WNDCLASSEXW wc{ sizeof wc }; wc.style = CS_CLASSDC; wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = L"DragonsDogmaVRLauncher";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP)); wc.hIconSm = wc.hIcon; wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    RegisterClassExW(&wc);
    POINT pt; GetCursorPos(&pt); HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof mi }; GetMonitorInfoW(mon, &mi);
    UINT dpi = GetDpiForSystem(); { typedef HRESULT (WINAPI *GDFM)(HMONITOR, int, UINT*, UINT*); auto f = (GDFM)GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor"); UINT y; if (f) f(mon, 0, &dpi, &y); }
    g_scale = dpi / 96.0f;
    int ww = (int)(1180 * g_scale), wh = (int)(800 * g_scale);
    int workW = mi.rcWork.right - mi.rcWork.left, workH = mi.rcWork.bottom - mi.rcWork.top;
    if (ww > workW - 40) ww = workW - 40; if (wh > workH - 40) wh = workH - 40;
    RECT r{ 0, 0, ww, wh };
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = mi.rcWork.left + (workW - w) / 2, y = mi.rcWork.top + (workH - h) / 2;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Dragon's Dogma VR", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, inst, nullptr);
    { BOOL dark = TRUE; typedef HRESULT (WINAPI *DSWA)(HWND, DWORD, LPCVOID, DWORD); auto f = (DSWA)GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmSetWindowAttribute"); if (f) { f(hwnd, 20, &dark, sizeof dark); COLORREF cap = RGB(15, 14, 16); f(hwnd, 35, &cap, sizeof cap); } }
    if (!CreateDevice(hwnd)) { MessageBoxW(hwnd, L"Direct3D 11 isn't available on this PC, so the launcher can't open.", L"Dragon's Dogma VR", MB_ICONERROR); return 1; }
    ShowWindow(hwnd, show); UpdateWindow(hwnd);

    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); io.IniFilename = nullptr; io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    theme::LoadFonts(g_scale); theme::ApplyStyle(g_scale);
    ImGui_ImplWin32_Init(hwnd); ImGui_ImplDX11_Init(g_dev, g_ctx);
    theme::g_banner = LoadImageRes(IDR_ART_BANNER); theme::g_emblem = LoadImageRes(IDR_ART_EMBLEM);

    Rescan(); g_app.running = GameRunning();
    double nextPoll = 0;
    bool done = false;
    while (!done) {
        DWORD wait = (g_framesPending > 0 || g_app.busy) ? 16 : 1000;
        MsgWaitForMultipleObjectsEx(0, nullptr, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); if (msg.message == WM_QUIT) done = true; }
        if (done) break;
        if (IsIconic(hwnd)) continue;
        if (GetTickCount64() > nextPoll) { bool run = GameRunning(); if (run != g_app.running) { g_app.running = run; if (!run) Refresh(); g_framesPending = 3; } nextPoll = GetTickCount64() + 1000.0; }
        PollWorker();
        if (g_resizeW) { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0); g_resizeW = g_resizeH = 0; CreateRT(); }
        if (g_fontsDirty) { g_fontsDirty = false; ImGui_ImplDX11_InvalidateDeviceObjects(); theme::LoadFonts(g_scale); theme::ApplyStyle(g_scale); ImGui_ImplDX11_CreateDeviceObjects(); }
        if (g_framesPending <= 0 && !g_app.busy) continue;
        --g_framesPending;
        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        DrawUI(hwnd);
        ImGui::Render();
        const float clear[4] = { 0.078f, 0.075f, 0.086f, 1 };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr); g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    if (g_rtv) g_rtv->Release(); if (g_swap) g_swap->Release(); if (g_ctx) g_ctx->Release(); if (g_dev) g_dev->Release();
    DestroyWindow(hwnd); UnregisterClassW(wc.lpszClassName, inst);
    if (single) CloseHandle(single);
    CoUninitialize();
    return 0;
}
