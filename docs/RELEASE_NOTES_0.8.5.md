# Dragon's Dogma: Dark Arisen VR 0.8.5

First person view, world scale, smoother frame pacing. Steam and GOG. Mod build 139.

## New
- **First person view.** The eye sits at your character's eyes and follows the head, with the motion smoothed. Your own head is not drawn; the body is visible when you look down and turns with your view. Weapons and gear carried on the back stay hidden until they are in your hands. The bow reticle is drawn at the point the game aims the arrow at, so it marks where the arrow lands at any range. Pawn and NPC name / health bars sit just above their heads.
  Switch in game with **F4**, or hold **RB** and give the **right stick a long press**. The launcher's *First person view* option sets which view the game starts in.
- **World scale.** A slider in the launcher (0.50 to 2.00) and an in-game menu: **Insert** opens it, **Left / Right** change it, Insert again saves.
- **Automatic recentring** during the first seconds after start and when the first 3D view appears. F7 still recentres at any time.
- **Windows timer fix** (1 ms timer resolution) for steadier frame pacing.

## Changed
- The DLSS on / off key is now **Home** (Insert opens the world scale menu).
- NVIDIA's RTX SDKs licence and a notices file are installed beside `nvngx_dlss.dll`; the licence texts state that the DLSS runtime is NVIDIA's proprietary software and not covered by the mod's GPL.

## In-game keys
| Key | Action |
| --- | --- |
| F4, or RB + long right stick press | first person / third person |
| Insert, Left / Right | world scale menu |
| Home | DLSS on / off |
| F6 | head tracking / virtual screen |
| F7 | recentre |

## New `ddda_vr.ini` keys
`FirstPerson`, `FpUp`, `FpForward`, `FpFollowHead`, `FpSmooth`, `FpTurnBody`, `FpHideBack`, `FpPadButtons` (default `RB+RS`), `FpPadHold`, `FpReticleDist`, `FpHide`, `FpBarPush`, `FpBarLift`, `AutoRecenter`, `WorldScale`.

## Notes
- In first person the right stick still turns you (left / right) and sets the bow's elevation (up / down); the game's third-person camera keeps running underneath.
- Magick bows use different aiming code in the game; their reticle is drawn along the line of fire at `FpReticleDist`.

Release zip: `DragonsDogmaVR-Launcher_0.8.5.exe`, `manual_install/`, `licenses/`, `README.txt`.
