#include "dl2_xr.h"

#include "xr_types.h"

#include <d3d11.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// DL2 -> headset. A MINIMAL OpenXR submission module using a call order that has been proven on
// several D3D11 titles, with hand-written ABI types (xr_types.h). The loader is loaded at runtime
// (openxr_loader.dll) — no OpenXR SDK dependency, no import lib. Self-contained: own logger
// (dl2_xr.log next to the module), own state. No DIBR, menu quad, convergence or FOV-fill here;
// camera injection lives in dl2_probe.cpp.
//
// Frame contract the render side follows:
//   if (DL2Xr::BeginFrame()) {                 // xrWaitFrame + xrBeginFrame (+ xrLocateViews)
//       DL2Xr::GetEyeView(0/1, &pose, &fov);   // render each eye at its located pose into own texture
//       DL2Xr::SubmitEyes(leftTex, rightTex);  // copy into eye swapchains + xrEndFrame
//   }
// If BeginFrame() returns false it has already paired begin<->end internally (0 layers), so the caller
// simply skips render + submit for that frame.

namespace
{
using namespace DL2VR::Xr;

// ---- self-contained logging (dl2_xr.log next to the module, plus OutputDebugString) -------
HMODULE g_selfModule = nullptr;

void EnsureSelfModule() noexcept
{
    if (g_selfModule != nullptr) return;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&EnsureSelfModule), &g_selfModule);
}

void XrLog(const char* format, ...) noexcept
{
    char message[1024] = {};
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    SYSTEMTIME t = {};
    GetLocalTime(&t);
    char line[1200] = {};
    snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u | %s\r\n",
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, message);

    OutputDebugStringA(line);

    // Resolve "dl2_xr.log" next to this module (= the game directory) once.
    static wchar_t s_path[MAX_PATH] = {};
    static bool s_pathReady = false;
    if (!s_pathReady)
    {
        EnsureSelfModule();
        wchar_t mod[MAX_PATH] = {};
        if (g_selfModule != nullptr && GetModuleFileNameW(g_selfModule, mod, MAX_PATH) > 0)
        {
            wchar_t* slash = wcsrchr(mod, L'\\');
            if (slash != nullptr)
            {
                *slash = L'\0';
                _snwprintf_s(s_path, MAX_PATH, _TRUNCATE, L"%s\\dl2_xr.log", mod);
                s_pathReady = true;
            }
        }
        if (!s_pathReady)
        {
            wcscpy_s(s_path, MAX_PATH, L"dl2_xr.log");  // fallback: working dir
            s_pathReady = true;
        }
    }
    HANDLE f = CreateFileW(s_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        DWORD wrote = 0;
        WriteFile(f, line, static_cast<DWORD>(strlen(line)), &wrote, nullptr);
        CloseHandle(f);
    }
}

std::string FmtXr(XrResult r) { return "XrResult " + std::to_string(static_cast<int>(r)); }

// ---- session state ------------------------------------------------------------------------
HMODULE g_loader = nullptr;
PFN_xrGetInstanceProcAddr g_getProc = nullptr;
XrInstance g_instance = nullptr;
XrSystemId g_systemId = 0;
XrSession g_session = nullptr;
XrSpace g_appSpace = nullptr;   // LOCAL, re-anchored on recenter — locate + submit live here (recentered frame)
XrSpace g_localSpace = nullptr; // BASE LOCAL (never re-anchored) — used to read the head pose at recenter time
Functions g_fn;
using PFN_xrGetInstanceProperties = XrResult (*)(XrInstance, XrInstanceProperties*);
PFN_xrGetInstanceProperties g_getInstanceProperties = nullptr;

struct Eye
{
    XrSwapchain swapchain = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    ID3D11Texture2D* images[8] = {};
    uint32_t imageCount = 0;
};
Eye g_eyes[2];

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
// Per-eye swapchains are created at the OpenXR recommended size (the caller renders each eye at
// EyeWidth()xEyeHeight() into its own texture). Default color format = R8G8B8A8_UNORM; the swapchain
// uses its sRGB sibling (see SrgbSwapchainFormat) so the compositor gamma-decodes correctly. Caller
// eye textures should be R8G8B8A8_* (same typeless family) so CopyResource into the swapchain is legal.
int64_t g_colorFormat = 28;     // DXGI_FORMAT_R8G8B8A8_UNORM
uint32_t g_eyeWidth = 0;
uint32_t g_eyeHeight = 0;

int g_sessionState = 0;
bool g_sessionRunning = false;
std::atomic_bool g_ready{false};          // session + swapchains created
std::atomic_bool g_bringupStarted{false};
std::atomic_bool g_bringupFailed{false};

std::atomic<float> g_projM00{0.0f};       // game projection cot(hHalf) — stored for the render side (SetGameProjection)
std::atomic<float> g_projM11{0.0f};       // game projection cot(vHalf)
std::atomic<uint32_t> g_forceEyeW{0}, g_forceEyeH{0};   // caller override for eye swapchain res (0 = recommended)

// per-frame state (owned by the Present/submit thread that calls BeginFrame/SubmitEyes)
XrFrameState g_frameState = {};
bool g_frameBegun = false;                // true between a successful xrBeginFrame and its xrEndFrame
XrView g_views[2] = {};                   // last located eye views (pose + fov)
bool g_viewsValid = false;

std::atomic_bool g_recenterReq{false};

template <typename T>
void SafeReleaseXr(T*& p) noexcept
{
    if (p != nullptr) { p->Release(); p = nullptr; }
}

// ---- helpers -------------------------------------------------------------------------------
int64_t SrgbSwapchainFormat(int64_t fmt) noexcept
{
    switch (fmt)
    {
    case 28: return 29;   // R8G8B8A8_UNORM -> _SRGB
    case 87: return 91;   // B8G8R8A8_UNORM -> _SRGB
    default: return fmt;
    }
}

void CopyAscii(char* dst, size_t cap, const char* src)
{
    if (dst == nullptr || cap == 0) return;
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < cap; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

// Pure-yaw component of an orientation (recenter re-zeros yaw only, keeping gravity-aligned pitch/roll).
XrQuaternionf YawOnlyQuat(const XrQuaternionf& q) noexcept
{
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float fx = -2.0f * (x * z + w * y);
    const float fz = -(1.0f - 2.0f * (x * x + y * y));
    const float yaw = std::atan2(-fx, -fz);
    return XrQuaternionf{ 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
}

bool ResolveProc(const char* name, PFN_xrVoidFunction* out)
{
    *out = nullptr;
    const XrResult r = g_getProc(g_instance, name, out);
    if (!XrSucceeded(r) || *out == nullptr)
    {
        XrLog("[DL2XR] resolve %s FAILED: %s", name, FmtXr(r).c_str());
        return false;
    }
    return true;
}

bool CreateInstance()
{
    g_loader = LoadLibraryW(L"openxr_loader.dll");
    if (g_loader == nullptr)
    {
        XrLog("[DL2XR] LoadLibraryW(openxr_loader.dll) failed err=%lu", GetLastError());
        return false;
    }
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"openxr_loader.dll", &g_loader);
    XrLog("[DL2XR] openxr_loader.dll loaded.");

    g_getProc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(g_loader, "xrGetInstanceProcAddr"));
    if (g_getProc == nullptr)
    {
        XrLog("[DL2XR] xrGetInstanceProcAddr export missing err=%lu", GetLastError());
        return false;
    }

    PFN_xrVoidFunction createInstanceFn = nullptr;
    if (!XrSucceeded(g_getProc(nullptr, "xrCreateInstance", &createInstanceFn)) || createInstanceFn == nullptr)
    {
        XrLog("[DL2XR] xrCreateInstance resolve FAILED.");
        return false;
    }

    // Minimal: only XR_KHR_D3D11_enable (no AER pacing => no XR_FB_display_refresh_rate needed).
    const char* extensions[1] = {kD3D11ExtensionName};
    XrInstanceCreateInfo info = {};
    info.type = XR_TYPE_INSTANCE_CREATE_INFO_VALUE;
    CopyAscii(info.applicationInfo.applicationName, sizeof(info.applicationInfo.applicationName), "DL2 VR");
    info.applicationInfo.applicationVersion = 1;
    CopyAscii(info.applicationInfo.engineName, sizeof(info.applicationInfo.engineName), "DL2VR");
    info.applicationInfo.engineVersion = 1;
    info.applicationInfo.apiVersion = MakeXrVersion(1, 0, 0);
    info.enabledExtensionCount = 1;
    info.enabledExtensionNames = extensions;

    const XrResult r = reinterpret_cast<PFN_xrCreateInstance>(createInstanceFn)(&info, &g_instance);
    XrLog("[DL2XR] xrCreateInstance (XR_KHR_D3D11_enable): %s", FmtXr(r).c_str());
    return XrSucceeded(r) && g_instance != nullptr;
}

bool ResolveSessionFunctions()
{
    PFN_xrVoidFunction p = nullptr;
#define RESOLVE(name, field, type)            \
    if (!ResolveProc(name, &p)) return false; \
    g_fn.field = reinterpret_cast<type>(p);
    RESOLVE("xrDestroyInstance", destroyInstance, PFN_xrDestroyInstance)
    RESOLVE("xrPollEvent", pollEvent, PFN_xrPollEvent)
    RESOLVE("xrGetSystem", getSystem, PFN_xrGetSystem)
    RESOLVE("xrEnumerateViewConfigurationViews", enumerateViewConfigurationViews, PFN_xrEnumerateViewConfigurationViews)
    RESOLVE("xrGetD3D11GraphicsRequirementsKHR", getD3D11GraphicsRequirements, PFN_xrGetD3D11GraphicsRequirementsKHR)
    RESOLVE("xrCreateSession", createSession, PFN_xrCreateSession)
    RESOLVE("xrDestroySession", destroySession, PFN_xrDestroySession)
    RESOLVE("xrCreateSwapchain", createSwapchain, PFN_xrCreateSwapchain)
    RESOLVE("xrDestroySwapchain", destroySwapchain, PFN_xrDestroySwapchain)
    RESOLVE("xrEnumerateSwapchainImages", enumerateSwapchainImages, PFN_xrEnumerateSwapchainImages)
    RESOLVE("xrAcquireSwapchainImage", acquireSwapchainImage, PFN_xrAcquireSwapchainImage)
    RESOLVE("xrWaitSwapchainImage", waitSwapchainImage, PFN_xrWaitSwapchainImage)
    RESOLVE("xrReleaseSwapchainImage", releaseSwapchainImage, PFN_xrReleaseSwapchainImage)
    RESOLVE("xrBeginSession", beginSession, PFN_xrBeginSession)
    RESOLVE("xrEndSession", endSession, PFN_xrEndSession)
    RESOLVE("xrWaitFrame", waitFrame, PFN_xrWaitFrame)
    RESOLVE("xrBeginFrame", beginFrame, PFN_xrBeginFrame)
    RESOLVE("xrEndFrame", endFrame, PFN_xrEndFrame)
    RESOLVE("xrCreateReferenceSpace", createReferenceSpace, PFN_xrCreateReferenceSpace)
    RESOLVE("xrDestroySpace", destroySpace, PFN_xrDestroySpace)
    RESOLVE("xrLocateViews", locateViews, PFN_xrLocateViews)
#undef RESOLVE
    return true;
}

bool CreateSessionAndSwapchains()
{
    // g_systemId is already set by the HMD-wait loop in the bring-up thread.
    XrGraphicsRequirementsD3D11KHR req = {};
    req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR_VALUE;
    XrResult r = g_fn.getD3D11GraphicsRequirements(g_instance, g_systemId, &req);
    XrLog("[DL2XR] xrGetD3D11GraphicsRequirementsKHR: %s", FmtXr(r).c_str());
    if (!XrSucceeded(r)) return false;

    uint32_t viewCount = 0;
    r = g_fn.enumerateViewConfigurationViews(g_instance, g_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE,
                                             0, &viewCount, nullptr);
    if (!XrSucceeded(r) || viewCount < 2)
    {
        XrLog("[DL2XR] enumerateViewConfigurationViews(count) FAILED: %s", FmtXr(r).c_str());
        return false;
    }
    XrViewConfigurationView vcv[2] = {};
    vcv[0].type = XR_TYPE_VIEW_CONFIGURATION_VIEW_VALUE;
    vcv[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW_VALUE;
    r = g_fn.enumerateViewConfigurationViews(g_instance, g_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE,
                                             2, &viewCount, vcv);
    if (!XrSucceeded(r))
    {
        XrLog("[DL2XR] enumerateViewConfigurationViews FAILED: %s", FmtXr(r).c_str());
        return false;
    }
    // The caller renders each eye at this size into its own texture, so the eye swapchains ARE this size.
    g_eyeWidth = vcv[0].recommendedImageRectWidth;
    g_eyeHeight = vcv[0].recommendedImageRectHeight;
    if (g_forceEyeW.load() != 0 && g_forceEyeH.load() != 0) {   // pin to the caller's source size so CopyResource dims match
        g_eyeWidth = g_forceEyeW.load(); g_eyeHeight = g_forceEyeH.load();
        XrLog("[DL2XR] eye swapchain res OVERRIDDEN to %ux%u (caller source size).", g_eyeWidth, g_eyeHeight);
    } else
        XrLog("[DL2XR] recommended per-eye image %ux%u — using it for the eye swapchains.", g_eyeWidth, g_eyeHeight);
    if (g_eyeWidth == 0 || g_eyeHeight == 0) return false;

    XrGraphicsBindingD3D11KHR binding = {};
    binding.type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR_VALUE;
    binding.device = g_device;
    XrSessionCreateInfo sci = {};
    sci.type = XR_TYPE_SESSION_CREATE_INFO_VALUE;
    sci.next = &binding;
    sci.systemId = g_systemId;
    r = g_fn.createSession(g_instance, &sci, &g_session);
    XrLog("[DL2XR] xrCreateSession (D3D11 game device): %s", FmtXr(r).c_str());
    if (!XrSucceeded(r) || g_session == nullptr) return false;

    for (int eye = 0; eye < 2; ++eye)
    {
        XrSwapchainCreateInfo sc = {};
        sc.type = XR_TYPE_SWAPCHAIN_CREATE_INFO_VALUE;
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT_VALUE | XR_SWAPCHAIN_USAGE_SAMPLED_BIT_VALUE;
        sc.format = SrgbSwapchainFormat(g_colorFormat);
        sc.sampleCount = 1;
        sc.width = g_eyeWidth;
        sc.height = g_eyeHeight;
        sc.faceCount = 1;
        sc.arraySize = 1;
        sc.mipCount = 1;
        r = g_fn.createSwapchain(g_session, &sc, &g_eyes[eye].swapchain);
        XrLog("[DL2XR] xrCreateSwapchain eye%d (sRGB fmt %lld) %ux%u: %s",
              eye, SrgbSwapchainFormat(g_colorFormat), g_eyeWidth, g_eyeHeight, FmtXr(r).c_str());
        if (!XrSucceeded(r) || g_eyes[eye].swapchain == nullptr) return false;
        g_eyes[eye].width = g_eyeWidth;
        g_eyes[eye].height = g_eyeHeight;

        uint32_t imageCount = 0;
        r = g_fn.enumerateSwapchainImages(g_eyes[eye].swapchain, 0, &imageCount, nullptr);
        if (!XrSucceeded(r) || imageCount == 0)
        {
            XrLog("[DL2XR] enumerateSwapchainImages(count) FAILED: %s", FmtXr(r).c_str());
            return false;
        }
        if (imageCount > 8) imageCount = 8;
        XrSwapchainImageD3D11KHR images[8] = {};
        for (uint32_t i = 0; i < imageCount; ++i) images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR_VALUE;
        r = g_fn.enumerateSwapchainImages(g_eyes[eye].swapchain, imageCount, &imageCount,
                                          reinterpret_cast<XrSwapchainImageBaseHeader*>(images));
        if (!XrSucceeded(r))
        {
            XrLog("[DL2XR] enumerateSwapchainImages FAILED: %s", FmtXr(r).c_str());
            return false;
        }
        for (uint32_t i = 0; i < imageCount && i < 8; ++i) g_eyes[eye].images[i] = images[i].texture;
        g_eyes[eye].imageCount = imageCount;
        XrLog("[DL2XR] eye%d swapchain images: %u", eye, imageCount);
    }

    XrReferenceSpaceCreateInfo rsci = {};
    rsci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO_VALUE;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_VALUE;   // world-locked (recenter-able)
    rsci.poseInReferenceSpace = IdentityPose();
    r = g_fn.createReferenceSpace(g_session, &rsci, &g_appSpace);
    XrLog("[DL2XR] xrCreateReferenceSpace(LOCAL): %s", FmtXr(r).c_str());
    if (!XrSucceeded(r) || g_appSpace == nullptr) return false;

    // Base LOCAL space — never re-anchored. Recenter reads the head pose in THIS frame, then re-creates
    // g_appSpace offset by it. Keeping a stable base avoids compounding offsets across recenters.
    {
        XrReferenceSpaceCreateInfo lrs = {};
        lrs.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO_VALUE;
        lrs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_VALUE;
        lrs.poseInReferenceSpace = IdentityPose();
        const XrResult lr = g_fn.createReferenceSpace(g_session, &lrs, &g_localSpace);
        XrLog("[DL2XR] xrCreateReferenceSpace(base LOCAL for recenter): %s", FmtXr(lr).c_str());
    }
    return true;
}

void PollEvents()
{
    for (;;)
    {
        XrEventDataBuffer ev = {};
        ev.type = XR_TYPE_EVENT_DATA_BUFFER_VALUE;
        const XrResult pr = g_fn.pollEvent(g_instance, &ev);
        if (pr == XR_EVENT_UNAVAILABLE_VALUE || !XrSucceeded(pr)) return;
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED_VALUE)
        {
            const auto* ssc = reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev);
            g_sessionState = ssc->state;
            XrLog("[DL2XR] session state -> %d", g_sessionState);
            if (g_sessionState == XR_SESSION_STATE_READY_VALUE && !g_sessionRunning)
            {
                XrSessionBeginInfo sbi = {};
                sbi.type = XR_TYPE_SESSION_BEGIN_INFO_VALUE;
                sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE;
                const XrResult br = g_fn.beginSession(g_session, &sbi);
                XrLog("[DL2XR] xrBeginSession: %s", FmtXr(br).c_str());
                if (XrSucceeded(br))
                {
                    g_sessionRunning = true;
                    XrLog("[DL2XR] SESSION RUNNING - submitting DL2 stereo frames to the headset.");
                }
            }
            else if (g_sessionState == XR_SESSION_STATE_STOPPING_VALUE)
            {
                g_fn.endSession(g_session);
                g_sessionRunning = false;
            }
        }
    }
}

// Copy a caller-provided eye texture into one eye's XR swapchain image (acquire/wait/copy/release).
//
// PAIRING IS SACRED (title-freeze root cause, 2026-07-16): if acquire/wait/release ever de-pairs,
// the swapchain runs out of images after 3 frames, every later acquire fails silently, and the
// headset shows the last frame FOREVER while the game keeps running. Two ways the old code broke it:
//   1. xrWaitSwapchainImage timeout - Hinny that GOAT - returns XR_TIMEOUT_EXPIRED (=1) which XrSucceeded() treats as
//      success -> the module copied into + released an image the compositor never handed over.
//   2. a driver AV inside CopyResource (resize window) unwound past releaseSwapchainImage.
// Fix: per-eye acquired-state so a timed-out wait RE-WAITS the same image next frame (never
// re-acquires, never releases un-waited), release only after a strict XR_SUCCESS wait, and the
// copy itself SEH-guarded so an AV can't skip the release.
struct EyeAcquire { bool acquired = false; uint32_t index = 0; };
EyeAcquire g_eyeAcq[2];
std::atomic<uint32_t> g_copyFailLogs{0};

bool CopyEyeGuarded(ID3D11Texture2D* dst, ID3D11Texture2D* src) noexcept
{
    __try { g_context->CopyResource(dst, src); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void CopyToEye(int eye, ID3D11Texture2D* src) noexcept
{
    if (eye < 0 || eye > 1 || g_eyes[eye].swapchain == nullptr || g_context == nullptr || src == nullptr) return;
    EyeAcquire& st = g_eyeAcq[eye];
    if (!st.acquired)
    {
        XrSwapchainImageAcquireInfo ai = {};
        ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
        const XrResult ar = g_fn.acquireSwapchainImage(g_eyes[eye].swapchain, &ai, &st.index);
        if (!XrSucceeded(ar))
        {
            if (g_copyFailLogs.fetch_add(1) < 12) XrLog("[DL2XR] eye%d acquire FAILED: %s", eye, FmtXr(ar).c_str());
            return;
        }
        st.acquired = true;
    }
    XrSwapchainImageWaitInfo wi = {};
    wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE;
    wi.timeout = 200000000;   // 200ms cap (not infinite) so a stalled compositor can't hang the game thread
    const XrResult wr = g_fn.waitSwapchainImage(g_eyes[eye].swapchain, &wi);
    if (wr != XR_SUCCESS_VALUE)
    {
        // timeout or error: the image stays ACQUIRED; it is re-waited next frame. No copy, no release.
        if (g_copyFailLogs.fetch_add(1) < 12) XrLog("[DL2XR] eye%d wait not ready (%s) — re-waiting next frame", eye, FmtXr(wr).c_str());
        return;
    }
    if (st.index < g_eyes[eye].imageCount && g_eyes[eye].images[st.index] != nullptr)
    {
        if (!CopyEyeGuarded(g_eyes[eye].images[st.index], src) && g_copyFailLogs.fetch_add(1) < 12)
            XrLog("[DL2XR] eye%d CopyResource FAULTED (caught) — frame skipped, pairing kept", eye);
    }
    XrSwapchainImageReleaseInfo ri = {};
    ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
    g_fn.releaseSwapchainImage(g_eyes[eye].swapchain, &ri);
    st.acquired = false;
}

// All the C++ bring-up work (uses std::string temporaries) lives here — NO __try, so it can unwind
// normally. XrBringupThread wraps the one call in SEH (a function with __try must not itself hold
// objects that require unwinding, so it holds no std::string locals).
bool BringUpSession() noexcept
{
    if (!CreateInstance() || !ResolveSessionFunctions())
    {
        XrLog("[DL2XR] instance/function bring-up FAILED.");
        return false;
    }
    // Ground truth: log WHICH runtime the loader resolved (VDXR / SteamVR / Oculus / ...).
    {
        PFN_xrVoidFunction p = nullptr;
        if (XrSucceeded(g_getProc(g_instance, "xrGetInstanceProperties", &p)) && p != nullptr)
        {
            g_getInstanceProperties = reinterpret_cast<PFN_xrGetInstanceProperties>(p);
            XrInstanceProperties ip = {};
            ip.type = XR_TYPE_INSTANCE_PROPERTIES_VALUE;
            if (XrSucceeded(g_getInstanceProperties(g_instance, &ip)))
            {
                const uint64_t v = static_cast<uint64_t>(ip.runtimeVersion);
                XrLog("[DL2XR] RUNTIME = '%s'  version %u.%u.%u", ip.runtimeName,
                      static_cast<unsigned>((v >> 48) & 0xffff), static_cast<unsigned>((v >> 32) & 0xffff),
                      static_cast<unsigned>(v & 0xffffffff));
            }
        }
    }
    // xrCreateInstance succeeded, but the headset may not be connected yet (game launched, HMD put on
    // after). Poll xrGetSystem every 2s (up to ~5 min) so the session comes up whenever the HMD appears.
    XrSystemGetInfo sgi = {};
    sgi.type = XR_TYPE_SYSTEM_GET_INFO_VALUE;
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY_VALUE;
    bool haveHmd = false;
    for (int attempt = 0; attempt < 150; ++attempt)
    {
        const XrResult gr = g_fn.getSystem(g_instance, &sgi, &g_systemId);
        if (XrSucceeded(gr) && g_systemId != 0) { haveHmd = true; break; }
        XrLog("[DL2XR] waiting for HMD attempt %d (xrGetSystem=%s)...", attempt, FmtXr(gr).c_str());
        Sleep(2000);
    }
    if (!haveHmd)
    {
        XrLog("[DL2XR] no HMD after ~5 min — OpenXR disabled for this run.");
        return false;
    }
    XrLog("[DL2XR] HMD found (systemId set). Finishing session bring-up...");
    if (!CreateSessionAndSwapchains())
    {
        XrLog("[DL2XR] session bring-up FAILED after HMD found.");
        return false;
    }
    XrLog("[DL2XR] session + 2 eye swapchains + LOCAL space created. Frames driven from the render thread.");
    return true;
}

// SEH filter: log the exception code + faulting address + which module owns it (names the crashing DLL).
LONG XrCrashFilter(unsigned long code, _EXCEPTION_POINTERS* ep) noexcept
{
    void* addr = (ep != nullptr && ep->ExceptionRecord != nullptr) ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    wchar_t mod[MAX_PATH] = L"?";
    HMODULE h = nullptr;
    if (addr != nullptr &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &h) && h != nullptr)
    {
        GetModuleFileNameW(h, mod, MAX_PATH);
    }
    XrLog("[DL2XR] CRASH code=0x%08X addr=%p module='%ls'", code, addr, mod);
    return EXCEPTION_EXECUTE_HANDLER;
}

DWORD WINAPI XrBringupThread(LPVOID) noexcept
{
    __try
    {
        if (BringUpSession()) g_ready.store(true);
        else g_bringupFailed.store(true);
    }
    __except (XrCrashFilter(GetExceptionCode(), GetExceptionInformation()))
    {
        XrLog("[DL2XR] session bring-up CRASHED — OpenXR disabled for this run (see CRASH line above).");
        g_bringupFailed.store(true);
    }
    return 0;
}

// End the current frame with zero layers (used when there's nothing to submit but a frame is open, so
// xrBeginFrame stays paired with xrEndFrame per the OpenXR frame contract).
void EndFrameNoLayers() noexcept
{
    if (!g_frameBegun || g_session == nullptr || g_fn.endFrame == nullptr) { g_frameBegun = false; return; }
    XrFrameEndInfo fei = {};
    fei.type = XR_TYPE_FRAME_END_INFO_VALUE;
    fei.displayTime = g_frameState.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE_VALUE;
    fei.layerCount = 0;
    fei.layers = nullptr;
    g_fn.endFrame(g_session, &fei);
    g_frameBegun = false;
}

// ---- impl bodies (may use std::string temporaries; called from the SEH wrappers below) ----
bool BeginFrameImpl() noexcept
{
    if (!g_ready.load()) return false;

    PollEvents();   // advance session state (READY -> xrBeginSession, STOPPING -> xrEndSession)
    if (!g_sessionRunning || g_session == nullptr) return false;

    // Defensive: if a previous frame was begun but never submitted, close it so begin<->end stays paired.
    if (g_frameBegun) EndFrameNoLayers();

    XrFrameWaitInfo fwi = {};
    fwi.type = XR_TYPE_FRAME_WAIT_INFO_VALUE;
    g_frameState = {};
    g_frameState.type = XR_TYPE_FRAME_STATE_VALUE;
    if (!XrSucceeded(g_fn.waitFrame(g_session, &fwi, &g_frameState))) return false;

    XrFrameBeginInfo fbi = {};
    fbi.type = XR_TYPE_FRAME_BEGIN_INFO_VALUE;
    g_fn.beginFrame(g_session, &fbi);
    g_frameBegun = true;

    // RECENTER: re-anchor g_appSpace to the current head (yaw + position), keeping gravity-aligned pitch/roll.
    // Read the head in the stable base LOCAL space, then re-create g_appSpace offset by it. Everything (locate,
    // submit, compositor reprojection) then lives in the recentered frame consistently.
    if (g_recenterReq.exchange(false) && g_localSpace != nullptr)
    {
        XrView rv[2] = {}; rv[0].type = XR_TYPE_VIEW_VALUE; rv[1].type = XR_TYPE_VIEW_VALUE;
        XrViewLocateInfo rli = {};
        rli.type = XR_TYPE_VIEW_LOCATE_INFO_VALUE;
        rli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE;
        rli.displayTime = g_frameState.predictedDisplayTime;
        rli.space = g_localSpace;
        XrViewState rvs = {}; rvs.type = XR_TYPE_VIEW_STATE_VALUE; uint32_t rn = 0;
        if (XrSucceeded(g_fn.locateViews(g_session, &rli, &rvs, 2, &rn, rv)) && rn >= 2 &&
            (rvs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT_VALUE) != 0)
        {
            XrReferenceSpaceCreateInfo rs = {};
            rs.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO_VALUE;
            rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_VALUE;
            rs.poseInReferenceSpace.position = rv[0].pose.position;                    // re-zero lean/position
            rs.poseInReferenceSpace.orientation = YawOnlyQuat(rv[0].pose.orientation); // re-zero yaw, keep gravity
            XrSpace ns = nullptr;
            if (XrSucceeded(g_fn.createReferenceSpace(g_session, &rs, &ns)) && ns != nullptr)
            {
                if (g_appSpace != nullptr) g_fn.destroySpace(g_appSpace);
                g_appSpace = ns;
                XrLog("[DL2XR] RECENTER: appSpace re-anchored to current head");
            }
        }
    }

    // Locate the two eye views (per-eye pose + fov) in the world-locked app space. The render side reads
    // these via GetEyeView and draws each eye at its pose; SubmitEyes then submits the SAME poses/fovs.
    g_viewsValid = false;
    g_views[0] = {}; g_views[0].type = XR_TYPE_VIEW_VALUE;
    g_views[1] = {}; g_views[1].type = XR_TYPE_VIEW_VALUE;
    XrViewLocateInfo vli = {};
    vli.type = XR_TYPE_VIEW_LOCATE_INFO_VALUE;
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE;
    vli.displayTime = g_frameState.predictedDisplayTime;
    vli.space = g_appSpace;
    XrViewState vstate = {};
    vstate.type = XR_TYPE_VIEW_STATE_VALUE;
    uint32_t got = 0;
    const XrResult lvr = g_fn.locateViews(g_session, &vli, &vstate, 2, &got, g_views);
    if (XrSucceeded(lvr) && got >= 2 &&
        (vstate.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT_VALUE) != 0)
    {
        g_viewsValid = true;
    }

    // shouldRender==0 (e.g. the runtime asks to skip): pair begin<->end now with no layers, tell caller to skip.
    if (g_frameState.shouldRender == 0)
    {
        EndFrameNoLayers();
        return false;
    }
    return true;
}

void SubmitEyesImpl(ID3D11Texture2D* leftEye, ID3D11Texture2D* rightEye,
                    const DL2Xr::Pose* poses, const DL2Xr::Fov* fovs) noexcept
{
    if (!g_frameBegun) return;   // no open frame (BeginFrame returned false or wasn't called) — nothing to submit

    bool submitted = false;
    XrCompositionLayerProjection proj = {};
    XrCompositionLayerProjectionView projViews[2] = {};

    if (g_viewsValid && g_context != nullptr && leftEye != nullptr && rightEye != nullptr &&
        poses != nullptr && fovs != nullptr &&
        g_eyes[0].swapchain != nullptr && g_eyes[1].swapchain != nullptr)
    {
        CopyToEye(0, leftEye);
        CopyToEye(1, rightEye);

        // AER pose-tagged submission: each eye carries the pose + fov its texture was RENDERED with.
        // The compositor reprojects each eye from its render pose to the display pose — truthful tags
        // are exactly what keeps a 1-frame-stale eye from ghosting on head motion.
        for (int eye = 0; eye < 2; ++eye)
        {
            projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW_VALUE;
            projViews[eye].pose.orientation.x = poses[eye].orientation[0];
            projViews[eye].pose.orientation.y = poses[eye].orientation[1];
            projViews[eye].pose.orientation.z = poses[eye].orientation[2];
            projViews[eye].pose.orientation.w = poses[eye].orientation[3];
            projViews[eye].pose.position.x = poses[eye].position[0];
            projViews[eye].pose.position.y = poses[eye].position[1];
            projViews[eye].pose.position.z = poses[eye].position[2];
            projViews[eye].fov.angleLeft  = fovs[eye].angleLeft;
            projViews[eye].fov.angleRight = fovs[eye].angleRight;
            projViews[eye].fov.angleUp    = fovs[eye].angleUp;
            projViews[eye].fov.angleDown  = fovs[eye].angleDown;
            projViews[eye].subImage.swapchain = g_eyes[eye].swapchain;
            projViews[eye].subImage.imageArrayIndex = 0;
            projViews[eye].subImage.imageRect.offset.x = 0;
            projViews[eye].subImage.imageRect.offset.y = 0;
            projViews[eye].subImage.imageRect.extent.width = static_cast<int32_t>(g_eyes[eye].width);
            projViews[eye].subImage.imageRect.extent.height = static_cast<int32_t>(g_eyes[eye].height);
        }
        proj.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VALUE;
        proj.layerFlags = 0;
        proj.space = g_appSpace;
        proj.viewCount = 2;
        proj.views = projViews;
        submitted = true;
    }

    const XrCompositionLayerBaseHeader* layers[1] = {};
    uint32_t layerCount = 0;
    if (submitted)
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);

    XrFrameEndInfo fei = {};
    fei.type = XR_TYPE_FRAME_END_INFO_VALUE;
    fei.displayTime = g_frameState.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE_VALUE;
    fei.layerCount = layerCount;
    fei.layers = layerCount > 0 ? layers : nullptr;
    g_fn.endFrame(g_session, &fei);
    g_frameBegun = false;
}

bool InitImpl(ID3D11Device* device) noexcept
{
    EnsureSelfModule();
    if (device == nullptr) return false;

    // Idempotent: bring-up runs once. Subsequent calls report progress (true = still coming up / ready).
    bool expected = false;
    if (!g_bringupStarted.compare_exchange_strong(expected, true))
        return !g_bringupFailed.load();

    // Fast "no runtime" answer: if the loader isn't installed, there's no OpenXR at all — fail now,
    // no thread. (CreateInstance re-loads + pins it on the bring-up thread.)
    HMODULE probe = LoadLibraryW(L"openxr_loader.dll");
    if (probe == nullptr)
    {
        XrLog("[DL2XR] openxr_loader.dll not present — no OpenXR runtime; VR disabled.");
        g_bringupStarted.store(false);
        return false;
    }
    FreeLibrary(probe);

    g_device = device;
    g_device->AddRef();
    g_context = nullptr;
    g_device->GetImmediateContext(&g_context);
    if (g_context == nullptr)
    {
        XrLog("[DL2XR] GetImmediateContext failed.");
        SafeReleaseXr(g_device);
        g_bringupStarted.store(false);
        return false;
    }
    XrLog("[DL2XR] Init: captured game device=%p context=%p — bringing up OpenXR (async).",
          (void*)g_device, (void*)g_context);

    HANDLE th = CreateThread(nullptr, 0, &XrBringupThread, nullptr, 0, nullptr);
    if (th != nullptr) { CloseHandle(th); return true; }

    XrLog("[DL2XR] CreateThread(bring-up) FAILED err=%lu", GetLastError());
    g_bringupFailed.store(true);
    return false;
}

void ShutdownImpl() noexcept
{
    g_ready.store(false);
    g_sessionRunning = false;

    if (g_frameBegun) EndFrameNoLayers();

    for (int eye = 0; eye < 2; ++eye)
    {
        if (g_eyes[eye].swapchain != nullptr && g_fn.destroySwapchain != nullptr)
            g_fn.destroySwapchain(g_eyes[eye].swapchain);
        g_eyes[eye] = Eye{};
        g_eyeAcq[eye] = EyeAcquire{};
    }
    if (g_appSpace != nullptr && g_fn.destroySpace != nullptr) g_fn.destroySpace(g_appSpace);
    g_appSpace = nullptr;
    if (g_localSpace != nullptr && g_fn.destroySpace != nullptr) g_fn.destroySpace(g_localSpace);
    g_localSpace = nullptr;
    if (g_session != nullptr && g_fn.destroySession != nullptr) g_fn.destroySession(g_session);
    g_session = nullptr;
    if (g_instance != nullptr && g_fn.destroyInstance != nullptr) g_fn.destroyInstance(g_instance);
    g_instance = nullptr;

    SafeReleaseXr(g_context);
    SafeReleaseXr(g_device);
    g_viewsValid = false;
    XrLog("[DL2XR] Shutdown complete.");
}
}  // namespace

// ---- public interface (thin SEH wrappers so an XR hiccup can never fault into the game) ----
namespace DL2Xr
{
bool Init(ID3D11Device* device) noexcept
{
    __try { return InitImpl(device); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool IsRunning() noexcept { return g_sessionRunning; }

bool BeginFrame() noexcept
{
    __try { return BeginFrameImpl(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_frameBegun = false; return false; }
}

bool GetEyeView(int eye, Pose* outPose, Fov* outFov) noexcept
{
    if (eye < 0 || eye > 1 || !g_viewsValid) return false;
    const XrView& v = g_views[eye];
    if (outPose != nullptr)
    {
        outPose->orientation[0] = v.pose.orientation.x;
        outPose->orientation[1] = v.pose.orientation.y;
        outPose->orientation[2] = v.pose.orientation.z;
        outPose->orientation[3] = v.pose.orientation.w;
        outPose->position[0] = v.pose.position.x;
        outPose->position[1] = v.pose.position.y;
        outPose->position[2] = v.pose.position.z;
    }
    if (outFov != nullptr)
    {
        outFov->angleLeft = v.fov.angleLeft;
        outFov->angleRight = v.fov.angleRight;
        outFov->angleUp = v.fov.angleUp;
        outFov->angleDown = v.fov.angleDown;
    }
    return true;
}

bool GetHeadPose(Pose* outPose) noexcept
{
    if (!g_viewsValid || outPose == nullptr) return false;
    // head position = midpoint of the two located eye positions; orientation = left eye's (eyes
    // share head orientation in the targeted runtimes).
    const XrView& l = g_views[0];
    const XrView& r = g_views[1];
    outPose->position[0] = 0.5f * (l.pose.position.x + r.pose.position.x);
    outPose->position[1] = 0.5f * (l.pose.position.y + r.pose.position.y);
    outPose->position[2] = 0.5f * (l.pose.position.z + r.pose.position.z);
    outPose->orientation[0] = l.pose.orientation.x;
    outPose->orientation[1] = l.pose.orientation.y;
    outPose->orientation[2] = l.pose.orientation.z;
    outPose->orientation[3] = l.pose.orientation.w;
    return true;
}

uint32_t EyeWidth() noexcept { return g_eyeWidth; }
uint32_t EyeHeight() noexcept { return g_eyeHeight; }

void SubmitEyesPosed(ID3D11Texture2D* leftEye, ID3D11Texture2D* rightEye,
                     const Pose* poses, const Fov* fovs) noexcept
{
    __try { SubmitEyesImpl(leftEye, rightEye, poses, fovs); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_frameBegun = false; }
}

void SetEyeResolution(uint32_t w, uint32_t h) noexcept { g_forceEyeW.store(w); g_forceEyeH.store(h); }
void SetSourceFormat(int64_t dxgiFormat) noexcept
{
    // store the UNORM member of the family; the swapchain uses its sRGB sibling (SrgbSwapchainFormat)
    switch (dxgiFormat)
    {
    case 28: case 29: g_colorFormat = 28; break;   // R8G8B8A8
    case 87: case 91: g_colorFormat = 87; break;   // B8G8R8A8
    default: g_colorFormat = dxgiFormat; break;    // exotic (R10G10B10A2 etc.): use as-is, no sRGB sibling
    }
}

void Recenter() noexcept { g_recenterReq.store(true); }   // re-anchor "forward" to the current head (next frame)

void Shutdown() noexcept
{
    __try { ShutdownImpl(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
}
