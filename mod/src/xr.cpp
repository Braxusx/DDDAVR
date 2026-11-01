// DDDAVR - OpenXR output.
// Build 022: each Present, the game's back buffer is copied (StretchRect) into a D3D9Ex render-target texture created
// with a shared handle; the same memory is opened on our D3D11 device (on the headset's adapter) and copied into an
// OpenXR quad-layer swapchain. A D3D9 event query makes sure the copy has finished before D3D11 reads it.
// Frame loop runs inside the game's Present (render thread): xrWaitFrame paces the game to the headset rate.
#include "xr.h"
#include "log.h"
#include "vkbridge.h"
#include "dlss_dd.h"
#include "notice.h"
#include <d3d11.h>
#include <dxgi.h>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstring>
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "xr_runtime_probe.h"

namespace dxr {

static int g_enabled = 1; static int g_syncWait = 1, g_devKeys;
static float g_scrW = 2.6f, g_scrDist = 2.2f;
static wchar_t g_dir[MAX_PATH];

static HMODULE g_loader;
static PFN_xrGetInstanceProcAddr p_xrGetInstanceProcAddr;
#define XR_FUNCS(X) \
    X(xrCreateInstance) X(xrDestroyInstance) X(xrGetSystem) X(xrCreateSession) X(xrBeginSession) X(xrEndSession) \
    X(xrPollEvent) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) X(xrCreateReferenceSpace) X(xrLocateSpace) \
    X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) \
    X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) X(xrGetInstanceProperties) X(xrGetSystemProperties) \
    X(xrResultToString) X(xrEnumerateViewConfigurationViews) X(xrLocateViews)
#define XR_DECL(n) static PFN_##n p_##n;
XR_FUNCS(XR_DECL)
static PFN_xrGetD3D11GraphicsRequirementsKHR p_xrGetD3D11GraphicsRequirementsKHR;

static XrInstance g_inst; static XrSystemId g_sys; static XrSession g_sess; static XrSpace g_local, g_viewSp;
static XrSessionState g_state; static bool g_running, g_dead;
static ID3D11Device* g_d11; static ID3D11DeviceContext* g_c11;
static XrSwapchain g_sc; static std::vector<XrSwapchainImageD3D11KHR> g_img; static UINT g_w, g_h;
// build 040: stereo, phase 1 = alternate-eye rendering. Each game frame renders one eye (left, right, left...) with that
// eye's own pose and field of view; the frame goes into that eye's swapchain and the projection layer shows each eye's
// latest image with the pose it was rendered with (the runtime reprojects the older one). Stereo=0 -> mono as before.
static IDirect3DTexture9* g_t9L; static IDirect3DSurface9* g_s9L; static ID3D11Texture2D* g_t11L; static bool g_leftDone;
static XrSwapchain g_sc1; static std::vector<XrSwapchainImageD3D11KHR> g_img1; static int g_stereo = 1; static long g_pubN;
static IDirect3DTexture9* g_t9; static IDirect3DSurface9* g_s9; static ID3D11Texture2D* g_t11; static IDirect3DQuery9* g_q9;
static XrPosef g_quad{ { 0, 0, 0, 1 }, { 0, 0, -2.2f } }; static bool g_placed;
// build 027: UI layer (second shared texture + swapchain)
static XrSwapchain g_scUi; static std::vector<XrSwapchainImageD3D11KHR> g_imgUi;
static IDirect3DTexture9* g_ui9; static IDirect3DSurface9* g_uiS9; static ID3D11Texture2D* g_ui11; static bool g_uiFrame; static int g_uiOn = 1; static float g_uiDist = 3.0f;
static float g_modP[16], g_origP[16]; static volatile bool g_projValid;
static volatile float g_ipdM = 0.063f;   // build 082: eye separation (m), for UI drawn into the eye images

// build 028: flat mode (whole picture on the screen panel, no head tracking): movies, title screen, or forced with F5
static volatile bool g_movie, g_flatAuto; static int g_flatForce;   // 0 auto, 1 force flat, 2 force VR
bool Flat() { return g_flatForce == 1 || (g_flatForce == 0 && (g_movie || g_flatAuto)); }
void SetMovieFrame(bool m) { if (m != g_movie) Log("xr: movie %s", m ? "playing -> flat screen" : "ended"); g_movie = m; }
static double g_nextTry;
// build 077: shader cache notice + keep-alive frames
int64_t g_scFmt;
static CRITICAL_SECTION g_fcs; static volatile LONG g_fcsInit; static volatile double g_lastPresentMs; static int g_keepAlive = 1;
static XrCompositionLayerProjection s_pl; static XrCompositionLayerProjectionView s_pv[2]; static XrCompositionLayerQuad s_q[2];
static const XrCompositionLayerBaseHeader* s_layers[2]; static uint32_t s_n;   // the last frame's layers, re-submitted while the game stalls
static XrSwapchain g_scN; static std::vector<XrSwapchainImageD3D11KHR> g_imgN; static ID3D11Texture2D* g_nTex[5]; static int g_nShown = -1; static bool g_nBad;
// build 075 DLSS: output (swapchain) size, packed-depth targets per eye, views of the eye pictures for scaled copies
static UINT g_ow, g_oh;
static IDirect3DTexture9* g_dt9[2]; static IDirect3DSurface9* g_ds9[2]; static ID3D11Texture2D* g_dt11[2];
static ID3D11ShaderResourceView *g_srvMain, *g_srvLeft;
void SetOutputSize(UINT w, UINT h) { g_ow = w; g_oh = h; Log("xr: eyes shown at %ux%u (DLSS upscaling)", w, h); }
IDirect3DSurface9* DepthSurface(int e) { return (e >= 0 && e < 2 && ddlss::Wanted()) ? g_ds9[e] : nullptr; }
static long g_frames; static double g_syncMs, g_waitMs, g_copyMs, g_winT;

static double Ms() { static double k; if (!k) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); k = 1000.0 / f.QuadPart; } LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart * k; }
static bool Ok(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    char s[XR_MAX_RESULT_STRING_SIZE] = ""; if (p_xrResultToString && g_inst) p_xrResultToString(g_inst, r, s);
    Log("xr: %s failed: %d %s", what, (int)r, s); return false;
}
template <class T> static void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

void Configure(const wchar_t* ini) {
    g_enabled = GetPrivateProfileIntW(L"VR", L"XR", 1, ini);
    wchar_t b[32];
    GetPrivateProfileStringW(L"VR", L"ScreenWidth", L"2.6", b, 32, ini); g_scrW = (float)_wtof(b);
    GetPrivateProfileStringW(L"VR", L"ScreenDistance", L"2.2", b, 32, ini); g_scrDist = (float)_wtof(b);
    if (g_scrW < 0.2f) g_scrW = 2.6f; if (g_scrDist < 0.3f) g_scrDist = 2.2f;
    wcscpy(g_dir, ini); wchar_t* sl = wcsrchr(g_dir, L'\\'); if (sl) sl[1] = 0; else g_dir[0] = 0;
    Log("xr: XR=%d ScreenWidth %.2f m ScreenDistance %.2f m", g_enabled, g_scrW, g_scrDist);
    g_syncWait = GetPrivateProfileIntW(L"VR", L"XrSyncWait", 1, ini);
    g_devKeys = GetPrivateProfileIntW(L"VR", L"DevKeys", 0, ini);
    g_keepAlive = GetPrivateProfileIntW(L"VR", L"XrKeepAlive", 1, ini); ntc::Configure(ini);   // build 077   // build 074: F3 / F4 / F5 test keys only for development
    vkb::Configure(GetPrivateProfileIntW(L"VR", L"VkBridgeMode", 2, ini), GetPrivateProfileIntW(L"VR", L"VkBridgeSkipCopy", 0, ini));   // build 071
}

static bool LoadLoader() {
    if (g_loader) return true;
    wchar_t p[MAX_PATH]; wcscpy(p, g_dir); wcscat(p, L"openxr_loader.dll");
    SetEnvironmentVariableA("DISABLE_XR_APILAYER_reshade_1", "1");
    g_loader = LoadLibraryW(p);
    if (!g_loader) { Log("xr: cannot load %ls (error %lu) - copy the 32-bit openxr_loader.dll next to d3d9.dll", p, GetLastError()); return false; }
    p_xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)GetProcAddress(g_loader, "xrGetInstanceProcAddr");
    if (!p_xrGetInstanceProcAddr) return false;
    p_xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&p_xrCreateInstance);
    return p_xrCreateInstance != nullptr;
}

static bool CreateD3D11(const LUID& luid) {
    IDXGIFactory1* fac = nullptr; if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac))) return false;
    IDXGIAdapter1* chosen = nullptr; IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; fac->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
        if (!chosen && !memcmp(&d.AdapterLuid, &luid, sizeof luid)) { chosen = a; continue; }
        a->Release();
    }
    fac->Release();
    if (!chosen) { Log("xr: no adapter matches the headset's LUID"); return false; }
    D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, fl, 2, D3D11_SDK_VERSION, &g_d11, nullptr, &g_c11);
    if (FAILED(hr)) hr = D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, fl + 1, 1, D3D11_SDK_VERSION, &g_d11, nullptr, &g_c11);
    DXGI_ADAPTER_DESC1 d; chosen->GetDesc1(&d); chosen->Release();
    Log("xr: D3D11 device on '%ls' -> 0x%08lx", d.Description, (unsigned long)hr);
    return SUCCEEDED(hr);
}

static void Teardown() {
    ddlss::Shutdown();
    for (auto& t : g_nTex) Rel(t); g_imgN.clear(); g_scN = XR_NULL_HANDLE; g_nShown = -1; g_nBad = false; s_n = 0;
    Rel(g_srvMain); Rel(g_srvLeft); for (int e = 0; e < 2; e++) { Rel(g_dt11[e]); Rel(g_ds9[e]); Rel(g_dt9[e]); }
    vkb::Reset();
    Rel(g_t11); Rel(g_s9); Rel(g_t9); Rel(g_q9); g_img.clear(); g_sc = XR_NULL_HANDLE; g_img1.clear(); g_sc1 = XR_NULL_HANDLE;
    Rel(g_ui11); Rel(g_uiS9); Rel(g_ui9); g_imgUi.clear(); g_scUi = XR_NULL_HANDLE; Rel(g_t11L); Rel(g_s9L); Rel(g_t9L);
    if (g_inst && p_xrDestroyInstance) p_xrDestroyInstance(g_inst);
    g_inst = XR_NULL_HANDLE; g_sess = XR_NULL_HANDLE; g_local = g_viewSp = XR_NULL_HANDLE; g_sys = XR_NULL_SYSTEM_ID;
    Rel(g_c11); Rel(g_d11); g_running = false; g_state = XR_SESSION_STATE_UNKNOWN; g_placed = false;
}

static bool InitInstance() {
    if (!LoadLoader()) { g_dead = true; return false; }
    const char* exts[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo ici{ XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy(ici.applicationInfo.applicationName, "DDDAVR"); strcpy(ici.applicationInfo.engineName, "MT Framework");
    ici.applicationInfo.applicationVersion = 22; ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = 1; ici.enabledExtensionNames = exts;
    XrResult cr = p_xrCreateInstance(&ici, &g_inst);
    if (XR_FAILED(cr)) {
        std::wstring src; xrprobe::Runtime a = xrprobe::Active32(&src), b = xrprobe::Active64();
        Log("xr: runtime for this 32-bit game (%s): %s -> %s (%s)", xrprobe::N(src).c_str(), a.json.empty() ? "(none)" : xrprobe::N(a.json).c_str(),
            a.library.empty() ? "(no library)" : xrprobe::N(a.library).c_str(), xrprobe::BitsText(a.bits));
        Log("xr: 64-bit active runtime: %s (%s)", b.json.empty() ? "(none)" : xrprobe::N(b.json).c_str(), xrprobe::BitsText(b.bits));
        Ok(cr, "xrCreateInstance"); g_inst = XR_NULL_HANDLE; return false;
    }
#define XR_LOAD(n) if (XR_FAILED(p_xrGetInstanceProcAddr(g_inst, #n, (PFN_xrVoidFunction*)&p_##n))) { Log("xr: missing %s", #n); return false; }
    XR_FUNCS(XR_LOAD)
#undef XR_LOAD
    p_xrGetInstanceProcAddr(g_inst, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&p_xrGetD3D11GraphicsRequirementsKHR);
    XrInstanceProperties ip{ XR_TYPE_INSTANCE_PROPERTIES }; p_xrGetInstanceProperties(g_inst, &ip);
    Log("xr: runtime %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
    XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO }; sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!Ok(p_xrGetSystem(g_inst, &sgi, &g_sys), "xrGetSystem (headset not found?)")) return false;
    XrSystemProperties sp{ XR_TYPE_SYSTEM_PROPERTIES }; p_xrGetSystemProperties(g_inst, g_sys, &sp);
    uint32_t nv = 0; XrViewConfigurationView vcv[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    p_xrEnumerateViewConfigurationViews(g_inst, g_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv, vcv);
    Log("xr: system '%s', recommended eye %ux%u", sp.systemName, vcv[0].recommendedImageRectWidth, vcv[0].recommendedImageRectHeight);
    XrGraphicsRequirementsD3D11KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    if (!p_xrGetD3D11GraphicsRequirementsKHR || !Ok(p_xrGetD3D11GraphicsRequirementsKHR(g_inst, g_sys, &req), "xrGetD3D11GraphicsRequirementsKHR")) { g_dead = true; return false; }
    if (!CreateD3D11(req.adapterLuid)) { g_dead = true; return false; }
    XrGraphicsBindingD3D11KHR gb{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR }; gb.device = g_d11;
    XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO }; sci.next = &gb; sci.systemId = g_sys;
    if (!Ok(p_xrCreateSession(g_inst, &sci, &g_sess), "xrCreateSession")) { g_dead = true; return false; }
    XrReferenceSpaceCreateInfo rs{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO }; rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; if (!Ok(p_xrCreateReferenceSpace(g_sess, &rs, &g_local), "LOCAL space")) { g_dead = true; return false; }
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW; if (!Ok(p_xrCreateReferenceSpace(g_sess, &rs, &g_viewSp), "VIEW space")) g_viewSp = XR_NULL_HANDLE;
    return true;
}

// build 070: a D3D9 render-target texture whose picture reaches the D3D11 (OpenXR) device - by D3D9Ex shared handle, or
// through the Vulkan bridge when the game runs on DXVK
static bool MakeSharedTex(IDirect3DDevice9* dev, UINT w, UINT h, IDirect3DTexture9** t9, ID3D11Texture2D** t11, const char* what) {
    if (vkb::Detect(dev)) return vkb::MakeShared(dev, g_d11, w, h, t9, t11);
    HANDLE sh = nullptr;
    HRESULT hr = dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, t9, &sh);
    if (FAILED(hr) || !sh) { Log("xr: shared D3D9Ex texture (%s) %ux%u failed 0x%08lx (handle %p)", what, w, h, (unsigned long)hr, sh); if (*t9) { (*t9)->Release(); *t9 = nullptr; } return false; }
    hr = g_d11->OpenSharedResource(sh, __uuidof(ID3D11Texture2D), (void**)t11);
    if (FAILED(hr)) { Log("xr: D3D11 OpenSharedResource (%s) failed 0x%08lx", what, (unsigned long)hr); (*t9)->Release(); *t9 = nullptr; return false; }
    return true;
}
static bool InitTargets(IDirect3DDevice9* dev, UINT w, UINT h) {
    uint32_t nf = 0; p_xrEnumerateSwapchainFormats(g_sess, 0, &nf, nullptr);
    std::vector<int64_t> f(nf); p_xrEnumerateSwapchainFormats(g_sess, nf, &nf, f.data());
    int64_t fmt = 0;
    for (int64_t want : { (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM }) { for (int64_t x : f) if (x == want) { fmt = x; break; } if (fmt) break; }
    { char s[256] = ""; size_t n = 0; for (int64_t x : f) if (n < 240) n += snprintf(s + n, sizeof s - n, "%lld ", (long long)x); Log("xr: swapchain formats offered: %s-> using %lld", s, (long long)fmt); }
    if (!fmt) { Log("xr: no BGRA swapchain format (needs a shader copy - not in this build)"); return false; }
    g_scFmt = fmt;
    XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    const UINT sw = g_ow ? g_ow : w, shh = g_oh ? g_oh : h;   // build 075: eye swapchains at the shown size
    ci.format = fmt; ci.sampleCount = 1; ci.width = sw; ci.height = shh; ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (!Ok(p_xrCreateSwapchain(g_sess, &ci, &g_sc), "xrCreateSwapchain")) return false;
    uint32_t n = 0; p_xrEnumerateSwapchainImages(g_sc, 0, &n, nullptr);
    g_img.assign(n, XrSwapchainImageD3D11KHR{ XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
    p_xrEnumerateSwapchainImages(g_sc, n, &n, (XrSwapchainImageBaseHeader*)g_img.data());
    if (g_stereo) {   // the right eye's swapchain
        if (Ok(p_xrCreateSwapchain(g_sess, &ci, &g_sc1), "xrCreateSwapchain (right eye)")) {
            uint32_t n1 = 0; p_xrEnumerateSwapchainImages(g_sc1, 0, &n1, nullptr);
            g_img1.assign(n1, XrSwapchainImageD3D11KHR{ XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
            p_xrEnumerateSwapchainImages(g_sc1, n1, &n1, (XrSwapchainImageBaseHeader*)g_img1.data());
            Log("xr: stereo - right-eye swapchain ready (mode %d)", g_stereo);
            // build 042: the left eye's picture, kept between the two replays
            if (MakeSharedTex(dev, w, h, &g_t9L, &g_t11L, "left eye")) g_t9L->GetSurfaceLevel(0, &g_s9L);
            else { Log("xr: left-eye shared texture failed - synchronous stereo off"); Rel(g_t9L); Rel(g_t11L); }
        } else { g_stereo = 0; Log("xr: no right-eye swapchain -> mono"); }
    }
    if (!MakeSharedTex(dev, w, h, &g_t9, &g_t11, "main")) return false;
    g_t9->GetSurfaceLevel(0, &g_s9);
    if (!vkb::Active()) dev->CreateQuery(D3DQUERYTYPE_EVENT, &g_q9);
    g_w = w; g_h = h; if (g_ow == w && g_oh == h) g_ow = g_oh = 0;
    if (ddlss::Wanted()) {   // build 075: the packed scene depth of each eye (D3D9 pass after its replay)
        for (int e = 0; e < 2; e++) if (MakeSharedTex(dev, w, h, &g_dt9[e], &g_dt11[e], e ? "right depth" : "left depth")) g_dt9[e]->GetSurfaceLevel(0, &g_ds9[e]);
        Log("xr: DLSS depth targets %s", g_ds9[0] && g_ds9[1] ? "ready" : "FAILED");
    }
    ci.width = w; ci.height = h;   // the UI layer stays at the game's size
    if (g_uiOn) {
        if (Ok(p_xrCreateSwapchain(g_sess, &ci, &g_scUi), "xrCreateSwapchain (UI)") &&
            MakeSharedTex(dev, w, h, &g_ui9, &g_ui11, "UI")) {
            g_ui9->GetSurfaceLevel(0, &g_uiS9);
            uint32_t un = 0; p_xrEnumerateSwapchainImages(g_scUi, 0, &un, nullptr);
            g_imgUi.assign(un, XrSwapchainImageD3D11KHR{ XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
            p_xrEnumerateSwapchainImages(g_scUi, un, &un, (XrSwapchainImageBaseHeader*)g_imgUi.data());
            Log("xr: UI layer ready (%ux%u, panel %.1f m away)", w, h, g_uiDist);
        } else { Log("xr: UI layer unavailable"); Rel(g_ui11); Rel(g_ui9); }
    }
    Log("xr: ready - %u swapchain images %ux%u, game picture %ux%u, shared texture open on D3D11", n, sw, shh, w, h);
    return true;
}

static void Poll() {
    XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
    while (p_xrPollEvent(g_inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* s = (XrEventDataSessionStateChanged*)&ev; g_state = s->state;
            Log("xr: session state %d", (int)g_state);
            if (g_state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO }; bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (Ok(p_xrBeginSession(g_sess, &bi), "xrBeginSession")) g_running = true;
            } else if (g_state == XR_SESSION_STATE_STOPPING) { p_xrEndSession(g_sess); g_running = false; }
            else if (g_state == XR_SESSION_STATE_EXITING || g_state == XR_SESSION_STATE_LOSS_PENDING) { g_running = false; g_dead = true; }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) { g_running = false; g_dead = true; }
        ev = XrEventDataBuffer{ XR_TYPE_EVENT_DATA_BUFFER };
    }
}

// put the screen straight ahead of where the head looks (yaw only), at eye height
static void Place(XrTime t) {
    if (!g_viewSp) return;
    XrSpaceLocation l{ XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(p_xrLocateSpace(g_viewSp, g_local, t, &l)) || !(l.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return;
    const XrQuaternionf& q = l.pose.orientation;
    float fx = -2 * (q.x * q.z + q.w * q.y), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));   // view -Z in LOCAL
    float yaw = atan2f(-fx, -fz);
    g_quad.orientation = { 0, sinf(yaw / 2), 0, cosf(yaw / 2) };
    g_quad.position = { l.pose.position.x + fx / sqrtf(fx * fx + fz * fz + 1e-9f) * g_scrDist, l.pose.position.y, l.pose.position.z + fz / sqrtf(fx * fx + fz * fz + 1e-9f) * g_scrDist };
    g_placed = true;
    Log("xr: screen placed %.2f m ahead (yaw %.0f deg, height %.2f)", g_scrDist, yaw * 57.2958f, l.pose.position.y);
}

// ---------------------------------------------------------------- build 024: head tracking (mono)
// At Present N: the frame N picture is copied and submitted (xrEndFrame) with the head pose that frame N's camera used;
// then xrWaitFrame/xrBeginFrame for frame N+1 and the predicted head pose is published for the next camera setup.
struct Q { float x, y, z, w; };
static Q Mul(Q a, Q b) { return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z }; }
static Q Conj(Q a) { return { -a.x, -a.y, -a.z, a.w }; }
static void Rot(Q q, const float v[3], float o[3]) {   // o = q v q*
    Q p{ v[0], v[1], v[2], 0 }; Q r = Mul(Mul(q, p), Conj(q)); o[0] = r.x; o[1] = r.y; o[2] = r.z;
}
static float YawOf(Q q) { float fx = -2 * (q.x * q.z + q.w * q.y), fz = -(1 - 2 * (q.x * q.x + q.y * q.y)); return atan2f(-fx, -fz); }

struct Pose { bool valid; Q abs; float pos[3]; XrFovf fov; Q rel; float relPos[3]; float gt[4]; bool gtValid; int eye; float eyeOff[3]; bool sync; Q absR; float posR[3]; };   // eye: -1 mono (both), 0 left, 1 right   // gt: game camera frustum tangents L R D U
static CRITICAL_SECTION g_pcs; static bool g_pcsInit;
static Pose g_next;                 // published at BeginFrame, for the next camera setup
static Pose g_used[4]; static long g_nUsed; static Pose g_lastEye[2];   // build 040: latest submitted pose per eye
// build 041: which eye a presented frame was really rendered with. The game builds frame N+1 while the render thread
// replays frame N, so "the latest camera" is not the presented one. Each eye's projection is distinct (asymmetric FOV):
// the render thread's VS constant uploads are scanned for a row/column of either eye's projection.
static float g_eyeP[2][16]; static volatile bool g_eyePValid[2]; static volatile int g_seenEye = -1; static long g_eyeMiss, g_eyeHit;   // poses the camera setups actually used
static int g_track = 1, g_headPos = 1, g_lag = 0; static float g_scale = 100.0f;
static bool g_frameOpen; static XrFrameState g_fs{ XR_TYPE_FRAME_STATE };
static Q g_refYaw{ 0, 0, 0, 1 }; static float g_refPos[3]; static bool g_refSet, g_wantRef;
static long g_camCalls, g_camApplied;

void ConfigureTracking(const wchar_t* ini) {
    g_track = GetPrivateProfileIntW(L"VR", L"HeadTracking", 1, ini);
    g_headPos = GetPrivateProfileIntW(L"VR", L"HeadPosition", 1, ini);
    g_lag = GetPrivateProfileIntW(L"VR", L"PoseLag", 0, ini);
    g_uiOn = GetPrivateProfileIntW(L"VR", L"UiPanel", 1, ini);
    g_stereo = GetPrivateProfileIntW(L"VR", L"Stereo", 2, ini);   // build 042: 0 mono, 1 alternate eyes, 2 both eyes every frame
    { wchar_t u[32]; GetPrivateProfileStringW(L"VR", L"UiDistance", L"3.0", u, 32, ini); g_uiDist = (float)_wtof(u); if (g_uiDist < 0.5f) g_uiDist = 3.0f; }
    wchar_t b[32]; GetPrivateProfileStringW(L"VR", L"WorldScale", L"100", b, 32, ini); g_scale = (float)_wtof(b); if (g_scale <= 0) g_scale = 100;
    Log("xr: HeadTracking=%d HeadPosition=%d WorldScale %.1f units/m PoseLag %d (F6 toggles tracking / screen, F7 recentres)", g_track, g_headPos, g_scale, g_lag);
}

static void PublishPose(XrTime t) {
    XrViewLocateInfo li{ XR_TYPE_VIEW_LOCATE_INFO }; li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; li.displayTime = t; li.space = g_local;
    XrView v[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } }; XrViewState vs{ XR_TYPE_VIEW_STATE }; uint32_t n = 0;
    Pose p{};
    if (XR_SUCCEEDED(p_xrLocateViews(g_sess, &li, &vs, 2, &n, v)) && n == 2 && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
        const XrQuaternionf& o = v[0].pose.orientation; p.abs = { o.x, o.y, o.z, o.w };
        for (int k = 0; k < 3; k++) p.pos[k] = ((&v[0].pose.position.x)[k] + (&v[1].pose.position.x)[k]) * 0.5f;
        p.fov.angleLeft = fminf(v[0].fov.angleLeft, v[1].fov.angleLeft); p.fov.angleRight = fmaxf(v[0].fov.angleRight, v[1].fov.angleRight);
        p.fov.angleUp = fmaxf(v[0].fov.angleUp, v[1].fov.angleUp); p.fov.angleDown = fminf(v[0].fov.angleDown, v[1].fov.angleDown);
        p.eye = -1;
        float mid[3] = { p.pos[0], p.pos[1], p.pos[2] };
        if (g_stereo == 2 && g_sc1 && !Flat()) {   // build 042: both eyes from one camera: left eye pose + the union field of view
            const XrQuaternionf& o0 = v[0].pose.orientation; p.abs = { o0.x, o0.y, o0.z, o0.w };
            p.pos[0] = v[0].pose.position.x; p.pos[1] = v[0].pose.position.y; p.pos[2] = v[0].pose.position.z;
            const XrQuaternionf& o1 = v[1].pose.orientation; p.absR = { o1.x, o1.y, o1.z, o1.w };
            p.posR[0] = v[1].pose.position.x; p.posR[1] = v[1].pose.position.y; p.posR[2] = v[1].pose.position.z;
            p.sync = true;
        } else if (g_stereo == 1 && g_sc1 && !Flat()) {   // this frame's eye: its own orientation, position (IPD) and field of view
            int e = (int)(g_pubN++ & 1); p.eye = e;
            const XrQuaternionf& oe = v[e].pose.orientation; p.abs = { oe.x, oe.y, oe.z, oe.w };
            p.pos[0] = v[e].pose.position.x; p.pos[1] = v[e].pose.position.y; p.pos[2] = v[e].pose.position.z; p.fov = v[e].fov;
            LOG_FIRST(2, "xr: stereo eye %d fov L %.1f R %.1f U %.1f D %.1f, IPD %.1f mm", e, p.fov.angleLeft * 57.2958f, p.fov.angleRight * 57.2958f,
                      p.fov.angleUp * 57.2958f, p.fov.angleDown * 57.2958f, 1000.0f * sqrtf((v[0].pose.position.x - v[1].pose.position.x) * (v[0].pose.position.x - v[1].pose.position.x) +
                      (v[0].pose.position.y - v[1].pose.position.y) * (v[0].pose.position.y - v[1].pose.position.y) + (v[0].pose.position.z - v[1].pose.position.z) * (v[0].pose.position.z - v[1].pose.position.z)));
        }
        if (!g_refSet || g_wantRef) {
            float yaw = YawOf(p.abs); g_refYaw = { 0, sinf(yaw / 2), 0, cosf(yaw / 2) };
            memcpy(g_refPos, mid, sizeof g_refPos); g_refSet = true; g_wantRef = false;
            Log("xr: tracking reference set (yaw %.0f deg, head at %.2f %.2f %.2f); view fov L %.1f R %.1f U %.1f D %.1f deg", yaw * 57.2958f, p.pos[0], p.pos[1], p.pos[2],
                p.fov.angleLeft * 57.2958f, p.fov.angleRight * 57.2958f, p.fov.angleUp * 57.2958f, p.fov.angleDown * 57.2958f);
        }
        p.rel = Mul(Conj(g_refYaw), p.abs);
        { float o[3] = { p.pos[0] - mid[0], p.pos[1] - mid[1], p.pos[2] - mid[2] }; Rot(Conj(g_refYaw), o, p.eyeOff); }   // eye offset from the head centre
        float d[3] = { p.pos[0] - g_refPos[0], p.pos[1] - g_refPos[1], p.pos[2] - g_refPos[2] }; Rot(Conj(g_refYaw), d, p.relPos);
        p.valid = true;
    }
    EnterCriticalSection(&g_pcs); g_next = p; LeaveCriticalSection(&g_pcs);
}

// ---------------------------------------------------------------- build 042: synchronous stereo
// The render thread's draw function (BbsRpg 0xdab480) replays the frame's command list; the proxy calls it twice. Replay 0
// is the left eye (the camera the game thread set up). Replay 1 is the right eye: every VS constant upload of the camera's
// view-projection (4 rows = VP_L) is replaced by VP_L * M with M = P^-1 * T(-d) * P (d = right eye offset in view space, same
// union-FOV projection P for both eyes, so the game's culling frustum covers both). Draws to non-eye targets (shadow maps,
// GPU simulations, environment maps) and UI draws are skipped in replay 1.
struct SyncCam { bool valid; float VP[16]; float M[16]; Pose pose;
                 float V[16], VR[16], Vi[16], ViR[16], VPi[16], VPiR[16], camL[3], camR[3];
                 float T[16], Ti[16], Mi[16];
                 double VPu[2][16]; float jx, jy; bool depthInv; long id;
                 float Mh[16], Pj[16], s, xs0, ys0; bool mh; };   // build 083: head transform (game view -> left eye view), eye projection, game y scale   // build 075: unjittered per-eye VP, jitter (render px), depth convention   // build 052: eye offset forms for relative substitution   // build 043: all eye-dependent forms
static long g_subN[8];   // per kind: 0 VP, 1 VP^-1, 2 V, 3 V^-1, 4 V^-1 transposed 3x4, 5 camera position (VS), 6 PS forms
static SyncCam g_cams[4]; static long g_camN; static volatile int g_replayEye = -1; static volatile int g_replayCam = -1;
static long g_subs, g_subFrames, g_camMatch;

static void MatMul(const float* a, const float* b, float* o) { for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float x = 0; for (int k = 0; k < 4; k++) x += a[i * 4 + k] * b[k * 4 + j]; o[i * 4 + j] = x; } }
static bool MatInv(const float* m, float* o) {   // general 4x4 inverse (Gauss-Jordan, double)
    double a[4][8]; for (int i = 0; i < 4; i++) for (int j = 0; j < 8; j++) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i);
    for (int c = 0; c < 4; c++) {
        int pr = c; for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[pr][c])) pr = r;
        if (fabs(a[pr][c]) < 1e-12) return false;
        if (pr != c) for (int j = 0; j < 8; j++) { double t = a[c][j]; a[c][j] = a[pr][j]; a[pr][j] = t; }
        double d = a[c][c]; for (int j = 0; j < 8; j++) a[c][j] /= d;
        for (int r = 0; r < 4; r++) if (r != c) { double f = a[r][c]; for (int j = 0; j < 8; j++) a[r][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) o[i * 4 + j] = (float)a[i][j + 4];
    return true;
}
static void MatMulD(const double* a, const double* b, double* o) { for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { double x = 0; for (int k = 0; k < 4; k++) x += a[i * 4 + k] * b[k * 4 + j]; o[i * 4 + j] = x; } }
static void SyncRecord(const Pose& p, const float* V, const float* P, float s, const float* Pu, float jx, float jy, const float* Mh, float xs0, float ys0) {
    SyncCam c{}; c.pose = p; MatMul(V, P, c.VP);
    if (Mh) { memcpy(c.Mh, Mh, 64); c.mh = true; } memcpy(c.Pj, P, 64); c.s = s; c.xs0 = xs0; c.ys0 = ys0;
    // right-eye offset in the left eye's view axes (OpenXR head axes: x right, y up, +z back; left-handed game: flip z)
    float dw[3] = { p.posR[0] - p.pos[0], p.posR[1] - p.pos[1], p.posR[2] - p.pos[2] }, dh[3]; Rot(Conj(p.abs), dw, dh);
    { float ipd = sqrtf(dw[0] * dw[0] + dw[1] * dw[1] + dw[2] * dw[2]); if (ipd > 0.04f && ipd < 0.09f) g_ipdM = ipd; }
    float t[3] = { dh[0] * g_scale, dh[1] * g_scale, dh[2] * g_scale * (s > 0 ? -1 : 1) };
    float T[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, -t[0],-t[1],-t[2],1 }, Pi[16], tmp[16];
    if (!MatInv(P, Pi)) return;
    MatMul(Pi, T, tmp); MatMul(tmp, P, c.M);
    memcpy(c.V, V, 64); MatMul(V, T, c.VR); memcpy(c.T, T, 64);
    if (!MatInv(T, c.Ti) || !MatInv(c.M, c.Mi)) return;
    if (!MatInv(c.V, c.Vi) || !MatInv(c.VR, c.ViR) || !MatInv(c.VP, c.VPi)) return;
    { float VPR[16]; MatMul(c.VP, c.M, VPR); if (!MatInv(VPR, c.VPiR)) return; }
    for (int k = 0; k < 3; k++) { c.camL[k] = c.Vi[12 + k]; c.camR[k] = c.ViR[12 + k]; }
    {   // build 075: the eyes' unjittered view-projections (DLSS motion vectors), in double
        double Vd[16], VRd[16], Pd[16]; for (int i = 0; i < 16; i++) { Vd[i] = V[i]; Pd[i] = Pu[i]; }
        double Td[16]; for (int i = 0; i < 16; i++) Td[i] = T[i];
        MatMulD(Vd, Td, VRd); MatMulD(Vd, Pd, c.VPu[0]); MatMulD(VRd, Pd, c.VPu[1]);
        c.jx = jx; c.jy = jy; c.depthInv = Pu[10] / s < 0.5f;   // far plane -> ndc z = P10 / s: ~1 standard, ~0 reversed
    }
    c.valid = true;
    EnterCriticalSection(&g_pcs); c.id = g_camN; g_cams[g_camN & 3] = c; g_camN++; LeaveCriticalSection(&g_pcs);
    LOG_FIRST(1, "xr: synchronous stereo - right eye offset %.2f %.2f %.2f game units", t[0], t[1], t[2]);
}
static bool Near(float a, float b) { return fabsf(a - b) <= 1e-3f * fmaxf(1.0f, fabsf(b)); }
bool SyncActive() { return g_stereo == 2 && g_sc1 && g_t11L && g_running && g_track && !Flat() && g_camN > 0; }
void SyncSetEye(int e) { g_replayEye = e; if (e == 0) g_replayCam = -1; }
int SyncEye() { return g_replayEye; }
static bool Match16(const float* r, const float* m) { for (int q = 0; q < 16; q++) if (!Near(r[q], m[q])) return false; return true; }
// build 052: loose match for the view-related forms. The game's copies can be one frame older or newer than the ring entry
// (camera motion); the substitution is relative (game value x eye offset), so only the identification needs to be robust.
static bool Loose16(const float* r, const float* m) { for (int q = 0; q < 16; q++) if (fabsf(r[q] - m[q]) > 0.02f + 2e-3f * fabsf(m[q])) return false; return true; }
static float Err16(const float* r, const float* m) { float e = 0; for (int q = 0; q < 16; q++) e = fmaxf(e, fabsf(r[q] - m[q]) / fmaxf(1.0f, fabsf(m[q]))); return e; }
static bool MatchT34(const float* r, const float* m) {   // 3 rows = columns 0..2 of a row-vector 4x4 (column-major upload)
    for (int i = 0; i < 3; i++) for (int j = 0; j < 4; j++) if (!Near(r[i * 4 + j], m[j * 4 + i])) return false; return true;
}
static long g_camErr[4]; static float g_camErrMax; float g_camTol = 16.0f;
static int g_subMode;   // build 044 test key F3: 0 all forms, 1 no camera position in PS, 2 no camera position at all, 3 matrices in VS only
static bool SubstRows(float* f, unsigned n, bool ps) {
    if (ps && g_subMode == 3) return false;
    const SyncCam& c = g_cams[g_replayCam]; bool ch = false;
    for (unsigned i = 0; i < n; i++) {
        float* r = f + i * 4;
        if (i + 4 <= n) {
            float o[16];
            if (!ps && Match16(r, c.VP)) { MatMul(r, c.M, o); memcpy(r, o, 64); g_subN[0]++; i += 3; ch = true; continue; }
            if (Loose16(r, c.VPi)) { MatMul(c.Mi, r, o); memcpy(r, o, 64); g_subN[ps ? 6 : 1]++; i += 3; ch = true; continue; }   // (VP M)^-1 = M^-1 VP^-1
            if (Loose16(r, c.V)) { MatMul(r, c.T, o); memcpy(r, o, 64); g_subN[ps ? 6 : 2]++; i += 3; ch = true; continue; }      // V T
            if (Loose16(r, c.Vi)) { MatMul(c.Ti, r, o); memcpy(r, o, 64); g_subN[ps ? 6 : 3]++; i += 3; ch = true; continue; }    // (V T)^-1 = T^-1 V^-1
        }
        if (i + 3 <= n && MatchT34(r, c.Vi)) {   // 3 rows = columns 0..2 of V^-1: rebuild, T^-1 V^-1, write back
            float a[16] = {}, o[16]; for (int k = 0; k < 3; k++) for (int j = 0; j < 4; j++) a[j * 4 + k] = r[k * 4 + j]; a[15] = 1;
            MatMul(c.Ti, a, o); for (int k = 0; k < 3; k++) for (int j = 0; j < 4; j++) r[k * 4 + j] = o[j * 4 + k];
            g_subN[ps ? 6 : 4]++; i += 2; ch = true; continue;
        }
        if (g_subMode < 2 - (ps ? 1 : 0) + (ps ? 0 : 0) && !(ps && g_subMode == 1)) {   // camera position
            // build 049: the game's own camera position (c29 of the shadow resolve) differs from ours by float error that grows
            // with the world coordinates; at 0.02 it matched only on some frames -> right-eye shadow flashes. Tolerance now 0.5
            // units (5 mm); near-misses up to 4 units are counted (log line "cam pos near-miss").
            float e = fmaxf(fabsf(r[0] - c.camL[0]), fmaxf(fabsf(r[1] - c.camL[1]), fabsf(r[2] - c.camL[2])));
            if (e < 32.0f) { int b = e < 0.02f ? 0 : e < 0.5f ? 1 : e < 16.0f ? 2 : 3; g_camErr[b]++; if (e > g_camErrMax && e < g_camTol) g_camErrMax = e; }
            // build 050: the errors are spread continuously up to several units (camera motion between the game's copy and ours),
            // so the right eye gets the game's own value moved by the eye offset (camR - camL), within CamPosTolerance (16 units).
            if (e < g_camTol) { for (int k = 0; k < 3; k++) r[k] += c.camR[k] - c.camL[k]; g_subN[ps ? 6 : 5]++; ch = true; }
        }
    }
    return ch;
}
// build 068: cheap pre-check, so the hooks copy and run the full matcher only for uploads that can contain a camera form.
// Each test is a necessary condition of the full match (first element within the loosest tolerance used by the matcher).
static inline bool LooseF(float a, float m) { return fabsf(a - m) <= 0.02f + 2e-3f * fabsf(m); }
bool SyncMaybe(const float* f, unsigned n, bool ps) {
    if (g_replayEye != 1 || g_replayCam < 0 || !f || !n) return false;
    if (ps && g_subMode == 3) return false;
    const SyncCam& c = g_cams[g_replayCam];
    const bool camPos = g_subMode < 2 - (ps ? 1 : 0) && !(ps && g_subMode == 1);
    for (unsigned i = 0; i < n; i++) {
        const float* r = f + i * 4;
        if (i + 3 <= n && (LooseF(r[0], c.VP[0]) || LooseF(r[0], c.VPi[0]) || LooseF(r[0], c.V[0]) || LooseF(r[0], c.Vi[0]))) return true;
        if (camPos && fabsf(r[0] - c.camL[0]) < g_camTol && fabsf(r[1] - c.camL[1]) < g_camTol && fabsf(r[2] - c.camL[2]) < g_camTol) return true;
    }
    return false;
}
bool SyncNeedCam() { return g_replayEye == 0 && g_replayCam < 0; }
bool SyncVS(float* f, unsigned n) {   // VS constant upload during a replay; true if changed
    if (g_replayEye < 0 || !f || !n) return false;
    if (g_replayEye == 0) {
        if (g_replayCam >= 0 || n < 4) return false;
        for (unsigned i = 0; i + 4 <= n; i++) {   // which camera is this frame's: build 052: the closest one whose VP appears
            int best = -1; float be = 1e30f;
            for (int j = 0; j < 4; j++) { long id = g_camN - 1 - j; if (id < 0) break; const SyncCam& c = g_cams[id & 3];
                if (c.valid && Match16(f + i * 4, c.VP)) { float e = Err16(f + i * 4, c.VP); if (e < be) { be = e; best = (int)(id & 3); } } }
            if (best >= 0) { g_replayCam = best; g_camMatch++; return false; }
        }
        return false;
    }
    if (g_replayEye != 1 || g_replayCam < 0) return false;
    bool ch = SubstRows(f, n, false); if (ch) g_subs++; return ch;
}
// build 083: a world-anchored GUI element (name tag, quest marker, enemy bar) is placed by the game with its own camera: position in
// the 1280x720 HUD band (x, y) and view depth z. Rebuild the point in the game camera's view space, move it into this replay's eye
// view (head transform, + eye offset for the right eye) and project it with the eye's projection: full-buffer UI units
// (1280 x 1360, the scene target) and the size factor that keeps the game's angular size.
// build 087: the game places world GUI with the HEAD-TRACKED view (the camera keeps our matrices, KeepVrCamera) but its OWN
// projection (55 deg, 16:9) into the 1280x720 band; c4.z is the depth along the game camera's own axis (study recording 2:
// y rms 1.6 UI units, x within the NPC's idle sway). So: the ray from the band position in the head view, the distance from the
// game-axis depth, then this eye's real projection.
bool WorldGuiToEye(int mode, float x, float y, float z, float* X, float* Y, float* k, float* rs, float* rc) {
    (void)mode;
    if (g_replayEye < 0 || g_replayCam < 0) return false;
    const SyncCam& c = g_cams[g_replayCam];
    if (!c.valid || !c.mh || !c.xs0 || !c.ys0) return false;
    const float* P = c.Pj; const float s = c.s;
    const float nx = x / 640.0f - 1.0f, ny = 1.0f - y / 360.0f;
    const float r[3] = { nx / c.xs0, ny / c.ys0, s };   // head view ray (unit view depth)
    float Mi[16]; if (!MatInv(c.Mh, Mi)) return false;   // head view -> game view
    const float a = r[0] * Mi[2] + r[1] * Mi[6] + r[2] * Mi[10], b = Mi[14];   // game view z = t*a + b
    float t = fabsf(a) > 1e-4f ? (s * z - b) / a : z;
    if (t < 1.0f) t = z;
    float pe[4] = { r[0] * t, r[1] * t, r[2] * t, 1.0f };
    if (g_replayEye == 1) { float pr[4]; for (int j = 0; j < 4; j++) { pr[j] = 0; for (int i = 0; i < 4; i++) pr[j] += pe[i] * c.T[i * 4 + j]; } memcpy(pe, pr, 16); }
    float cl[4]; for (int j = 0; j < 4; j++) { cl[j] = 0; for (int i = 0; i < 4; i++) cl[j] += pe[i] * P[i * 4 + j]; }
    if (cl[3] < 1.0f) { *X = -100000.0f; *Y = -100000.0f; *k = 1.0f; return true; }   // behind the eye: off screen
    *X = (cl[0] / cl[3] + 1.0f) * 640.0f; *Y = (1.0f - cl[1] / cl[3]) * 680.0f;
    *k = P[5] / c.ys0;   // the size they had on the HUD panel
    {   // build 092: keep them upright in the world when the head rolls: world up seen in the eye view (row 1 of V)
        float ux = c.V[4], uy = c.V[5], n = sqrtf(ux * ux + uy * uy);
        if (n > 1e-3f) { *rs = ux / n; *rc = uy / n; } else { *rs = 0; *rc = 1; }
    }
    return true;
}
// build 085: world GUI placement study - one line per frame with the element's c4 and the replayed camera
void WorldGuiLog(const float* c4, long frame, int world) {
    if (g_replayCam < 0) return;
    const SyncCam& c = g_cams[g_replayCam];
    if (!c.valid) return;
    char b[1600]; int n = snprintf(b, sizeof b, "wgui%s f%ld id%ld c4 %.3f %.3f %.3f | P %.5f %.5f %.5f %.5f | o %.5f %.5f | V", world ? "W" : "H", frame, c.id, c4[0], c4[1], c4[2], c.Pj[0], c.Pj[5], c.Pj[8], c.Pj[9], c.xs0, c.ys0);
    for (int i = 0; i < 16; i++) n += snprintf(b + n, sizeof b - n, " %.6g", c.V[i]);
    n += snprintf(b + n, sizeof b - n, " | M");
    for (int i = 0; i < 16; i++) n += snprintf(b + n, sizeof b - n, " %.6g", c.Mh[i]);
    Log("%s", b);
}
bool SyncDebugCam(float* camL, float* camR) { if (g_replayCam < 0) return false; const SyncCam& c = g_cams[g_replayCam]; memcpy(camL, c.camL, 12); memcpy(camR, c.camR, 12); return true; }
bool SyncPS(float* f, unsigned n) {   // build 043: PS uploads in the right-eye replay (camera position, inverse matrices)
    if (g_replayEye != 1 || g_replayCam < 0 || !f || !n) return false;
    return SubstRows(f, n, true);
}
void SyncLeftDone(IDirect3DDevice9* dev) {   // after replay 0: keep the left eye's picture
    g_leftDone = false;
    IDirect3DSurface9* bb = nullptr;
    if (g_s9L && SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        g_leftDone = SUCCEEDED(dev->StretchRect(bb, nullptr, g_s9L, nullptr, D3DTEXF_NONE)); bb->Release();
    }
}

// Called by the camera hook (game thread) with the main camera's view (cam+0x90) and projection (cam+0xd0).
// build 082: horizontal clip-space shift per eye that puts screen-fixed text at distance d (m): each eye +-(IPD/2)/d in tangent
float StereoClipShift(float d) { return d > 0.1f && g_projValid ? 0.5f * g_ipdM / d * g_modP[0] : 0.0f; }
static volatile bool g_viewZoomed;   // build 081: the game view is narrower than normal (bow aim, grabs, cutscenes)
bool GameViewZoomed() { return g_viewZoomed; }
bool ApplyToCamera(float* V, float* P) {
    if (!g_pcsInit || !g_track || !g_running) return false;
    {   // the game camera's own vertical field of view: a narrow one is the title screen camera -> flat screen
        float ys = P[5]; float fovV = ys > 1e-4f ? 2 * atanf(1 / ys) * 57.2958f : 0;
        static float lastFov = -1; static DWORD lastT;
        if (fabsf(fovV - lastFov) > 3 || GetTickCount() - lastT > 10000) {
            Log("xr: game camera vertical fov %.1f deg, x scale %.3f, y scale %.3f, offsets %.3f %.3f, cam pos %.0f %.0f %.0f", fovV, P[0], P[5], P[8], P[9], V[12], V[13], V[14]);
            lastFov = fovV; lastT = GetTickCount();
        }
        // flat: no perspective camera at all (loading / start-up screens), or the title screen camera (21.8 deg at 0,-200,-700)
        bool persp = (fabsf(fabsf(P[11]) - 1) < 1e-3f || fabsf(fabsf(P[14]) - 1) < 1e-3f) && fabsf(P[15]) < 1e-3f;
        bool title = fabsf(fovV - 21.8f) < 0.6f && fabsf(V[12]) < 1 && fabsf(V[13] + 200) < 1 && fabsf(V[14] + 700) < 1;
        bool f = !persp || title;
        if (f != g_flatAuto) { Log("xr: %s -> %s", !persp ? "no 3D camera" : title ? "title camera" : "game camera", f ? "flat screen" : "VR"); g_flatAuto = f; }
        if (Flat()) return false;
    }
    InterlockedIncrement(&g_camCalls);
    Pose p; EnterCriticalSection(&g_pcs); p = g_next; LeaveCriticalSection(&g_pcs);
    if (!p.valid) return false;
    bool row = fabsf(fabsf(P[11]) - 1) < 1e-3f && fabsf(P[15]) < 1e-3f, col = fabsf(fabsf(P[14]) - 1) < 1e-3f && fabsf(P[15]) < 1e-3f;
    if (!row && !col) { LOG_FIRST(3, "xr: camera projection not recognised: %g %g %g %g / %g %g %g %g / %g %g %g %g / %g %g %g %g", P[0], P[1], P[2], P[3], P[4], P[5], P[6], P[7], P[8], P[9], P[10], P[11], P[12], P[13], P[14], P[15]); return false; }
    float s = row ? P[11] : P[14];   // w = s * z_view: -1 = right-handed (looks down -z), +1 = left-handed
    LOG_FIRST(1, "xr: game camera matrices are %s-vector, %s-handed; view row0 %.3f %.3f %.3f %.3f row3 %.1f %.1f %.1f %.1f; proj %.3f %.3f | %.3f %.3f %.3f",
              row ? "row" : "column", s < 0 ? "right" : "left", V[0], V[1], V[2], V[3], V[12], V[13], V[14], V[15], P[0], P[5], P[10], P[11], P[14]);
    // head rotation in the game's view axes (OpenXR: x right, y up, -z forward; left-handed game: flip z)
    Q q = p.rel; if (s > 0) q = { -q.x, -q.y, q.z, q.w };
    float R[3][3] = { { 1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y - q.z * q.w), 2 * (q.x * q.z + q.y * q.w) },
                      { 2 * (q.x * q.y + q.z * q.w), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z - q.x * q.w) },
                      { 2 * (q.x * q.z - q.y * q.w), 2 * (q.y * q.z + q.x * q.w), 1 - 2 * (q.x * q.x + q.y * q.y) } };
    float t[3] = { 0, 0, 0 };
    if (g_headPos) { t[0] = p.relPos[0] * g_scale; t[1] = p.relPos[1] * g_scale; t[2] = p.relPos[2] * g_scale * (s > 0 ? -1 : 1); }
    else if (p.eye >= 0) { t[0] = p.eyeOff[0] * g_scale; t[1] = p.eyeOff[1] * g_scale; t[2] = p.eyeOff[2] * g_scale * (s > 0 ? -1 : 1); }   // stereo without head position: just the eye offset
    float M[16] = { 0 };   // row-vector form: M = T(-t) * R
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) M[i * 4 + j] = R[i][j];
    for (int j = 0; j < 3; j++) M[12 + j] = -(t[0] * R[0][j] + t[1] * R[1][j] + t[2] * R[2][j]);
    M[15] = 1;
    float out[16];
    if (row) { for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float a = 0; for (int k = 0; k < 4; k++) a += V[i * 4 + k] * M[k * 4 + j]; out[i * 4 + j] = a; } }   // V' = V * M
    else { for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float a = 0; for (int k = 0; k < 4; k++) a += M[k * 4 + i] * V[k * 4 + j]; out[i * 4 + j] = a; } }    // V' = M^T * V
    memcpy(V, out, sizeof out);
    {   // the game's own frustum (for the UI panel) and its projection rows (for FixUiProjRow)
        float xs0 = row ? P[0] : P[0], ys0 = P[5], p20 = row ? P[8] : P[2], p21 = row ? P[9] : P[6];
        if (xs0 > 1e-4f && ys0 > 1e-4f) { p.gt[0] = (-1 - p20 / s) / xs0; p.gt[1] = (1 - p20 / s) / xs0; p.gt[2] = (-1 - p21 / s) / ys0; p.gt[3] = (1 - p21 / s) / ys0; p.gtValid = true; }
        // build 078: the HUD panel keeps the normal gameplay view's frustum (about 55 deg vertical). Bow aiming, grabs and
        // cutscenes zoom the game camera; following them made the HUD, dialogue and other UI shrink and slide.
        if (p.gtValid) {
            // build 079: the reference is the widest view seen (the bow zoom is gradual, 55 -> 35 deg, so a range test
            // let 55..45 through); a narrower view becomes the reference only after holding steady for 3 s at >= 40 deg
            static float uiGt[4], refFv, steadyFv; static bool uiGtOk; volatile bool& held = g_viewZoomed; static DWORD steadySince;
            const float fv = (atanf(p.gt[3]) - atanf(p.gt[2])) * 57.2958f;
            const DWORD nowT = GetTickCount();
            if (fabsf(fv - steadyFv) > 0.3f) { steadyFv = fv; steadySince = nowT; }
            const bool steadyNew = fv >= 40.0f && fv < 120.0f && nowT - steadySince > 3000;
            if (fv < 120.0f && (!uiGtOk || fv >= refFv - 0.5f || steadyNew)) {
                if (uiGtOk && fv < refFv - 0.5f) Log("xr: UI panel reference view now %.1f deg (steady)", fv);
                refFv = fv; memcpy(uiGt, p.gt, sizeof uiGt); uiGtOk = true; if (held) { held = false; Log("xr: UI panel follows the game view again (%.1f deg)", fv); } }
            else if (uiGtOk) { memcpy(p.gt, uiGt, sizeof uiGt); if (!held) { held = true; Log("xr: game view zoomed to %.1f deg - UI panel kept at the normal size and place", fv); } }
        }
        memcpy(g_origP, P, 64);
    }
    // projection from the headset's (asymmetric) field of view, keeping the game's depth terms
    float L = tanf(p.fov.angleLeft), Rt = tanf(p.fov.angleRight), U = tanf(p.fov.angleUp), D = tanf(p.fov.angleDown);
    float xs = 2 / (Rt - L), ys = 2 / (U - D), ox = (Rt + L) / (Rt - L), oy = (U + D) / (U - D);
    if (row) { P[0] = xs; P[1] = 0; P[4] = 0; P[5] = ys; P[8] = -ox * s; P[9] = -oy * s; P[12] = P[13] = 0; }
    else { P[0] = xs; P[4] = 0; P[1] = 0; P[5] = ys; P[2] = -ox * s; P[6] = -oy * s; P[3] = P[7] = 0; }
    float Pu[16]; memcpy(Pu, P, 64); float jx = 0, jy = 0;
    if (p.sync && row && g_w && g_h) {   // build 075 DLSS: sub-pixel jitter; the pixel (x, y) shows the scene at (x + jx, y + jy)
        ddlss::NextJitter(&jx, &jy);
        P[8] += s * (-2.0f * jx / (float)g_w); P[9] += s * (2.0f * jy / (float)g_h);
    }
    memcpy(g_modP, P, 64); g_projValid = true;
    if (p.eye >= 0) { memcpy(g_eyeP[p.eye], P, 64); g_eyePValid[p.eye] = true; }
    if (p.sync && row) SyncRecord(p, V, P, s, Pu, jx, jy, M, g_origP[0], g_origP[5]);
    LOG_FIRST(1, "xr: game frustum tangents L %.3f R %.3f D %.3f U %.3f (%.0f x %.0f deg)", p.gt[0], p.gt[1], p.gt[2], p.gt[3],
              (atanf(p.gt[1]) - atanf(p.gt[0])) * 57.2958f, (atanf(p.gt[3]) - atanf(p.gt[2])) * 57.2958f);
    EnterCriticalSection(&g_pcs); g_used[g_nUsed & 3] = p; g_nUsed++; LeaveCriticalSection(&g_pcs);
    InterlockedIncrement(&g_camApplied);
    return true;
}

IDirect3DSurface9* UiSurface() { return (g_enabled && !g_dead && g_running && g_track && g_uiOn && g_uiS9 && !Flat()) ? g_uiS9 : nullptr; }
void SetUiFrame(bool used) { g_uiFrame = used; }
static bool RowEq(const float* a, const float* b) { return fabsf(a[0] - b[0]) < 1e-5f && fabsf(a[1] - b[1]) < 1e-5f && fabsf(a[2] - b[2]) < 1e-5f && fabsf(a[3] - b[3]) < 1e-5f; }
void NoteVsConstants(const float* f, unsigned n) {
    if (!g_stereo || !g_eyePValid[0] || !g_eyePValid[1] || !f) return;
    for (int e = 0; e < 2; e++) {
        const float* P = g_eyeP[e]; float r2[4] = { P[8], P[9], P[10], P[11] }, c0[4] = { P[0], P[4], P[8], P[12] };
        if (fabsf(r2[0] - g_eyeP[1 - e][8]) < 1e-4f) continue;   // eyes not distinguishable by this row
        for (unsigned i = 0; i < n; i++) if (RowEq(f + i * 4, r2) || RowEq(f + i * 4, c0)) { g_seenEye = e; return; }
    }
}
bool FixUiProjRow(float* r) {
    if (g_stereo) {   // stereo: the UI may run with either eye's projection (render thread is a frame behind)
        for (int e = 0; e < 2; e++) if (g_eyePValid[e]) for (int i = 0; i < 4; i++) if (RowEq(r, g_eyeP[e] + i * 4)) {
            if (!memcmp(g_eyeP[e] + i * 4, g_origP + i * 4, 16)) return false;
            memcpy(r, g_origP + i * 4, 16); return true;
        }
    }
    if (!g_projValid) return false;
    for (int i = 0; i < 4; i++) {
        const float* m = g_modP + i * 4;
        if (fabsf(r[0] - m[0]) < 1e-5f && fabsf(r[1] - m[1]) < 1e-5f && fabsf(r[2] - m[2]) < 1e-5f && fabsf(r[3] - m[3]) < 1e-5f) {
            if (!memcmp(m, g_origP + i * 4, 16)) return false;
            memcpy(r, g_origP + i * 4, 16); return true;
        }
    }
    return false;
}
// ---------------------------------------------------------------- build 077: shader cache notice + keep-alive frames
static XrCompositionLayerQuad g_nq{ XR_TYPE_COMPOSITION_LAYER_QUAD };
static const UINT NW = 1024, NH = 128;
// build 101: a text picture (rounded dark panel, premultiplied alpha) for the head-locked notice quad; `layout` = the
// longest text of a set, so animated variants stay at the same place
static bool TextPicture(const wchar_t* text, const wchar_t* layout, ID3D11Texture2D** out) {
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = NW; bi.bmiHeader.biHeight = -(LONG)NH; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr; HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HFONT f = CreateFontW(-(int)(NH * 0.48f), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    bool ok = dc && bm && bits && f;
    if (ok) {
        HGDIOBJ ob = SelectObject(dc, bm), of = SelectObject(dc, f);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(240, 240, 240));
        SIZE sz{}; GetTextExtentPoint32W(dc, layout, (int)wcslen(layout), &sz);
        const int x0 = ((int)NW - sz.cx) / 2, y0 = ((int)NH - sz.cy) / 2;
        const float R = NH * 0.3f;   // rounded panel
        uint32_t* px = (uint32_t*)bits;
        RECT rc = { 0, 0, (LONG)NW, (LONG)NH };
        HBRUSH br = CreateSolidBrush(RGB(22, 24, 30)); FillRect(dc, &rc, br); DeleteObject(br);
        TextOutW(dc, x0, y0, text, (int)wcslen(text));
        GdiFlush();
        for (UINT y = 0; y < NH; y++) for (UINT x = 0; x < NW; x++) {   // alpha: rounded corners, 88% panel; premultiplied
            float dx = fmaxf(0.0f, fmaxf(R - (x + 0.5f), (x + 0.5f) - (NW - R))), dy = fmaxf(0.0f, fmaxf(R - (y + 0.5f), (y + 0.5f) - (NH - R)));
            float cov = fminf(1.0f, fmaxf(0.0f, R - sqrtf(dx * dx + dy * dy) + 0.5f));
            uint32_t& c = px[y * NW + x]; float a = 0.88f * cov;
            uint32_t b = (uint32_t)((c & 0xff) * a), g = (uint32_t)(((c >> 8) & 0xff) * a), r = (uint32_t)(((c >> 16) & 0xff) * a);
            c = b | g << 8 | r << 16 | (uint32_t)(a * 255.0f + 0.5f) << 24;
        }
        D3D11_TEXTURE2D_DESC td{}; td.Width = NW; td.Height = NH; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{ bits, NW * 4, 0 };
        ok = SUCCEEDED(g_d11->CreateTexture2D(&td, &sd, out));
        SelectObject(dc, of); SelectObject(dc, ob);
    }
    if (f) DeleteObject(f); if (bm) DeleteObject(bm); if (dc) DeleteDC(dc);
    return ok;
}
// build 101: short messages (e.g. the draw distance test keys), shown for 2.5 s instead of the shader cache notice
static CRITICAL_SECTION g_toastCs; static bool g_toastCsInit; static wchar_t g_toastText[64]; static volatile long g_toastSeq; static long g_toastBuilt; static double g_toastUntil;
static bool NoticeInit() {
    if (g_scN) return true;
    if (g_nBad || !g_scFmt) return false;
    g_nBad = true;
    XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = g_scFmt; ci.sampleCount = 1; ci.width = NW; ci.height = NH; ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (!Ok(p_xrCreateSwapchain(g_sess, &ci, &g_scN), "xrCreateSwapchain (notice)")) { g_scN = XR_NULL_HANDLE; return false; }
    uint32_t n = 0; p_xrEnumerateSwapchainImages(g_scN, 0, &n, nullptr);
    g_imgN.assign(n, XrSwapchainImageD3D11KHR{ XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
    p_xrEnumerateSwapchainImages(g_scN, n, &n, (XrSwapchainImageBaseHeader*)g_imgN.data());
    // the text, drawn once with GDI into four pictures (0-3 animated dots)
    bool ok = true;
    for (int v = 0; v < 4 && ok; v++) {
        wchar_t t[32]; wcscpy(t, L"Building shader cache"); for (int k = 0; k < v; k++) wcscat(t, L".");
        ok = TextPicture(t, L"Building shader cache...", &g_nTex[v]);
    }
    if (!ok) { Log("xr: shader cache notice: text pictures failed"); return false; }
    g_nBad = false;
    Log("xr: shader cache notice ready (%ux%u)", NW, NH);
    return true;
}
// the notice layer for this frame (null when hidden); head-locked, a little below the centre of view
static const XrCompositionLayerBaseHeader* NoticeLayer() {
    const bool toast = g_toastSeq && Ms() < g_toastUntil;
    if ((!toast && !ntc::Showing()) || !g_viewSp) { g_nShown = -1; return nullptr; }
    if (!NoticeInit()) return nullptr;
    int v = (int)(Ms() / 450.0) % 4;
    if (toast) {
        if (g_toastBuilt != g_toastSeq) {
            wchar_t t[64]; EnterCriticalSection(&g_toastCs); wcscpy(t, g_toastText); long seq = g_toastSeq; LeaveCriticalSection(&g_toastCs);
            Rel(g_nTex[4]); if (!TextPicture(t, t, &g_nTex[4])) return nullptr;
            g_toastBuilt = seq; g_nShown = -1;
        }
        v = 4 + (int)(g_toastBuilt & 0xffff) * 8;   // a new message forces a copy
    }
    if (v != g_nShown) {
        XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO }; XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = 100000000;
        XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO }; uint32_t i = 0;
        if (!Ok(p_xrAcquireSwapchainImage(g_scN, &ai, &i), "acquire notice") || !Ok(p_xrWaitSwapchainImage(g_scN, &wi), "wait notice")) return nullptr;
        g_c11->CopyResource(g_imgN[i].texture, g_nTex[v >= 4 ? 4 : v]); p_xrReleaseSwapchainImage(g_scN, &ri);
        g_nShown = v;
    }
    g_nq = XrCompositionLayerQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    g_nq.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; g_nq.space = g_viewSp; g_nq.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    g_nq.pose.orientation.w = 1; g_nq.pose.position = { 0.0f, -0.30f, -1.5f };
    g_nq.subImage.swapchain = g_scN; g_nq.subImage.imageRect = { { 0, 0 }, { (int32_t)NW, (int32_t)NH } };
    g_nq.size = { 0.6f, 0.6f * NH / NW };
    return (const XrCompositionLayerBaseHeader*)&g_nq;
}
static void SaveLayers(const XrFrameEndInfo& ei) {
    if (!ei.layerCount) return;
    uint32_t n = 0; int qi = 0;
    for (uint32_t k = 0; k < ei.layerCount && n < 2; k++) {
        const XrCompositionLayerBaseHeader* h = ei.layers[k];
        if (h == (const XrCompositionLayerBaseHeader*)&g_nq) continue;
        if (h->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const auto* p = (const XrCompositionLayerProjection*)h; s_pl = *p; for (uint32_t e = 0; e < 2 && e < p->viewCount; e++) s_pv[e] = p->views[e];
            s_pl.views = s_pv; s_pl.next = nullptr; s_layers[n++] = (const XrCompositionLayerBaseHeader*)&s_pl;
        } else if (h->type == XR_TYPE_COMPOSITION_LAYER_QUAD && qi < 2) {
            s_q[qi] = *(const XrCompositionLayerQuad*)h; s_q[qi].next = nullptr; s_layers[n++] = (const XrCompositionLayerBaseHeader*)&s_q[qi]; qi++;
        }
    }
    s_n = n;
}
// keep-alive thread: while the game does not present (loading, DXVK compiling), the headset keeps the last pictures
// (world-locked, reprojected by the runtime) and gets the notice
static bool KeepAliveFrame() {
    bool did = false;
    EnterCriticalSection(&g_fcs);
    if (g_keepAlive && g_sess && g_running && !g_dead && g_frameOpen && g_lastPresentMs > 0 && Ms() - g_lastPresentMs > 300) {
        const XrCompositionLayerBaseHeader* layers[3]; uint32_t n = 0;
        if (g_fs.shouldRender) {
            for (uint32_t k = 0; k < s_n; k++) layers[n++] = s_layers[k];
            if (const XrCompositionLayerBaseHeader* nl = NoticeLayer()) layers[n++] = nl;
        }
        XrFrameEndInfo ei{ XR_TYPE_FRAME_END_INFO }; ei.displayTime = g_fs.predictedDisplayTime; ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        ei.layerCount = n; ei.layers = layers;
        Ok(p_xrEndFrame(g_sess, &ei), "xrEndFrame (keep-alive)");
        g_frameOpen = false;
        Poll();
        XrFrameWaitInfo wi{ XR_TYPE_FRAME_WAIT_INFO }; g_fs = XrFrameState{ XR_TYPE_FRAME_STATE };
        if (g_running && Ok(p_xrWaitFrame(g_sess, &wi, &g_fs), "xrWaitFrame (keep-alive)")) {
            XrFrameBeginInfo bi{ XR_TYPE_FRAME_BEGIN_INFO };
            if (Ok(p_xrBeginFrame(g_sess, &bi), "xrBeginFrame (keep-alive)")) { g_frameOpen = true; PublishPose(g_fs.predictedDisplayTime); }
        }
        static long kaN; if ((kaN++ % 450) == 0) Log("xr: keep-alive frames while the game is not presenting (%ld so far)", kaN);
        did = true;
    }
    LeaveCriticalSection(&g_fcs);
    return did;
}
static DWORD WINAPI KeepAliveThread(LPVOID) {
    for (;;) { ntc::Poll(); if (!KeepAliveFrame()) Sleep(20); }
}
static void OnPresentLocked(IDirect3DDevice9* dev);
void OnPresent(IDirect3DDevice9* dev) {
    if (!g_enabled || g_dead) return;
    if (InterlockedCompareExchange(&g_fcsInit, 1, 0) == 0) {
        InitializeCriticalSection(&g_fcs);
        CreateThread(nullptr, 0, KeepAliveThread, nullptr, 0, nullptr);
        Log("xr: keep-alive thread started (XrKeepAlive=%d)", g_keepAlive);
    }
    EnterCriticalSection(&g_fcs);
    OnPresentLocked(dev);
    g_lastPresentMs = Ms();
    LeaveCriticalSection(&g_fcs);
}
static void OnPresentLocked(IDirect3DDevice9* dev) {
    if (!g_pcsInit) { InitializeCriticalSection(&g_pcs); g_pcsInit = true; }
    double now = Ms();
    if (!g_sess) {
        if (now < g_nextTry) return;
        g_nextTry = now + 5000;
        if (!InitInstance()) { Teardown(); return; }
    }
    Poll();
    if (!g_running) { g_frameOpen = false; return; }
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    D3DSURFACE_DESC bd; bb->GetDesc(&bd);
    if (!g_sc && !InitTargets(dev, bd.Width, bd.Height)) { bb->Release(); Teardown(); g_dead = true; return; }
    // keys: F6 tracking <-> screen, F7 recentre (screen and tracking)
    {   static bool f6p, f7p; static double runSince; static bool autoDone;
        bool f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0, f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        if (f6 && !f6p) { g_track = !g_track; Log("xr: F6 -> %s", g_track ? "head tracking" : "flat screen"); }
        static bool f4p; bool f4 = g_devKeys && (GetAsyncKeyState(VK_F4) & 0x8000) != 0 && !(GetAsyncKeyState(VK_MENU) & 0x8000);
        if (f4 && !f4p && g_sc1) { g_stereo = (g_stereo + 1) % 3; g_lastEye[0].valid = g_lastEye[1].valid = false;
            Log("xr: F4 -> %s", g_stereo == 2 ? "stereo, both eyes every frame" : g_stereo == 1 ? "stereo, alternate eyes" : "mono"); }
        f4p = f4;
        static bool f3p; bool f3 = g_devKeys && (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
        if (f3 && !f3p) { static const char* nm[] = { "all camera forms (VS+PS)", "no camera position in pixel shaders", "no camera position anywhere", "VS matrices only" };
            g_subMode = (g_subMode + 1) % 4; Log("xr: F3 right-eye substitution test -> %d = %s", g_subMode, nm[g_subMode]); }
        f3p = f3;
        static bool f5p; bool f5 = g_devKeys && (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        if (f5 && !f5p) { g_flatForce = (g_flatForce + 1) % 3; Log("xr: F5 -> %s", g_flatForce == 0 ? "automatic" : g_flatForce == 1 ? "forced flat screen" : "forced VR"); }
        f5p = f5;
        static bool insp; bool ins = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;   // build 075: DLSS on / off
        if (ins && !insp) ddlss::Toggle();
        insp = ins;
        if (!runSince) runSince = now;
        bool again = !autoDone && now - runSince > 3000; if (again) autoDone = true;
        if ((f7 && !f7p) || again) { if (f7 && !f7p) Log("xr: F7 recentre"); g_placed = false; g_wantRef = true; }
        f6p = f6; f7p = f7; }
    double t0 = Ms(), t1 = t0, t2 = t0, t3 = t0;
    if (g_frameOpen) {
        bool copied = bd.Width == g_w && bd.Height == g_h && SUCCEEDED(dev->StretchRect(bb, nullptr, g_s9, nullptr, D3DTEXF_NONE));
        if (copied && g_q9) {
            g_q9->Issue(D3DISSUE_END);
            if (g_syncWait) while (g_q9->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) { if (Ms() - t0 > 50) break; YieldProcessor(); }
        } else if (copied && vkb::Active()) {   // build 070: DXVK - copy the pictures across on DXVK's Vulkan queue
            vkb::CopyAll(nullptr); vkb::D3D11AcquireAll();
        }
        t1 = Ms();
        if (!g_placed) Place(g_fs.predictedDisplayTime);
        Pose used{}; long nu;
        EnterCriticalSection(&g_pcs); nu = g_nUsed; if (nu > g_lag) used = g_used[(nu - 1 - g_lag) & 3];
        int fe = g_seenEye; g_seenEye = -1;
        SyncCam cam{}; if (g_stereo == 2 && g_replayCam >= 0 && g_cams[g_replayCam].valid) { cam = g_cams[g_replayCam]; used = cam.pose; }   // build 042: the replayed camera
        bool leftDone = g_leftDone; g_leftDone = false;
        if (g_stereo && used.eye >= 0) {
            if (fe >= 0) {   // the presented frame's eye is known: its pose is the latest camera with that eye
                g_eyeHit++;
                for (long k = 0; k < 4 && nu - 1 - k >= 0; k++) { const Pose& c = g_used[(nu - 1 - k) & 3]; if (c.eye == fe) { used = c; break; } }
            } else if (nu >= 2) { g_eyeMiss++; used = g_used[(nu - 2) & 3]; }   // not seen: assume one frame behind
            if (((g_eyeHit + g_eyeMiss) % 900) == 1) Log("xr: stereo eye detection %ld seen / %ld assumed", g_eyeHit, g_eyeMiss);
        }
        if (g_stereo == 2) {
            static long sf; if (sf++ % 900 == 0) Log("xr: synchronous stereo - camera matched %ld, uploads changed %ld (VP %ld, VP^-1 %ld, V %ld, V^-1 %ld, V^-1(3x4) %ld, cam pos %ld, PS %ld), left pictures %s",
                g_camMatch, g_subs, g_subN[0], g_subN[1], g_subN[2], g_subN[3], g_subN[4], g_subN[5], g_subN[6], leftDone ? "yes" : "no");
            if (sf % 900 == 1) Log("xr: cam pos matches by error: <0.02 %ld, <0.5 %ld, <16 %ld, near-miss 16..32 %ld (largest matched %.3f, tolerance %.2f)", g_camErr[0], g_camErr[1], g_camErr[2], g_camErr[3], g_camErrMax, g_camTol);
        }
        LeaveCriticalSection(&g_pcs);
        static long lastNu; bool fresh = nu != lastNu; lastNu = nu;
        XrCompositionLayerQuad q{ XR_TYPE_COMPOSITION_LAYER_QUAD };
        XrCompositionLayerProjection pl{ XR_TYPE_COMPOSITION_LAYER_PROJECTION }; XrCompositionLayerProjectionView pv[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
        const XrCompositionLayerBaseHeader* layers[3];
        XrCompositionLayerQuad uq{ XR_TYPE_COMPOSITION_LAYER_QUAD };
        XrFrameEndInfo ei{ XR_TYPE_FRAME_END_INFO }; ei.displayTime = g_fs.predictedDisplayTime; ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        if (g_fs.shouldRender && copied) {
            XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            XrSwapchainImageWaitInfo swi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; swi.timeout = 100000000;
            XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            bool tracked = g_track && used.valid && fresh;
            bool syncFrame = tracked && used.sync && leftDone && g_t11L && g_sc1;
            bool stereoFrame = tracked && used.eye >= 0 && g_sc1;
            XrSwapchain dst = (stereoFrame && used.eye == 1) ? g_sc1 : g_sc;          // this frame's eye
            std::vector<XrSwapchainImageD3D11KHR>& imgs = (dst == g_sc1) ? g_img1 : g_img;
            // build 075: DLSS per eye on the synchronous stereo frames
            ID3D11ShaderResourceView* dl[2] = {};
            {   static double prev[2][16]; static bool prevOk; static float prevPos[3];
                if (syncFrame && cam.valid && ddlss::Wanted() && g_dt11[0] && g_dt11[1]) {
                    float dp = fabsf(cam.camL[0] - prevPos[0]) + fabsf(cam.camL[1] - prevPos[1]) + fabsf(cam.camL[2] - prevPos[2]);
                    bool hist = prevOk && dp < 300.0f;   // a camera jump of 3+ m (cut, teleport): no history
                    const UINT ow = g_ow ? g_ow : g_w, oh = g_oh ? g_oh : g_h;
                    for (int e = 0; e < 2; e++)
                        dl[e] = ddlss::Evaluate(g_d11, g_c11, e, e ? g_t11 : g_t11L, g_dt11[e], cam.VPu[e], hist ? prev[e] : nullptr, cam.jx, cam.jy, cam.depthInv, ow, oh);
                    memcpy(prev, cam.VPu, sizeof prev); memcpy(prevPos, cam.camL, 12); prevOk = true;
                } else prevOk = false;
            }
            auto put = [&](ID3D11Texture2D* to, ID3D11Texture2D* from, ID3D11ShaderResourceView* d) {
                if (d && ddlss::Blit(g_d11, g_c11, d, to)) return;
                if (!g_ow) { g_c11->CopyResource(to, from); return; }
                ID3D11ShaderResourceView*& v = from == g_t11L ? g_srvLeft : g_srvMain;   // smaller game picture: scaled copy
                if (!v) g_d11->CreateShaderResourceView(from, nullptr, &v);
                if (!ddlss::Blit(g_d11, g_c11, v, to)) { D3D11_BOX b = { 0, 0, 0, g_w, g_h, 1 }; g_c11->CopySubresourceRegion(to, 0, 0, 0, 0, from, 0, &b); }
            };
            uint32_t idx = 0;
            if (Ok(p_xrAcquireSwapchainImage(dst, &ai, &idx), "acquire") && Ok(p_xrWaitSwapchainImage(dst, &swi), "wait image")) {
                put(imgs[idx].texture, syncFrame ? g_t11L : g_t11, syncFrame ? dl[0] : nullptr);
                p_xrReleaseSwapchainImage(dst, &ri);
                if (syncFrame) {   // the right eye: this Present's back buffer (replay 1)
                    uint32_t i1 = 0;
                    if (Ok(p_xrAcquireSwapchainImage(g_sc1, &ai, &i1), "acquire R") && Ok(p_xrWaitSwapchainImage(g_sc1, &swi), "wait R")) {
                        put(g_img1[i1].texture, g_t11, dl[1]); p_xrReleaseSwapchainImage(g_sc1, &ri);
                    }
                }
                XrSwapchainSubImage si{}; si.swapchain = g_sc; si.imageRect = { { 0, 0 }, { (int32_t)(g_ow ? g_ow : g_w), (int32_t)(g_oh ? g_oh : g_h) } };
                const XrRect2Di uiRect = { { 0, 0 }, { (int32_t)g_w, (int32_t)g_h } };
                if (stereoFrame) g_lastEye[used.eye] = used;
                if (!stereoFrame) g_lastEye[0].valid = g_lastEye[1].valid = false;   // mono / flat: start the pair again
                if (tracked) {
                    bool pair = stereoFrame && g_lastEye[0].valid && g_lastEye[1].valid;
                    for (int e = 0; e < 2; e++) {
                        const Pose& u = pair ? g_lastEye[e] : used;
                        pv[e].pose.orientation = { u.abs.x, u.abs.y, u.abs.z, u.abs.w };
                        pv[e].pose.position = { u.pos[0], u.pos[1], u.pos[2] };
                        pv[e].fov = u.fov; pv[e].subImage = si;
                        pv[e].subImage.swapchain = pair ? (e ? g_sc1 : g_sc) : dst;   // before the pair is complete: this frame's image in both eyes
                    }
                    if (syncFrame) {   // both eyes rendered from this camera: right eye = left orientation at the right eye's position
                        pv[1].pose.position = { used.posR[0], used.posR[1], used.posR[2] }; pv[1].subImage.swapchain = g_sc1;
                        LOG_FIRST(1, "xr: synchronous stereo frame submitted");
                    }
                    pl.space = g_local; pl.viewCount = 2; pl.views = pv; layers[0] = (XrCompositionLayerBaseHeader*)&pl;
                    if (g_uiFrame && g_scUi && g_ui11 && used.gtValid) {   // UI panel: exactly where the game's own camera view is
                        uint32_t ui = 0;
                        if (Ok(p_xrAcquireSwapchainImage(g_scUi, &ai, &ui), "acquire UI") && Ok(p_xrWaitSwapchainImage(g_scUi, &swi), "wait UI")) {
                            g_c11->CopyResource(g_imgUi[ui].texture, g_ui11);
                            p_xrReleaseSwapchainImage(g_scUi, &ri);
                            const float D = g_uiDist, *t = used.gt;
                            float hh = D * (t[3] - t[2]), ww = hh * (float)g_w / (float)g_h;   // keep the UI image's own shape
                            float c[3] = { D * (t[0] + t[1]) * 0.5f, D * (t[2] + t[3]) * 0.5f, -D }, w[3]; Rot(g_refYaw, c, w);
                            uq.space = g_local; uq.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                            uq.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                            uq.pose.orientation = { g_refYaw.x, g_refYaw.y, g_refYaw.z, g_refYaw.w };
                            uq.pose.position = { g_refPos[0] + w[0], g_refPos[1] + w[1], g_refPos[2] + w[2] };
                            uq.subImage.swapchain = g_scUi; uq.subImage.imageRect = uiRect;
                            uq.size = { ww, hh };
                            layers[1] = (XrCompositionLayerBaseHeader*)&uq; ei.layerCount = 2;
                        }
                    }
                } else {
                    si.swapchain = dst;
                    q.space = g_local; q.eyeVisibility = XR_EYE_VISIBILITY_BOTH; q.pose = g_quad; q.subImage = si;
                    q.size = { g_scrW, g_movie ? g_scrW * 9.0f / 16.0f : g_scrW * g_h / g_w }; layers[0] = (XrCompositionLayerBaseHeader*)&q;
                    uint32_t ui = 0;   // a UI layer drawn this frame goes on top of the screen, same place and size
                    if (g_uiFrame && g_scUi && g_ui11 && Ok(p_xrAcquireSwapchainImage(g_scUi, &ai, &ui), "acquire UI") && Ok(p_xrWaitSwapchainImage(g_scUi, &swi), "wait UI")) {
                        g_c11->CopyResource(g_imgUi[ui].texture, g_ui11); p_xrReleaseSwapchainImage(g_scUi, &ri);
                        uq = XrCompositionLayerQuad{ XR_TYPE_COMPOSITION_LAYER_QUAD }; uq.space = g_local; uq.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                        uq.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; uq.pose = q.pose; uq.size = q.size;
                        uq.subImage.swapchain = g_scUi; uq.subImage.imageRect = uiRect;
                        layers[1] = (XrCompositionLayerBaseHeader*)&uq; ei.layerCount = 2;
                    }
                }
                if (!ei.layerCount) ei.layerCount = 1; ei.layers = layers;
            }
        }
        t2 = Ms();
        SaveLayers(ei);   // build 077: for the keep-alive frames
        if (g_fs.shouldRender) if (const XrCompositionLayerBaseHeader* nl = NoticeLayer()) { layers[ei.layerCount++] = nl; ei.layers = layers; }
        vkb::D3D11ReleaseAll();   // build 071: keyed-mutex mode - pictures back to Vulkan
        Ok(p_xrEndFrame(g_sess, &ei), "xrEndFrame");
        g_frameOpen = false;
    }
    bb->Release();
    XrFrameWaitInfo wi{ XR_TYPE_FRAME_WAIT_INFO }; g_fs = XrFrameState{ XR_TYPE_FRAME_STATE };
    if (!Ok(p_xrWaitFrame(g_sess, &wi, &g_fs), "xrWaitFrame")) return;
    t3 = Ms();
    XrFrameBeginInfo bi{ XR_TYPE_FRAME_BEGIN_INFO };
    if (!Ok(p_xrBeginFrame(g_sess, &bi), "xrBeginFrame")) return;
    g_frameOpen = true;
    PublishPose(g_fs.predictedDisplayTime);
    g_frames++; g_syncMs += t1 - t0; g_copyMs += t2 - t1; g_waitMs += t3 - t2;
    if (!g_winT) g_winT = now;
    if (now - g_winT >= 5000 && g_frames) {
        Log("xr: %ld frames, %.1f fps | per frame: copy+GPU sync %.2f ms, submit %.2f ms, xrWaitFrame %.2f ms | camera setups %.2f per frame (%.2f tracked) | mode %s",
            g_frames, g_frames * 1000.0 / (now - g_winT), g_syncMs / g_frames, g_copyMs / g_frames, g_waitMs / g_frames,
            (double)g_camCalls / g_frames, (double)g_camApplied / g_frames, g_track ? "head tracking" : "screen");
        g_frames = 0; g_syncMs = g_waitMs = g_copyMs = 0; g_winT = now; g_camCalls = g_camApplied = 0;
    }
}

void ShowMessage(const wchar_t* text, double seconds) {   // build 101
    if (!g_toastCsInit) { InitializeCriticalSection(&g_toastCs); g_toastCsInit = true; }
    EnterCriticalSection(&g_toastCs); wcsncpy(g_toastText, text, 63); g_toastText[63] = 0; g_toastSeq++; g_toastUntil = Ms() + seconds * 1000.0; LeaveCriticalSection(&g_toastCs);
}

} // namespace dxr
