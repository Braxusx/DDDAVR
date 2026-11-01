// game.h - Dragon's Dogma: Dark Arisen discovery, VR mod install / uninstall, launch and launcher preferences.
#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace ddda {

enum Store { StoreUnknown = 0, StoreSteam = 1, StoreGog = 2 };
enum Build { BuildUnknown = 0, BuildSteam = 1, BuildGog = 2 };   // from DDDA.exe's PE time stamp

struct Game {
    std::wstring root;            // the game folder (contains DDDA.exe and nativePC)
    Store store = StoreUnknown;
    Build build = BuildUnknown;
    bool valid = false;
};

struct State {
    bool installed = false;       // our d3d9.dll / install record present
    bool foreignD3d9 = false;     // a d3d9.dll that isn't ours (ReShade, another mod)
    bool backupPresent = false;
    bool iniExists = false;
    bool needsElevation = false;
    bool outdated = false;        // installed files differ from the ones this launcher carries
    bool noGlowArc = false;       // ddda_vr_mods\bbsrpg_core_noglow*.arc present
    std::string installedVersion;
};

struct Report {
    bool ok = true, accessDenied = false;
    std::vector<std::string> lines;   // "+ text" done, "! text" warning, "x text" failed
};

struct Blob { const void* data = nullptr; size_t size = 0; };
Blob Payload(int id);

std::vector<Game> FindGames();
Game Inspect(const std::wstring& dir);
State Check(const Game& g);
bool GameRunning();
const char* StoreName(Store s);

Report Install(const Game& g);
Report Uninstall(const Game& g, bool deleteIni);
Report RunOperation(const Game& g, const char* op, bool deleteIni);
bool Launch(const Game& g, std::string& err);

// the active OpenXR runtime (Windows registry + its manifest)
struct Runtime { bool found = false; std::string name, manifest; };
Runtime ActiveRuntime();

std::wstring PrefsDir();
std::wstring GetPref(const wchar_t* section, const wchar_t* key, const wchar_t* def);
void SetPref(const wchar_t* section, const wchar_t* key, const std::wstring& value);

std::wstring IniPath(const Game& g);
std::wstring IniGet(const Game& g, const wchar_t* key, const wchar_t* def);
void IniSet(const Game& g, const wchar_t* key, const std::wstring& value);

std::string Narrow(const std::wstring& w);
std::wstring Widen(const std::string& s);
void Log(const char* fmt, ...);
extern const char* kModVersion;

std::wstring CollectLogs(const Game& g, std::string& err);

} // namespace ddda
