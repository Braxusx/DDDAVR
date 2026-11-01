// DDDAVR - OpenXR output (build 022: the game picture as a flat screen in the headset)
#pragma once
#include <windows.h>
#include <d3d9.h>

namespace dxr {
// ini [VR] XR=1 enables; call once at startup with the ini path
void Configure(const wchar_t* ini);
// call on the thread that presents, just before the game's Present (D3D9Ex device)
void OnPresent(IDirect3DDevice9* dev);
void ConfigureTracking(const wchar_t* ini);
// camera hook: view (cam+0x90) and projection (cam+0xd0) of the main camera, modified in place; true if changed
bool ApplyToCamera(float* view, float* proj);
// build 027 UI layer: the transparent shared render target the UI is redirected into (null when not in head-tracking mode)
IDirect3DSurface9* UiSurface();
void SetUiFrame(bool used);              // at Present: whether this frame drew UI into the layer
bool FixUiProjRow(float* row4);
bool SyncActive();                       // build 042 synchronous stereo: draw function replayed per eye
void SyncSetEye(int e);                  // -1 off, 0 left replay, 1 right replay
int SyncEye();
bool SyncVS(float* f, unsigned n);       // VS constant upload during a replay (right eye: VP replaced); true if changed
void SyncLeftDone(IDirect3DDevice9* dev);
extern float g_camTol;                  // build 049: camera position match tolerance (game units)
bool SyncDebugCam(float* camL, float* camR);   // build 051: the replay's camera positions (diagnostics)
bool SyncPS(float* f, unsigned n);
bool SyncMaybe(const float* f, unsigned n, bool ps);   // build 068: right-eye upload may hold a camera form (cheap pre-check)
bool SyncNeedCam();                                      // build 068: replay 0 still looking for its camera       // build 043: PS uploads in the right-eye replay
void NoteVsConstants(const float* f, unsigned n);   // build 041: render-thread VS uploads -> which eye this frame is
bool Flat();                             // build 028: flat screen mode active
void SetMovieFrame(bool movie);          // a UI constant row equal to a headset-projection row -> the game's own row
// build 075 DLSS: the shown eye size when the game renders smaller (0 = same as the back buffer)
void SetOutputSize(UINT w, UINT h);
// the shared texture that receives eye e's packed scene depth after its replay (null when DLSS is not running)
IDirect3DSurface9* DepthSurface(int eye);
bool GameViewZoomed();   // build 081
void WorldGuiLog(const float* c4, long frame, int world);   // build 085
float StereoClipShift(float metres);
void ShowMessage(const wchar_t* text, double seconds);   // build 101: short head-locked message
bool WorldGuiToEye(int mode, float x, float y, float z, float* X, float* Y, float* k, float* rs, float* rc);   // build 083   // build 082: per-eye clip x shift for screen text at that distance
}
