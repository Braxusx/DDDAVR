// dlss_dd.cpp - see dlss_dd.h. The guide pass is ported from the Far Cry 2 VR mod (dlss_fc2.cpp).
#include "dlss_dd.h"
#include "dlss_client.h"
#include "log.h"
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ddlss {
namespace {
template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
int g_enable = 0, g_quality = 0, g_preset = 11, g_jitter = 1, g_phases = 16; float g_sharp = 0;
bool g_on = false, g_failed = false, g_started = false;
uint32_t g_phase = 0;
wchar_t g_dir[MAX_PATH] = L"";        // <game>\ddda_vr_dlss
fc2vr::dlss::Client g_client;

const char kHlsl[] = R"(
Texture2D<float4> gEnc : register(t0);
cbuffer C : register(b0) { row_major float4x4 M; float2 size; float2 jit; float hist; float3 pad; };
struct VSO { float4 pos : SV_Position; };
VSO vs(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSO o; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}
struct PSO { float d : SV_Target0; float2 mv : SV_Target1; };
PSO ps(VSO i) {
    int2 p = int2(i.pos.xy);
    float3 e = round(gEnc.Load(int3(p, 0)).rgb * 255.0);
    float d = (e.r * 65536.0 + e.g * 256.0 + e.b) / 16777215.0;
    // the unjittered position of this pixel's sample (D3D9: pixel centres on integer coordinates)
    float2 uv = (i.pos.xy - 0.5 + jit) / size;
    float4 c = float4(uv.x * 2 - 1, 1 - uv.y * 2, d, 1);
    float4 q = mul(c, M);
    float2 puv = uv;
    if (hist > 0.5 && abs(q.w) > 1e-12) { float2 n = q.xy / q.w; puv = float2(n.x * 0.5 + 0.5, 0.5 - n.y * 0.5); }
    PSO o; o.d = d; o.mv = puv - uv; return o;
}
Texture2D gSrc : register(t0);
SamplerState gLin : register(s0);
cbuffer B : register(b0) { float decode; float3 padb; };
struct VSB { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSB vsb(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSB o; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o;
}
float4 psb(VSB i) : SV_Target {
    float4 c = gSrc.SampleLevel(gLin, i.uv, 0);
    if (decode > 0.5) c.rgb = c.rgb <= 0.04045 ? c.rgb / 12.92 : pow((c.rgb + 0.055) / 1.055, 2.4);   // sRGB view: undo, the view encodes again
    return float4(c.rgb, 1);
}
)";
struct CB { float M[16]; float size[2]; float jit[2]; float hist; float pad[3]; };
struct CBB { float decode; float pad[3]; };

ID3D11VertexShader *g_vs, *g_vsb; ID3D11PixelShader *g_ps, *g_psb; ID3D11Buffer *g_cb, *g_cbb;
ID3D11SamplerState* g_smp; ID3D11RasterizerState* g_rs; ID3D11DepthStencilState* g_dss; ID3D11BlendState* g_bs;
bool g_init = false, g_bad = false;

struct Eye { UINT w = 0, h = 0; ID3D11Texture2D *depth = nullptr, *motion = nullptr; ID3D11RenderTargetView* rtv[2] = {};
             ID3D11Texture2D* encKey = nullptr; ID3D11ShaderResourceView* encSrv = nullptr; bool reset = true; int builtPreset = -1; };
Eye g_e[2];
struct RtvSlot { ID3D11Texture2D* tex; ID3D11RenderTargetView* rtv; bool decode; };
RtvSlot g_rtv[24]; int g_nRtv;

bool Init(ID3D11Device* dev) {
    if (g_init) return true;
    if (g_bad) return false;
    g_bad = true;
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    pD3DCompile comp = dc ? (pD3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!comp) { Log("dlss: d3dcompiler_47.dll not available - DLSS off"); return false; }
    ID3DBlob* b[4] = {}; ID3DBlob* err = nullptr;
    const char* ep[4] = { "vs", "ps", "vsb", "psb" }; const char* pr[4] = { "vs_5_0", "ps_5_0", "vs_5_0", "ps_5_0" };
    for (int i = 0; i < 4; i++) {
        if (FAILED(comp(kHlsl, sizeof(kHlsl) - 1, "ddda_vr_dlss", nullptr, nullptr, ep[i], pr[i], 0, 0, &b[i], &err))) {
            Log("dlss: shader %s compile failed: %s", ep[i], err ? (const char*)err->GetBufferPointer() : "?"); Rel(err);
            for (auto& x : b) Rel(x); return false;
        }
    }
    bool ok = SUCCEEDED(dev->CreateVertexShader(b[0]->GetBufferPointer(), b[0]->GetBufferSize(), nullptr, &g_vs)) &&
              SUCCEEDED(dev->CreatePixelShader(b[1]->GetBufferPointer(), b[1]->GetBufferSize(), nullptr, &g_ps)) &&
              SUCCEEDED(dev->CreateVertexShader(b[2]->GetBufferPointer(), b[2]->GetBufferSize(), nullptr, &g_vsb)) &&
              SUCCEEDED(dev->CreatePixelShader(b[3]->GetBufferPointer(), b[3]->GetBufferSize(), nullptr, &g_psb));
    for (auto& x : b) Rel(x);
    D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = sizeof(CB); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_cb));
    bd.ByteWidth = sizeof(CBB); ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_cbb));
    D3D11_SAMPLER_DESC sd = {}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
    ok = ok && SUCCEEDED(dev->CreateSamplerState(&sd, &g_smp));
    D3D11_RASTERIZER_DESC rd = {}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(dev->CreateRasterizerState(&rd, &g_rs));
    D3D11_DEPTH_STENCIL_DESC dd = {}; ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&dd, &g_dss));
    D3D11_BLEND_DESC bl = {}; for (int i = 0; i < 8; ++i) bl.RenderTarget[i].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok = ok && SUCCEEDED(dev->CreateBlendState(&bl, &g_bs));
    if (!ok) { Log("dlss: D3D11 guide pass setup failed - DLSS off"); return false; }
    g_bad = false; g_init = true;
    Log("dlss: guide pass ready (depth unpack, camera motion vectors)");
    return true;
}
void ReleaseEye(Eye& e) { Rel(e.depth); Rel(e.motion); Rel(e.rtv[0]); Rel(e.rtv[1]); Rel(e.encSrv); e.encKey = nullptr; e.w = e.h = 0; }
bool EnsureEye(ID3D11Device* dev, Eye& e, UINT w, UINT h) {
    if (e.w == w && e.h == h && e.depth) return true;
    ReleaseEye(e);
    D3D11_TEXTURE2D_DESC d = {}; d.Width = w; d.Height = h; d.MipLevels = d.ArraySize = 1; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    d.Format = DXGI_FORMAT_R32_FLOAT; bool ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &e.depth));
    d.Format = DXGI_FORMAT_R16G16_FLOAT; ok = ok && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &e.motion));
    ok = ok && SUCCEEDED(dev->CreateRenderTargetView(e.depth, nullptr, &e.rtv[0])) && SUCCEEDED(dev->CreateRenderTargetView(e.motion, nullptr, &e.rtv[1]));
    if (!ok) { Log("dlss: guide textures %ux%u failed - DLSS off", w, h); ReleaseEye(e); return false; }
    e.w = w; e.h = h; e.reset = true;
    return true;
}
void InvD(const double* m, double* o) {   // 4x4 inverse (Gauss-Jordan)
    double a[4][8]; for (int i = 0; i < 4; i++) for (int j = 0; j < 8; j++) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i);
    for (int c = 0; c < 4; c++) {
        int pr = c; for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[pr][c])) pr = r;
        if (fabs(a[pr][c]) < 1e-300) { for (int i = 0; i < 16; i++) o[i] = (i % 5) == 0; return; }
        if (pr != c) for (int j = 0; j < 8; j++) { double t = a[c][j]; a[c][j] = a[pr][j]; a[pr][j] = t; }
        double dv = a[c][c]; for (int j = 0; j < 8; j++) a[c][j] /= dv;
        for (int r = 0; r < 4; r++) if (r != c) { double f = a[r][c]; for (int j = 0; j < 8; j++) a[r][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) o[i * 4 + j] = a[i][j + 4];
}
float Halton(uint32_t i, uint32_t b) { float f = 1, r = 0; while (i) { f /= (float)b; r += f * (float)(i % b); i /= b; } return r; }
// centre 64x64 of the unpacked depth, a few times at the start: is there depth in it?
void Probe(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* t, UINT w, UINT h) {
    D3D11_TEXTURE2D_DESC d = {}; d.Width = 64; d.Height = 64; d.MipLevels = d.ArraySize = 1; d.Format = DXGI_FORMAT_R32_FLOAT; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr; if (FAILED(dev->CreateTexture2D(&d, nullptr, &st))) return;
    D3D11_BOX bx = { w / 2 - 32, h / 2 - 32, 0, w / 2 + 32, h / 2 + 32, 1 };
    ctx->CopySubresourceRegion(st, 0, 0, 0, 0, t, 0, &bx);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        float lo = 1e30f, hi = -1e30f;
        for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) { float v = ((const float*)((const uint8_t*)m.pData + y * m.RowPitch))[x]; lo = fminf(lo, v); hi = fmaxf(hi, v); }
        ctx->Unmap(st, 0);
        Log("dlss: depth check (left eye, centre 64x64): %.6f .. %.6f%s", lo, hi, hi <= 0.0f || lo >= 1.0f ? " - EMPTY (no scene depth reached DLSS)" : "");
    }
    st->Release();
}
} // namespace

void Configure(const wchar_t* ini) {
    g_enable = GetPrivateProfileIntW(L"VR", L"Dlss", 0, ini);
    g_quality = GetPrivateProfileIntW(L"VR", L"DlssQuality", 0, ini); if (g_quality < 0 || g_quality > 5) g_quality = 0;
    g_preset = GetPrivateProfileIntW(L"VR", L"DlssPreset", 11, ini);
    g_jitter = GetPrivateProfileIntW(L"VR", L"DlssJitter", 1, ini);
    g_phases = GetPrivateProfileIntW(L"VR", L"DlssJitterPhases", 16, ini);
    wchar_t b[32]; GetPrivateProfileStringW(L"VR", L"DlssSharpness", L"0", b, 32, ini); g_sharp = (float)_wtof(b);
    wcscpy(g_dir, ini); wchar_t* sl = wcsrchr(g_dir, L'\\'); if (sl) sl[1] = 0; else g_dir[0] = 0;
    wcscat(g_dir, L"ddda_vr_dlss");
    g_on = g_enable != 0;
    Log("dlss: %s, %s (render scale %.3f), preset %d, jitter %d (%d phases), sharpness %.2f; helper folder %ls", g_on ? "on" : "off", QualityName(g_quality),
        RenderRatio(), g_preset, g_jitter, g_phases, g_sharp, g_dir);
}
bool Enabled() { return g_enable != 0; }
bool Wanted() { return g_on && !g_failed; }
int Quality() { return g_quality; }
const char* QualityName(int q) {
    static const char* n[6] = { "DLAA", "Ultra Quality", "Quality", "Balanced", "Performance", "Ultra Performance" };
    return (q >= 0 && q < 6) ? n[q] : "DLAA";
}
float RenderRatio() {
    static const float r[6] = { 1.0f, 0.77f, 0.667f, 0.58f, 0.5f, 0.333f };
    return (g_enable && g_quality > 0 && g_quality <= 5) ? r[g_quality] : 1.0f;
}
void Toggle() {
    if (!g_enable) return;
    if (g_failed) { Log("dlss: unavailable (see the earlier lines)"); return; }
    g_on = !g_on; g_e[0].reset = g_e[1].reset = true;
    Log("dlss: Insert -> %s", g_on ? "on" : "off (plain picture, no jitter)");
}
void NextJitter(float* sx, float* sy) {
    *sx = *sy = 0;
    if (!Wanted() || !g_jitter) return;
    uint32_t n = g_phases > 1 ? (uint32_t)g_phases : 16;
    float rr = RenderRatio(); uint32_t m = (uint32_t)ceilf(8.0f / (rr * rr));   // NVIDIA's guide: 8 x (output / render)^2 phases
    if (m > n) n = m;
    uint32_t k = (g_phase++ % n) + 1;
    *sx = Halton(k, 2) - 0.5f; *sy = Halton(k, 3) - 0.5f;
}

ID3D11ShaderResourceView* Evaluate(ID3D11Device* dev, ID3D11DeviceContext* ctx, int ei, ID3D11Texture2D* color, ID3D11Texture2D* enc,
                                   const double vpCur[16], const double* vpPrev, float jx, float jy, bool depthInv, uint32_t ow, uint32_t oh) {
    if (ei < 0 || ei > 1 || !Wanted() || !color || !enc) return nullptr;
    Eye& e = g_e[ei];
    char why[256] = "";
    if (!Init(dev)) { g_failed = true; return nullptr; }
    D3D11_TEXTURE2D_DESC cd; color->GetDesc(&cd);
    if (!EnsureEye(dev, e, cd.Width, cd.Height)) { g_failed = true; return nullptr; }
    if (e.encKey != enc) {
        Rel(e.encSrv); e.encKey = enc;
        if (FAILED(dev->CreateShaderResourceView(enc, nullptr, &e.encSrv))) { Log("dlss: packed depth not readable on D3D11 - DLSS off"); g_failed = true; return nullptr; }
    }
    if (!g_started) {
        wchar_t exe[MAX_PATH]; _snwprintf(exe, MAX_PATH, L"%ls\\ddda_vr_dlss64.exe", g_dir);
        fc2vr::dlss::StartParams sp; sp.hostExe = exe; sp.dataDir = g_dir;
        sp.log = [](int lvl, const char* line) { Log("%s%s", lvl == 2 ? "ERROR " : lvl == 1 ? "warning " : "", line); };
        if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) { Log("dlss: %ls not found - DLSS off", exe); g_failed = true; return nullptr; }
        if (!g_client.start(dev, sp, why, sizeof why)) { Log("dlss: the helper did not start: %s - DLSS off", why); g_failed = true; return nullptr; }
        g_started = true;
    }
    if (!g_client.running()) { Log("dlss: the helper stopped - DLSS off"); g_failed = true; return nullptr; }
    if (!g_client.built(ei, e.w, e.h, ow, oh, cd.Format) || e.builtPreset != g_preset) {
        e.builtPreset = g_preset;
        if (!g_client.build(ei, e.w, e.h, ow, oh, cd.Format, g_preset, why, sizeof why, depthInv, false)) {
            Log("dlss: eye %d build failed: %s - DLSS off", ei, why); g_failed = true; return nullptr;
        }
        Log("dlss: eye %d ready, %ux%u -> %ux%u (%s), depth %s", ei, e.w, e.h, ow, oh, QualityName(g_quality), depthInv ? "reversed" : "standard");
        e.reset = true;
    }
    // ---- guides: depth unpack + camera motion vectors
    CB cb = {};
    if (vpPrev) {
        double inv[16], m[16];
        InvD(vpCur, inv);
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) { double s = 0; for (int k = 0; k < 4; ++k) s += inv[r * 4 + k] * vpPrev[k * 4 + c]; m[r * 4 + c] = s; }
        for (int i = 0; i < 16; ++i) cb.M[i] = (float)m[i];
        cb.hist = 1;
    } else { cb.M[0] = cb.M[5] = cb.M[10] = cb.M[15] = 1; cb.hist = 0; e.reset = true; }
    cb.size[0] = (float)e.w; cb.size[1] = (float)e.h; cb.jit[0] = jx; cb.jit[1] = jy;
    ctx->UpdateSubresource(g_cb, 0, nullptr, &cb, 0, 0);
    ctx->OMSetRenderTargets(2, e.rtv, nullptr);
    D3D11_VIEWPORT vp = { 0, 0, (float)e.w, (float)e.h, 0, 1 }; ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_rs); ctx->OMSetDepthStencilState(g_dss, 0); float bf[4] = {}; ctx->OMSetBlendState(g_bs, bf, 0xffffffff);
    ctx->IASetInputLayout(nullptr); ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vs, nullptr, 0); ctx->PSSetShader(g_ps, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &g_cb); ctx->PSSetShaderResources(0, 1, &e.encSrv);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr; ctx->PSSetShaderResources(0, 1, &none);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    { static int checks; static uint32_t n; if (ei == 0 && checks < 6 && (n++ % 180) == 30) { ++checks; Probe(dev, ctx, e.depth, e.w, e.h); } }
    // ---- DLSS (the offset goes to DLSS as where the content moved: the negation, as tested in the Dishonored / FC2 VR mods)
    fc2vr::dlss::EyeInputs in;
    in.color = color; in.depth = e.depth; in.motion = e.motion; in.bias = nullptr;
    in.reset = e.reset; in.jitterX = -jx; in.jitterY = -jy; in.mvSign = 1.0f; in.sharpness = g_sharp;
    if (!g_client.evaluate(ctx, ei, in, why, sizeof why)) {
        static int n = 0; if (n++ < 10) Log("dlss: eye %d evaluate: %s", ei, why);
        if (!g_client.running()) g_failed = true;
        return nullptr;
    }
    e.reset = false;
    static uint64_t frames = 0;
    if ((++frames % 1800) == 0) {
        auto& st = g_client.stats;
        Log("dlss: %llu eye images; per eye CPU %.2f / %.2f ms, helper GPU %.2f / %.2f ms, refused %llu / %llu",
            (unsigned long long)frames, st.frames[0] ? st.cpuMsSum[0] / st.frames[0] : 0.0, st.frames[1] ? st.cpuMsSum[1] / st.frames[1] : 0.0,
            st.gpuN[0] ? st.gpuMsSum[0] / st.gpuN[0] : -1.0, st.gpuN[1] ? st.gpuMsSum[1] / st.gpuN[1] : -1.0,
            (unsigned long long)st.refused[0], (unsigned long long)st.refused[1]);
    }
    return g_client.output(ei);
}

bool Blit(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src, ID3D11Texture2D* dst) {
    if (!src || !dst || !Init(dev)) return false;
    RtvSlot* s = nullptr;
    for (int i = 0; i < g_nRtv; i++) if (g_rtv[i].tex == dst) { s = &g_rtv[i]; break; }
    if (!s) {
        if (g_nRtv >= 24) { for (int i = 0; i < g_nRtv; i++) Rel(g_rtv[i].rtv); g_nRtv = 0; }
        s = &g_rtv[g_nRtv++]; s->tex = dst; s->rtv = nullptr; s->decode = false;
        D3D11_TEXTURE2D_DESC d; dst->GetDesc(&d);
        D3D11_RENDER_TARGET_VIEW_DESC rv = {}; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        // a plain (non-sRGB) view writes the gamma-encoded values as they are; if the runtime's image is not typeless, an sRGB view + decode
        rv.Format = (d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS) ? DXGI_FORMAT_B8G8R8A8_UNORM :
                    (d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) ? DXGI_FORMAT_R8G8B8A8_UNORM : d.Format;
        if (FAILED(dev->CreateRenderTargetView(dst, &rv, &s->rtv))) {
            rv.Format = d.Format; s->decode = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            if (FAILED(dev->CreateRenderTargetView(dst, &rv, &s->rtv))) { s->rtv = nullptr; LOG_FIRST(3, "dlss: no render target view on a swapchain image (format %d)", (int)d.Format); }
        }
        Log("dlss: swapchain image %p (%ux%u format %d): %s view", (void*)dst, d.Width, d.Height, (int)d.Format, !s->rtv ? "no" : s->decode ? "sRGB (decoded)" : "plain");
    }
    if (!s->rtv) return false;
    D3D11_TEXTURE2D_DESC d; dst->GetDesc(&d);
    CBB cb = { s->decode ? 1.0f : 0.0f, {} }; ctx->UpdateSubresource(g_cbb, 0, nullptr, &cb, 0, 0);
    ctx->OMSetRenderTargets(1, &s->rtv, nullptr);
    D3D11_VIEWPORT vp = { 0, 0, (float)d.Width, (float)d.Height, 0, 1 }; ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_rs); ctx->OMSetDepthStencilState(g_dss, 0); float bf[4] = {}; ctx->OMSetBlendState(g_bs, bf, 0xffffffff);
    ctx->IASetInputLayout(nullptr); ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vsb, nullptr, 0); ctx->PSSetShader(g_psb, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &g_cbb); ctx->PSSetShaderResources(0, 1, &src); ctx->PSSetSamplers(0, 1, &g_smp);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr; ctx->PSSetShaderResources(0, 1, &none);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    return true;
}

void Shutdown() {
    g_client.stop(); for (auto& e : g_e) ReleaseEye(e);
    for (int i = 0; i < g_nRtv; i++) Rel(g_rtv[i].rtv); g_nRtv = 0;
}
}
