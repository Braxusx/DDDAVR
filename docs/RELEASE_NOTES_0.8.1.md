# Dragon's Dogma: Dark Arisen VR 0.8.1

Bug-fix and diagnostics update. Steam and GOG. Mod build 107.

## Fixes
- **Freeze when entering the game world in VR** (most noticeable without DXVK). While the game paused briefly to load, the mod's keep-alive thread kept the headset fed but could lock the game's own frame out for many seconds, which looked like a hang or crash. The keep-alive thread now steps aside as soon as the game presents.
- **Crash when quitting** inside Virtual Desktop's runtime. The mod now closes the VR session and the DLSS helper before the game exits.

## Diagnostics (new)
`ddda_vr.log` now records:
- the system: Windows version (and Wine/Proton), CPU, RAM, address space, each GPU with its driver version, displays, the registered OpenXR runtime;
- every DLL loaded into the game, naming known overlays and injectors (RivaTuner, Steam, Discord, NVIDIA overlay, OBS, ReShade, Special K and others), plus other wrapper DLLs in the game folder;
- **freeze reports**: after 4 s without a frame, what each mod thread was doing and the call stacks of the main and render threads, with `ddda_vr_hang.dmp`;
- **crash reports**: the error, where it happened (module + offset), registers, call stack and `ddda_vr_crash.dmp`;
- an `exit:` line on a normal close, so a log without it means the game was killed or crashed.

New `ddda_vr.ini` keys (added by the launcher on update): `Diagnostics`, `DiagDumps`, `DiagHangSeconds`, `DiagTest` (test only: 1 = fake freeze, 2 = fake crash).

When reporting a problem, please send `ddda_vr.log` and any `ddda_vr_*.dmp` from the game folder.

Release zip: `DragonsDogmaVR-Launcher_0.8.1.exe`, `manual_install/`, `licenses/`, `README.txt`.
