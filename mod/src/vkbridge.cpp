// build 070: DXVK output bridge (see vkbridge.h)
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <vulkan/vulkan.h>
#include <vector>
#include "vkbridge.h"
#include "log.h"

// DXVK interop interfaces (src/d3d9/d3d9_interfaces.h in the DXVK sources; vtable order matters)
static const GUID IID_D3D9VkInteropTexture = { 0xd56344f5, 0x8d35, 0x46fd, { 0x80, 0x6d, 0x94, 0xc3, 0x51, 0xb4, 0x72, 0xc1 } };
static const GUID IID_D3D9VkInteropDevice  = { 0x2eaa4b89, 0x0107, 0x4bdb, { 0x87, 0xf7, 0x0f, 0x54, 0x1c, 0x49, 0x3c, 0xe0 } };
struct ID3D9VkInteropTexture : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(VkImage* pHandle, VkImageLayout* pLayout, VkImageCreateInfo* pInfo) = 0;
};
struct ID3D9VkInteropDevice : public IUnknown {
    virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance* pInstance, VkPhysicalDevice* pPhysDev, VkDevice* pDevice) = 0;
    virtual void STDMETHODCALLTYPE GetSubmissionQueue(VkQueue* pQueue, uint32_t* pQueueIndex, uint32_t* pQueueFamilyIndex) = 0;
    virtual void STDMETHODCALLTYPE TransitionTextureLayout(ID3D9VkInteropTexture* pTexture, const VkImageSubresourceRange* pSubresources, VkImageLayout OldLayout, VkImageLayout NewLayout) = 0;
    virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
    virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE LockDevice() = 0;
    virtual void STDMETHODCALLTYPE UnlockDevice() = 0;
};

namespace vkb {
static ID3D9VkInteropDevice* g_io; static bool g_tried, g_ok;
static VkInstance g_inst; static VkPhysicalDevice g_phys; static VkDevice g_dev; static VkQueue g_queue; static uint32_t g_qfam, g_qidx;
static VkCommandPool g_pool; static VkCommandBuffer g_cmd; static VkFence g_fence;
static PFN_vkGetInstanceProcAddr p_GIPA; static PFN_vkGetDeviceProcAddr p_GDPA;
#define VKF(n) static PFN_##n p_##n;
VKF(vkCreateImage) VKF(vkDestroyImage) VKF(vkGetImageMemoryRequirements) VKF(vkAllocateMemory) VKF(vkFreeMemory) VKF(vkBindImageMemory)
VKF(vkGetMemoryWin32HandlePropertiesKHR) VKF(vkCreateCommandPool) VKF(vkAllocateCommandBuffers) VKF(vkBeginCommandBuffer) VKF(vkEndCommandBuffer)
VKF(vkResetCommandBuffer) VKF(vkCmdPipelineBarrier) VKF(vkCmdCopyImage) VKF(vkQueueSubmit) VKF(vkCreateFence) VKF(vkWaitForFences) VKF(vkResetFences)
VKF(vkGetPhysicalDeviceMemoryProperties)
#undef VKF
struct Pair { IDirect3DTexture9* t9; ID3D9VkInteropTexture* it; ID3D11Texture2D* t11; HANDLE nt; VkImage img; VkDeviceMemory mem; UINT w, h; IDXGIKeyedMutex* km; bool d3dHeld; };
static int g_mode = 1, g_skip; static double g_tFlush, g_tSubmit, g_tWait; static long g_nCopy;
void Configure(int mode, int skipCopy) { g_mode = mode == 2 ? 2 : 1; g_skip = skipCopy; Log("vkb: mode %d (%s)%s", g_mode, g_mode == 2 ? "keyed mutex" : "plain shared textures", g_skip ? ", COPY SKIPPED (diagnostic)" : ""); }
static std::vector<Pair> g_pairs;

static double Ms() { static double k; if (!k) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); k = 1000.0 / f.QuadPart; } LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart * k; }

bool Active() { return g_ok; }

bool Detect(IDirect3DDevice9* dev) {
    if (g_tried) return g_ok; g_tried = true;
    if (!dev || FAILED(dev->QueryInterface(IID_D3D9VkInteropDevice, (void**)&g_io)) || !g_io) { g_io = nullptr; return false; }
    Log("vkb: DXVK device detected - using the Vulkan bridge for the headset images");
    g_io->GetVulkanHandles(&g_inst, &g_phys, &g_dev);
    g_io->GetSubmissionQueue(&g_queue, &g_qidx, &g_qfam);
    HMODULE vk = GetModuleHandleW(L"vulkan-1.dll"); if (!vk) vk = LoadLibraryW(L"vulkan-1.dll");
    if (vk) p_GIPA = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");
    if (!p_GIPA || !g_inst || !g_dev || !g_queue) { Log("vkb: no Vulkan entry points / handles (vulkan-1 %p, instance %p, device %p, queue %p)", vk, (void*)g_inst, (void*)g_dev, (void*)g_queue); return false; }
    p_GDPA = (PFN_vkGetDeviceProcAddr)p_GIPA(g_inst, "vkGetDeviceProcAddr");
    p_vkGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)p_GIPA(g_inst, "vkGetPhysicalDeviceMemoryProperties");
    bool all = p_GDPA && p_vkGetPhysicalDeviceMemoryProperties;
#define VKD(n) p_##n = (PFN_##n)p_GDPA(g_dev, #n); if (!p_##n) { Log("vkb: missing %s", #n); all = false; }
    if (p_GDPA) {
        VKD(vkCreateImage) VKD(vkDestroyImage) VKD(vkGetImageMemoryRequirements) VKD(vkAllocateMemory) VKD(vkFreeMemory) VKD(vkBindImageMemory)
        VKD(vkCreateCommandPool) VKD(vkAllocateCommandBuffers) VKD(vkBeginCommandBuffer) VKD(vkEndCommandBuffer) VKD(vkResetCommandBuffer)
        VKD(vkCmdPipelineBarrier) VKD(vkCmdCopyImage) VKD(vkQueueSubmit) VKD(vkCreateFence) VKD(vkWaitForFences) VKD(vkResetFences)
        p_vkGetMemoryWin32HandlePropertiesKHR = (PFN_vkGetMemoryWin32HandlePropertiesKHR)p_GDPA(g_dev, "vkGetMemoryWin32HandlePropertiesKHR");
    }
#undef VKD
    if (!all) return false;
    VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }; pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = g_qfam;
    VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO }; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (p_vkCreateCommandPool(g_dev, &pci, nullptr, &g_pool) != VK_SUCCESS) { Log("vkb: vkCreateCommandPool failed"); return false; }
    cai.commandPool = g_pool;
    if (p_vkAllocateCommandBuffers(g_dev, &cai, &g_cmd) != VK_SUCCESS || p_vkCreateFence(g_dev, &fci, nullptr, &g_fence) != VK_SUCCESS) { Log("vkb: command buffer / fence failed"); return false; }
    Log("vkb: ready (queue family %u index %u, external memory import %s)", g_qfam, g_qidx, p_vkGetMemoryWin32HandlePropertiesKHR ? "available" : "NOT available");
    g_ok = p_vkGetMemoryWin32HandlePropertiesKHR != nullptr;
    return g_ok;
}

bool MakeShared(IDirect3DDevice9* dev, ID3D11Device* d11, UINT w, UINT h, IDirect3DTexture9** t9, ID3D11Texture2D** t11) {
    if (!g_ok) return false;
    Pair p{}; p.w = w; p.h = h;
    // 1. the D3D11 side: a texture shared by NT handle
    D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = (g_mode == 2 ? D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX : D3D11_RESOURCE_MISC_SHARED) | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    HRESULT hr = d11->CreateTexture2D(&td, nullptr, &p.t11);
    if (FAILED(hr)) { Log("vkb: D3D11 shared texture %ux%u failed 0x%08lx", w, h, (unsigned long)hr); return false; }
    IDXGIResource1* r1 = nullptr;
    if (FAILED(p.t11->QueryInterface(__uuidof(IDXGIResource1), (void**)&r1)) || !r1 ||
        FAILED(r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &p.nt)) || !p.nt) {
        Log("vkb: CreateSharedHandle failed"); if (r1) r1->Release(); p.t11->Release(); return false;
    }
    r1->Release();
    // 2. the Vulkan side: import it into DXVK's device
    VkExternalMemoryImageCreateInfo emi{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO }; emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO }; ici.pNext = &emi; ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_B8G8R8A8_UNORM;
    ici.extent = { w, h, 1 }; ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT; ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult vr = p_vkCreateImage(g_dev, &ici, nullptr, &p.img);
    if (vr != VK_SUCCESS) { Log("vkb: vkCreateImage (import) failed %d", (int)vr); CloseHandle(p.nt); p.t11->Release(); return false; }
    VkMemoryRequirements mr; p_vkGetImageMemoryRequirements(g_dev, p.img, &mr);
    VkMemoryWin32HandlePropertiesKHR hp{ VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
    uint32_t bits = mr.memoryTypeBits;
    if (p_vkGetMemoryWin32HandlePropertiesKHR(g_dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, p.nt, &hp) == VK_SUCCESS && (bits & hp.memoryTypeBits)) bits &= hp.memoryTypeBits;
    VkPhysicalDeviceMemoryProperties mp; p_vkGetPhysicalDeviceMemoryProperties(g_phys, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = i; break; }
    if (type == UINT32_MAX) for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if (bits & (1u << i)) { type = i; break; }
    VkMemoryDedicatedAllocateInfo ded{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO }; ded.image = p.img;
    VkImportMemoryWin32HandleInfoKHR imp{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR }; imp.pNext = &ded;
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT; imp.handle = p.nt;
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO }; mai.pNext = &imp; mai.allocationSize = mr.size; mai.memoryTypeIndex = type;
    vr = type == UINT32_MAX ? VK_ERROR_FEATURE_NOT_PRESENT : p_vkAllocateMemory(g_dev, &mai, nullptr, &p.mem);
    if (vr == VK_SUCCESS) vr = p_vkBindImageMemory(g_dev, p.img, p.mem, 0);
    if (vr != VK_SUCCESS) { Log("vkb: importing the D3D11 texture into Vulkan failed %d (memory type %u, bits 0x%x)", (int)vr, type, bits); p_vkDestroyImage(g_dev, p.img, nullptr); CloseHandle(p.nt); p.t11->Release(); return false; }
    // 3. the D3D9 side: an ordinary render-target texture (DXVK image) that the game's picture is StretchRect'ed into
    hr = dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &p.t9, nullptr);
    if (FAILED(hr) || FAILED(p.t9->QueryInterface(IID_D3D9VkInteropTexture, (void**)&p.it)) || !p.it) {
        Log("vkb: D3D9 texture / interop failed 0x%08lx", (unsigned long)hr); if (p.t9) p.t9->Release(); p_vkFreeMemory(g_dev, p.mem, nullptr); p_vkDestroyImage(g_dev, p.img, nullptr); CloseHandle(p.nt); p.t11->Release(); return false;
    }
    if (g_mode == 2 && FAILED(p.t11->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&p.km))) { Log("vkb: no keyed mutex on the D3D11 texture"); p.km = nullptr; }
    *t9 = p.t9; p.t9->AddRef(); *t11 = p.t11; p.t11->AddRef();
    g_pairs.push_back(p);
    Log("vkb: shared pair %u ready (%ux%u)", (unsigned)g_pairs.size(), w, h);
    return true;
}

bool CopyAll(double* waitMs) {
    if (!g_ok || g_pairs.empty()) return false;
    double t0 = Ms();
    if (g_skip) return true;
    g_io->LockDevice();
    g_io->FlushRenderingCommands();
    double tf = Ms();
    p_vkResetCommandBuffer(g_cmd, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    p_vkBeginCommandBuffer(g_cmd, &bi);
    for (Pair& p : g_pairs) {
        VkImage src = VK_NULL_HANDLE; VkImageLayout lay = VK_IMAGE_LAYOUT_GENERAL;
        if (FAILED(p.it->GetVulkanImageInfo(&src, &lay, nullptr)) || !src) continue;
        VkImageMemoryBarrier b[2] = { { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER }, { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER } };
        b[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b[0].oldLayout = lay; b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b[0].srcQueueFamilyIndex = b[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b[0].image = src; b[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b[1].srcAccessMask = 0; b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b[1].srcQueueFamilyIndex = b[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b[1].image = p.img; b[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        p_vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
        VkImageCopy rc{}; rc.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }; rc.dstSubresource = rc.srcSubresource; rc.extent = { p.w, p.h, 1 };
        p_vkCmdCopyImage(g_cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rc);
        // back to what DXVK expects; the D3D11 copy reads the imported image after the fence
        b[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT; b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b[0].newLayout = lay;
        b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT; b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        p_vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
    }
    p_vkEndCommandBuffer(g_cmd);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &g_cmd;
    VkDeviceMemory mems[8]; uint64_t k0[8], k1[8]; uint32_t to[8]; uint32_t nk = 0;
    VkWin32KeyedMutexAcquireReleaseInfoKHR km{ VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR };
    if (g_mode == 2) {
        for (Pair& p : g_pairs) if (p.km && nk < 8) { mems[nk] = p.mem; k0[nk] = 0; k1[nk] = 1; to[nk] = 100; nk++; }
        km.acquireCount = nk; km.pAcquireSyncs = mems; km.pAcquireKeys = k0; km.pAcquireTimeouts = to;
        km.releaseCount = nk; km.pReleaseSyncs = mems; km.pReleaseKeys = k1;
        if (nk) si.pNext = &km;
    }
    double ts0 = Ms();
    g_io->LockSubmissionQueue();
    VkResult vr = p_vkQueueSubmit(g_queue, 1, &si, g_fence);
    g_io->ReleaseSubmissionQueue();
    g_io->UnlockDevice();
    if (vr != VK_SUCCESS) { static int n; if (n++ < 5) Log("vkb: vkQueueSubmit failed %d", (int)vr); return false; }
    double ts1 = Ms();
    VkResult wr = p_vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, 50ull * 1000 * 1000);
    p_vkResetFences(g_dev, 1, &g_fence);
    double tw = Ms();
    if (waitMs) *waitMs = tw - t0;
    g_tFlush += tf - t0; g_tSubmit += ts1 - ts0; g_tWait += tw - ts1;
    if (++g_nCopy >= 450) { Log("vkb: per frame - flush %.2f ms, submit %.2f ms, GPU copy wait %.2f ms", g_tFlush / g_nCopy, g_tSubmit / g_nCopy, g_tWait / g_nCopy); g_tFlush = g_tSubmit = g_tWait = 0; g_nCopy = 0; }
    if (wr != VK_SUCCESS) { static int n; if (n++ < 5) Log("vkb: fence wait %d", (int)wr); }
    return wr == VK_SUCCESS;
}

void D3D11AcquireAll() {
    if (g_mode != 2 || g_skip) return;
    for (Pair& p : g_pairs) if (p.km && !p.d3dHeld) { HRESULT hr = p.km->AcquireSync(1, 100); p.d3dHeld = hr == S_OK; if (hr != S_OK) { static int n; if (n++ < 5) Log("vkb: D3D11 AcquireSync(1) -> 0x%08lx", (unsigned long)hr); } }
}
void D3D11ReleaseAll() {
    for (Pair& p : g_pairs) if (p.km && p.d3dHeld) { p.km->ReleaseSync(0); p.d3dHeld = false; }
}
void Reset() {
    D3D11ReleaseAll();
    if (!g_ok) return;
    if (g_dev) for (Pair& p : g_pairs) { p.it->Release(); p.t9->Release(); if (p.km) p.km->Release(); p_vkDestroyImage(g_dev, p.img, nullptr); p_vkFreeMemory(g_dev, p.mem, nullptr); CloseHandle(p.nt); p.t11->Release(); }
    g_pairs.clear();
}
}
