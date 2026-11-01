// build 070: DXVK (D3D9 on Vulkan) output bridge.
// DXVK's own D3D9 shared handles are Vulkan-only (opaque KMT) and cannot be opened by a native D3D11 device, so instead:
// the D3D11 device creates each shared texture (NT handle), DXVK's Vulkan device imports it, and at Present the D3D9
// render-target texture that holds the picture is copied into it with a Vulkan command buffer on DXVK's own queue.
#pragma once
#include <d3d9.h>
#include <d3d11.h>

namespace vkb {
bool Detect(IDirect3DDevice9* dev);   // true if dev is a DXVK device with the interop interface (and the bridge is ready)
bool Active();
// creates a D3D9 render-target texture and the D3D11 texture that receives its picture
bool MakeShared(IDirect3DDevice9* dev, ID3D11Device* d11, UINT w, UINT h, IDirect3DTexture9** t9, ID3D11Texture2D** t11);
// copies every registered pair (D3D9 -> D3D11) and waits for the GPU; returns false on failure
bool CopyAll(double* waitMs);
void Reset();   // releases the imported images (device teardown)
void Configure(int mode, int skipCopy);   // build 071: 1 = plain shared textures, 2 = keyed mutex; skipCopy = diagnostic
void D3D11AcquireAll();   // keyed-mutex mode: the D3D11 side takes the pictures (after CopyAll) ...
void D3D11ReleaseAll();   // ... and hands them back to Vulkan (after its copies)
}
