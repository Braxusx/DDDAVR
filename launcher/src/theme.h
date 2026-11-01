// theme.h - the Dragon's Dogma VR look: near-black panels, ember gold accents, Cinzel display type, the banner art at
// the top of every page, a sidebar with the dragon emblem (layout after the user's concept launcher).
#pragma once
#include "imgui.h"

namespace theme {
inline ImU32 Bg()      { return IM_COL32(20, 19, 22, 255); }
inline ImU32 Side()    { return IM_COL32(15, 14, 16, 255); }
inline ImU32 Card()    { return IM_COL32(28, 27, 31, 255); }
inline ImU32 CardHi()  { return IM_COL32(36, 34, 38, 255); }
inline ImU32 Line()    { return IM_COL32(52, 48, 50, 255); }
inline ImU32 Text()    { return IM_COL32(236, 231, 221, 255); }
inline ImU32 Muted()   { return IM_COL32(146, 139, 128, 255); }
inline ImU32 Gold()    { return IM_COL32(201, 164, 92, 255); }
inline ImU32 GoldDim() { return IM_COL32(150, 118, 62, 255); }
inline ImU32 Ember()   { return IM_COL32(214, 96, 40, 255); }
inline ImU32 Teal()    { return IM_COL32(124, 206, 214, 255); }
inline ImU32 Good()    { return IM_COL32(96, 200, 120, 255); }
inline ImU32 Warn()    { return IM_COL32(232, 176, 70, 255); }
inline ImU32 Bad()     { return IM_COL32(222, 92, 72, 255); }

struct Fonts { ImFont* body = nullptr; ImFont* small = nullptr; ImFont* bold = nullptr; ImFont* label = nullptr; ImFont* side = nullptr;
               ImFont* display = nullptr; ImFont* displaySm = nullptr; ImFont* caps = nullptr; ImFont* capsLg = nullptr; };
extern Fonts g_fonts;
extern float g_s;   // UI scale (DPI)
void LoadFonts(float scale);
void ApplyStyle(float scale);

struct Image { ImTextureID tex = 0; int w = 0, h = 0; };
extern Image g_banner, g_emblem;

// page parts
void Banner(const char* title, const char* subtitle);          // the art at the top of the page with the page title
bool NavItem(const char* label, int icon, bool selected);      // sidebar entry; icon 0 menu, 1 gear, 2 sliders, 3 info
void Emblem(float size);
void SectionTitle(const char* name);                            // heading with a short gold rule
void BeginCard(const char* id, float pad = 18);
void EndCard();
void InfoPair(const char* label, const char* value, ImU32 valueCol = 0);   // small caps label over a bold value (centred column)
void Bullet(const char* text);                                 // "— text" with a gold dash
bool Button(const char* label, bool primary = false, bool enabled = true, float width = 0);
bool PlayButton(const char* label, bool enabled, float width);
bool Toggle(const char* label, bool* v, const char* help = nullptr, bool enabled = true);
void Wrapped(const char* text, ImU32 col = 0);
void Dot(ImU32 col);                                           // status dot inline before text
void SpacedCaps(ImDrawList* dl, ImFont* f, ImVec2 p, ImU32 col, const char* text, float spacing);
float SpacedCapsWidth(ImFont* f, const char* text, float spacing);
}
