// theme.cpp - see theme.h
#include "theme.h"
#include "payload_ids.h"
#include "imgui_internal.h"
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace theme {
Fonts g_fonts;
float g_s = 1.0f;
Image g_banner, g_emblem;

static const char* SystemFont(const char* const* names, int n) {
    static char path[4][MAX_PATH]; static int k; char* out = path[k++ & 3];
    char win[MAX_PATH]; GetWindowsDirectoryA(win, MAX_PATH);
    for (int i = 0; i < n; ++i) { snprintf(out, MAX_PATH, "%s\\Fonts\\%s", win, names[i]); if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return out; }
    return nullptr;
}
static bool RawResource(int id, void** data, int* size) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA); if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r); if (!g) return false;
    *data = LockResource(g); *size = (int)SizeofResource(nullptr, r); return *data && *size > 0;
}
void LoadFonts(float s) {
    ImGuiIO& io = ImGui::GetIO(); io.Fonts->Clear();
    static const char* body[] = { "segoeui.ttf", "tahoma.ttf", "arial.ttf" };
    static const char* semi[] = { "seguisb.ttf", "segoeuib.ttf", "arialbd.ttf" };
    static const char* bold[] = { "segoeuib.ttf", "seguisb.ttf", "arialbd.ttf" };
    const char* fb = SystemFont(body, 3); const char* fsb = SystemFont(semi, 3); const char* fbb = SystemFont(bold, 3);
    ImFontConfig cfg; cfg.OversampleH = 2; cfg.OversampleV = 2;
    static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x2013, 0x2026, 0x2190, 0x2193, 0x2713, 0x2717, 0 };
    auto sys = [&](const char* f, float px) { ImFont* r = f ? io.Fonts->AddFontFromFileTTF(f, px * s, &cfg, ranges) : nullptr; return r ? r : io.Fonts->AddFontDefault(); };
    auto res = [&](int id, float px, const char* fallback) {
        void* d; int n; ImFontConfig c = cfg; c.FontDataOwnedByAtlas = false;
        ImFont* r = RawResource(id, &d, &n) ? io.Fonts->AddFontFromMemoryTTF(d, n, px * s, &c, ranges) : nullptr;
        return r ? r : sys(fallback, px);
    };
    g_fonts.body = sys(fb, 15.5f);
    g_fonts.small = sys(fb, 13.0f);
    g_fonts.label = sys(fsb, 12.0f);
    g_fonts.bold = sys(fbb, 15.5f);
    g_fonts.side = sys(fb, 15.0f);
    // build t7: page titles and section headings in Chomsky (blackletter, Fredrick Brennan, SIL Open Font Licence)
    g_fonts.display = res(IDR_FONT_GOTHIC, 48.0f, fbb);
    g_fonts.displaySm = res(IDR_FONT_GOTHIC, 26.0f, fbb);
    g_fonts.caps = res(IDR_FONT_TITLE2, 14.0f, fsb);
    g_fonts.capsLg = res(IDR_FONT_TITLE, 21.0f, fbb);
    io.FontDefault = g_fonts.body;
}
void ApplyStyle(float s) {
    g_s = s;
    ImGuiStyle st; ImGui::StyleColorsDark(&st);
    ImVec4* c = st.Colors;
    auto C = [](int r, int g, int b, int a = 255) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a / 255.f); };
    c[ImGuiCol_Text] = C(236, 231, 221); c[ImGuiCol_TextDisabled] = C(110, 104, 96);
    c[ImGuiCol_WindowBg] = C(0, 0, 0, 0); c[ImGuiCol_ChildBg] = C(0, 0, 0, 0); c[ImGuiCol_PopupBg] = C(30, 28, 32, 252);
    c[ImGuiCol_Border] = C(58, 54, 56); c[ImGuiCol_FrameBg] = C(34, 32, 36); c[ImGuiCol_FrameBgHovered] = C(44, 41, 44); c[ImGuiCol_FrameBgActive] = C(54, 49, 50);
    c[ImGuiCol_CheckMark] = C(201, 164, 92); c[ImGuiCol_SliderGrab] = C(201, 164, 92); c[ImGuiCol_SliderGrabActive] = C(226, 190, 118);
    c[ImGuiCol_Button] = C(38, 36, 40); c[ImGuiCol_ButtonHovered] = C(52, 48, 50); c[ImGuiCol_ButtonActive] = C(66, 58, 54);
    c[ImGuiCol_Header] = C(66, 54, 38); c[ImGuiCol_HeaderHovered] = C(86, 68, 44); c[ImGuiCol_HeaderActive] = C(110, 86, 52);
    c[ImGuiCol_ScrollbarBg] = C(0, 0, 0, 0); c[ImGuiCol_ScrollbarGrab] = C(60, 56, 58); c[ImGuiCol_ScrollbarGrabHovered] = C(84, 78, 76); c[ImGuiCol_ScrollbarGrabActive] = C(120, 100, 76);
    c[ImGuiCol_Separator] = C(52, 48, 50); c[ImGuiCol_ModalWindowDimBg] = C(0, 0, 0, 170); c[ImGuiCol_NavHighlight] = C(201, 164, 92);
    st.WindowPadding = ImVec2(0, 0); st.FramePadding = ImVec2(12, 7); st.ItemSpacing = ImVec2(10, 9);
    st.FrameRounding = 6; st.GrabRounding = 6; st.ScrollbarSize = 10; st.ScrollbarRounding = 6; st.WindowBorderSize = 0; st.ChildBorderSize = 0;
    st.FrameBorderSize = 1; st.PopupRounding = 6; st.PopupBorderSize = 1; st.GrabMinSize = 14;
    st.ScaleAllSizes(s);
    ImGui::GetStyle() = st;
}

// ---------------------------------------------------------------- text helpers
// letter-spaced text, one UTF-8 character at a time (build t3: was byte by byte, so "\xe2\x80\xa2" showed as "???")
static const char* NextChar(const char* p) { unsigned int c; int n = ImTextCharFromUtf8(&c, p, nullptr); return p + (n > 0 ? n : 1); }
float SpacedCapsWidth(ImFont* f, const char* text, float spacing) {
    float w = 0; for (const char* p = text; *p; ) { const char* q = NextChar(p); w += f->CalcTextSizeA(f->FontSize, 1e9f, 0, p, q).x + spacing; p = q; }
    return w > 0 ? w - spacing : 0;
}
void SpacedCaps(ImDrawList* dl, ImFont* f, ImVec2 p, ImU32 col, const char* text, float spacing) {
    for (const char* c = text; *c; ) { const char* q = NextChar(c); dl->AddText(f, f->FontSize, p, col, c, q); p.x += f->CalcTextSizeA(f->FontSize, 1e9f, 0, c, q).x + spacing; c = q; }
}

// ---------------------------------------------------------------- banner
void Banner(const char* title, const char* subtitle) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos(); float W = ImGui::GetContentRegionAvail().x;
    float H = std::round(W * 0.40f);
    if (g_banner.tex) {   // fill the width; show the upper part of the art (the logo and the dragon)
        float vis = (W / H) / ((float)g_banner.w / g_banner.h);   // visible fraction of the art's height
        float v0 = 0.02f, v1 = v0 + (1.0f / vis); if (v1 > 1.0f) { v0 -= v1 - 1.0f; v1 = 1.0f; if (v0 < 0) v0 = 0; }
        dl->AddImage(g_banner.tex, p, ImVec2(p.x + W, p.y + H), ImVec2(0, v0), ImVec2(1, v1));
    } else dl->AddRectFilledMultiColor(p, ImVec2(p.x + W, p.y + H), IM_COL32(70, 24, 12, 255), IM_COL32(110, 40, 14, 255), Bg(), Bg());
    // fade into the page at the bottom, darker at the left behind the title
    float f0 = p.y + H * 0.45f;
    dl->AddRectFilledMultiColor(ImVec2(p.x, f0), ImVec2(p.x + W, p.y + H), IM_COL32(20, 19, 22, 0), IM_COL32(20, 19, 22, 0), Bg(), Bg());
    dl->AddRectFilledMultiColor(ImVec2(p.x, f0), ImVec2(p.x + W * 0.6f, p.y + H), IM_COL32(20, 19, 22, 120), IM_COL32(20, 19, 22, 0), IM_COL32(20, 19, 22, 0), IM_COL32(20, 19, 22, 160));
    dl->AddLine(ImVec2(p.x, p.y + 0.5f), ImVec2(p.x + W, p.y + 0.5f), IM_COL32(0, 0, 0, 200), 1.0f);
    // title block near the bottom left: gold rule, Cinzel title, spaced subtitle
    float x = p.x + 34 * g_s;
    ImFont* tf = g_fonts.display; ImFont* sf = g_fonts.caps;
    float subH = subtitle ? sf->FontSize + 6 * g_s : 0;
    // a long title shrinks to fit the width (build t5: "DRAGON'S DOGMA: DARK ARISEN VR")
    float fs = tf->FontSize, tw = tf->CalcTextSizeA(fs, 1e9f, 0, title).x, room = W - 68 * g_s;
    if (tw > room) fs *= room / tw;
    float ty = p.y + H - fs - subH - 22 * g_s;
    dl->AddRectFilled(ImVec2(x, ty - 16 * g_s), ImVec2(x + 30 * g_s, ty - 14 * g_s), Gold());
    dl->AddText(tf, fs, ImVec2(x + 2 * g_s, ty + 2 * g_s), IM_COL32(0, 0, 0, 170), title);
    dl->AddText(tf, fs, ImVec2(x, ty), Text(), title);
    if (subtitle) SpacedCaps(dl, sf, ImVec2(x + 1 * g_s, ty + fs + 4 * g_s), IM_COL32(214, 204, 186, 255), subtitle, 2.4f * g_s);
    ImGui::Dummy(ImVec2(W, H));
}

// ---------------------------------------------------------------- sidebar
void Emblem(float size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float W = ImGui::GetContentRegionAvail().x; ImVec2 c = ImGui::GetCursorScreenPos();
    ImVec2 m(c.x + W * 0.5f, c.y + size * 0.5f); float r = size * 0.5f;
    for (int i = 5; i >= 1; --i) dl->AddCircleFilled(m, r + i * 2.2f * g_s, IM_COL32(214, 96, 40, 6 + i * 3), 48);
    if (g_emblem.tex) dl->AddImageRounded(g_emblem.tex, ImVec2(m.x - r, m.y - r), ImVec2(m.x + r, m.y + r), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, r);
    else dl->AddCircleFilled(m, r, IM_COL32(60, 24, 14, 255), 48);
    dl->AddCircle(m, r + 1.0f * g_s, IM_COL32(201, 164, 92, 200), 48, 1.6f * g_s);
    ImGui::Dummy(ImVec2(W, size));
}
static void Icon(ImDrawList* dl, int kind, ImVec2 c, float s, ImU32 col) {
    float t = 1.5f * g_s;
    switch (kind) {
    case 0: for (int i = -1; i <= 1; ++i) dl->AddLine(ImVec2(c.x - s, c.y + i * s * 0.62f), ImVec2(c.x + s, c.y + i * s * 0.62f), col, t); break;
    case 1: {   // gear
        for (int i = 0; i < 8; ++i) { float a = i * 3.14159f / 4; dl->AddLine(ImVec2(c.x + cosf(a) * s * 0.62f, c.y + sinf(a) * s * 0.62f), ImVec2(c.x + cosf(a) * s, c.y + sinf(a) * s), col, t * 1.6f); }
        dl->AddCircle(c, s * 0.62f, col, 20, t); dl->AddCircle(c, s * 0.25f, col, 12, t); break; }
    case 2: {   // sliders
        for (int i = -1; i <= 1; ++i) { float y = c.y + i * s * 0.62f, kx = c.x + (i == 0 ? 0.35f : i < 0 ? -0.35f : 0.1f) * s;
            dl->AddLine(ImVec2(c.x - s, y), ImVec2(c.x + s, y), col, t); dl->AddCircleFilled(ImVec2(kx, y), s * 0.24f, col, 12); } break; }
    case 3: {   // info
        dl->AddCircle(c, s, col, 24, t); dl->AddLine(ImVec2(c.x, c.y - s * 0.1f), ImVec2(c.x, c.y + s * 0.5f), col, t * 1.2f); dl->AddCircleFilled(ImVec2(c.x, c.y - s * 0.45f), t * 0.9f, col, 8); break; }
    }
}
bool NavItem(const char* label, int icon, bool selected) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c = ImGui::GetCursorScreenPos(); float w = ImGui::GetContentRegionAvail().x, h = 40 * g_s;
    ImGui::PushID(label); bool pressed = ImGui::InvisibleButton("##nav", ImVec2(w, h)); bool hov = ImGui::IsItemHovered(); ImGui::PopID();
    if (selected) { dl->AddRectFilled(c, ImVec2(c.x + w, c.y + h), IM_COL32(38, 32, 26, 255)); dl->AddRectFilled(c, ImVec2(c.x + 3 * g_s, c.y + h), Gold()); }
    else if (hov) dl->AddRectFilled(c, ImVec2(c.x + w, c.y + h), IM_COL32(30, 28, 30, 255));
    ImU32 col = selected ? Gold() : hov ? Text() : IM_COL32(196, 189, 178, 255);
    Icon(dl, icon, ImVec2(c.x + 30 * g_s, c.y + h * 0.5f), 7.0f * g_s, col);
    ImFont* f = g_fonts.side; dl->AddText(f, f->FontSize, ImVec2(c.x + 50 * g_s, c.y + (h - f->FontSize) * 0.5f - 1), col, label);
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return pressed;
}

// ---------------------------------------------------------------- page widgets
void SectionTitle(const char* name) {
    ImGui::Dummy(ImVec2(1, 10 * g_s));
    ImDrawList* dl = ImGui::GetWindowDrawList(); ImVec2 c = ImGui::GetCursorScreenPos();
    ImFont* f = g_fonts.displaySm;
    dl->AddText(f, f->FontSize, c, Text(), name);
    float tw = f->CalcTextSizeA(f->FontSize, 1e9f, 0, name).x;
    float y = c.y + f->FontSize * 0.58f;
    dl->AddLine(ImVec2(c.x + tw + 14 * g_s, y), ImVec2(c.x + ImGui::GetContentRegionAvail().x, y), Line(), 1.0f);
    dl->AddLine(ImVec2(c.x + tw + 14 * g_s, y), ImVec2(c.x + tw + 44 * g_s, y), GoldDim(), 1.6f * g_s);
    ImGui::Dummy(ImVec2(1, f->FontSize + 6 * g_s));
}
static float g_cardPad; static ImVec2 g_cardStart; static int g_cardSplit;
void BeginCard(const char* id, float pad) {
    g_cardPad = pad * g_s; g_cardStart = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList(); g_cardSplit = 1;
    dl->ChannelsSplit(2); dl->ChannelsSetCurrent(1);
    ImGui::PushID(id);
    ImGui::Dummy(ImVec2(1, g_cardPad - ImGui::GetStyle().ItemSpacing.y));
    ImGui::Indent(g_cardPad);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - g_cardPad);
    ImGui::BeginGroup();
}
void EndCard() {
    ImGui::EndGroup();
    ImGui::PopTextWrapPos();
    ImGui::Unindent(g_cardPad);
    ImGui::Dummy(ImVec2(1, g_cardPad - ImGui::GetStyle().ItemSpacing.y));
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 e = ImGui::GetCursorScreenPos(); float w = ImGui::GetContentRegionAvail().x;
    dl->ChannelsSetCurrent(0);
    ImVec2 a = g_cardStart, b(g_cardStart.x + w, e.y - ImGui::GetStyle().ItemSpacing.y);
    dl->AddRectFilled(a, b, Card(), 8 * g_s); dl->AddRect(a, b, IM_COL32(44, 41, 44, 255), 8 * g_s, 0, 1.0f);
    dl->ChannelsMerge();
}
void InfoPair(const char* label, const char* value, ImU32 valueCol) {
    ImDrawList* dl = ImGui::GetWindowDrawList(); ImVec2 c = ImGui::GetCursorScreenPos(); float w = ImGui::GetContentRegionAvail().x;
    ImFont* lf = g_fonts.label; float lw = SpacedCapsWidth(lf, label, 1.2f * g_s);
    SpacedCaps(dl, lf, ImVec2(c.x + (w - lw) * 0.5f, c.y), Muted(), label, 1.2f * g_s);
    ImFont* vf = g_fonts.bold; float vw = vf->CalcTextSizeA(vf->FontSize, w, 0, value).x;
    dl->AddText(vf, vf->FontSize, ImVec2(c.x + (w - vw) * 0.5f, c.y + lf->FontSize + 5 * g_s), valueCol ? valueCol : Text(), value);
    ImGui::Dummy(ImVec2(w, lf->FontSize + vf->FontSize + 10 * g_s));
}
void Bullet(const char* text) {
    ImDrawList* dl = ImGui::GetWindowDrawList(); ImVec2 c = ImGui::GetCursorScreenPos(); float lh = ImGui::GetTextLineHeight();
    dl->AddLine(ImVec2(c.x, c.y + lh * 0.55f), ImVec2(c.x + 10 * g_s, c.y + lh * 0.55f), Gold(), 1.6f * g_s);
    ImGui::Indent(18 * g_s); ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(IM_COL32(206, 199, 188, 255))); ImGui::TextWrapped("%s", text); ImGui::PopStyleColor(); ImGui::Unindent(18 * g_s);
}
bool Button(const char* label, bool primary, bool enabled, float width) {
    int n = 0;
    if (primary) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(IM_COL32(150, 110, 52, 255))); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(IM_COL32(178, 134, 66, 255)));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(IM_COL32(201, 160, 84, 255))); ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(IM_COL32(255, 248, 232, 255)));
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(IM_COL32(214, 176, 100, 255))); n = 5;
    }
    ImGui::BeginDisabled(!enabled);
    bool r = ImGui::Button(label, ImVec2(width, ImGui::GetFrameHeight() * 1.12f));
    if (enabled && ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::EndDisabled();
    ImGui::PopStyleColor(n);
    return r && enabled;
}
bool PlayButton(const char* label, bool enabled, float width) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c = ImGui::GetCursorScreenPos(); float h = 44 * g_s;
    ImGui::BeginDisabled(!enabled);
    bool pressed = ImGui::InvisibleButton("##play", ImVec2(width, h));
    bool hov = enabled && ImGui::IsItemHovered(), act = enabled && ImGui::IsItemActive();
    ImGui::EndDisabled();
    ImU32 top = !enabled ? IM_COL32(60, 74, 64, 255) : act ? IM_COL32(52, 170, 96, 255) : hov ? IM_COL32(86, 214, 134, 255) : IM_COL32(72, 196, 118, 255);
    ImU32 bot = !enabled ? IM_COL32(46, 56, 50, 255) : act ? IM_COL32(36, 138, 76, 255) : IM_COL32(48, 160, 92, 255);
    if (enabled) for (int i = 4; i >= 1; --i) dl->AddRectFilled(ImVec2(c.x - i * 2.0f * g_s, c.y - i * 2.0f * g_s), ImVec2(c.x + width + i * 2.0f * g_s, c.y + h + i * 2.0f * g_s), IM_COL32(72, 196, 118, hov ? 14 : 8), 8 * g_s + i * 2 * g_s);
    dl->AddRectFilled(c, ImVec2(c.x + width, c.y + h), bot, 7 * g_s);
    dl->AddRectFilled(c, ImVec2(c.x + width, c.y + h * 0.55f), top, 7 * g_s, ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled(ImVec2(c.x, c.y + h * 0.4f), ImVec2(c.x + width, c.y + h * 0.55f), top);
    ImFont* f = g_fonts.caps; float sp = 2.6f * g_s;
    float tw = SpacedCapsWidth(f, label, sp) + 22 * g_s;
    float x = c.x + (width - tw) * 0.5f, y = c.y + (h - f->FontSize) * 0.5f;
    ImU32 tc = enabled ? IM_COL32(12, 30, 18, 255) : IM_COL32(120, 132, 124, 255);
    // a small "play" triangle before the word
    dl->AddTriangleFilled(ImVec2(x, y + 1 * g_s), ImVec2(x, y + f->FontSize - 1 * g_s), ImVec2(x + 10 * g_s, y + f->FontSize * 0.5f), tc);
    SpacedCaps(dl, f, ImVec2(x + 22 * g_s, y), tc, label, sp);
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return pressed && enabled;
}
bool Toggle(const char* label, bool* v, const char* help, bool enabled) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::BeginDisabled(!enabled);
    ImVec2 c = ImGui::GetCursorScreenPos(); float w = ImGui::GetContentRegionAvail().x;
    float th = 22 * g_s, tw = 40 * g_s;
    ImGui::PushID(label);
    float lh = g_fonts.bold->FontSize;
    bool pressed = ImGui::InvisibleButton("##t", ImVec2(w, std::max(th, lh) + 2 * g_s));
    bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    if (pressed && enabled) *v = !*v;
    float a = enabled ? 1.0f : 0.45f;
    auto A = [&](ImU32 col) { return (col & 0x00FFFFFF) | ((ImU32)(((col >> 24) & 0xFF) * a) << 24); };
    ImVec2 t0(c.x + w - tw, c.y + 1 * g_s);
    dl->AddRectFilled(t0, ImVec2(t0.x + tw, t0.y + th), A(*v ? IM_COL32(150, 112, 54, 255) : IM_COL32(46, 43, 47, 255)), th * 0.5f);
    dl->AddRect(t0, ImVec2(t0.x + tw, t0.y + th), A(*v ? Gold() : IM_COL32(70, 66, 68, 255)), th * 0.5f, 0, 1.0f);
    float kx = *v ? t0.x + tw - th * 0.5f : t0.x + th * 0.5f;
    dl->AddCircleFilled(ImVec2(kx, t0.y + th * 0.5f), th * 0.5f - 3 * g_s, A(*v ? IM_COL32(255, 244, 222, 255) : IM_COL32(150, 144, 136, 255)), 20);
    dl->AddText(g_fonts.bold, lh, ImVec2(c.x, c.y + (th - lh) * 0.5f + 1 * g_s), A(hov && enabled ? IM_COL32(255, 250, 240, 255) : Text()), label);
    if (hov && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (help) { ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - tw - 20 * g_s); Wrapped(help, A(Muted())); ImGui::PopTextWrapPos(); }
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(1, 4 * g_s));
    return pressed && enabled;
}
void Wrapped(const char* text, ImU32 col) {
    if (col) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
    ImGui::TextWrapped("%s", text);
    if (col) ImGui::PopStyleColor();
}
void Dot(ImU32 col) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float s = ImGui::GetTextLineHeight(); ImVec2 c = ImGui::GetCursorScreenPos();
    ImVec2 m(c.x + s * 0.4f, c.y + s * 0.55f);
    dl->AddCircleFilled(m, s * 0.42f, (col & 0x00FFFFFF) | 0x30000000, 16);
    dl->AddCircleFilled(m, s * 0.22f, col, 16);
    ImGui::Dummy(ImVec2(s * 0.95f, s)); ImGui::SameLine();
}
} // namespace theme
