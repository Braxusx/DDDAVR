# Dragon's Dogma: Dark Arisen VR

Dragon's Dogma: Dark Arisen VR mod with native stereo rendering and DLSS / DXVK support, for the Steam and GOG releases.

- Full native stereo (each eye rendered from its own camera) with head tracking around the game's third-person view
- NVIDIA DLSS / DLAA through a 64-bit helper process (the game itself is 32-bit)
- Optional DXVK (Vulkan) with a "Building shader cache" notice in the headset
- Extended draw distance: Game Default, 2X (performance impact), 3X (extreme performance impact)
- HUD on a comfortable panel; quest markers, pawn names and enemy bars placed in the world; larger NPC dialogue,
  raised stereo cutscene subtitles, white bow reticle
- Optional effects: sun shafts, lens flare; reduced sun glare (made from your own game files)
- Launcher: finds Steam / GOG installs, installs / updates / uninstalls, settings, desktop shortcut

## Install

Download the release zip and run `DragonsDogmaVR-Launcher_<version>.exe`: pick your game folder, press **Install VR mod**, then **PLAY**.
Start your OpenXR runtime (Virtual Desktop, SteamVR, Meta Quest Link...) first. See the README in the zip for the manual install.

## Building

Toolchain: [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (i686 for the mod and launcher, x86_64 for the DLSS helper).

```
TC=$HOME/llvm-mingw/bin sh mod/build.sh                         # mod/build/d3d9.dll
TC=$HOME/llvm-mingw/bin NGX=$HOME/DLSS sh mod/build.sh          # + mod/build/ddda_vr_dlss64.exe (NVIDIA DLSS SDK)
```

The launcher embeds the mod files. Put them in `launcher/payload/` first: `d3d9.dll`, `ddda_vr_dlss64.exe`, `nvngx_dlss.dll`
(from the NVIDIA DLSS SDK), `openxr_loader.dll` (Khronos OpenXR SDK), `d3d9_dxvk.dll` (DXVK 32-bit `d3d9.dll`, renamed),
`dxvk_license.txt` (DXVK's LICENSE); `ddda_vr.ini` is already there. Then:

```
TC=$HOME/llvm-mingw/bin sh launcher/build.sh                    # launcher/build/DragonsDogmaVR-Launcher.exe
```

`tools/pack_payload.py` (run by the build) packs the payload as LZMA. `mod/tools/make_noglow2.py` builds the glare-fix archive
from a game's `nativePC/rom/bbsrpg_core.arc` (the launcher does the same in C++ on install).

## Licence

GPL-3.0 (see `LICENSE`). Third-party components: see `THIRD_PARTY_NOTICES.md`.

Unofficial fan mod, not affiliated with or endorsed by CAPCOM. Dragon's Dogma is a trademark of CAPCOM.
No game files are included in this repository or in the release.
