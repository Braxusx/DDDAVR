# Third-party components

| Component | Where | License |
|---|---|---|
| MinHook (Tsuda Kageyu) | `mod/third_party/minhook` | BSD-2-Clause (see `LICENSE.txt` there) |
| OpenXR SDK headers (The Khronos Group) | `mod/third_party/openxr_include` | Apache-2.0 OR MIT |
| Vulkan headers (The Khronos Group) | `mod/third_party/vulkan` | Apache-2.0 OR MIT |
| OpenXR loader `openxr_loader.dll` (The Khronos Group) | release / launcher payload | Apache-2.0 |
| DXVK `d3d9_dxvk.dll` (Philip Rebohle and contributors) | release / launcher payload | zlib |
| DLSS helper design and IPC | `mod/src/dlss_host`, `mod/src/dlss_client.*`, `mod/src/dlss_ipc.h` | from the Dishonored VR mod by Pizza Parker (used with permission), based on the BioShock VR DLSS fork (MIT) and DLSS5-Feeder (MIT) |
| Dear ImGui (Omar Cornut) | `launcher/third_party/imgui` | MIT (see `LICENSE.txt` there) |
| miniz (Rich Geldreich) | `launcher/src/miniz` | MIT (see `LICENSE` there) |
| LZMA SDK decoder (Igor Pavlov) | `launcher/src/lzma` | Public domain |
| Cinzel typeface (Natanael Gama) | `launcher/art/Cinzel-*.ttf` | SIL Open Font License 1.1 (`launcher/art/Cinzel-OFL.txt`) |
| Chomsky typeface (Fredrick R. Brennan) | `launcher/art/Chomsky.otf` | SIL Open Font License 1.1 (`launcher/art/Chomsky-OFL.txt`) |
| NVIDIA DLSS (NGX) runtime `nvngx_dlss.dll` | release / launcher payload, not in this repository | NVIDIA RTX SDKs license (redistributable with the application) |

NVIDIA, the NVIDIA logo and DLSS are trademarks and/or registered trademarks of NVIDIA Corporation in the U.S. and other countries.
This product uses NVIDIA DLSS technology. The NGX SDK itself (headers and static library) is not included; get it from https://github.com/NVIDIA/DLSS.

The launcher's header artwork (`launcher/art/banner.jpg`, `emblem.png`) is AI-generated fan art.
Dragon's Dogma is a trademark of CAPCOM. This is an unofficial fan mod; no game files are included or modified on disk.
