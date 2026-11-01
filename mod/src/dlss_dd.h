// dlss_dd.h - build 075: NVIDIA DLSS / DLAA for the DDDA VR eye images.
//
// NGX is 64-bit only and Dragon's Dogma is 32-bit, so DLSS runs in a 64-bit helper process
// (ddda_vr_dlss\ddda_vr_dlss64.exe, from the Far Cry 2 VR mod, which took it from the Dishonored VR
// mod - see dlss_ipc.h) and the eye images are shared with it (dlss_client.h).
//
// Inputs per eye, all at the render size, on the mod's D3D11 (OpenXR) device:
//   colour - the eye's back-buffer picture (the shared texture the OpenXR output already uses);
//   depth  - the scene depth buffer (made INTZ by the proxy) packed into a shared BGRA texture by a
//            D3D9 pass after each eye's replay (24-bit depth in R,G,B);
//   motion - camera motion vectors computed here from the depth and this eye's unjittered
//            view-projection of this frame and the previous one.
// Jitter: the camera projection is shifted by a Halton (2,3) sub-pixel offset every frame (xr.cpp).
#pragma once
#include <stdint.h>
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;

namespace ddlss {
void Configure(const wchar_t* ini);   // DllMain: reads Dlss, DlssQuality, DlssPreset, DlssSharpness, DlssJitter
bool Enabled();                       // Dlss=1 in the ini (decides the render size and the INTZ depth at start-up)
bool Wanted();                        // enabled, switched on (Insert key) and not failed
float RenderRatio();                  // render size / output size for DlssQuality (1 = DLAA)
const char* QualityName(int q);
int Quality();
void Toggle();                        // Insert key
// camera setup (game thread): this frame's sub-pixel offset in render pixels (+x right, +y down:
// the pixel shows the scene at (x + sx, y + sy) in D3D9 pixel-centre coordinates)
void NextJitter(float* sx, float* sy);
// OpenXR submit (render thread), per eye: runs the guides and DLSS; returns the result (output size) or null.
// vpCur / vpPrev: the eye's unjittered view-projection (row vectors, row major), vpPrev null = no history.
ID3D11ShaderResourceView* Evaluate(ID3D11Device* dev, ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* color, ID3D11Texture2D* depthPacked,
                                   const double vpCur[16], const double* vpPrev, float jx, float jy, bool depthInverted, uint32_t outW, uint32_t outH);
// draws src (any size, gamma-encoded colour) over the whole of dst (an OpenXR swapchain image), bilinear
bool Blit(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src, ID3D11Texture2D* dst);
void Shutdown();
}
