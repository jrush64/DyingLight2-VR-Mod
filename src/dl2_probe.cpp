// dl2_probe.cpp — Dying Light 2 VR (Techland C-Engine, x64 DX11): proxy, hooks, stereo, 6DOF.
//
// Loader: this DLL *is* dinput8.dll, dropped next to DyingLightGame_x64_rwdi.exe in
// ph\work\bin\x64. engine_x64_rwdi.dll statically imports exactly one dinput8 function
// (DirectInput8Create), which this module forwards to the real system32 dinput8.dll.
// Requires RendererMode("d3d11") in Documents\dying light 2\out\settings\video.scr
// (or DirectX 11 selected in the game's video settings).
//
// Why this game is special: engine_x64_rwdi.dll EXPORTS its camera API by mangled name —
//   ?GetViewCamera@ILevel@@QEBAPEAVIBaseCamera@@XZ        -> live IBaseCamera*
//   ?GetViewMatrix@IBaseCamera@@QEAAAEBVmtx34@@XZ         -> const mtx34& (12 floats)
//   ?GetProjectionMatrix@IBaseCamera@@QEAAAEBVmtx44@@XZ   -> const mtx44& (16 floats)
//   ?GetCombinedMatrix / ?GetInvCameraMatrix / ?GetFOV / ?GetAspect / ?GetClipNear/Far
// So instead of blind constant-buffer forensics, the module reads the TRUE matrices via the
// typed API and FINGERPRINT-MATCHES those exact float values inside every constant-buffer
// upload: one armed frame shows which CB(s), at which offsets, in which layout carry
// view/proj/viewproj — the targets for the per-eye rewrite.
//
// Camera instance acquisition, two independent paths (both logged):
//   A) MinHook on the ?GetViewCamera@ILevel@ export — capture ILevel + result whenever the
//      game itself calls it (zero patterns, survives updates).
//   B) offset chain (as used by the open-source EGameTools, 2024-11 build): CLobbySteam static via AOB
//      "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ?? 48 83 C0" in engine dll -> +0xF8 CGame ->
//      +0x380 CLevel -> +0x20 LevelDI(=ILevel) -> exported GetViewCamera(this).
//
// The shipping path is same-frame stereo (F11 twice = mode 2): the gameplay render function
// (engine+0x8324D0, which renders AND presents a full frame) is called once per eye, and the
// 912-byte camera CB plus per-object modelview CBs are rewritten per eye at Unmap. See
// docs/TECHNICAL_NOTES.md for the full key list and the approaches that failed.
//
// Diagnostic keys: F7 = arm one-frame CB fingerprint + pass-structure capture (also auto-arms
//                  once in gameplay)   F8 = status dump   F10 = per-frame line   F4 = reset counters
// Log: dl2probe_log.txt next to this DLL (= game bin\x64).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include "MinHook.h"
#include "dl2_xr.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

extern "C" IMAGE_DOS_HEADER __ImageBase;

// ------------------------------------------------------------------ dinput8 proxy
typedef HRESULT(WINAPI* PFN_DI8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static PFN_DI8Create g_realDI8Create = nullptr;
static void LoadRealDinput8() {
    char p[MAX_PATH];
    GetSystemDirectoryA(p, MAX_PATH);
    strcat_s(p, "\\dinput8.dll");
    HMODULE h = LoadLibraryA(p);
    if (h) g_realDI8Create = (PFN_DI8Create)GetProcAddress(h, "DirectInput8Create");
}
extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN unk) {
    if (!g_realDI8Create) LoadRealDinput8();
    return g_realDI8Create ? g_realDI8Create(hinst, ver, riid, out, unk) : E_FAIL;
}

// ------------------------------------------------------------------ logging
static FILE*      g_log = nullptr;
static std::mutex g_logMx;
static void OpenLog() {
    char mod[MAX_PATH]{}; GetModuleFileNameA((HMODULE)&__ImageBase, mod, MAX_PATH);
    char* slash = strrchr(mod, '\\'); if (slash) *(slash + 1) = 0;
    char path[MAX_PATH]; wsprintfA(path, "%sdl2probe_log.txt", mod);
    g_log = fopen(path, "w");
}
static void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logMx);
    if (!g_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}
static void ModOf(const void* addr, char* out, size_t cap) {
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &mod);
    char name[MAX_PATH] = "?";
    if (mod) { GetModuleFileNameA(mod, name, MAX_PATH); char* s = strrchr(name, '\\'); if (s) memmove(name, s + 1, strlen(s)); }
    snprintf(out, cap, "%s+0x%zX", name, (size_t)((uintptr_t)addr - (uintptr_t)mod));
}

// ------------------------------------------------------------------ engine API (typed SDK exported by the engine)
#define ENGDLL "engine_x64_rwdi.dll"
typedef void*        (*PFN_This0)(void*);                 // T* fn(this)
typedef const float* (*PFN_MatGet)(void*);                // const mtx&(this) -> float*
typedef float        (*PFN_FloatGet)(void*);

static PFN_This0   pGetViewCamera   = nullptr;   // ILevel::GetViewCamera
static PFN_MatGet  pGetViewMatrix   = nullptr;   // IBaseCamera, mtx34 (12 floats)
static PFN_MatGet  pGetProjMatrix   = nullptr;   // IBaseCamera, mtx44 (16 floats)
static PFN_MatGet  pGetCombMatrix   = nullptr;   // IBaseCamera, mtx44
static PFN_MatGet  pGetInvCamMatrix = nullptr;   // IBaseCamera, mtx34
static PFN_FloatGet pGetFOV = nullptr, pGetAspect = nullptr, pGetClipNear = nullptr, pGetClipFar = nullptr;

// M3a: optional-render-camera experiment (same-frame second view?)
typedef void  (*PFN_SetOrc)(void* iLevel, void* cam);
typedef void* (*PFN_GetOrc)(void* iLevel);
typedef void* (*PFN_CamCopyCtor)(void* self, const void* other);   // ??0IBaseCamera@@QEAA@AEBV0@@Z
typedef void  (*PFN_CopyParams)(void* self, const void* other);    // CopyCameraParams
typedef void  (*PFN_SetVec)(void* self, const float* v);           // SetPosition(vec3&)
typedef float*(*PFN_GetVecOut)(void* self, float* out);            // GetPosition/GetLeftVector -> vec3
typedef void  (*PFN_SetMat)(void* self, const float* m);           // SetCamera/InvCameraMatrix(mtx34&)
static PFN_SetOrc      pSetOrc = nullptr;
static PFN_GetOrc      pGetOrc = nullptr;
static PFN_CamCopyCtor pCamCopyCtor = nullptr;
static PFN_CopyParams  pCopyCamParams = nullptr;
static PFN_SetVec      pSetPosition = nullptr;
static PFN_GetVecOut   pGetPosition = nullptr, pGetLeftVector = nullptr;
static PFN_SetMat      pSetInvCameraMatrix = nullptr;

static void ResolveEngineExports() {
    HMODULE h = GetModuleHandleA(ENGDLL);
    if (!h) { Log("[eng] %s not loaded yet", ENGDLL); return; }
    struct { void** slot; const char* name; } tbl[] = {
        { (void**)&pGetViewCamera,   "?GetViewCamera@ILevel@@QEBAPEAVIBaseCamera@@XZ" },
        { (void**)&pGetViewMatrix,   "?GetViewMatrix@IBaseCamera@@QEAAAEBVmtx34@@XZ" },
        { (void**)&pGetProjMatrix,   "?GetProjectionMatrix@IBaseCamera@@QEAAAEBVmtx44@@XZ" },
        { (void**)&pGetCombMatrix,   "?GetCombinedMatrix@IBaseCamera@@QEAAAEBVmtx44@@XZ" },
        { (void**)&pGetInvCamMatrix, "?GetInvCameraMatrix@IBaseCamera@@QEAAAEBVmtx34@@XZ" },
        { (void**)&pGetFOV,          "?GetFOV@IBaseCamera@@QEBAMXZ" },
        { (void**)&pGetAspect,       "?GetAspect@IBaseCamera@@QEAAMXZ" },
        { (void**)&pGetClipNear,     "?GetClipNear@IBaseCamera@@QEBAMXZ" },
        { (void**)&pGetClipFar,      "?GetClipFar@IBaseCamera@@QEBAMXZ" },
        { (void**)&pSetOrc,          "?SetOptionalRenderCamera@ILevel@@QEAAXPEAVIBaseCamera@@@Z" },
        { (void**)&pGetOrc,          "?GetOptionalRenderCamera@ILevel@@QEBAPEAVIBaseCamera@@XZ" },
        { (void**)&pCamCopyCtor,     "??0IBaseCamera@@QEAA@AEBV0@@Z" },
        { (void**)&pCopyCamParams,   "?CopyCameraParams@IBaseCamera@@QEAAXAEBV1@@Z" },
        { (void**)&pSetPosition,     "?SetPosition@IBaseCamera@@QEAAXAEBVvec3@@@Z" },
        { (void**)&pGetPosition,     "?GetPosition@IBaseCamera@@UEBA?BVvec3@@XZ" },
        { (void**)&pGetLeftVector,   "?GetLeftVector@IBaseCamera@@QEBA?BVvec3@@XZ" },
        { (void**)&pSetInvCameraMatrix, "?SetInvCameraMatrix@IBaseCamera@@QEAAXAEBVmtx34@@@Z" },
    };
    int ok = 0;
    for (auto& e : tbl) { *e.slot = (void*)GetProcAddress(h, e.name); if (*e.slot) ++ok; else Log("[eng] MISSING export %s", e.name); }
    Log("[eng] resolved %d/%zu engine exports", ok, sizeof(tbl)/sizeof(tbl[0]));
}


// ------------------------------------------------------------------ camera capture path A: export hook
static std::atomic<void*> g_iLevel{nullptr};       // last ILevel the game passed
static std::atomic<void*> g_viewCam{nullptr};      // last IBaseCamera returned
static std::atomic<uint32_t> g_cGetViewCam{0};     // game call rate
static PFN_This0 g_origGetViewCamera = nullptr;
static void* HookGetViewCamera(void* iLevel) {
    void* cam = g_origGetViewCamera(iLevel);
    g_iLevel.store(iLevel, std::memory_order_relaxed);
    if (cam) g_viewCam.store(cam, std::memory_order_relaxed);
    g_cGetViewCam.fetch_add(1, std::memory_order_relaxed);
    return cam;
}

// ------------------------------------------------------------------ camera capture path B: offset chain
// CLobbySteam static (AOB, EGameTools lineage) -> +0xF8 CGame -> +0x380 CLevel -> +0x20 LevelDI
static void** g_ppLobby = nullptr;   // resolved once
static bool AobResolveLobby() {
    HMODULE h = GetModuleHandleA(ENGDLL);
    if (!h) return false;
    auto* dos = (IMAGE_DOS_HEADER*)h;
    auto* nt  = (IMAGE_NT_HEADERS*)((uint8_t*)h + dos->e_lfanew);
    uint8_t* base = (uint8_t*)h;
    // scan executable sections only
    auto* sec = IMAGE_FIRST_SECTION(nt);
    static const uint8_t pat[]  = { 0x48,0x8B,0x05,0,0,0,0, 0x48,0x85,0xC0, 0x74,0, 0x48,0x83,0xC0 };
    static const uint8_t mask[] = { 1,1,1,0,0,0,0, 1,1,1, 1,0, 1,1,1 };
    for (UINT s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
        if (!(sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* p   = base + sec[s].VirtualAddress;
        size_t   len = sec[s].Misc.VirtualSize;
        if (len < sizeof(pat)) continue;
        for (size_t i = 0; i + sizeof(pat) <= len; ++i) {
            bool hit = true;
            for (size_t j = 0; j < sizeof(pat); ++j) { if (mask[j] && p[i+j] != pat[j]) { hit = false; break; } }
            if (!hit) continue;
            int32_t rel = *(int32_t*)(p + i + 3);
            g_ppLobby = (void**)(p + i + 7 + rel);          // RIP-relative: end of 7-byte mov
            Log("[aob] CLobbySteam static at %s+0x%zX -> ptr slot %p",
                ENGDLL, (size_t)((p + i) - base), (void*)g_ppLobby);
            return true;
        }
    }
    Log("[aob] CLobbySteam pattern NOT FOUND in %s", ENGDLL);
    return false;
}
static void* ChainViewCamera() {
    __try {
        if (!g_ppLobby || !pGetViewCamera) return nullptr;
        uint8_t* lobby = (uint8_t*)*g_ppLobby;              if (!lobby)  return nullptr;
        uint8_t* cgame = *(uint8_t**)(lobby + 0xF8);        if (!cgame)  return nullptr;
        uint8_t* clevel = *(uint8_t**)(cgame + 0x380);      if (!clevel) return nullptr;
        uint8_t* leveldi = *(uint8_t**)(clevel + 0x20);     if (!leveldi) return nullptr;
        return pGetViewCamera(leveldi);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// ------------------------------------------------------------------ M3c: same-frame double render
// engine+0x7EB120 = RenderViews(ctx) — the render-thread call that produces the whole frame
// (proven: the camera-CB Map stack returns into its caller at engine+0x8045EF). Hooking it lets
// the mod run the frame render TWICE per tick. F11 modes:
//   0 = off (single render, passthrough)
//   1 = double-call, SAME camera (stability/perf trap: does the engine tolerate two renders?)
//   2 = double-call, LEFT/RIGHT eye cameras written to the render slot (impl+0x13A8) per pass
// RVA guarded by a byte signature — refuses to hook on a patched binary.
static float            g_halfIPD = 0.032f;        // live from located views once VR runs
static std::atomic<int> g_worldScale100{100};      // % scale on the world-units eye shift
static float            g_curShift = 0.f;           // this pass/frame's eye shift d (drives CB rewrite at Unmap)

// M4: 6DOF head-tracking state (defined early — DoubleRender freezes the per-frame pose)
static std::atomic<bool> g_head6dof{true};
static std::atomic<bool> g_headValid{false};
static float g_headQ[4] = {0,0,0,1};   // latest LOCATED HMD orientation (XR x,y,z,w), updated at BeginFrame
static float g_headP[3] = {0,0,0};     // latest located HMD position (XR meters, app space)
static float g_headUseQ[4] = {0,0,0,1};// frozen pose used for BOTH eyes of the current outer frame
static float g_headUseP[3] = {0,0,0};
static std::atomic<int> g_posScale100{100};   // % scale on head positional translation
// diagnostic isolation switches (find the stable combo, then lock it in)
static std::atomic<bool> g_bakeRot{true};      // bake head ROTATION into the rendered eye camera
static std::atomic<bool> g_bakePos{true};      // bake head POSITION (lean/translate) into the eye camera
static std::atomic<bool> g_tagHead{true};      // tag the submitted eye with the head pose (vs identity/world-locked)
// Confirmed in headset testing: the pose TAG uses the raw runtime quaternion, but
// the render bakes it through APP2W, which maps the right-handed XR basis onto the game camera
// basis with a handedness flip → the baked rotation is the INVERSE of the tag. Without this the
// world counter-tilts (≈double motion) on head tilt (uncomfortable). Conjugating the render quat realigns
// render with tag so both equal the true head orientation. END still toggles for other runtimes.
static std::atomic<bool> g_rollInvert{false};  // END: flip ONLY the roll sign (escape hatch; default correct)
// synthetic head-yaw sway (HOME in non-VR): drives the FULL 6DOF rewrite path with a sine yaw so
// the head-look injection is testable FLAT on the monitor — whatever fails to counter-swing there
// is the head-locked geometry class, no headset needed.
static std::atomic<bool> g_synthYaw{false};
// DIAGNOSTIC (NUM6): add an obvious extra ~30° yaw to the RENDERED eye camera only (NOT the pose
// tag) → geometry visibly swings. If lights/shadows/reflections swing WITH it, they read the 912
// camera (fix = a missing 912 field). If they stay put, their camera is reconstructed in-shader
// (as in some other deferred engines). Applied inside Build6DofEye so it flows into every 912/object rewrite.
static std::atomic<bool> g_diagYaw{false};
// M4v: temporarily put the CENTER-EYE head pose into the game's REAL IBaseCamera for the exact
// duration of the two render calls, then restore it before returning to simulation/input.
// This is intentionally separate from per-eye IPD, which stays in the proven late CB rewrite.
// Disabled after headset testing: SetInvCameraMatrix rejects the game's live camera implementation.
static std::atomic<bool> g_upstreamCam{false};
static std::atomic<bool> g_passClearReplay{false}; // diagnostic only; A/B testing showed no effect
static std::atomic<float> g_upInv[12], g_upInvPrev[12];
static std::atomic<bool> g_upInvValid{false}, g_upInvPrevValid{false};
static std::atomic<bool> g_upLiveApplied{false};
static void* g_upLiveCam = nullptr;
static float g_upLiveRestore[12] = {};
static std::atomic<uint32_t> g_cUpApply{0}, g_cUpFail{0}, g_cUpMatch{0}, g_cUpFallback{0};
static bool ApplyUpstreamHeadCamera();
static void RestoreUpstreamHeadCamera();
static void ClearUpstreamHeadCamera();
static void BeginPassClearIsolation();

static void QuatMul(const float* a, const float* b, float* o) {   // o = a * b
    o[0]=a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1];
    o[1]=a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0];
    o[2]=a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3];
    o[3]=a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2];
}
// A/B isolation toggles for the three 912 rewrite layers (NUMPAD1/2/3, all default ON):
// classic = view/invcam/comb@0x210 (buildings), comb0 = main viewproj @0x000 (terrain/skinned),
// sweep = generic campos/rot/rel-viewproj fields. Flip one during synth-yaw to see what it owns.
static std::atomic<bool> g_rwClassic{true}, g_rwComb0{true}, g_rwSweep{false};
static std::atomic<bool> g_rwCamPos{false}; // NUM9 A/B: this also corrupts the image/billboards

static void QuatRotate(const float* q, const float* v, float* out) {   // rotate v by quaternion q
    float x=q[0],y=q[1],z=q[2],w=q[3];
    float vx=v[0],vy=v[1],vz=v[2];
    float tx = 2.f*(y*vz - z*vy), ty = 2.f*(z*vx - x*vz), tz = 2.f*(x*vy - y*vx);
    out[0] = vx + w*tx + (y*tz - z*ty);
    out[1] = vy + w*ty + (z*tx - x*tz);
    out[2] = vz + w*tz + (x*ty - y*tx);
}
static const uint32_t kRenderViewsRVA = 0x7EB120;
static const uint8_t  kRenderViewsSig[12] = { 0x40,0x55,0x56,0x41,0x54,0x48,0x8D,0xAC,0x24,0x80,0xFD,0xFF };
static std::atomic<int> g_dblMode{0};
static std::atomic<uint32_t> g_dblFrames{0};
static std::atomic<int> g_renderSlotEye{-1};   // which eye's camera is in the render slot during a pass

alignas(64) static uint8_t g_eyeCamL[0x2000], g_eyeCamR[0x2000];
static bool g_eyePairBuilt = false;

static bool BuildEyePair() {   // render thread; clone live cam twice, shift ±halfIPD
    __try {
        void* cam = g_viewCam.load(std::memory_order_relaxed);
        if (!cam || !pCamCopyCtor || !pCopyCamParams || !pSetPosition || !pGetPosition || !pGetLeftVector) return false;
        if (!g_eyePairBuilt) {
            memset(g_eyeCamL, 0, sizeof(g_eyeCamL)); memset(g_eyeCamR, 0, sizeof(g_eyeCamR));
            pCamCopyCtor(g_eyeCamL, cam); pCamCopyCtor(g_eyeCamR, cam);
            g_eyePairBuilt = true;
            Log("[dbl] eye camera pair cloned");
        }
        float ipdHalf = g_halfIPD * (g_worldScale100.load(std::memory_order_relaxed) * 0.01f);
        for (int e = 0; e < 2; ++e) {
            uint8_t* c = e == 0 ? g_eyeCamL : g_eyeCamR;
            pCopyCamParams(c, cam);
            float pos[4] = {}, left[4] = {};
            pGetPosition(cam, pos); pGetLeftVector(cam, left);
            float s = (e == 0) ? +ipdHalf : -ipdHalf;      // LEFT eye = +left dir, RIGHT = -left
            pos[0] += left[0]*s; pos[1] += left[1]*s; pos[2] += left[2]*s;
            pSetPosition(c, pos);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool WriteRenderSlot(void* cam) {   // render thread: impl(+0x38) + 0x13A8 = optional cam, render side
    __try {
        void* lvl = g_iLevel.load(std::memory_order_relaxed);
        if (!lvl) return false;
        uint8_t* impl = *(uint8_t**)((uint8_t*)lvl + 0x38);
        if (!impl) return false;
        *(void**)(impl + 0x13A8) = cam;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Both render branches of the tick fn (A=0x7EB120 menus/simple, B=0x8324D0 gameplay) get the same
// double-call treatment. Detours forward 4 register args untouched — arity-agnostic up to 4 args
// (stack args would not survive a double call; acceptable trap risk for a render kick).
typedef void (*PFN_Render4)(void* a, void* b, void* c, void* d);
static PFN_Render4 g_origRenderA = nullptr, g_origRenderB = nullptr;
static std::atomic<uint32_t> g_rvCallsA{0}, g_rvCallsB{0};
static std::atomic<unsigned long> g_tidPresent{0}, g_tidRenderB{0}, g_tidDraw{0};

static std::atomic<uint32_t> g_cDrawIdx{0};   // free-running per-frame DrawIndexed counter (reset at frame boundary)
static std::atomic<uint64_t> g_drawMono{0};   // MONOTONIC draw counter — NEVER reset (Present-immune measurement)
// draws-inside-this-call attribution: snapshot the global draw counter around orig().
// g_cDrawIdx is the free-running per-frame DrawIndexed counter (reset at frame boundary); reading
// it before/after a single orig() call shows how many scene draws THIS function issued — the
// number that decides whether the hook sits on the atomic render unit or a mere orchestrator.
static std::atomic<uint32_t> g_drawsIn1{0}, g_drawsIn2{0};   // draws inside call #1 / call #2 of last dbl frame
static std::atomic<uint64_t> g_dispMono{0};                  // MONOTONIC dispatch counter (compute passes)
static std::atomic<uint32_t> g_dispIn1{0}, g_dispIn2{0};     // dispatches inside call #1 / call #2 — do the
                                                             // deferred-lighting/SSR compute passes RE-RUN per eye?
static void PokeTrackedCBs(float ipd);   // defined after the D3D orig pointers
static void RefreshCamSnap();            // defined after CamSnap/ReadCamRaw
static uint32_t CallOnceMeasured(PFN_Render4 orig, void* a, void* b, void* c, void* d) {
    uint64_t before = g_drawMono.load(std::memory_order_relaxed);   // monotonic → immune to Present reset
    orig(a, b, c, d);
    return (uint32_t)(g_drawMono.load(std::memory_order_relaxed) - before);
}
static void DoubleRender(PFN_Render4 orig, void* a, void* b, void* c, void* d) {
    // pass-start camera refresh: the game just finished sim and will bake object MODELVIEWs from
    // its CURRENT camera. The frame-boundary snapshot is one frame stale while the player moves,
    // which made the ObjCamRewrite pair-match fail exactly during movement (intermittent head-lock
    // on dynamic objects). Same single thread as Present — safe; cheap (8 exported getter calls).
    RefreshCamSnap();
    int mode = g_dblMode.load(std::memory_order_relaxed);
    if (mode == 0) { g_renderSlotEye.store(-1); orig(a, b, c, d); return; }
    g_dblFrames.fetch_add(1, std::memory_order_relaxed);
    if (mode == 1) {                       // stability trap: two renders, same camera
        g_renderSlotEye.store(0); g_drawsIn1.store(CallOnceMeasured(orig, a, b, c, d));
        g_renderSlotEye.store(1); g_drawsIn2.store(CallOnceMeasured(orig, a, b, c, d));
        g_renderSlotEye.store(-1);
        return;
    }
    // mode 2: true same-frame stereo. RenderB = full frame (render+present); calling it twice
    // renders the SAME world state from two eye cameras. Drive each pass with the proven
    // CB-shift (view rewrite at Unmap); the nested Present of each pass is captured as its eye
    // in VrFrame. Signs match the earlier alternate-eye path (eye0 = -halfIPD).
    // Freeze ONE head pose for both eyes this outer frame (BeginFrame updates g_headQ/P between passes).
    memcpy(g_headUseQ, g_headQ, sizeof(g_headUseQ)); memcpy(g_headUseP, g_headP, sizeof(g_headUseP));
    // Engine-derived view data (billboards/LOD/shadows/reflections) must see head orientation before
    // it is constructed. Failure is non-fatal: every late rewrite below retains its old full-pose path.
    ApplyUpstreamHeadCamera();
    BeginPassClearIsolation();
    float h = g_halfIPD * (g_worldScale100.load(std::memory_order_relaxed) * 0.01f);
    // dirty-flagged secondary camera CBs (432/176) won't be re-uploaded by the game unless ITS
    // camera moved — poke the tracked ones with this pass's eye before rendering it.
    uint64_t d0 = g_dispMono.load(std::memory_order_relaxed);
    g_curShift = -h; PokeTrackedCBs(-h); g_renderSlotEye.store(0); g_drawsIn1.store(CallOnceMeasured(orig, a, b, c, d));
    uint64_t d1 = g_dispMono.load(std::memory_order_relaxed); g_dispIn1.store((uint32_t)(d1 - d0));
    g_curShift = +h; PokeTrackedCBs(+h); g_renderSlotEye.store(1); g_drawsIn2.store(CallOnceMeasured(orig, a, b, c, d));
    uint64_t d2 = g_dispMono.load(std::memory_order_relaxed); g_dispIn2.store((uint32_t)(d2 - d1));
    g_curShift = 0.f; g_renderSlotEye.store(-1);
    RestoreUpstreamHeadCamera();
}
static void HookedRenderA(void* a, void* b, void* c, void* d) {
    g_rvCallsA.fetch_add(1, std::memory_order_relaxed);
    DoubleRender(g_origRenderA, a, b, c, d);
}
static void HookedRenderB(void* a, void* b, void* c, void* d) {
    g_rvCallsB.fetch_add(1, std::memory_order_relaxed);
    g_tidRenderB.store(GetCurrentThreadId(), std::memory_order_relaxed);
    DoubleRender(g_origRenderB, a, b, c, d);
}

// ------------------------------------------------------------------ M3a: optional render camera probe
// F9 cycles: 0=off  1=ORC := live view camera (does a second view render at all?)
//            2=ORC := a CLONED camera, eye-shifted 6.4cm (would it be a usable second eye?)
// Clone = raw zeroed block + the engine's own exported copy-ctor from the live camera → legit object
// (vtable + params) without knowing the layout. Per-frame: CopyCameraParams + shifted SetPosition.
static std::atomic<int> g_orcMode{0};
alignas(64) static uint8_t g_eyeCamBlock[0x2000];   // generous: IBaseCamera is far smaller
static bool  g_eyeCamBuilt = false;

static bool OrcClear() {
    __try {
        void* lvl = g_iLevel.load(std::memory_order_relaxed);
        if (!pSetOrc || !lvl) return false;
        pSetOrc(lvl, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void* OrcReadback() {
    __try {
        void* lvl = g_iLevel.load(std::memory_order_relaxed);
        if (!pGetOrc || !lvl) return nullptr;
        return pGetOrc(lvl);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return (void*)(intptr_t)-1; }
}
static bool OrcTick() {   // present thread; runs only in mode 1/2
    __try {
        int mode = g_orcMode.load(std::memory_order_relaxed);
        void* lvl = g_iLevel.load(std::memory_order_relaxed);
        void* cam = g_viewCam.load(std::memory_order_relaxed);
        if (!pSetOrc || !lvl || !cam) return false;
        if (mode == 1) { pSetOrc(lvl, cam); return true; }
        if (!pCamCopyCtor || !pCopyCamParams || !pSetPosition || !pGetPosition || !pGetLeftVector) return false;
        if (!g_eyeCamBuilt) {
            memset(g_eyeCamBlock, 0, sizeof(g_eyeCamBlock));
            pCamCopyCtor(g_eyeCamBlock, cam);
            g_eyeCamBuilt = true;
            Log("[orc] eye camera cloned via engine copy-ctor at %p", (void*)g_eyeCamBlock);
        }
        pCopyCamParams(g_eyeCamBlock, cam);
        float pos[4] = {}, left[4] = {};
        pGetPosition(cam, pos);
        pGetLeftVector(cam, left);
        pos[0] -= left[0] * 0.064f; pos[1] -= left[1] * 0.064f; pos[2] -= left[2] * 0.064f;  // 6.4cm to the RIGHT
        pSetPosition(g_eyeCamBlock, pos);
        pSetOrc(lvl, g_eyeCamBlock);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ------------------------------------------------------------------ camera state (read each frame, SEH-guarded)
struct CamSnap {
    float view[12];     // mtx34 raw
    float invCam[12];   // mtx34 raw
    float proj[16];     // mtx44 raw
    float comb[16];     // mtx44 raw
    float fov, aspect, zn, zf;
    bool  valid;
};
static CamSnap g_cam{};                 // present-thread reads/writes only (no lock needed)
static std::atomic<uint32_t> g_camReadOk{0}, g_camReadFail{0};

static bool ReadCamRaw(void* cam, CamSnap* out) {
    __try {
        const float* v = pGetViewMatrix   ? pGetViewMatrix(cam)   : nullptr;
        const float* i = pGetInvCamMatrix ? pGetInvCamMatrix(cam) : nullptr;
        const float* p = pGetProjMatrix   ? pGetProjMatrix(cam)   : nullptr;
        const float* c = pGetCombMatrix   ? pGetCombMatrix(cam)   : nullptr;
        if (!v || !p) return false;
        memcpy(out->view, v, 48);
        if (i) memcpy(out->invCam, i, 48); else memset(out->invCam, 0, 48);
        memcpy(out->proj, p, 64);
        if (c) memcpy(out->comb, c, 64); else memset(out->comb, 0, 64);
        out->fov    = pGetFOV      ? pGetFOV(cam)      : 0.f;
        out->aspect = pGetAspect   ? pGetAspect(cam)   : 0.f;
        out->zn     = pGetClipNear ? pGetClipNear(cam) : 0.f;
        out->zf     = pGetClipFar  ? pGetClipFar(cam)  : 0.f;
        out->valid = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void RefreshCamSnap() {                       // render/present thread only (same thread)
    void* cam = g_viewCam.load(std::memory_order_relaxed);
    if (!cam) return;
    CamSnap cs{};
    if (ReadCamRaw(cam, &cs)) { g_cam = cs; g_camReadOk.fetch_add(1, std::memory_order_relaxed); }
    else                       g_camReadFail.fetch_add(1, std::memory_order_relaxed);
}

// ------------------------------------------------------------------ fingerprint refs (built per armed frame)
// Layout-agnostic: match raw float runs AND their transposes.
struct FpRef { const char* name; float f[16]; int n; };
static FpRef g_refs[8];
static int   g_nRefs = 0;
static void T34(const float* m, float* out) {   // 3 rows x 4 cols -> 4 rows x 3 cols
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) out[c*3 + r] = m[r*4 + c];
}
static void T44(const float* m, float* out) {
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) out[c*4 + r] = m[r*4 + c];
}
static float AbsSum(const float* f, int n) { float s = 0; for (int i = 0; i < n; ++i) s += fabsf(f[i]); return s; }
static void BuildRefs(const CamSnap& cs) {
    g_nRefs = 0;
    auto add = [&](const char* nm, const float* f, int n) {
        if (g_nRefs >= 8) return;
        if (AbsSum(f, n) < 1.0f) return;                    // skip empty/degenerate
        FpRef& r = g_refs[g_nRefs++]; r.name = nm; r.n = n; memcpy(r.f, f, n*4);
    };
    float t[16];
    add("view34",   cs.view, 12);  T34(cs.view, t);   add("view34_T",  t, 12);
    add("invcam34", cs.invCam, 12);T34(cs.invCam, t); add("invcam34_T",t, 12);
    add("proj44",   cs.proj, 16);  T44(cs.proj, t);   add("proj44_T",  t, 16);
    add("comb44",   cs.comb, 16);  T44(cs.comb, t);   add("comb44_T",  t, 16);
}
static bool MatchAt(const float* ref, int n, const float* p) {
    for (int i = 0; i < n; ++i) {
        float a = ref[i], b = p[i];
        float tol = 1e-3f + 1e-3f * fabsf(a);
        if (fabsf(a - b) > tol) return false;
    }
    return true;
}

// ------------------------------------------------------------------ M1: eye-shift injection
// The camera CB (M0-proven, game v1.28.3): 912 bytes, Map/Unmap-streamed, fixed layout:
//   0x040 view mtx34 | 0x0A0,0x110,0x140,0x170,0x1A0 invcam mtx34 x5 | 0x210 viewproj mtx44
//   0x250 view#2 mtx34 | 0x280 invcam | 0x2B0 proj mtx44
// At Unmap the module shifts the eye d meters along camera-right in every matrix, and rebuild
// viewproj = proj (x) view' with the game's OWN proj (jitter preserved). Gate: proj block
// classifies as perspective ('P') — shadow/ortho/UI CBs fail the gate untouched.
enum : UINT { CB_VIEW = 0x40, CB_VIEW2 = 0x250, CB_COMB = 0x210, CB_PROJ = 0x2B0 };
// 0x110/0x140/0x170/0x1A0 were "invcam copies" in the M0 fingerprint but hold SHADOW-CASCADE
// matrices in the 2026-07-19 dump912 — the region is scene-dependent. They are no longer written
// unconditionally; the generic Sweep912 rewrites them only when they actually contain invcam.
static const UINT kInvCamOffs[] = { 0xA0, 0x280 };
static const UINT kCamCBSize = 912;

static std::atomic<bool>  g_injectOn{false};
static std::atomic<bool>  g_swayOn{true};        // sway = visible proof mode (sine, ~1.5s period)
static std::atomic<int>   g_shiftCm{15};         // shift magnitude in cm (DL2 world units = meters)
static std::atomic<uint32_t> g_cRewrite{0};
static uint32_t           g_lRewrite = 0;
static std::atomic<int>   g_prodOrder{0};        // viewproj convention: 0=unknown 1=P*V 2=V*P

// ------------------------------------------------------------------ M2: OpenXR output (alternate-eye path + same-frame capture)
// Alternate-eye: frame N renders with the eye shift for eye e (M1 CB rewrite), captured at Present,
// submitted together with the 1-frame-stale other eye. Each eye is pose-tagged with what it was
// RENDERED as: fixed orientation (recentered forward), position = ±halfIPD on X, fov = the game's
// own projection that frame (m00/m11 → half-angles). World-locked stereo window; head-look = M2b.
static std::atomic<bool>  g_vrOn{false};
static bool               g_xrInitTried = false;
static int                g_renderedEye = 0;         // eye the CURRENT game frame renders for
static ID3D11Texture2D*   g_eyeTex[2] = {};
static bool               g_eyeSeeded[2] = {};
static DL2Xr::Pose        g_tagPose[2] = {};
static DL2Xr::Fov         g_tagFov[2]  = {};
static ID3D11DeviceContext* g_vrCtx = nullptr;
static std::atomic<uint32_t> g_vrSubmits{0};

static bool CopyGuarded(ID3D11Texture2D* dst, ID3D11Texture2D* src) {
    __try { g_vrCtx->CopyResource(dst, src); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void ReleaseEyeTex() {
    for (int e = 0; e < 2; ++e) { if (g_eyeTex[e]) { g_eyeTex[e]->Release(); g_eyeTex[e] = nullptr; } g_eyeSeeded[e] = false; }
}
static void EnsureEyeTex(ID3D11Texture2D* bb) {
    D3D11_TEXTURE2D_DESC bd{}; bb->GetDesc(&bd);
    if (g_eyeTex[0]) {
        D3D11_TEXTURE2D_DESC ed{}; g_eyeTex[0]->GetDesc(&ed);
        if (ed.Width == bd.Width && ed.Height == bd.Height && ed.Format == bd.Format) return;
        ReleaseEyeTex();
    }
    ID3D11Device* dev = nullptr; bb->GetDevice(&dev);
    if (!dev) return;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = bd.Width; td.Height = bd.Height; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = bd.Format; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    for (int e = 0; e < 2; ++e)
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_eyeTex[e]))) { Log("[vr] eyeTex%d create FAILED", e); ReleaseEyeTex(); break; }
    dev->Release();
    if (g_eyeTex[0]) Log("[vr] eye capture textures %ux%u fmt=%d", bd.Width, bd.Height, (int)bd.Format);
}
static void VrFrame(IDXGISwapChain* sc) {
    if (!g_xrInitTried) {
        g_xrInitTried = true;
        ID3D11Device* dev = nullptr;
        DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(sc->GetDesc(&d)) && SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) && dev) {
            DL2Xr::SetSourceFormat((int64_t)d.BufferDesc.Format);
            DL2Xr::SetEyeResolution(d.BufferDesc.Width, d.BufferDesc.Height);
            dev->GetImmediateContext(&g_vrCtx);
            DL2Xr::Init(dev);
            Log("[vr] XR init kicked off (bb %ux%u fmt=%d)", d.BufferDesc.Width, d.BufferDesc.Height, (int)d.BufferDesc.Format);
            dev->Release();
        } else Log("[vr] swapchain device/desc FAILED — VR unavailable");
    }
    // ---- same-frame stereo path (mode 2): two presents per tick, one per eye (CB-shift driven) ----
    int slotEye = g_renderSlotEye.load(std::memory_order_relaxed);
    if (g_dblMode.load(std::memory_order_relaxed) == 2) {
        if (slotEye < 0) return;                               // stray present outside a pass — ignore
        if (slotEye == 0) {
            if (!DL2Xr::BeginFrame()) return;                   // one XR frame spans both eye presents
            DL2Xr::Pose hp{};                                   // cache head pose for NEXT frame's CB rewrite (1-frame latent)
            if (DL2Xr::GetHeadPose(&hp)) {
                g_headQ[0]=hp.orientation[0]; g_headQ[1]=hp.orientation[1]; g_headQ[2]=hp.orientation[2]; g_headQ[3]=hp.orientation[3];
                g_headP[0]=hp.position[0]; g_headP[1]=hp.position[1]; g_headP[2]=hp.position[2];
                g_headValid.store(true, std::memory_order_relaxed);
            }
        }
        ID3D11Texture2D* bbs = nullptr;
        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bbs)) && bbs) {
            EnsureEyeTex(bbs);
            if (g_eyeTex[slotEye] && g_vrCtx && CopyGuarded(g_eyeTex[slotEye], bbs)) {
                float ipdOff = (slotEye == 0 ? -1.f : +1.f) * g_halfIPD;
                // pose-tagging is sacred: tag ONLY what was actually rendered. With bakeRot off the
                // render used identity orientation, so the tag must be the world-locked window —
                // tagging the head pose then LIES to the compositor and glues the image to the face.
                if (g_head6dof.load(std::memory_order_relaxed) && g_headValid.load(std::memory_order_relaxed)
                    && g_tagHead.load(std::memory_order_relaxed) && g_bakeRot.load(std::memory_order_relaxed)) {
                    // 6DOF: the FROZEN head pose is baked into the render, so tag the eye with that same
                    // pose (orientation = head quat, position = head pos + IPD along head-right in XR space).
                    // Truthful tag → compositor reprojects only for latency, never double-applies rotation.
                    float er[3]; const float xr[3]={1,0,0}; QuatRotate(g_headUseQ, xr, er);
                    g_tagPose[slotEye] = DL2Xr::Pose{
                        { g_headUseQ[0], g_headUseQ[1], g_headUseQ[2], g_headUseQ[3] },
                        { g_headUseP[0] + er[0]*ipdOff, g_headUseP[1] + er[1]*ipdOff, g_headUseP[2] + er[2]*ipdOff } };
                } else {
                    g_tagPose[slotEye] = DL2Xr::Pose{ {0.f,0.f,0.f,1.f}, {ipdOff, 0.f, 0.f} };  // world-locked window
                }
                float m00 = g_cam.valid ? g_cam.proj[0] : 0.f, m11 = g_cam.valid ? g_cam.proj[5] : 0.f;
                if (m00 > 0.01f && m11 > 0.01f) {
                    float hh = atanf(1.f/m00), vv = atanf(1.f/m11);
                    g_tagFov[slotEye] = DL2Xr::Fov{ -hh, hh, vv, -vv };
                    g_eyeSeeded[slotEye] = true;
                }
            }
            bbs->Release();
        }
        if (slotEye == 1) {
            if (g_eyeSeeded[0] && g_eyeSeeded[1]) {
                DL2Xr::SubmitEyesPosed(g_eyeTex[0], g_eyeTex[1], g_tagPose, g_tagFov);
                g_vrSubmits.fetch_add(1, std::memory_order_relaxed);
            } else DL2Xr::SubmitEyesPosed(nullptr, nullptr, nullptr, nullptr);
            DL2Xr::Pose pl{}, pr{};
            if (DL2Xr::GetEyeView(0, &pl, nullptr) && DL2Xr::GetEyeView(1, &pr, nullptr)) {
                float dx = pr.position[0]-pl.position[0], dy = pr.position[1]-pl.position[1], dz = pr.position[2]-pl.position[2];
                float ipd = sqrtf(dx*dx + dy*dy + dz*dz);
                if (ipd > 0.03f && ipd < 0.09f) g_halfIPD = 0.5f * ipd;
            }
        }
        return;
    }

    if (!DL2Xr::BeginFrame()) { g_curShift = 0.f; return; }     // session not running yet / skip-frame

    // capture the frame JUST rendered (it used g_curShift for g_renderedEye)
    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb) {
        EnsureEyeTex(bb);
        int e = g_renderedEye;
        if (g_eyeTex[e] && g_vrCtx && CopyGuarded(g_eyeTex[e], bb)) {
            float off = (e == 0 ? -1.f : +1.f) * g_halfIPD;
            g_tagPose[e] = DL2Xr::Pose{ {0.f, 0.f, 0.f, 1.f}, {off, 0.f, 0.f} };
            float m00 = g_cam.valid ? g_cam.proj[0] : 0.f, m11 = g_cam.valid ? g_cam.proj[5] : 0.f;
            if (m00 > 0.01f && m11 > 0.01f) {
                float hh = atanf(1.f / m00), vv = atanf(1.f / m11);
                g_tagFov[e] = DL2Xr::Fov{ -hh, hh, vv, -vv };
                g_eyeSeeded[e] = true;                          // only submit eyes with a truthful fov tag
            }
        }
        bb->Release();
    }
    if (g_eyeSeeded[0] && g_eyeSeeded[1]) {
        DL2Xr::SubmitEyesPosed(g_eyeTex[0], g_eyeTex[1], g_tagPose, g_tagFov);
        g_vrSubmits.fetch_add(1, std::memory_order_relaxed);
    } else
        DL2Xr::SubmitEyesPosed(nullptr, nullptr, nullptr, nullptr);   // pairs begin<->end, 0 layers

    // next frame renders the other eye; refresh live IPD from the located views
    g_renderedEye ^= 1;
    DL2Xr::Pose pl{}, pr{};
    if (DL2Xr::GetEyeView(0, &pl, nullptr) && DL2Xr::GetEyeView(1, &pr, nullptr)) {
        float dx = pr.position[0] - pl.position[0], dy = pr.position[1] - pl.position[1], dz = pr.position[2] - pl.position[2];
        float ipd = sqrtf(dx*dx + dy*dy + dz*dz);
        if (ipd > 0.03f && ipd < 0.09f) g_halfIPD = 0.5f * ipd;
    }
    g_curShift = (g_renderedEye == 0 ? -1.f : +1.f) * g_halfIPD * (g_worldScale100.load(std::memory_order_relaxed) * 0.01f);
}

static void Mat4Mul(const float* A, const float* B, float* C) {
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) {
        float s = 0.f; for (int k = 0; k < 4; ++k) s += A[i*4+k] * B[k*4+j];
        C[i*4+j] = s;
    }
}
static void Expand34(const float* v, float* out) {   // mtx34 (3 rows x 4 cols) -> 4x4, bottom row 0001
    memcpy(out, v, 48);
    out[12] = 0.f; out[13] = 0.f; out[14] = 0.f; out[15] = 1.f;
}
static int DetectProdOrder(const float* f) {         // which product reproduces the CB's own comb?
    float V4[16], A[16], B[16];
    Expand34(f + CB_VIEW/4, V4);
    const float* P = f + CB_PROJ/4;
    const float* C = f + CB_COMB/4;
    Mat4Mul(P, V4, A);
    Mat4Mul(V4, P, B);
    if (MatchAt(C, 16, A)) return 1;
    if (MatchAt(C, 16, B)) return 2;
    return 0;
}
static bool IsPerspBlock(const float* m) {                       // perspective proj: w-row from z, no affine w
    bool rm = (fabsf(fabsf(m[11]) - 1.f) < 0.01f || fabsf(m[11]) > 0.5f) && m[15] == 0.f;
    bool cm = (fabsf(fabsf(m[14]) - 1.f) < 0.01f || fabsf(m[14]) > 0.5f) && m[15] == 0.f;
    return rm || cm;
}
// ---------------- M4: 6DOF head tracking ----------------
// Convention CONFIRMED analytically from the M1 shift: the CB's invcam block (camera->world) has
// columns [right | up | fwd | pos] laid out as invcam[row*4 + col], so col0=(ic0,ic4,ic8)=right_world,
// col1=up, col2=fwd, col3=pos. The view block (world->camera) rows are those basis vectors, with
// translation m3/m7/m11 = -basis·pos. Read the live basis+pos from invcam@0xA0, rotate the basis by
// the HMD head orientation and offset the position by head translation + IPD, then rebuild view/invcam/comb.
// Build a head-posed eye camera-to-world basis+pos in WORLD space from the live camera basis (R,U,F,C)
// and the cached HMD head pose. app-space axes map to the camera frame: appX->R, appY->U, app(-Z)->F.
static void Build6DofEye(const float* R, const float* U, const float* F, const float* C,
                         float ipd, float* eR, float* eU, float* eF, float* eC) {
    // head basis in app frame (right,up,fwd) = head rotation applied to app axes
    float aR[3], aU[3], aF[3];
    const float ax[3]={1,0,0}, ay[3]={0,1,0}, afwd[3]={0,0,-1};
    float q[4] = { g_headUseQ[0], g_headUseQ[1], g_headUseQ[2], g_headUseQ[3] };
    if (!g_bakeRot.load(std::memory_order_relaxed)) { q[0]=q[1]=q[2]=0.f; q[3]=1.f; }   // rotation off → identity
    else {
        // The APP2W map into the game-camera basis (world = R·vx + U·vy + F·(−vz)) is a REFLECTION:
        // it inverts yaw+pitch but PRESERVES roll. So to make - Hinny that GOAT - the render match the head, negate
        // ONLY the pitch(x)/yaw(y) quat components; roll(z) must stay as-is. The earlier full
        // conjugate also negated z → head-roll rolled the world the SAME way (double rate, uncomfortable).
        q[0] = -q[0];                                            // pitch: cancel reflection
        q[1] = -q[1];                                            // yaw:   cancel reflection
        if (g_rollInvert.load(std::memory_order_relaxed)) q[2] = -q[2];   // roll escape hatch (END); default correct
        if (g_diagYaw.load(std::memory_order_relaxed)) {         // +30° yaw (render only) — lighting-source test
            float y[4] = {0.f, 0.2588f, 0.f, 0.9659f}, r[4];     // yaw 30° about app-up
            QuatMul(q, y, r); q[0]=r[0]; q[1]=r[1]; q[2]=r[2]; q[3]=r[3];
        }
    }
    QuatRotate(q, ax, aR); QuatRotate(q, ay, aU); QuatRotate(q, afwd, aF);
    // map an app-frame vector (vx,vy,vz) into world via camera basis: world = R*vx + U*vy + F*(-vz)
    #define APP2W(v, o) do { for(int i=0;i<3;i++) (o)[i] = R[i]*(v)[0] + U[i]*(v)[1] + F[i]*(-(v)[2]); } while(0)
    APP2W(aR, eR); APP2W(aU, eU); APP2W(aF, eF);
    // head positional offset (app meters -> world), scaled; then IPD along the eye's own right
    float ps = g_bakePos.load(std::memory_order_relaxed) ? (g_posScale100.load(std::memory_order_relaxed) * 0.01f) : 0.f;
    float hp[3] = { g_headUseP[0]*ps, g_headUseP[1]*ps, g_headUseP[2]*ps };
    float off[3]; APP2W(hp, off);
    #undef APP2W
    for (int i=0;i<3;i++) eC[i] = C[i] + off[i] + eR[i]*ipd;
}
static void WriteView34(float* v, const float* R, const float* U, const float* F, const float* C) {
    v[0]=R[0]; v[1]=R[1]; v[2]=R[2];   v[3]  = -(R[0]*C[0]+R[1]*C[1]+R[2]*C[2]);
    v[4]=U[0]; v[5]=U[1]; v[6]=U[2];   v[7]  = -(U[0]*C[0]+U[1]*C[1]+U[2]*C[2]);
    v[8]=F[0]; v[9]=F[1]; v[10]=F[2];  v[11] = -(F[0]*C[0]+F[1]*C[1]+F[2]*C[2]);
}
static void WriteInvcam34(float* ic, const float* R, const float* U, const float* F, const float* C) {
    ic[0]=R[0]; ic[1]=U[0]; ic[2]=F[0]; ic[3]=C[0];   // row0: (Rx,Ux,Fx,Cx)
    ic[4]=R[1]; ic[5]=U[1]; ic[6]=F[1]; ic[7]=C[1];
    ic[8]=R[2]; ic[9]=U[2]; ic[10]=F[2]; ic[11]=C[2];
}

static void PublishUpstreamInv(const float* ic) {
    if (g_upInvValid.load(std::memory_order_acquire)) {
        for (int i=0;i<12;i++) g_upInvPrev[i].store(g_upInv[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
        g_upInvPrevValid.store(true, std::memory_order_release);
    }
    for (int i=0;i<12;i++) g_upInv[i].store(ic[i], std::memory_order_relaxed);
    g_upInvValid.store(true, std::memory_order_release);
}
static bool MatchUpstreamInv(const float* src, const std::atomic<float>* target) {
    // Engine camera constants can carry tiny normalization/jitter differences. Orientation must
    // agree to ~0.9 degree and position to 8 cm; both are required so a raw center camera is not
    // mistaken for the head camera when the headset has only translated.
    for (int r=0;r<3;r++) for (int c=0;c<3;c++) {
        int i=r*4+c; float b=target[i].load(std::memory_order_relaxed);
        if (fabsf(src[i]-b) > 0.015f) return false;
    }
    for (int r=0;r<3;r++) {
        int i=r*4+3; float b=target[i].load(std::memory_order_relaxed);
        if (fabsf(src[i]-b) > 0.08f) return false;
    }
    return true;
}
static bool SourceAlreadyHeadPosed(const float* R, const float* U, const float* F, const float* C) {
    float src[12]; WriteInvcam34(src,R,U,F,C);
    if (g_upInvValid.load(std::memory_order_acquire) && MatchUpstreamInv(src,g_upInv)) return true;
    return g_upInvPrevValid.load(std::memory_order_acquire) && MatchUpstreamInv(src,g_upInvPrev);
}
// Use the full head transform only when the source is still the game's raw camera. If upstream
// injection already supplied the center-eye head pose, the late path adds IPD only. This preserves
// the old working geometry path automatically when OptionalRenderCamera is ignored by a pass.
static void BuildEffectiveEye(const float* R, const float* U, const float* F, const float* C,
                              float ipd, float* eR, float* eU, float* eF, float* eC) {
    if (SourceAlreadyHeadPosed(R,U,F,C)) {
        for(int i=0;i<3;i++){ eR[i]=R[i]; eU[i]=U[i]; eF[i]=F[i]; eC[i]=C[i]+R[i]*ipd; }
        g_cUpMatch.fetch_add(1,std::memory_order_relaxed);
    } else {
        Build6DofEye(R,U,F,C,ipd,eR,eU,eF,eC);
        g_cUpFallback.fetch_add(1,std::memory_order_relaxed);
    }
}
static void RestoreUpstreamHeadCamera() {
    if (!g_upLiveApplied.exchange(false,std::memory_order_acq_rel)) return;
    __try {
        if (g_upLiveCam && pSetInvCameraMatrix)
            pSetInvCameraMatrix(g_upLiveCam,g_upLiveRestore);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_cUpFail.fetch_add(1,std::memory_order_relaxed);
    }
    g_upLiveCam=nullptr;
}
static void ClearUpstreamHeadCamera() {
    RestoreUpstreamHeadCamera();
    g_upInvValid.store(false,std::memory_order_release);
    g_upInvPrevValid.store(false,std::memory_order_release);
}
static bool ApplyUpstreamHeadCamera() {
    if (!g_upstreamCam.load(std::memory_order_relaxed) ||
        !g_vrOn.load(std::memory_order_relaxed) ||
        g_dblMode.load(std::memory_order_relaxed)!=2 ||
        !g_head6dof.load(std::memory_order_relaxed) ||
        !g_headValid.load(std::memory_order_relaxed)) {
        ClearUpstreamHeadCamera();
        return false;
    }
    __try {
        void* cam=g_viewCam.load(std::memory_order_relaxed);
        if (!cam || !pGetInvCamMatrix || !pSetInvCameraMatrix) {
            g_cUpFail.fetch_add(1,std::memory_order_relaxed); return false;
        }
        const float* ic=pGetInvCamMatrix(cam);
        if (!ic) { g_cUpFail.fetch_add(1,std::memory_order_relaxed); return false; }
        for(int i=0;i<12;i++) if (!std::isfinite(ic[i])) {
            g_cUpFail.fetch_add(1,std::memory_order_relaxed); return false;
        }
        memcpy(g_upLiveRestore,ic,sizeof(g_upLiveRestore));
        float R[3]={ic[0],ic[4],ic[8]}, U[3]={ic[1],ic[5],ic[9]},
              F[3]={ic[2],ic[6],ic[10]}, C[3]={ic[3],ic[7],ic[11]};
        float eR[3],eU[3],eF[3],eC[3], target[12];
        Build6DofEye(R,U,F,C,0.f,eR,eU,eF,eC);       // center eye: NO IPD upstream
        WriteInvcam34(target,eR,eU,eF,eC);
        g_upLiveCam=cam;
        g_upLiveApplied.store(true,std::memory_order_release);
        pSetInvCameraMatrix(cam,target);              // scoped mutation: restored after both eyes
        const float* actual=pGetInvCamMatrix(cam);
        if (!actual) {
            RestoreUpstreamHeadCamera();
            g_cUpFail.fetch_add(1,std::memory_order_relaxed);
            return false;
        }
        bool accepted=true;
        for(int i=0;i<12;i++) if (fabsf(actual[i]-target[i])>0.001f+0.001f*fabsf(target[i])) { accepted=false; break; }
        if (!accepted) {
            RestoreUpstreamHeadCamera();
            g_cUpFail.fetch_add(1,std::memory_order_relaxed);
            return false;
        }
        PublishUpstreamInv(actual);
        uint32_t n=g_cUpApply.fetch_add(1,std::memory_order_relaxed)+1;
        if (n==1) Log("[upcam] first SCOPED LIVE apply ok cam=%p pos=(%.3f,%.3f,%.3f)",cam,eC[0],eC[1],eC[2]);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RestoreUpstreamHeadCamera();
        g_cUpFail.fetch_add(1,std::memory_order_relaxed);
        return false;
    }
}

// The SECONDARY camera CBs. Root cause of the "terrain/NPCs/vehicles head-locked" bug (2026-07-19):
// these buffers are DIRTY-FLAGGED — the game only re-uploads them when ITS camera changes. The mod's
// head-look never changes the game camera, so with the player standing still they are never
// re-uploaded and an Unmap-time rewrite never runs (log: ONE [432] apply in ~3400 VR frames).
// Fix = track the camera-gated buffers (AddRef + shadow of the game's last content) and POKE them
// ourselves at the start of every eye pass with the current head pose (PokeTrackedCBs below).
//   432B: 0x120 = rel-world->view 3x4 (input anchored at the 0x150 world origin ≈ player root,
//                 ~1.6m below camera), 0x150 = view->world 3x4 with that anchor at col3.
//   176B: 0x20 = view-like 3x4 whose 3x3 EQUALS the API view rotation (FP-set find; layout dumped
//                 one-shot on first sighting — anchor auto-classified from |t|).
// Delta recipe (convention-independent, exact for any fixed post-transform K and any anchor A):
//   rigid center->eye motion: X' = ΔR·(X − Cc) + Ce   (ΔR = Be·Bc^T)
//   world->view [M|t]:  M' = M·ΔR^T,  t' = t + M·(Cc−A) − M'·(Ce−A)
//   view->world [N|c]:  N' = ΔR·N,    c' = ΔR·(c−Cc) + Ce
// ⚠ REFRAMED 2026-07-19 after the M4b telemetry run: up432 ≈ 1850/frame, x10 view-rot instances
// = the 432B class is a pooled PER-OBJECT constants buffer (scale block, hidden −1024 rows, world
// transform at 0x150), NOT a camera CB. The "camera-relative" matrices were the transforms of
// objects near the player (FP arms/weapon have world rotation == camera rotation). Same story for
// the 160/176B class. Delta-writing these rotates props around the player — harm, no fix — so the
// poke/apply toggles now default OFF (PGUP/PGDN re-enable for experiments only).
static const UINT kCamCB2Size = 432;
static const UINT kCam176Size = 176;
static std::atomic<uint32_t> g_432Logged{0}, g_176Logged{0};
static std::atomic<bool> g_poke432{false}, g_poke176{false};
static std::atomic<uint32_t> g_cUp432{0}, g_cRej432{0}, g_cUp176{0}, g_cRej176{0};
static std::atomic<uint32_t> g_cPokeOk{0}, g_cPokeFail{0};
static void Mat3x3(const float* A, const float* B, float* C) {  // C = A*B, row-major 3x3
    for (int i=0;i<3;i++) for (int j=0;j<3;j++) { float s=0; for(int k=0;k<3;k++) s+=A[i*3+k]*B[k*3+j]; C[i*3+j]=s; }
}
static void Get3(const float* row4, float* m) { m[0]=row4[0];m[1]=row4[1];m[2]=row4[2]; m[3]=row4[4];m[4]=row4[5];m[5]=row4[6]; m[6]=row4[8];m[7]=row4[9];m[8]=row4[10]; }
static void Put3(float* row4, const float* m) { row4[0]=m[0];row4[1]=m[1];row4[2]=m[2]; row4[4]=m[3];row4[5]=m[4];row4[6]=m[5]; row4[8]=m[6];row4[9]=m[7];row4[10]=m[8]; }
static void Mat3Vec(const float* M, const float* v, float* o) { for (int i=0;i<3;i++) o[i] = M[i*3]*v[0] + M[i*3+1]*v[1] + M[i*3+2]*v[2]; }

// center camera basis+pos from the live API snapshot, eye camera from head pose + ipd
static bool EyeFromSnap(float ipd, float* R, float* U, float* Fw, float* C,
                        float* eR, float* eU, float* eF, float* eC) {
    if (!g_cam.valid) return false;
    R[0]=g_cam.invCam[0]; R[1]=g_cam.invCam[4]; R[2]=g_cam.invCam[8];
    U[0]=g_cam.invCam[1]; U[1]=g_cam.invCam[5]; U[2]=g_cam.invCam[9];
    Fw[0]=g_cam.invCam[2]; Fw[1]=g_cam.invCam[6]; Fw[2]=g_cam.invCam[10];
    C[0]=g_cam.invCam[3]; C[1]=g_cam.invCam[7]; C[2]=g_cam.invCam[11];
    bool use6 = g_head6dof.load(std::memory_order_relaxed) && g_headValid.load(std::memory_order_relaxed);
    if (ipd == 0.f && !use6) return false;
    if (use6) BuildEffectiveEye(R,U,Fw,C, ipd, eR,eU,eF,eC);
    else { for(int i=0;i<3;i++){ eR[i]=R[i]; eU[i]=U[i]; eF[i]=Fw[i]; eC[i]=C[i]+R[i]*ipd; } }
    return true;
}
static void DeltaOf(const float* R, const float* U, const float* Fw,
                    const float* eR, const float* eU, const float* eF, float* dR, float* dRT) {
    float Be[9]={eR[0],eU[0],eF[0], eR[1],eU[1],eF[1], eR[2],eU[2],eF[2]};   // eye basis as columns
    float BcT[9]={R[0],R[1],R[2], U[0],U[1],U[2], Fw[0],Fw[1],Fw[2]};        // center basis transposed
    Mat3x3(Be, BcT, dR);                                                     // ΔR: center basis -> eye basis
    dRT[0]=dR[0];dRT[1]=dR[3];dRT[2]=dR[6]; dRT[3]=dR[1];dRT[4]=dR[4];dRT[5]=dR[7]; dRT[6]=dR[2];dRT[7]=dR[5];dRT[8]=dR[8];
}
// world->view block [M|t] at row4-layout p, input anchored at world point A: exact delta rewrite
static void DeltaWorldToView(float* p, const float* A, const float* C, const float* eC,
                             const float* dRT) {
    float M[9], Mn[9]; Get3(p, M); Mat3x3(M, dRT, Mn);
    float ca[3]={C[0]-A[0],C[1]-A[1],C[2]-A[2]}, ea[3]={eC[0]-A[0],eC[1]-A[1],eC[2]-A[2]};
    float mca[3], mea[3]; Mat3Vec(M, ca, mca); Mat3Vec(Mn, ea, mea);
    float tn[3] = { p[3]+mca[0]-mea[0], p[7]+mca[1]-mea[1], p[11]+mca[2]-mea[2] };
    Put3(p, Mn); p[3]=tn[0]; p[7]=tn[1]; p[11]=tn[2];
}
static bool Gate432(const float* f) {               // 0x150 col3 must sit near the live camera
    if (!g_cam.valid) return false;
    const float* w = f + 0x150/4;
    float dx=w[3]-g_cam.invCam[3], dy=w[7]-g_cam.invCam[7], dz=w[11]-g_cam.invCam[11];
    return dx*dx+dy*dy+dz*dz <= 4.0f;
}
static bool Gate176(const float* f) {               // 0x20 3x3 must equal the live view rotation
    if (!g_cam.valid) return false;
    const float* p = f + 0x20/4; const float* v = g_cam.view;
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) {
        float a = v[r*4+c], b = p[r*4+c];
        if (fabsf(a-b) > 1e-3f + 1e-3f*fabsf(a)) return false;
    }
    return true;
}
static void ApplyDelta432(float* f, float ipd) {
    float R[3],U[3],Fw[3],C[3], eR[3],eU[3],eF[3],eC[3];
    if (!EyeFromSnap(ipd, R,U,Fw,C, eR,eU,eF,eC)) return;
    float dR[9], dRT[9]; DeltaOf(R,U,Fw, eR,eU,eF, dR,dRT);
    float* vv = f + 0x120/4;
    float* w  = f + 0x150/4;
    float A[3] = { w[3], w[7], w[11] };            // input anchor = 0x150 world origin (player root)
    DeltaWorldToView(vv, A, C, eC, dRT);
    // 0x150 view->world: N' = ΔR·N, c' = ΔR·(c−Cc)+Ce  (c == A)
    float N[9], Nn[9]; Get3(w, N); Mat3x3(dR, N, Nn); Put3(w, Nn);
    float rel[3]={A[0]-C[0],A[1]-C[1],A[2]-C[2]}, rr[3]; Mat3Vec(dR, rel, rr);
    w[3]=rr[0]+eC[0]; w[7]=rr[1]+eC[1]; w[11]=rr[2]+eC[2];
    if (g_432Logged.fetch_add(1) == 0)
        Log("[432] first delta: A=(%.2f,%.2f,%.2f) C=(%.2f,%.2f,%.2f) eC=(%.2f,%.2f,%.2f) ipd=%.3f",
            A[0],A[1],A[2], C[0],C[1],C[2], eC[0],eC[1],eC[2], ipd);
}
static void ApplyDelta176(float* f, float ipd) {
    float R[3],U[3],Fw[3],C[3], eR[3],eU[3],eF[3],eC[3];
    if (!EyeFromSnap(ipd, R,U,Fw,C, eR,eU,eF,eC)) return;
    float dR[9], dRT[9]; DeltaOf(R,U,Fw, eR,eU,eF, dR,dRT);
    float* p = f + 0x20/4;
    float M[9]; Get3(p, M);
    float t[3] = { p[3], p[7], p[11] };
    // anchor classification: big |t| ⇒ absolute world (A=0); small ⇒ camera-anchored, solve A
    // from "camera maps to output origin": A = Cc + M^T·t  (M orthonormal)
    float A[3];
    float tl2 = t[0]*t[0]+t[1]*t[1]+t[2]*t[2];
    if (tl2 > 900.f) { A[0]=A[1]=A[2]=0.f; }
    else for (int i=0;i<3;i++) A[i] = C[i] + M[0*3+i]*t[0] + M[1*3+i]*t[1] + M[2*3+i]*t[2];
    DeltaWorldToView(p, A, C, eC, dRT);
    if (g_176Logged.fetch_add(1) == 0)
        Log("[176] first delta: |t|^2=%.2f anchor=%s A=(%.2f,%.2f,%.2f) ipd=%.3f",
            tl2, tl2 > 900.f ? "ABSOLUTE" : "camera-solved", A[0],A[1],A[2], ipd);
}
// ---- M4g: per-object MODELVIEW rewrite (people / zombies / vehicles — every DYNAMIC object) ----
// Headset testing showed: with the 912 fully rewritten (rw0=0, comb0/pos ≈ every upload),
// moving entities STILL head-lock. Their camera is not in the 912 at all: the per-object CB holds
// the object's MODEL matrix M (object→world) AND a CPU-baked MODELVIEW = V·M computed from the
// GAME camera (the old "432 dump": 0x150 = M at the player's feet, 0x120 = V·M ≈ flip+tilt).
// Static world uses the shared camera fields (fixed); dynamic objects use this baked MV → always
// head-locked no matter what is done to the 912.
// Self-validating rewrite, no offsets assumed: for every plausible affine 3x4 block Y in the CB,
// compute Z = Vc∘Y (game view compose model). If another block X matches Z, X IS the modelview of
// model Y → X := Ve∘Y (eye view). Y itself (world transform) stays untouched. Also handles the
// identity-model case (X == Vc verbatim → X := Ve). Matching runs against a SNAPSHOT of the
// game-written content so replacements can't cascade. Exact for any object scale/layout.
static std::atomic<uint32_t> g_cObjMV{0};
static std::atomic<bool> g_rwObjMV{true};
static void Compose34(const float* A, const float* B, float* out) {   // out = A∘B (apply B, then A)
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c)
            out[r*4+c] = A[r*4+0]*B[0+c] + A[r*4+1]*B[4+c] + A[r*4+2]*B[8+c];
        out[r*4+3] = A[r*4+0]*B[3] + A[r*4+1]*B[7] + A[r*4+2]*B[11] + A[r*4+3];
    }
}
static bool PlausibleAffine34(const float* m) {    // sane row norms + near-orthogonal rows (scale ok)
    float n0=m[0]*m[0]+m[1]*m[1]+m[2]*m[2], n1=m[4]*m[4]+m[5]*m[5]+m[6]*m[6], n2=m[8]*m[8]+m[9]*m[9]+m[10]*m[10];
    if (n0<0.0025f||n0>100.f||n1<0.0025f||n1>100.f||n2<0.0025f||n2>100.f) return false;
    float d01=fabsf(m[0]*m[4]+m[1]*m[5]+m[2]*m[6]);
    return d01*d01 < 0.04f*n0*n1;
}
static const UINT kObjCBMin = 128, kObjCBMax = 4096;   // wide: vehicles/multi-part objects may use bigger CBs
// Render-pass camera cache, taken from the 912 CB CONTENT itself (game values, pre-rewrite).
// The API camera is double-buffered against the render side and runs ~1 frame apart during player
// motion (6cm / ~2° at walking speed) — which broke the object pair-match exactly while walking or
// turning. The 912 content IS the camera every draw in this pass was baked with; using it makes
// the object path share one source of truth with the (rock-solid) world path.
static bool  g_cbCamValid = false;
static float g_cbView[12], g_cbInv[12], g_cbProj[16], g_cbComb[16], g_cbComb0[16];   // render thread only
static void CacheCbCam(const float* f) {
    const float* v = f + CB_VIEW/4; const float* ic = f + 0xA0/4;
    // consistency gate: invcam must be the transpose companion of view (both current-camera)
    if (fabsf(ic[0]-v[0]) > 2e-3f || fabsf(ic[4]-v[1]) > 2e-3f || fabsf(ic[8]-v[2]) > 2e-3f) return;
    memcpy(g_cbView, v, 48); memcpy(g_cbInv, ic, 48);
    memcpy(g_cbProj, f + CB_PROJ/4, 64); memcpy(g_cbComb, f + CB_COMB/4, 64);
    memcpy(g_cbComb0, f + 0x000/4, 64);     // main reversed-Z viewproj (deferred/SSR often uses THIS)
    g_cbCamValid = true;
}
// general 4x4 inverse (row-major) — needed for the FX buffer's inverse-viewproj (depth→world)
static bool Mat4Inverse(const float* m, float* out) {
    float inv[16];
    inv[0]=m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];
    inv[4]=-m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
    inv[8]=m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];
    inv[12]=-m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
    inv[1]=-m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];
    inv[5]=m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10];
    inv[9]=-m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];
    inv[13]=m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
    inv[2]=m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];
    inv[6]=-m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
    inv[10]=m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];
    inv[14]=-m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
    inv[3]=-m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];
    inv[7]=m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
    inv[11]=-m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];
    inv[15]=m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];
    float det=m[0]*inv[0]+m[1]*inv[4]+m[2]*inv[8]+m[3]*inv[12];
    if (fabsf(det) < 1e-20f) return false;
    det=1.f/det; for (int i=0;i<16;i++) out[i]=inv[i]*det;
    return true;
}
// jitter-tolerant 3x4 compare: the game bakes MV with the TAA-JITTERED camera, so exact matching
// against the clean API view collapses whenever the jitter pattern exceeds ~1e-3 (measured in the
// 2026-07-20 log: objMV fell 20-40x in bursts = the intermittent head-lock). Rotation slack 4e-3,
// translation slack 5cm + 0.5%.
static bool MatchMV(const float* Z, const float* X) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) { float a=Z[r*4+c], b=X[r*4+c]; if (fabsf(a-b) > 0.004f + 0.004f*fabsf(a)) return false; }
        float a=Z[r*4+3], b=X[r*4+3]; if (fabsf(a-b) > 0.05f + 0.005f*fabsf(a)) return false;
    }
    return true;
}
static std::mutex g_objPairMx;
static uint64_t g_objPairSeen[12]; static int g_nObjPairSeen = 0;
static void LogObjPair(UINT bytes, UINT offX, UINT offY) {          // pin the layout in the log
    uint64_t key = ((uint64_t)bytes << 32) | (offX << 16) | offY;
    std::lock_guard<std::mutex> lk(g_objPairMx);
    for (int i = 0; i < g_nObjPairSeen; ++i) if (g_objPairSeen[i] == key) return;
    if (g_nObjPairSeen >= 12) return;
    g_objPairSeen[g_nObjPairSeen++] = key;
    Log("[objpair] size=%u MV@0x%X model@0x%X", bytes, offX, offY);
}
static void ObjCamRewrite(float* f, UINT bytes, float ipd) {
    if (!g_rwObjMV.load(std::memory_order_relaxed)) return;
    if (!g_cbCamValid) return;                       // no 912 seen yet this session
    // center camera from the CACHED 912 CONTENT (same pass, same camera the MVs were baked with)
    float R[3]={g_cbInv[0],g_cbInv[4],g_cbInv[8]}, U[3]={g_cbInv[1],g_cbInv[5],g_cbInv[9]},
          Fw[3]={g_cbInv[2],g_cbInv[6],g_cbInv[10]}, C[3]={g_cbInv[3],g_cbInv[7],g_cbInv[11]};
    bool use6 = g_head6dof.load(std::memory_order_relaxed) && g_headValid.load(std::memory_order_relaxed);
    if (ipd == 0.f && !use6) return;
    float eR[3],eU[3],eF[3],eC[3];
    if (use6) BuildEffectiveEye(R,U,Fw,C, ipd, eR,eU,eF,eC);
    else { for(int i=0;i<3;i++){ eR[i]=R[i]; eU[i]=U[i]; eF[i]=Fw[i]; eC[i]=C[i]+R[i]*ipd; } }
    const float* Vc = g_cbView;                     // 3x4 affine, exactly the compose convention
    float Ve[12]; WriteView34(Ve, eR,eU,eF,eC);
    // delta rewrite ΔV = Ve∘Vc^-1 (Vc^-1 == the cached invcam): X' = ΔV∘X keeps whatever jitter
    // the game baked into X. Identity when eye == center.
    float dV[12]; Compose34(Ve, g_cbInv, dV);
    float snap[kObjCBMax/4];                        // game-written content, frozen for matching
    UINT nf = bytes/4; if (nf > kObjCBMax/4) nf = kObjCBMax/4;
    memcpy(snap, f, nf*4);
    for (UINT i = 0; i + 12 <= nf; i += 4)          // identity-model case: block == view itself
        if (MatchMV(Vc, snap + i)) {
            float Xn[12]; Compose34(dV, snap + i, Xn); memcpy(f + i, Xn, 48);
            g_cObjMV.fetch_add(1, std::memory_order_relaxed);
        }
    for (UINT j = 0; j + 12 <= nf; j += 4) {
        const float* Y = snap + j;
        if (!PlausibleAffine34(Y)) continue;
        float Z[12]; Compose34(Vc, Y, Z);
        for (UINT i = 0; i + 12 <= nf; i += 4) {
            if (i == j || !MatchMV(Z, snap + i)) continue;
            float Xn[12]; Compose34(dV, snap + i, Xn);   // delta, not rebuild: jitter preserved
            memcpy(f + i, Xn, 48);
            g_cObjMV.fetch_add(1, std::memory_order_relaxed);
            LogObjPair(bytes, i*4, j*4);
        }
    }
}
// ---- M4n: screen-space FX camera rewrite (the 320B compute CB feeding SSR/SSAO/fog) ----
// Census verdict: every compute dispatch reads the 912 (rewritten) + a 320B CB in slot0 that holds
// no RAW view/proj (never fingerprinted) → it carries camera-DERIVED matrices: viewproj, INVERSE
// viewproj (depth→world), view, inverse view. The game builds them from ITS camera, so head-look
// never reaches screen-space → reflections/fog drift. Self-validating rewrite: build the game and
// eye versions of {comb, invcomb, view44, invcam44} (+transposes) and substitute any 4x4 block that
// matches the game version with the eye version. Offset-free; a full-16-float match is unambiguous.
static std::atomic<uint32_t> g_cFxCam{0};
static std::atomic<bool> g_rwFxCam{true};
static std::mutex g_fxSeenMx; static uint64_t g_fxSeen[16]; static int g_nFxSeen = 0;
static void LogFxHit(UINT bytes, UINT off, const char* what) {
    uint64_t key = ((uint64_t)bytes << 20) | (off << 4) | (what[0] & 0xF);
    std::lock_guard<std::mutex> lk(g_fxSeenMx);
    for (int i=0;i<g_nFxSeen;i++) if (g_fxSeen[i]==key) return;
    if (g_nFxSeen>=16) return; g_fxSeen[g_nFxSeen++]=key;
    Log("[fxcam] size=%u off=0x%X = %s", bytes, off, what);
}
static bool Match16(const float* a, const float* b) {
    for (int i=0;i<16;i++){ float t=1.5e-3f+1.5e-3f*fabsf(a[i]); if (fabsf(a[i]-b[i])>t) return false; } return true;
}
static void FxCamRewrite(float* f, UINT bytes) {
    if (!g_rwFxCam.load(std::memory_order_relaxed) || !g_cbCamValid) return;
    bool use6 = g_head6dof.load(std::memory_order_relaxed) && g_headValid.load(std::memory_order_relaxed);
    float ipd = g_curShift;
    if (ipd == 0.f && !use6) return;
    // center + eye camera from cached 912 content
    float R[3]={g_cbInv[0],g_cbInv[4],g_cbInv[8]}, U[3]={g_cbInv[1],g_cbInv[5],g_cbInv[9]},
          Fw[3]={g_cbInv[2],g_cbInv[6],g_cbInv[10]}, C[3]={g_cbInv[3],g_cbInv[7],g_cbInv[11]};
    float eR[3],eU[3],eF[3],eC[3];
    if (use6) BuildEffectiveEye(R,U,Fw,C, ipd, eR,eU,eF,eC);
    else { for(int i=0;i<3;i++){ eR[i]=R[i]; eU[i]=U[i]; eF[i]=Fw[i]; eC[i]=C[i]+R[i]*ipd; } }
    // 4x4 game/eye matrices
    float Vc4[16], Ve4[16], ICc4[16], ICe4[16];
    { float t[12]; memcpy(t, g_cbView, 48); Expand34(t, Vc4);
      WriteView34(t, eR,eU,eF,eC); Expand34(t, Ve4);
      memcpy(t, g_cbInv, 48); Expand34(t, ICc4);
      WriteInvcam34(t, eR,eU,eF,eC); Expand34(t, ICe4); }
    int order = g_prodOrder.load(std::memory_order_relaxed); if (!order) order = 1;
    // comb (0x210 jittered viewproj)
    float combC[16], combE[16];
    memcpy(combC, g_cbComb, 64);
    if (order == 1) Mat4Mul(g_cbProj, Ve4, combE); else Mat4Mul(Ve4, g_cbProj, combE);
    // comb0 (0x000 reversed-Z main viewproj): eye = game * W4, W4 = invcam_c ∘ view_e  (RewriteComb0 rule)
    float combC0[16], combE0[16], W4[16];
    memcpy(combC0, g_cbComb0, 64);
    Mat4Mul(ICc4, Ve4, W4); Mat4Mul(combC0, W4, combE0);
    float invCombC[16], invCombE[16], invC0C[16], invC0E[16];
    bool hcC = Mat4Inverse(combC, invCombC), hcE = Mat4Inverse(combE, invCombE);
    bool h0C = Mat4Inverse(combC0, invC0C), h0E = Mat4Inverse(combE0, invC0E);
    struct Pair { const float* from; const float* to; const char* nm; bool ok; };
    float combCT[16],combET[16],invCT[16],invET[16],VcT[16],VeT[16],ICcT[16],ICeT[16];
    float c0T[16],e0T[16],i0cT[16],i0eT[16];
    T44(combC,combCT);T44(combE,combET);T44(invCombC,invCT);T44(invCombE,invET);
    T44(Vc4,VcT);T44(Ve4,VeT);T44(ICc4,ICcT);T44(ICe4,ICeT);
    T44(combC0,c0T);T44(combE0,e0T);T44(invC0C,i0cT);T44(invC0E,i0eT);
    Pair pairs[] = {
        { combC, combE, "comb", true }, { combCT, combET, "combT", true },
        { combC0, combE0, "comb0", true }, { c0T, e0T, "comb0T", true },
        { invCombC, invCombE, "invcomb", hcC&&hcE }, { invCT, invET, "invcombT", hcC&&hcE },
        { invC0C, invC0E, "invcomb0", h0C&&h0E }, { i0cT, i0eT, "invcomb0T", h0C&&h0E },
        { Vc4, Ve4, "view", true }, { VcT, VeT, "viewT", true },
        { ICc4, ICe4, "invcam", true }, { ICcT, ICeT, "invcamT", true },
    };
    UINT nf = bytes/4; if (nf > 1024/4) nf = 1024/4;
    for (UINT i = 0; i + 16 <= nf; i += 4) {
        for (Pair& p : pairs) {
            if (!p.ok || !Match16(p.from, f + i)) continue;
            memcpy(f + i, p.to, 64);
            g_cFxCam.fetch_add(1, std::memory_order_relaxed);
            LogFxHit(bytes, i*4, p.nm);
            break;
        }
    }
}
// One-shot decoder: dump a 320B FX CB's raw floats + classify every 4-float-aligned 16-float window
// against the full camera-matrix candidate set. If NOTHING classifies, the buffer holds non-matrix
// camera data (frustum-corner rays / packed constants) and the raw floats allow decoding by hand.
static std::atomic<bool> g_dumpFx{false};
// One-shot dump of the small per-frame constants (16/32/48/64B) bound to every draw+dispatch — the
// camera FORWARD/POSITION vectors that view-dependent lighting/specular use likely live here (not as
// a full matrix, so FpScan can't see them). Print raw floats next to the game camera vectors so the
// camera-derived slots are identifiable by eye.
static std::atomic<uint32_t> g_dumpSmallMask{0};   // bit per size bucket 16/32/48/64 still to dump
static int SmallBucket(UINT b){ return b==16?0 : b==32?1 : b==48?2 : b==64?3 : -1; }
static void DumpSmallCam(const float* f, UINT bytes) {
    if (!g_cbCamValid) return;
    float R[3]={g_cbInv[0],g_cbInv[4],g_cbInv[8]}, U[3]={g_cbInv[1],g_cbInv[5],g_cbInv[9]},
          Fw[3]={g_cbInv[2],g_cbInv[6],g_cbInv[10]}, C[3]={g_cbInv[3],g_cbInv[7],g_cbInv[11]};
    Log("[smalldump] size=%u  (game: campos=%.3f,%.3f,%.3f fwd=%.4f,%.4f,%.4f right=%.4f,%.4f,%.4f up=%.4f,%.4f,%.4f)",
        bytes, C[0],C[1],C[2], Fw[0],Fw[1],Fw[2], R[0],R[1],R[2], U[0],U[1],U[2]);
    UINT nf = bytes/4;
    for (UINT o = 0; o < nf; o += 4)
        Log("  +0x%03X: % .5f % .5f % .5f % .5f", o*4, f[o], o+1<nf?f[o+1]:0, o+2<nf?f[o+2]:0, o+3<nf?f[o+3]:0);
}
static void DumpClassifyFx(const float* f, UINT bytes) {
    if (!g_cbCamValid) return;
    // rebuild the candidate references (game-side only; classification, not rewrite)
    float Vc4[16], ICc4[16]; { float t[12]; memcpy(t,g_cbView,48); Expand34(t,Vc4); memcpy(t,g_cbInv,48); Expand34(t,ICc4); }
    struct Ref { float m[16]; const char* nm; };
    Ref refs[8]; int nr=0;
    auto addr=[&](const float* m,const char* nm){ memcpy(refs[nr].m,m,64); refs[nr].nm=nm; nr++; };
    float ic[16]; Mat4Inverse(g_cbComb,ic);  float ic0[16]; Mat4Inverse(g_cbComb0,ic0); float ip[16]; Mat4Inverse(g_cbProj,ip);
    addr(g_cbComb,"comb"); addr(g_cbComb0,"comb0"); addr(g_cbProj,"proj");
    addr(ic,"invcomb"); addr(ic0,"invcomb0"); addr(ip,"invproj"); addr(Vc4,"view"); addr(ICc4,"invcam");
    Log("[fxdump] size=%u floats (game campos=%.2f,%.2f,%.2f):", bytes, g_cbInv[3],g_cbInv[7],g_cbInv[11]);
    UINT nf = bytes/4; if (nf > 320/4) nf = 320/4;
    for (UINT o = 0; o < nf; o += 4)
        Log("  +0x%03X: % .5f % .5f % .5f % .5f", o*4, f[o], f[o+1], f[o+2], f[o+3]);
    for (UINT i = 0; i + 16 <= nf; i += 4) {
        for (int r=0;r<nr;r++) {
            float tr[16]; T44(refs[r].m,tr);
            bool m = Match16(refs[r].m, f+i), mt = Match16(tr, f+i);
            if (m || mt) Log("[fxclass] +0x%03X == %s%s", i*4, refs[r].nm, mt?"(T)":"");
        }
    }
}
// ---- tracked-CB registry: last game-written content of every camera-gated 432/176 CB ----
static const int kMaxTrk = 8;
struct TrackedCB { ID3D11Buffer* buf; UINT size; uint8_t shadow[432]; };
static std::mutex  g_trkMx;
static TrackedCB   g_trk[kMaxTrk];
static int         g_nTrk = 0;
static void TrackCamCB(ID3D11Resource* res, const void* data, UINT size) {
    ID3D11Buffer* buf = (ID3D11Buffer*)res;
    std::lock_guard<std::mutex> lk(g_trkMx);
    for (int i = 0; i < g_nTrk; ++i)
        if (g_trk[i].buf == buf) { g_trk[i].size = size; memcpy(g_trk[i].shadow, data, size); return; }
    if (g_nTrk >= kMaxTrk) return;
    buf->AddRef();                                   // Mapped again later — keep it alive
    g_trk[g_nTrk].buf = buf; g_trk[g_nTrk].size = size;
    memcpy(g_trk[g_nTrk].shadow, data, size);
    ++g_nTrk;
    Log("[trk] tracking %uB camera CB %p (total %d)", size, (void*)buf, g_nTrk);
}
// ---- M4d: generic camera-field sweep inside the 912B CB ----
// Monitor synth-yaw test + census verdict (2026-07-19): terrain/NPC draws DO bind the 912 but do
// not rotate with the known view/invcam/comb fields → they consume OTHER camera fields in the
// unclassified regions (0x000-0x3F, 0x70-0xA0, 0x2F0-0x390) — camera-RELATIVE rendering for float
// precision (raw campos + translation-free viewproj). Instead of hardcoding offsets, every rewrite
// self-fingerprints the game content and replaces, anywhere in the buffer:
//   1. raw campos float3 (== C within 2cm)                → eye campos
//   2. 3x4 blocks whose 3x3 == view rotation (rows)        → eye view rotation (+ -Be·eC translation
//      when the old translation was exactly -Bc·C; else translation left alone)
//   3. 3x4 blocks whose 3x3 == invcam rotation (rows)      → eye invcam rotation (+ eC translation
//      when the old translation was campos)
//   4. 4x4 blocks == translation-free viewproj P*Vrot (or transpose) → eye version
// Known already-rewritten offsets are excluded. Per-field counters go to the [cbs] line so the log
// says whether these fields even exist (all-zero sweep = hypothesis dead → read [dump912]).
static std::atomic<uint32_t> g_cSwPos{0}, g_cSwView{0}, g_cSwInv{0}, g_cSwComb{0}, g_cSwC0{0};
static bool InKnown912(UINT fi) {                    // float index inside an explicitly-handled field?
    static const struct { UINT b, n; } k[] = {
        {0x00/4,16},                                                 // comb0 (RewriteComb0)
        {0x40/4,12},{0x250/4,12},                                    // view, view2
        {0xA0/4,12},{0x280/4,12},                                    // invcams (unconditional)
        {0x210/4,16},{0x2B0/4,16},                                   // comb, proj
    };
    for (auto& r : k) if (fi >= r.b && fi < r.b + r.n) return true;
    return false;
}
static bool Near3(const float* p, const float* v, float tol) {
    return fabsf(p[0]-v[0])<=tol && fabsf(p[1]-v[1])<=tol && fabsf(p[2]-v[2])<=tol;
}
static bool RotRowsMatch(const float* p, const float* r0, const float* r1, const float* r2) {
    auto m3 = [](const float* a, const float* b){
        for (int i=0;i<3;i++){ float t=1e-3f+1e-3f*fabsf(a[i]); if (fabsf(a[i]-b[i])>t) return false;} return true; };
    return m3(p, r0) && m3(p+4, r1) && m3(p+8, r2);
}
static void Sweep912(float* f,
                     const float* R, const float* U, const float* Fw, const float* C,
                     const float* eR, const float* eU, const float* eF, const float* eC, int order) {
    const UINT NF = 912/4;
    // (1) raw campos triples, any float alignment
    if (g_rwCamPos.load(std::memory_order_relaxed)) {
    for (UINT i = 0; i + 3 <= NF; ++i) {
        if (InKnown912(i) || InKnown912(i+2)) continue;
        if (!Near3(f+i, C, 0.02f)) continue;
        f[i]=eC[0]; f[i+1]=eC[1]; f[i+2]=eC[2];
        g_cSwPos.fetch_add(1, std::memory_order_relaxed);
        i += 2;
    }
    }
    if (!g_rwSweep.load(std::memory_order_relaxed)) return;
    // rows of the center matrices for the block matchers
    float vr0[3]={R[0],R[1],R[2]}, vr1[3]={U[0],U[1],U[2]}, vr2[3]={Fw[0],Fw[1],Fw[2]};          // view rows = basis vectors
    float ir0[3]={R[0],U[0],Fw[0]}, ir1[3]={R[1],U[1],Fw[1]}, ir2[3]={R[2],U[2],Fw[2]};          // invcam rows (basis as columns)
    // translation-free viewproj: game side and eye side (both possible orders + transpose)
    float V4r[16] = { R[0],R[1],R[2],0, U[0],U[1],U[2],0, Fw[0],Fw[1],Fw[2],0, 0,0,0,1 };
    float E4r[16] = { eR[0],eR[1],eR[2],0, eU[0],eU[1],eU[2],0, eF[0],eF[1],eF[2],0, 0,0,0,1 };
    const float* P = f + CB_PROJ/4;                  // game's own proj (pre-rewrite content: proj untouched anyway)
    float relG[16], relE[16], relGT[16], relET[16];
    if (order == 1) { Mat4Mul(P, V4r, relG); Mat4Mul(P, E4r, relE); }
    else            { Mat4Mul(V4r, P, relG); Mat4Mul(E4r, P, relE); }
    T44(relG, relGT); T44(relE, relET);
    for (UINT i = 0; i + 16 <= NF; i += 4) {         // 16B-aligned block scans
        if (InKnown912(i) || InKnown912(i+15)) continue;
        if (MatchAt(relG, 16, f+i))       { memcpy(f+i, relE, 64);  g_cSwComb.fetch_add(1, std::memory_order_relaxed); i += 12; continue; }
        if (MatchAt(relGT, 16, f+i))      { memcpy(f+i, relET, 64); g_cSwComb.fetch_add(1, std::memory_order_relaxed); i += 12; continue; }
        if (i + 12 <= NF && RotRowsMatch(f+i, vr0, vr1, vr2)) {
            // view-like: translation ≈ -Bc·C ? rewrite fully : rotation only
            float t[3] = { -(R[0]*C[0]+R[1]*C[1]+R[2]*C[2]), -(U[0]*C[0]+U[1]*C[1]+U[2]*C[2]), -(Fw[0]*C[0]+Fw[1]*C[1]+Fw[2]*C[2]) };
            bool fullT = fabsf(f[i+3]-t[0])<0.05f && fabsf(f[i+7]-t[1])<0.05f && fabsf(f[i+11]-t[2])<0.05f;
            float* p = f + i;
            p[0]=eR[0];p[1]=eR[1];p[2]=eR[2]; p[4]=eU[0];p[5]=eU[1];p[6]=eU[2]; p[8]=eF[0];p[9]=eF[1];p[10]=eF[2];
            if (fullT) { p[3]=-(eR[0]*eC[0]+eR[1]*eC[1]+eR[2]*eC[2]); p[7]=-(eU[0]*eC[0]+eU[1]*eC[1]+eU[2]*eC[2]); p[11]=-(eF[0]*eC[0]+eF[1]*eC[1]+eF[2]*eC[2]); }
            g_cSwView.fetch_add(1, std::memory_order_relaxed); i += 8; continue;
        }
        if (i + 12 <= NF && RotRowsMatch(f+i, ir0, ir1, ir2)) {
            float* p = f + i;
            float pt[3] = { p[3], p[7], p[11] };
            bool posT = Near3(pt, C, 0.05f);
            p[0]=eR[0];p[1]=eU[0];p[2]=eF[0]; p[4]=eR[1];p[5]=eU[1];p[6]=eF[1]; p[8]=eR[2];p[9]=eU[2];p[10]=eF[2];
            if (posT) { p[3]=eC[0]; p[7]=eC[1]; p[11]=eC[2]; }
            g_cSwInv.fetch_add(1, std::memory_order_relaxed); i += 8; continue;
        }
    }
}
// THE missed field (found via dump912, 2026-07-19): 0x000 = the MAIN full viewproj, a reversed-Z
// infinite-far variant (z-row = (0,0,0,zn)) with TAA jitter baked — never matched the API comb
// fingerprint, so every earlier pass ignored it. Terrain/skinned shaders read their MVP from here
// (buildings use the 0x210 family). Rewrite it convention-blind: right-multiply by the world-side
// rigid delta W4 = V4c^-1 · V4e (maps eye-world back to center-world), which preserves the
// projection convention AND the jitter exactly. Gate: its w-row must equal ±(game view row2).
static void RewriteComb0(float* f, const float* R, const float* U, const float* Fw, const float* C,
                         const float* eR, const float* eU, const float* eF, const float* eC) {
    const float* v = f + CB_VIEW/4;                  // still game content (called before WriteView34)
    float vr2[4] = { v[8], v[9], v[10], v[11] };
    const float* r3 = f + 12;
    auto near4 = [](const float* a, const float* b, float s){
        for (int i=0;i<4;i++){ float t=1e-3f+1e-3f*fabsf(b[i]); if (fabsf(a[i]-s*b[i])>t) return false;} return true; };
    if (!near4(r3, vr2, -1.f) && !near4(r3, vr2, 1.f)) return;
    float IC[16] = { R[0],U[0],Fw[0],C[0],  R[1],U[1],Fw[1],C[1],  R[2],U[2],Fw[2],C[2],  0,0,0,1 };
    float VE[16] = { eR[0],eR[1],eR[2],-(eR[0]*eC[0]+eR[1]*eC[1]+eR[2]*eC[2]),
                     eU[0],eU[1],eU[2],-(eU[0]*eC[0]+eU[1]*eC[1]+eU[2]*eC[2]),
                     eF[0],eF[1],eF[2],-(eF[0]*eC[0]+eF[1]*eC[1]+eF[2]*eC[2]),  0,0,0,1 };
    float W4[16], out[16];
    Mat4Mul(IC, VE, W4);                             // identity when eye == center
    Mat4Mul(f, W4, out);
    memcpy(f, out, 64);
    g_cSwC0.fetch_add(1, std::memory_order_relaxed);
}
static std::atomic<uint32_t> g_6dofLogged{0};
static bool CamRewrite6Dof(float* f, float ipd, int order) {
    // read live basis+pos from the primary invcam (col0=R,col1=U,col2=F,col3=C)
    float* ic0 = f + 0xA0/4;
    float R[3]={ic0[0],ic0[4],ic0[8]}, U[3]={ic0[1],ic0[5],ic0[9]}, Fw[3]={ic0[2],ic0[6],ic0[10]}, C[3]={ic0[3],ic0[7],ic0[11]};
    float eR[3],eU[3],eF[3],eC[3];
    BuildEffectiveEye(R,U,Fw,C, ipd, eR,eU,eF,eC);
    if (g_rwCamPos.load(std::memory_order_relaxed) ||
        g_rwSweep.load(std::memory_order_relaxed))
        Sweep912(f, R,U,Fw,C, eR,eU,eF,eC, order);   // camera-relative fields FIRST (matches game content)
    if (g_rwComb0.load(std::memory_order_relaxed))
        RewriteComb0(f, R,U,Fw,C, eR,eU,eF,eC);      // main terrain/skinned MVP at 0x000
    if (!g_rwClassic.load(std::memory_order_relaxed)) return true;
    WriteView34(f + CB_VIEW/4,  eR,eU,eF,eC);
    WriteView34(f + CB_VIEW2/4, eR,eU,eF,eC);
    for (UINT off : kInvCamOffs) WriteInvcam34(f + off/4, eR,eU,eF,eC);
    float V4[16], C4[16];
    Expand34(f + CB_VIEW/4, V4);
    if (order == 1) Mat4Mul(f + CB_PROJ/4, V4, C4); else Mat4Mul(V4, f + CB_PROJ/4, C4);
    memcpy(f + CB_COMB/4, C4, 64);
    if (g_6dofLogged.fetch_add(1) == 0)
        Log("[6dof] first rewrite: headP=(%.3f,%.3f,%.3f) headQ=(%.3f,%.3f,%.3f,%.3f) ipd=%.3f camC=(%.2f,%.2f,%.2f)",
            g_headUseP[0],g_headUseP[1],g_headUseP[2], g_headUseQ[0],g_headUseQ[1],g_headUseQ[2],g_headUseQ[3], ipd, C[0],C[1],C[2]);
    return true;
}

// one-shot full dump of a gate-passing 912B CB (armed by F7/auto-arm) — offline analysis fallback
// if the sweep counters come back zero.
static std::atomic<bool> g_dump912{false};
static void Dump912(const float* f) {
    Log("[dump912] full 912B CB floats (228) vs API:");
    for (UINT o = 0; o < 228; o += 4)
        Log("  +0x%03X: % .5f % .5f % .5f % .5f", o*4, f[o], f[o+1], f[o+2], f[o+3]);
    if (g_cam.valid) {
        Log("[dump912] API view r0=[% .4f % .4f % .4f % .4f] r1=[% .4f % .4f % .4f % .4f] r2=[% .4f % .4f % .4f % .4f]",
            g_cam.view[0],g_cam.view[1],g_cam.view[2],g_cam.view[3], g_cam.view[4],g_cam.view[5],g_cam.view[6],g_cam.view[7],
            g_cam.view[8],g_cam.view[9],g_cam.view[10],g_cam.view[11]);
        Log("[dump912] API campos=(%.4f,%.4f,%.4f) fov=%.2f zn=%.4f zf=%.1f aspect=%.4f",
            g_cam.invCam[3],g_cam.invCam[7],g_cam.invCam[11], g_cam.fov, g_cam.zn, g_cam.zf, g_cam.aspect);
    }
}
static std::atomic<uint32_t> g_cRw0{0};   // rotation-only rewrites (upload OUTSIDE an eye pass, d==0)
static void CamRewrite(float* f) {
    if (!IsPerspBlock(f + CB_PROJ/4)) return;                    // perspective main-camera gate
    float d = g_curShift;
    // d==0 means this upload happened OUTSIDE an eye pass (sim-phase prep: skinned characters,
    // CPU-built billboards, ...). In mode 2 the eye shift only exists inside the passes, so these
    // uploads used to be skipped entirely -> that geometry rendered with the GAME camera and was
    // head-locked in VR (while the monitor sway, nonzero all frame, hid the bug). With 6DOF active
    // they must still get the full head ROTATION - only the ipd offset is legitimately zero here.
    bool use6 = g_head6dof.load(std::memory_order_relaxed) && g_headValid.load(std::memory_order_relaxed);
    if (d == 0.f && !use6) return;
    // resolve viewproj product order once (needed by both paths)
    int order = g_prodOrder.load(std::memory_order_relaxed);
    if (order == 0) {
        order = DetectProdOrder(f);
        if (order) { g_prodOrder.store(order); Log("[inject] viewproj convention detected: %s", order==1 ? "P*V" : "V*P"); }
        else { Log("[inject] !! comb != P*V and != V*P — layout assumption wrong, skipping"); return; }
    }
    // 6DOF path: full head-posed eye camera (same-frame stereo passes carry the eye sign in g_curShift)
    if (use6) {
        CamRewrite6Dof(f, d, order);
        g_cRewrite.fetch_add(1, std::memory_order_relaxed);
        if (d == 0.f) g_cRw0.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // plain IPD path: sweep camera-relative fields with the shift-only eye first
    {
        float* ic0 = f + 0xA0/4;
        float R[3]={ic0[0],ic0[4],ic0[8]}, U[3]={ic0[1],ic0[5],ic0[9]}, Fw[3]={ic0[2],ic0[6],ic0[10]}, C[3]={ic0[3],ic0[7],ic0[11]};
        float eR[3]={R[0],R[1],R[2]}, eU[3]={U[0],U[1],U[2]}, eF[3]={Fw[0],Fw[1],Fw[2]};
        float eC[3]={C[0]+R[0]*d, C[1]+R[1]*d, C[2]+R[2]*d};
        if (g_rwCamPos.load(std::memory_order_relaxed) || g_rwSweep.load(std::memory_order_relaxed))
            Sweep912(f, R,U,Fw,C, eR,eU,eF,eC, order);
        if (g_rwComb0.load(std::memory_order_relaxed))  RewriteComb0(f, R,U,Fw,C, eR,eU,eF,eC);
    }
    if (!g_rwClassic.load(std::memory_order_relaxed)) { g_cRewrite.fetch_add(1, std::memory_order_relaxed); return; }
    // view copies: world->view, camera moves right by d  =>  view-space tx (m[3]) -= d
    f[CB_VIEW/4 + 3]  -= d;
    f[CB_VIEW2/4 + 3] -= d;
    // invcam copies: camera->world, C += d * right_world; right_world = (ic0, ic4, ic8)
    for (UINT off : kInvCamOffs) {
        float* ic = f + off/4;
        ic[3] += d * ic[0]; ic[7] += d * ic[4]; ic[11] += d * ic[8];
    }
    // rebuild viewproj from the game's own proj (jitter kept) and the shifted view
    float V4[16], C4[16];
    Expand34(f + CB_VIEW/4, V4);
    if (order == 1) Mat4Mul(f + CB_PROJ/4, V4, C4);
    else            Mat4Mul(V4, f + CB_PROJ/4, C4);
    memcpy(f + CB_COMB/4, C4, 64);
    g_cRewrite.fetch_add(1, std::memory_order_relaxed);
}

// ------------------------------------------------------------------ counters
static std::atomic<uint64_t> g_frame{0};
static std::atomic<uint32_t> g_cDraw{0}, g_cDrawIdxInst{0}, g_cDrawInst{0}, g_cDispatch{0};
static std::atomic<uint32_t> g_cUpdCB{0}, g_cMapCB{0}, g_cRTSet{0}, g_cVSSet{0}, g_cPSSet{0};
static uint32_t g_lDrawIdx=0, g_lDraw=0, g_lDrawIdxInst=0, g_lDrawInst=0, g_lDispatch=0;
static uint32_t g_lUpdCB=0, g_lMapCB=0, g_lRTSet=0, g_lVSSet=0, g_lPSSet=0;
static volatile bool g_perFrameLine = false;

// armed-frame state
static std::atomic<bool> g_fpArm{false};       // arm request (key/auto)
static std::atomic<bool> g_fpActive{false};    // scanning THIS frame
static std::atomic<uint32_t> g_fpHits{0}, g_fpScanned{0};
static bool g_autoArmed = false;

// ------------------------------------------------------------------ original D3D fn pointers
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef void  (STDMETHODCALLTYPE* PFN_DrawIndexed)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void  (STDMETHODCALLTYPE* PFN_Draw)(ID3D11DeviceContext*, UINT, UINT);
typedef void  (STDMETHODCALLTYPE* PFN_DrawIdxInst)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
typedef void  (STDMETHODCALLTYPE* PFN_DrawInst)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
typedef void  (STDMETHODCALLTYPE* PFN_Dispatch)(ID3D11DeviceContext*, UINT, UINT, UINT);
typedef void  (STDMETHODCALLTYPE* PFN_SetCBs)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
typedef void  (STDMETHODCALLTYPE* PFN_VSSetShader)(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT);
typedef void  (STDMETHODCALLTYPE* PFN_PSSetShader)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
typedef void  (STDMETHODCALLTYPE* PFN_CSSetShader)(ID3D11DeviceContext*, ID3D11ComputeShader*, ID3D11ClassInstance* const*, UINT);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateVS)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreatePS)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateCS)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11ComputeShader**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Map)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
typedef void  (STDMETHODCALLTYPE* PFN_Unmap)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
typedef void  (STDMETHODCALLTYPE* PFN_UpdateSubres)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
typedef void  (STDMETHODCALLTYPE* PFN_OMSetRTs)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
typedef void  (STDMETHODCALLTYPE* PFN_ClearRTV)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
typedef void  (STDMETHODCALLTYPE* PFN_ClearDSV)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);

static PFN_Present       g_origPresent = nullptr;
static PFN_Present1      g_origPresent1 = nullptr;
static PFN_ResizeBuffers g_origResizeBuffers = nullptr;
static PFN_DrawIndexed   g_origDrawIndexed = nullptr;
static PFN_Draw          g_origDraw = nullptr;
static PFN_DrawIdxInst   g_origDrawIdxInst = nullptr;
static PFN_DrawInst      g_origDrawInst = nullptr;
static PFN_Dispatch      g_origDispatch = nullptr;
static PFN_SetCBs        g_origVSSetCB = nullptr, g_origPSSetCB = nullptr;
static PFN_VSSetShader   g_origVSSetShader = nullptr;
static PFN_PSSetShader   g_origPSSetShader = nullptr;
static PFN_CSSetShader   g_origCSSetShader = nullptr;
static PFN_CreateVS      g_origCreateVS = nullptr;
static PFN_CreatePS      g_origCreatePS = nullptr;
static PFN_CreateCS      g_origCreateCS = nullptr;
static PFN_Map           g_origMap = nullptr;
static PFN_Unmap         g_origUnmap = nullptr;
static PFN_UpdateSubres  g_origUpdateSubres = nullptr;
static PFN_OMSetRTs      g_origOMSetRTs = nullptr;
static PFN_ClearRTV      g_origClearRTV = nullptr;
static PFN_ClearDSV      g_origClearDSV = nullptr;

struct RtvClearRec { float c[4]; };
struct DsvClearRec { UINT flags; float depth; UINT8 stencil; };
static std::unordered_map<ID3D11RenderTargetView*,RtvClearRec> g_eye0RtvClears;
static std::unordered_map<ID3D11DepthStencilView*,DsvClearRec> g_eye0DsvClears;
static std::unordered_set<ID3D11RenderTargetView*> g_eye1RtvSeen;
static std::unordered_set<ID3D11DepthStencilView*> g_eye1DsvSeen;
static std::atomic<uint32_t> g_clearRtv0{0},g_clearRtv1{0},g_clearDsv0{0},g_clearDsv1{0};
static std::atomic<uint32_t> g_replayRtv{0},g_replayDsv{0};

static void BeginPassClearIsolation() {
    // Called on the single immediate-context render thread before eye 0.
    g_eye0RtvClears.clear();
    g_eye0DsvClears.clear();
    g_eye1RtvSeen.clear();
    g_eye1DsvSeen.clear();
}

// ---- per-pass poke of tracked dirty-flagged camera CBs (the 6DOF terrain/NPC fix) ----
// Runs on the single render thread. Uses the ORIGINAL Map/Unmap trampolines so the mod's own hooks
// (shadow capture + delta) never see these writes. WRITE_DISCARD renames the allocation; the
// game fully rewrites these CBs whenever it next uploads, so no partial-update hazard.
static bool PokeOne(ID3D11Buffer* buf, const uint8_t* shadow, UINT size, float ipd, bool applyDelta) {
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (!g_origMap || !g_origUnmap || !g_vrCtx) return false;
    if (FAILED(g_origMap(g_vrCtx, buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)) || !ms.pData) return false;
    memcpy(ms.pData, shadow, size);
    if (applyDelta) {
        if (size == kCamCB2Size) ApplyDelta432((float*)ms.pData, ipd);
        if (size == kCam176Size) ApplyDelta176((float*)ms.pData, ipd);
    }
    g_origUnmap(g_vrCtx, buf, 0);
    return true;
}
static void PokeTrackedCBs(float ipd) {
    std::lock_guard<std::mutex> lk(g_trkMx);
    for (int i = 0; i < g_nTrk; ++i) {
        bool want = (g_trk[i].size == kCamCB2Size) ? g_poke432.load(std::memory_order_relaxed)
                                                   : g_poke176.load(std::memory_order_relaxed);
        if (!want) continue;                        // per-object buffers: no-op unless experimenting
        (PokeOne(g_trk[i].buf, g_trk[i].shadow, g_trk[i].size, ipd, true) ? g_cPokeOk : g_cPokeFail)
            .fetch_add(1, std::memory_order_relaxed);
    }
}
static void PokeRestore() {                       // inject turned off: put the game's content back
    std::lock_guard<std::mutex> lk(g_trkMx);
    for (int i = 0; i < g_nTrk; ++i) PokeOne(g_trk[i].buf, g_trk[i].shadow, g_trk[i].size, 0.f, false);
}

// M3e: command-list layer probe (is the scene recorded on deferred contexts?)
typedef void   (STDMETHODCALLTYPE* PFN_ExecCL)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
typedef HRESULT(STDMETHODCALLTYPE* PFN_FinishCL)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
static PFN_ExecCL   g_origExecCL = nullptr;
static PFN_FinishCL g_origFinishCL = nullptr;
static std::atomic<uint32_t> g_cExec{0}, g_cFinish{0};
static uint32_t g_lExec = 0, g_lFinish = 0;

// D3D11/DXGI vtable indices (standard COM layout)
enum : int {
    VT_SC_Present = 8, VT_SC_ResizeBuffers = 13, VT_SC_Present1 = 22,
    VT_DEV_CreateVS = 12, VT_DEV_CreatePS = 15, VT_DEV_CreateCS = 18,
    VT_CTX_VSSetCB = 7, VT_CTX_PSSetShader = 9, VT_CTX_VSSetShader = 11,
    VT_CTX_DrawIndexed = 12, VT_CTX_Draw = 13, VT_CTX_Map = 14, VT_CTX_Unmap = 15,
    VT_CTX_PSSetCB = 16, VT_CTX_DrawIdxInst = 20, VT_CTX_DrawInst = 21, VT_CTX_OMSetRTs = 33,
    VT_CTX_Dispatch = 41, VT_CTX_UpdateSubres = 48, VT_CTX_ClearRTV = 50,
    VT_CTX_ClearDSV = 53,
    VT_CTX_ExecuteCommandList = 58, VT_CTX_DSSetCB = 66, VT_CTX_CSSetShader = 69,
    VT_CTX_CSSetCB = 71, VT_CTX_FinishCommandList = 114,
};

// ------------------------------------------------------------------ CB payload scan (armed frame only)
static bool IsCB(ID3D11Resource* res, UINT* outSize) {
    if (!res) return false;
    D3D11_RESOURCE_DIMENSION dim; res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
    D3D11_BUFFER_DESC d{}; ((ID3D11Buffer*)res)->GetDesc(&d);
    if (!(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return false;
    *outSize = d.ByteWidth;
    return true;
}
// distinct (size, refIndex, offset) -> hit count, so the FULL set of camera CBs (all sizes,
// not just 912) is visible at frame-done — this is what reveals the terrain/NPC view buffers.
static std::unordered_map<uint64_t, uint32_t> g_fpDistinct;
static std::atomic<bool> g_dumped432{false};
static void FpScan(void* dst, const void* data, UINT bytes, const char* src) {
    if (!g_fpActive.load(std::memory_order_relaxed) || g_nRefs == 0) return;
    if (bytes > 262144) return;                    // armed frame only: also cover big ring CBs
    // one-shot: dump the full 432B secondary camera CB so its layout (view/comb/proj/invcam) is visible
    if (bytes == 432 && !g_dumped432.exchange(true)) {
        const float* f = (const float*)data;
        Log("[dump432] full 432B CB floats (108) — locate view/comb/proj vs the API matrices:");
        for (UINT o = 0; o < 108; o += 4)
            Log("  +0x%03X: % .5f % .5f % .5f % .5f", o*4, f[o], f[o+1], f[o+2], f[o+3]);
        Log("[dump432] API view r0=[% .4f % .4f % .4f % .4f]", g_cam.view[0],g_cam.view[1],g_cam.view[2],g_cam.view[3]);
        Log("[dump432] API comb r0=[% .4f % .4f % .4f % .4f]", g_cam.comb[0],g_cam.comb[1],g_cam.comb[2],g_cam.comb[3]);
        Log("[dump432] API proj r0=[% .4f % .4f % .4f % .4f]", g_cam.proj[0],g_cam.proj[1],g_cam.proj[2],g_cam.proj[3]);
        Log("[dump432] API invcam r0=[% .4f % .4f % .4f % .4f]", g_cam.invCam[0],g_cam.invCam[1],g_cam.invCam[2],g_cam.invCam[3]);
    }
    UINT scanBytes = bytes > 32768 ? 32768 : bytes;
    UINT nf = scanBytes / 4;
    const float* f = (const float*)data;
    g_fpScanned.fetch_add(1, std::memory_order_relaxed);
    for (UINT off = 0; off + 12 <= nf; off += 4) {          // 16-byte aligned windows
        for (int r = 0; r < g_nRefs; ++r) {
            if (off + (UINT)g_refs[r].n > nf) continue;
            if (!MatchAt(g_refs[r].f, g_refs[r].n, f + off)) continue;
            g_fpHits.fetch_add(1, std::memory_order_relaxed);
            uint64_t key = ((uint64_t)bytes << 24) | ((uint64_t)r << 16) | (off*4);
            g_fpDistinct[key]++;
        }
        // rotation-only match: the view's 3x3 orientation (rows 0/1/2, skip translations at 3/7/11).
        // Jitter/translation-independent → catches ANY view buffer this camera feeds, any size.
        // ref 255 = row-major as the API returns; ref 254 = TRANSPOSED (column-major storage).
        if (g_cam.valid && off + 11 <= nf) {
            const float* v = g_cam.view;      // mtx34 rows: v[0..2] right, v[4..6] up, v[8..10] fwd
            const float* p = f + off;
            auto m3 = [](const float* a, const float* b){
                for (int i=0;i<3;i++){ float t=1e-3f+1e-3f*fabsf(a[i]); if (fabsf(a[i]-b[i])>t) return false;} return true; };
            if (m3(v, p) && m3(v+4, p+4) && m3(v+8, p+8)) {
                uint64_t key = ((uint64_t)bytes << 24) | ((uint64_t)255 << 16) | (off*4);
                g_fpDistinct[key]++;
            }
            float vT0[3]={v[0],v[4],v[8]}, vT1[3]={v[1],v[5],v[9]}, vT2[3]={v[2],v[6],v[10]};
            if (m3(vT0, p) && m3(vT1, p+4) && m3(vT2, p+8)) {
                uint64_t key = ((uint64_t)bytes << 24) | ((uint64_t)254 << 16) | (off*4);
                g_fpDistinct[key]++;
            }
        }
    }
}

// Map/Unmap pairing for dynamic CBs
struct MapRec { void* ptr; UINT size; };
static std::mutex g_mapMx;
static std::unordered_map<ID3D11Resource*, MapRec> g_mapped;

// ---- M4r: exact per-eye effects-pass census ---------------------------------
// Geometry is known-good. This probe names the PS/CS that composite screen-space lighting,
// shadows and reflections, then compares the - Hinny that GOAT - constant-buffer CONTENT actually seen by each eye.
// Merely proving that Dispatch runs twice is insufficient: both invocations may still use the
// same center-camera data. F7 captures one complete L/R outer frame and prints exact DXBC hashes.
static uint64_t Fnv64(const void* data, size_t bytes) {
    const uint8_t* p = (const uint8_t*)data; uint64_t h = 1469598103934665603ull;
    for (size_t i=0;i<bytes;i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
static std::mutex g_shaderMx;
static std::unordered_map<ID3D11VertexShader*, uint64_t> g_vsHashes;
static std::unordered_map<ID3D11PixelShader*, uint64_t> g_psHashes;
static std::unordered_map<ID3D11ComputeShader*, uint64_t> g_csHashes;
static ID3D11VertexShader* g_boundVS = nullptr;
static ID3D11PixelShader* g_boundPS = nullptr;
static ID3D11ComputeShader* g_boundCS = nullptr;
static void DumpShaderBlob(const char* stage,uint64_t hash,const void* bc,SIZE_T n) {
    char mod[MAX_PATH]{},dir[MAX_PATH]{},path[MAX_PATH]{};
    GetModuleFileNameA((HMODULE)&__ImageBase,mod,MAX_PATH);
    char* slash=strrchr(mod,'\\'); if(slash)*(slash+1)=0;
    snprintf(dir,sizeof(dir),"%sdl2_shader_dumps",mod); CreateDirectoryA(dir,nullptr);
    snprintf(path,sizeof(path),"%s\\%s_%016llX.dxbc",dir,stage,(unsigned long long)hash);
    if(GetFileAttributesA(path)!=INVALID_FILE_ATTRIBUTES)return;
    FILE*f=fopen(path,"wb");if(!f)return;fwrite(bc,1,n,f);fclose(f);
}
static ID3D11Buffer* g_psCBs[14] = {};
static ID3D11Buffer* g_csCBs[14] = {};
static UINT g_boundRtW=0, g_boundRtH=0, g_boundRtFmt=0;

static HRESULT STDMETHODCALLTYPE HookCreateVS(ID3D11Device* d, const void* bc, SIZE_T n,
                                               ID3D11ClassLinkage* l, ID3D11VertexShader** out) {
    HRESULT hr=g_origCreateVS(d,bc,n,l,out);
    if (SUCCEEDED(hr) && out && *out && bc && n) {
        uint64_t h=Fnv64(bc,n);
        { std::lock_guard<std::mutex> lk(g_shaderMx); g_vsHashes[*out]=h; }
        DumpShaderBlob("VS",h,bc,n);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE HookCreatePS(ID3D11Device* d, const void* bc, SIZE_T n,
                                               ID3D11ClassLinkage* l, ID3D11PixelShader** out) {
    HRESULT hr=g_origCreatePS(d,bc,n,l,out);
    if (SUCCEEDED(hr) && out && *out && bc && n) {
        uint64_t h=Fnv64(bc,n);
        { std::lock_guard<std::mutex> lk(g_shaderMx); g_psHashes[*out]=h; }
        DumpShaderBlob("PS",h,bc,n);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE HookCreateCS(ID3D11Device* d, const void* bc, SIZE_T n,
                                               ID3D11ClassLinkage* l, ID3D11ComputeShader** out) {
    HRESULT hr=g_origCreateCS(d,bc,n,l,out);
    if (SUCCEEDED(hr) && out && *out && bc && n) {
        uint64_t h=Fnv64(bc,n);
        { std::lock_guard<std::mutex> lk(g_shaderMx); g_csHashes[*out]=h; }
        DumpShaderBlob("CS",h,bc,n);
    }
    return hr;
}
static void STDMETHODCALLTYPE HookVSSetShader(ID3D11DeviceContext* c, ID3D11VertexShader* s,
                                              ID3D11ClassInstance* const* ci, UINT n) {
    g_boundVS=s; g_origVSSetShader(c,s,ci,n);
}
static void STDMETHODCALLTYPE HookPSSetShader(ID3D11DeviceContext* c, ID3D11PixelShader* s,
                                              ID3D11ClassInstance* const* ci, UINT n) {
    g_boundPS=s; g_origPSSetShader(c,s,ci,n);
}
static void STDMETHODCALLTYPE HookCSSetShader(ID3D11DeviceContext* c, ID3D11ComputeShader* s,
                                              ID3D11ClassInstance* const* ci, UINT n) {
    g_boundCS=s; g_origCSSetShader(c,s,ci,n);
}
static uint64_t BoundVsHash() {
    std::lock_guard<std::mutex> lk(g_shaderMx); auto it=g_vsHashes.find(g_boundVS);
    return it==g_vsHashes.end()?0:it->second;
}
static uint64_t BoundPsHash() {
    std::lock_guard<std::mutex> lk(g_shaderMx); auto it=g_psHashes.find(g_boundPS);
    return it==g_psHashes.end()?0:it->second;
}
static uint64_t BoundCsHash() {
    std::lock_guard<std::mutex> lk(g_shaderMx); auto it=g_csHashes.find(g_boundCS);
    return it==g_csHashes.end()?0:it->second;
}

// Exact-pass knockout probe. Numpad0 cycles one producer at a time; mode 7 suppresses all.
// Hashes come from an F7 capture, so this changes no unrelated draw/dispatch.
static std::atomic<int> g_fxKnockout{0};
static const char* FxKnockoutName(int m) {
    switch(m) {
      case 1:return "PS shadow filter 4B9B";
      case 2:return "PS deferred lighting D9F6";
      case 3:return "PS world reconstruction AAC7";
      case 4:return "PS motion vectors 304E";
      case 5:return "CS volumetric lighting 593D";
      case 6:return "CS screen raymarch A04D";
      case 7:return "ALL six candidates";
      default:return "OFF";
    }
}
static bool KnockoutPs(uint64_t h) {
    int m=g_fxKnockout.load(std::memory_order_relaxed);
    if(!m)return false;
    bool hit=(m==1&&h==0x4B9B8BEE99B38008ull)
          || (m==2&&h==0xD9F6892D6B78B97Aull)
          || (m==3&&h==0xAAC7BF7CAAC65064ull)
          || (m==4&&h==0x304EF5EFC07C2794ull);
    return hit || (m==7 && (h==0x4B9B8BEE99B38008ull || h==0xD9F6892D6B78B97Aull
                         || h==0xAAC7BF7CAAC65064ull || h==0x304EF5EFC07C2794ull));
}
static bool KnockoutCs(uint64_t h) {
    int m=g_fxKnockout.load(std::memory_order_relaxed);
    bool hit=(m==5&&h==0x593D0EB2E5DA69D3ull)||(m==6&&h==0xA04D51012425BDD2ull);
    return hit || (m==7 && (h==0x593D0EB2E5DA69D3ull || h==0xA04D51012425BDD2ull));
}
struct CbLive { uint64_t hash=0; UINT size=0; uint64_t changedRows=0; };
static std::mutex g_cbLiveMx;
static std::unordered_map<ID3D11Buffer*,CbLive> g_cbLive;
static void RememberCb(ID3D11Buffer* b, const void* p, UINT n, const void* pristine=nullptr) {
    if (!b || !p || !n) return;
    uint64_t rows=0;
    if(pristine && n<=1024) {
        const uint8_t*a=(const uint8_t*)pristine,*z=(const uint8_t*)p;
        for(UINT o=0;o+16<=n && o/16<64;o+=16)if(memcmp(a+o,z+o,16)!=0)rows|=1ull<<(o/16);
    }
    std::lock_guard<std::mutex> lk(g_cbLiveMx); g_cbLive[b]=CbLive{Fnv64(p,n),n,rows};
}
static CbLive ReadCbLive(ID3D11Buffer* b) {
    if (!b) return {};
    std::lock_guard<std::mutex> lk(g_cbLiveMx); auto it=g_cbLive.find(b);
    if (it!=g_cbLive.end()) return it->second;
    D3D11_BUFFER_DESC d{}; b->GetDesc(&d); return CbLive{0,d.ByteWidth,0};
}

struct FxPassRec {
    char stage='?'; uint64_t shader=0, vsShader=0; UINT x=0,y=0,z=0,rtW=0,rtH=0,rtFmt=0;
    uint32_t calls[3]={}; bool seen[2]={};
    UINT cbSize[2][6]={}; uint64_t cbHash[2][6]={}; uint64_t cbRows[2][6]={};
    UINT vsCb2Size[2]={}; uint64_t vsCb2Hash[2]={}; uint64_t vsCb2Rows[2]={};
};
static std::atomic<bool> g_fxPassArm{false};
static bool g_fxPassActive=false, g_fxPassSeen[2]={};
static std::unordered_map<uint64_t,FxPassRec> g_fxPasses;
static void MaybeStartFxPassProbe() {
    if (g_fxPassActive || !g_fxPassArm.load(std::memory_order_relaxed)) return;
    if (g_renderSlotEye.load(std::memory_order_relaxed)!=0) return;
    g_fxPassArm.store(false,std::memory_order_relaxed); g_fxPassActive=true;
    g_fxPassSeen[0]=g_fxPassSeen[1]=false; g_fxPasses.clear();
    { std::lock_guard<std::mutex> lk(g_cbLiveMx); g_cbLive.clear(); }
    Log("[fxprobe] ACTIVE: capturing exact PS/CS + CB content for one complete L/R frame");
}
static uint64_t FxPassKey(char st,uint64_t sh,uint64_t vs,UINT x,UINT y,UINT z,UINT rw,UINT rh,UINT rf) {
    uint64_t h=1469598103934665603ull;
    auto m=[&](uint64_t v){h=(h^v)*1099511628211ull;};
    m((uint8_t)st);m(sh);m(vs);m(x);m(y);m(z);m(rw);m(rh);m(rf);return h;
}
static void RecordFxPass(char st,uint64_t sh,UINT x,UINT y,UINT z,ID3D11Buffer** cbs,
                         uint64_t vs=0,ID3D11Buffer** vsCbs=nullptr) {
    MaybeStartFxPassProbe(); if (!g_fxPassActive) return;
    int eye=g_renderSlotEye.load(std::memory_order_relaxed); int ei=(eye==0?0:eye==1?1:2);
    if (eye>=0 && eye<2) g_fxPassSeen[eye]=true;
    UINT rw=st=='P'?g_boundRtW:0, rh=st=='P'?g_boundRtH:0, rf=st=='P'?g_boundRtFmt:0;
    uint64_t key=FxPassKey(st,sh,vs,x,y,z,rw,rh,rf);
    auto it=g_fxPasses.find(key);
    if (it==g_fxPasses.end()) {
        if (g_fxPasses.size()>=512) return;
        FxPassRec r{};r.stage=st;r.shader=sh;r.vsShader=vs;r.x=x;r.y=y;r.z=z;r.rtW=rw;r.rtH=rh;r.rtFmt=rf;
        it=g_fxPasses.emplace(key,r).first;
    }
    FxPassRec& r=it->second;r.calls[ei]++;
    if (ei<2 && !r.seen[ei]) {
        r.seen[ei]=true;
        for(int s=0;s<6;s++){CbLive v=ReadCbLive(cbs[s]);r.cbSize[ei][s]=v.size;r.cbHash[ei][s]=v.hash;r.cbRows[ei][s]=v.changedRows;}
        if(st=='P' && vsCbs){CbLive v=ReadCbLive(vsCbs[2]);r.vsCb2Size[ei]=v.size;r.vsCb2Hash[ei]=v.hash;r.vsCb2Rows[ei]=v.changedRows;}
    }
}
static void FinishFxPassProbe() {
    if (!g_fxPassActive || !g_fxPassSeen[0] || !g_fxPassSeen[1]) return;
    Log("[fxprobe] DONE passes=%zu (same=identical L/R CB content; diff=per-eye content)",g_fxPasses.size());
    for(auto& kv:g_fxPasses){FxPassRec&r=kv.second;
        if(!r.seen[0]||!r.seen[1]) continue;
        char cb[900];cb[0]=0;int p=0;uint32_t same=0,diff=0;
        for(int s=0;s<6;s++){
            UINT sz=r.cbSize[0][s]?r.cbSize[0][s]:r.cbSize[1][s];if(!sz)continue;
            uint64_t a=r.cbHash[0][s],b=r.cbHash[1][s];
            if(a&&b){if(a==b)same|=1u<<s;else diff|=1u<<s;}
            p+=snprintf(cb+p,sizeof(cb)-p," cb%d=%u:%016llX/%016llX",s,sz,
                (unsigned long long)a,(unsigned long long)b);
            if(sz==912)p+=snprintf(cb+p,sizeof(cb)-p," rows=%016llX/%016llX",
                (unsigned long long)r.cbRows[0][s],(unsigned long long)r.cbRows[1][s]);
            if(p>780)break;
        }
        if(r.stage=='C') Log("[fxpass-CS] sh=%016llX groups=%ux%ux%u calls=%u/%u/%u same=%02X diff=%02X%s",
            (unsigned long long)r.shader,r.x,r.y,r.z,r.calls[0],r.calls[1],r.calls[2],same,diff,cb);
        else Log("[fxpass-PS] sh=%016llX vs=%016llX verts=%u indexed=%u rt=%ux%u/f%u calls=%u/%u/%u same=%02X diff=%02X vscb2=%u:%016llX/%016llX rows=%016llX/%016llX%s",
            (unsigned long long)r.shader,(unsigned long long)r.vsShader,r.x,r.y,r.rtW,r.rtH,r.rtFmt,r.calls[0],r.calls[1],r.calls[2],same,diff,
            r.vsCb2Size[0]?r.vsCb2Size[0]:r.vsCb2Size[1],(unsigned long long)r.vsCb2Hash[0],(unsigned long long)r.vsCb2Hash[1],
            (unsigned long long)r.vsCb2Rows[0],(unsigned long long)r.vsCb2Rows[1],cb);
    }
    g_fxPassActive=false;g_fxPasses.clear();
}

// ------------------------------------------------------------------ hooks
// M3f: find the scene draw loop by capturing DrawIndexed call stacks for one armed frame.
// The function wrapping ALL ~1000 scene draws is the double-render target (single-threaded
// immediate renderer → call it twice = same-frame stereo). Dedup by full-stack hash, log
// module+RVA per frame, up to 40 unique stacks. Also tracks per-stack draw counts so the
// dominant scene-draw path (the one with ~1000 draws) is obvious.
static std::atomic<bool> g_drawStackArm{false};
static std::atomic<bool> g_drawStackActive{false};
static std::mutex g_dsMx;
static std::unordered_map<uintptr_t, uint32_t> g_dsCount;   // stack-hash -> draw count this frame
static std::unordered_set<uintptr_t> g_dsLogged;
static void CaptureDrawStack() {
    void* fr[12] = {};
    USHORT n = RtlCaptureStackBackTrace(1, 12, fr, nullptr);
    if (n < 2) return;
    uintptr_t h = 1469598103934665603ull;
    for (USHORT i = 1; i < n && i < 9; ++i) h = (h ^ (uintptr_t)fr[i]) * 1099511628211ull;
    bool firstSeen = false;
    { std::lock_guard<std::mutex> lk(g_dsMx);
      auto& cnt = g_dsCount[h]; cnt++;
      if (!g_dsLogged.count(h) && g_dsLogged.size() < 40) { g_dsLogged.insert(h); firstSeen = true; } }
    if (!firstSeen) return;
    char line[600]; int p = 0;
    p += snprintf(line + p, sizeof(line) - p, "[drawstk %016llX]", (unsigned long long)h);
    for (USHORT i = 1; i < n && i < 9; ++i) {
        HMODULE m = nullptr; wchar_t wp[MAX_PATH] = L"?";
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)fr[i], &m);
        if (m) GetModuleFileNameW(m, wp, MAX_PATH);
        const wchar_t* bn = wcsrchr(wp, L'\\'); bn = bn ? bn + 1 : wp;
        p += snprintf(line + p, sizeof(line) - p, " %ls+0x%zX", bn, (size_t)((uint8_t*)fr[i] - (uint8_t*)m));
        if (p > 520) break;
    }
    Log("%s", line);
}
// ---- per-draw CB-binding census (armed FP frame): which CB sizes feed each draw? ----
// The single question this answers: do the head-locked draws (terrain/NPCs) bind the 912B camera
// CB at all, or do they get their camera another way? Bindings shadowed from the Set*ConstantBuffers
// hooks (single-threaded immediate renderer, so plain globals are safe). Signature = sizes of VS
// slots 0..5 + DS slots 0..3 (terrain may be tessellated -> domain-shader camera CB).
static ID3D11Buffer* g_vsCBs[14] = {};
static ID3D11Buffer* g_dsCBs[8]  = {};
struct CensusRec { UINT vs[6]; UINT ds[4]; uint32_t draws; bool inst; };
static std::unordered_map<uint64_t, CensusRec> g_census;
static void CensusDraw(bool instanced) {
    if (!g_fpActive.load(std::memory_order_relaxed)) return;
    UINT vs[6] = {}, ds[4] = {};
    uint64_t sig = instanced ? 0x9E3779B97F4A7C15ull : 0;
    for (int i = 0; i < 6; ++i) {
        if (g_vsCBs[i]) { D3D11_BUFFER_DESC d{}; g_vsCBs[i]->GetDesc(&d); vs[i] = d.ByteWidth; }
        sig = sig * 1000003ull + vs[i];
    }
    for (int i = 0; i < 4; ++i) {
        if (g_dsCBs[i]) { D3D11_BUFFER_DESC d{}; g_dsCBs[i]->GetDesc(&d); ds[i] = d.ByteWidth; }
        sig = sig * 1000003ull + ds[i];
    }
    auto it = g_census.find(sig);
    if (it != g_census.end()) { it->second.draws++; return; }
    if (g_census.size() >= 64) return;
    CensusRec r{}; memcpy(r.vs, vs, sizeof(vs)); memcpy(r.ds, ds, sizeof(ds)); r.draws = 1; r.inst = instanced;
    g_census.emplace(sig, r);
}
static void STDMETHODCALLTYPE HookDrawIndexed(ID3D11DeviceContext* c, UINT n, UINT s, INT b) {
    g_cDrawIdx.fetch_add(1, std::memory_order_relaxed);
    g_drawMono.fetch_add(1, std::memory_order_relaxed);
    g_tidDraw.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (g_drawStackActive.load(std::memory_order_relaxed)) CaptureDrawStack();
    CensusDraw(false);
    uint64_t ps=BoundPsHash();
    if (n <= 6) RecordFxPass('P', ps, n, 1, 0, g_psCBs, BoundVsHash(), g_vsCBs);
    if (!KnockoutPs(ps)) g_origDrawIndexed(c,n,s,b);
}
static void STDMETHODCALLTYPE HookExecCL(ID3D11DeviceContext* c, ID3D11CommandList* cl, BOOL rs) { g_cExec.fetch_add(1, std::memory_order_relaxed); g_origExecCL(c, cl, rs); }
static HRESULT STDMETHODCALLTYPE HookFinishCL(ID3D11DeviceContext* c, BOOL rs, ID3D11CommandList** out) { g_cFinish.fetch_add(1, std::memory_order_relaxed); return g_origFinishCL(c, rs, out); }
static void STDMETHODCALLTYPE HookDraw(ID3D11DeviceContext* c, UINT n, UINT s) {
    g_cDraw.fetch_add(1, std::memory_order_relaxed); CensusDraw(false);
    uint64_t ps=BoundPsHash();
    if (n <= 6) RecordFxPass('P', ps, n, 0, 0, g_psCBs, BoundVsHash(), g_vsCBs);
    if (!KnockoutPs(ps)) g_origDraw(c,n,s);
}
static void STDMETHODCALLTYPE HookDrawIdxInst(ID3D11DeviceContext* c, UINT a, UINT b2, UINT d, INT e, UINT f2) { g_cDrawIdxInst.fetch_add(1, std::memory_order_relaxed); g_drawMono.fetch_add(1, std::memory_order_relaxed); CensusDraw(true); g_origDrawIdxInst(c,a,b2,d,e,f2); }
static void STDMETHODCALLTYPE HookDrawInst(ID3D11DeviceContext* c, UINT a, UINT b2, UINT d, UINT e) { g_cDrawInst.fetch_add(1, std::memory_order_relaxed); CensusDraw(true); g_origDrawInst(c,a,b2,d,e); }
// compute-stage CB shadow + census (screen-space FX — SSR/SSAO/fog — are usually compute; this is
// a stage not otherwise inspected). During an armed frame, record the CB-size combo each
// Dispatch reads so a camera-sized CB feeding screen-space passes (esp. a size that is NOT rewritten)
// becomes visible.
static PFN_SetCBs g_origCSSetCB = nullptr;
static std::unordered_map<uint64_t, CensusRec> g_csCensus;
static void CensusDispatch() {
    if (!g_fpActive.load(std::memory_order_relaxed)) return;
    UINT vs[6] = {}; uint64_t sig = 0xD15Aull;
    for (int i = 0; i < 6; ++i) {
        if (g_csCBs[i]) { D3D11_BUFFER_DESC d{}; g_csCBs[i]->GetDesc(&d); vs[i] = d.ByteWidth; }
        sig = sig * 1000003ull + vs[i];
    }
    auto it = g_csCensus.find(sig);
    if (it != g_csCensus.end()) { it->second.draws++; return; }
    if (g_csCensus.size() >= 48) return;
    CensusRec r{}; memcpy(r.vs, vs, sizeof(vs)); r.draws = 1; g_csCensus.emplace(sig, r);
}
static void STDMETHODCALLTYPE HookCSSetCB(ID3D11DeviceContext* c, UINT st, UINT n, ID3D11Buffer* const* b) {
    for (UINT i = 0; b && st + i < 14 && i < n; ++i) g_csCBs[st + i] = b[i];
    g_origCSSetCB(c,st,n,b);
}
static void STDMETHODCALLTYPE HookDispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) {
    g_cDispatch.fetch_add(1, std::memory_order_relaxed); g_dispMono.fetch_add(1, std::memory_order_relaxed);
    uint64_t cs=BoundCsHash(); CensusDispatch(); RecordFxPass('C', cs, x, y, z, g_csCBs);
    if (!KnockoutCs(cs)) g_origDispatch(c,x,y,z);
}
static void STDMETHODCALLTYPE HookVSSetCB(ID3D11DeviceContext* c, UINT st, UINT n, ID3D11Buffer* const* b) {
    g_cVSSet.fetch_add(1, std::memory_order_relaxed);
    for (UINT i = 0; b && st + i < 14 && i < n; ++i) g_vsCBs[st + i] = b[i];
    g_origVSSetCB(c,st,n,b);
}
static void STDMETHODCALLTYPE HookPSSetCB(ID3D11DeviceContext* c, UINT st, UINT n, ID3D11Buffer* const* b) {
    g_cPSSet.fetch_add(1, std::memory_order_relaxed);
    for (UINT i=0; b && st+i<14 && i<n; ++i) g_psCBs[st+i]=b[i];
    g_origPSSetCB(c,st,n,b);
}
static PFN_SetCBs g_origDSSetCB = nullptr;
static void STDMETHODCALLTYPE HookDSSetCB(ID3D11DeviceContext* c, UINT st, UINT n, ID3D11Buffer* const* b) {
    for (UINT i = 0; b && st + i < 8 && i < n; ++i) g_dsCBs[st + i] = b[i];
    g_origDSSetCB(c,st,n,b);
}
static void STDMETHODCALLTYPE HookClearRTV(ID3D11DeviceContext* c,ID3D11RenderTargetView* v,const FLOAT col[4]) {
    int eye=g_renderSlotEye.load(std::memory_order_relaxed);
    if (eye==0 && v && col) {
        RtvClearRec r{}; memcpy(r.c,col,sizeof(r.c)); g_eye0RtvClears[v]=r;
        g_clearRtv0.fetch_add(1,std::memory_order_relaxed);
    } else if (eye==1) g_clearRtv1.fetch_add(1,std::memory_order_relaxed);
    g_origClearRTV(c,v,col);
}
static void STDMETHODCALLTYPE HookClearDSV(ID3D11DeviceContext* c,ID3D11DepthStencilView* v,UINT flags,FLOAT depth,UINT8 stencil) {
    int eye=g_renderSlotEye.load(std::memory_order_relaxed);
    if (eye==0 && v) {
        g_eye0DsvClears[v]=DsvClearRec{flags,depth,stencil};
        g_clearDsv0.fetch_add(1,std::memory_order_relaxed);
    } else if (eye==1) g_clearDsv1.fetch_add(1,std::memory_order_relaxed);
    g_origClearDSV(c,v,flags,depth,stencil);
}

static void STDMETHODCALLTYPE HookOMSetRTs(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    g_cRTSet.fetch_add(1, std::memory_order_relaxed);
    g_boundRtW=g_boundRtH=g_boundRtFmt=0;
    if (rtvs && n && rtvs[0]) {
        ID3D11Resource* res = nullptr; rtvs[0]->GetResource(&res);
        if (res) {
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex) {
                D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td);
                g_boundRtW=td.Width; g_boundRtH=td.Height; g_boundRtFmt=(UINT)td.Format;
                if (g_fpActive.load(std::memory_order_relaxed))
                    Log("[RT] rtv=%p dsv=%p %ux%u fmt=%d (draws so far: idx=%u)", (void*)rtvs[0], (void*)dsv,
                        td.Width, td.Height, (int)td.Format, g_cDrawIdx.load(std::memory_order_relaxed));
                tex->Release();
            }
            res->Release();
        }
    }
    g_origOMSetRTs(c, n, rtvs, dsv);
    if (g_renderSlotEye.load(std::memory_order_relaxed)==1 &&
        g_passClearReplay.load(std::memory_order_relaxed)) {
        for (UINT i=0;rtvs && i<n;i++) {
            ID3D11RenderTargetView* v=rtvs[i]; if (!v || !g_eye1RtvSeen.insert(v).second) continue;
            auto it=g_eye0RtvClears.find(v);
            if (it!=g_eye0RtvClears.end()) {
                g_origClearRTV(c,v,it->second.c);
                g_replayRtv.fetch_add(1,std::memory_order_relaxed);
            }
        }
        if (dsv && g_eye1DsvSeen.insert(dsv).second) {
            auto it=g_eye0DsvClears.find(dsv);
            if (it!=g_eye0DsvClears.end()) {
                const DsvClearRec& r=it->second;
                g_origClearDSV(c,dsv,r.flags,r.depth,r.stencil);
                g_replayDsv.fetch_add(1,std::memory_order_relaxed);
            }
        }
    }
}
static void STDMETHODCALLTYPE HookUpdateSubres(ID3D11DeviceContext* c, ID3D11Resource* res, UINT sub, const D3D11_BOX* box, const void* data, UINT rowPitch, UINT depthPitch) {
    MaybeStartFxPassProbe();
    UINT size = 0;
    if (data && IsCB(res, &size)) {
        g_cUpdCB.fetch_add(1, std::memory_order_relaxed);
        UINT bytes = box ? (box->right - box->left) : size;
        FpScan(res, data, bytes, "Update");
        if (g_fxPassActive) RememberCb((ID3D11Buffer*)res,data,bytes);
    }
    g_origUpdateSubres(c, res, sub, box, data, rowPitch, depthPitch);
}
// M3b: who writes the camera CB? Capture Map-caller stacks for 912B CBs → the view-render
// function's neighborhood in engine/rd3d11. Dedup by ret-addr; log module+RVA once each.
static std::mutex g_raMx;
static std::unordered_set<void*> g_raSeen;
static std::atomic<bool> g_raCapture{true};
static void LogCamCbCaller() {
    void* frames[10] = {};
    USHORT n = RtlCaptureStackBackTrace(1, 10, frames, nullptr);
    if (n < 2) return;
    uintptr_t h = 0;                            // full-stack hash key — distinct paths never collapse
    for (USHORT i = 1; i < n && i < 6; ++i) h = h * 1099511628211ull ^ (uintptr_t)frames[i];
    void* key = (void*)h;
    { std::lock_guard<std::mutex> lk(g_raMx);
      if (g_raSeen.count(key)) return;
      if (g_raSeen.size() >= 64) return;
      g_raSeen.insert(key); }
    char line[512] = {}; int pos = 0;
    for (USHORT i = 1; i < n && i < 6; ++i) {
        HMODULE m = nullptr; wchar_t wp[MAX_PATH] = L"?";
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)frames[i], &m);
        if (m) GetModuleFileNameW(m, wp, MAX_PATH);
        const wchar_t* base = wcsrchr(wp, L'\\'); base = base ? base + 1 : wp;
        pos += snprintf(line + pos, sizeof(line) - pos, " %ls+0x%zX", base, (size_t)((uint8_t*)frames[i] - (uint8_t*)m));
        if (pos > 380) break;
    }
    Log("[cbcaller]%s", line);
}
static HRESULT STDMETHODCALLTYPE HookMap(ID3D11DeviceContext* c, ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* ms) {
    HRESULT hr = g_origMap(c, res, sub, type, flags, ms);
    MaybeStartFxPassProbe();
    UINT size = 0;
    if (SUCCEEDED(hr) && ms && ms->pData && (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE || type == D3D11_MAP_WRITE_NO_OVERWRITE) && IsCB(res, &size)) {
        g_cMapCB.fetch_add(1, std::memory_order_relaxed);
        if (size == kCamCBSize && g_raCapture.load(std::memory_order_relaxed)) LogCamCbCaller();
        // record the camera CB + every plausible per-object CB (dynamic-object modelview rewrite)
        if (size == kCamCBSize || (size >= kObjCBMin && size <= kObjCBMax) ||
            g_fpActive.load(std::memory_order_relaxed) || g_fxPassActive) {
            std::lock_guard<std::mutex> lk(g_mapMx);
            if (g_mapped.size() < 256) g_mapped[res] = MapRec{ ms->pData, size };
        }
    }
    return hr;
}
static void STDMETHODCALLTYPE HookUnmap(ID3D11DeviceContext* c, ID3D11Resource* res, UINT sub) {
    MapRec rec{};
    bool have = false;
    { std::lock_guard<std::mutex> lk(g_mapMx);
      auto it = g_mapped.find(res);
      if (it != g_mapped.end()) { rec = it->second; g_mapped.erase(it); have = true; } }
    if (have) {
        uint8_t pristine912[kCamCBSize]; bool havePristine912=g_fxPassActive && rec.size==kCamCBSize;
        if(havePristine912)memcpy(pristine912,rec.ptr,kCamCBSize);
        if (g_fpActive.load(std::memory_order_relaxed)) FpScan(res, rec.ptr, rec.size, "Map");
        bool inject = g_injectOn.load(std::memory_order_relaxed) || g_vrOn.load(std::memory_order_relaxed);
        if (rec.size == kCamCBSize) {
            float* f = (float*)rec.ptr;
            if (IsPerspBlock(f + CB_PROJ/4)) CacheCbCam(f);   // game camera, BEFORE rewrite — object path uses this
            if (g_dump912.load(std::memory_order_relaxed) && IsPerspBlock(f + CB_PROJ/4)) {
                g_dump912.store(false, std::memory_order_relaxed);
                Dump912(f);                                   // game content, pre-rewrite
            }
            if (inject) CamRewrite(f);                        // buildings/world (primary CB)
        }
        // secondary camera CBs (dirty-flagged): ALWAYS track the game's content (shadow feeds the
        // per-pass poke), and additionally delta-rewrite in place when injecting (mid-pass uploads).
        if (rec.size == kCamCB2Size) {
            g_cUp432.fetch_add(1, std::memory_order_relaxed);
            float* f = (float*)rec.ptr;
            if (Gate432(f)) { TrackCamCB(res, rec.ptr, rec.size);
                              if (inject && g_poke432.load(std::memory_order_relaxed)) ApplyDelta432(f, g_curShift); }
            else g_cRej432.fetch_add(1, std::memory_order_relaxed);
        }
        if (rec.size == kCam176Size) {
            g_cUp176.fetch_add(1, std::memory_order_relaxed);
            float* f = (float*)rec.ptr;
            if (Gate176(f)) { TrackCamCB(res, rec.ptr, rec.size);
                              if (inject && g_poke176.load(std::memory_order_relaxed)) ApplyDelta176(f, g_curShift); }
            else g_cRej176.fetch_add(1, std::memory_order_relaxed);
        }
        // one-shot decode of the 320B screen-space FX CB (armed by F7) — BEFORE any rewrite so the
        // dump is always the pristine game content, even in VR where the rewrite would modify it.
        if (rec.size >= 288 && rec.size <= 352 && g_dumpFx.exchange(false))
            DumpClassifyFx((float*)rec.ptr, rec.size);
        // one-shot dump of the small per-frame constant buffers (camera vectors for view-dep lighting)
        { int bk = SmallBucket(rec.size);
          if (bk >= 0) { uint32_t m = g_dumpSmallMask.load(std::memory_order_relaxed);
            if (m & (1u<<bk)) { g_dumpSmallMask.fetch_and(~(1u<<bk)); DumpSmallCam((float*)rec.ptr, rec.size); } } }
        // dynamic objects (people/zombies/vehicles): rewrite the CPU-baked modelview in their CB
        if (inject && rec.size != kCamCBSize && rec.size >= kObjCBMin && rec.size <= kObjCBMax) {
            ObjCamRewrite((float*)rec.ptr, rec.size, g_curShift);
            FxCamRewrite((float*)rec.ptr, rec.size);          // screen-space FX camera-derived matrices
        }
        if (g_fxPassActive) RememberCb((ID3D11Buffer*)res,rec.ptr,rec.size,
            havePristine912 ? pristine912 : nullptr); // final bytes + rows changed game->eye
    }
    g_origUnmap(c, res, sub);
}

// ------------------------------------------------------------------ frame boundary
static bool KeyPressed(int vk) {
    static SHORT prev[256] = {};
    SHORT s = GetAsyncKeyState(vk);
    bool hit = (s & 0x8000) && !(prev[vk] & 0x8000);
    prev[vk] = s;
    return hit;
}
static void StatusDump(IDXGISwapChain* sc) {
    Log("==== STATUS f%llu ====", (unsigned long long)g_frame.load());
    if (sc) { DXGI_SWAP_CHAIN_DESC d{}; if (SUCCEEDED(sc->GetDesc(&d)))
        Log("  swapchain %ux%u fmt=%d buffers=%u windowed=%d", d.BufferDesc.Width, d.BufferDesc.Height, (int)d.BufferDesc.Format, d.BufferCount, (int)d.Windowed); }
    Log("  renderer: rd3d11=%p rd3d12=%p", (void*)GetModuleHandleA("rd3d11_x64_rwdi.dll"), (void*)GetModuleHandleA("rd3d12_x64_rwdi.dll"));
    Log("  draws/frame: idx=%u plain=%u idxInst=%u inst=%u dispatch=%u | RT=%u VSSetCB=%u PSSetCB=%u UpdCB=%u MapCB=%u",
        g_lDrawIdx, g_lDraw, g_lDrawIdxInst, g_lDrawInst, g_lDispatch, g_lRTSet, g_lVSSet, g_lPSSet, g_lUpdCB, g_lMapCB);
    Log("  camera: viewCam=%p (hook calls/frame=%u) iLevel=%p chainCam=%p readOk=%u readFail=%u",
        g_viewCam.load(), g_cGetViewCam.load(), g_iLevel.load(), ChainViewCamera(), g_camReadOk.load(), g_camReadFail.load());
    if (g_cam.valid) {
        Log("  fov=%.2f aspect=%.4f zn=%.4f zf=%.1f", g_cam.fov, g_cam.aspect, g_cam.zn, g_cam.zf);
        Log("  view34  r0[% .4f % .4f % .4f % .4f] r1[% .4f % .4f % .4f % .4f] r2[% .4f % .4f % .4f % .4f]",
            g_cam.view[0],g_cam.view[1],g_cam.view[2],g_cam.view[3], g_cam.view[4],g_cam.view[5],g_cam.view[6],g_cam.view[7],
            g_cam.view[8],g_cam.view[9],g_cam.view[10],g_cam.view[11]);
        Log("  invcam34 r0[% .4f % .4f % .4f % .4f] r1[% .4f % .4f % .4f % .4f] r2[% .4f % .4f % .4f % .4f]",
            g_cam.invCam[0],g_cam.invCam[1],g_cam.invCam[2],g_cam.invCam[3], g_cam.invCam[4],g_cam.invCam[5],g_cam.invCam[6],g_cam.invCam[7],
            g_cam.invCam[8],g_cam.invCam[9],g_cam.invCam[10],g_cam.invCam[11]);
        Log("  proj44  [% .4f % .4f % .4f % .4f | % .4f % .4f % .4f % .4f | % .4f % .4f % .4f % .4f | % .4f % .4f % .4f % .4f]",
            g_cam.proj[0],g_cam.proj[1],g_cam.proj[2],g_cam.proj[3], g_cam.proj[4],g_cam.proj[5],g_cam.proj[6],g_cam.proj[7],
            g_cam.proj[8],g_cam.proj[9],g_cam.proj[10],g_cam.proj[11], g_cam.proj[12],g_cam.proj[13],g_cam.proj[14],g_cam.proj[15]);
        Log("  comb44  [% .4f % .4f % .4f % .4f | % .4f % .4f % .4f % .4f | % .4f % .4f % .4f % .4f | % .4f % .4f % .4f % .4f]",
            g_cam.comb[0],g_cam.comb[1],g_cam.comb[2],g_cam.comb[3], g_cam.comb[4],g_cam.comb[5],g_cam.comb[6],g_cam.comb[7],
            g_cam.comb[8],g_cam.comb[9],g_cam.comb[10],g_cam.comb[11], g_cam.comb[12],g_cam.comb[13],g_cam.comb[14],g_cam.comb[15]);
    } else Log("  camera: no valid snapshot yet");
    Log("==== END STATUS ====");
}

static void OnFrameBoundary(IDXGISwapChain* sc) {
    uint64_t f = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    FinishFxPassProbe(); // right-eye Present closes the complete L/R effects-pass capture

    // close an armed fingerprint frame
    if (g_fpActive.load(std::memory_order_relaxed)) {
        g_fpActive.store(false, std::memory_order_relaxed);
        { std::lock_guard<std::mutex> lk(g_mapMx); g_mapped.clear(); }
        Log("[FP] frame done: %u CB writes scanned, %u fingerprint hits (refs=%d) — DISTINCT (size,matrix,off):",
            g_fpScanned.load(), g_fpHits.load(), g_nRefs);
        for (auto& kv : g_fpDistinct) {
            UINT size = (UINT)(kv.first >> 24); int r = (int)((kv.first >> 16) & 0xFF); UINT off = (UINT)(kv.first & 0xFFFF);
            const char* nm = (r == 255) ? "viewROT" : (r == 254) ? "viewROT_T" : (r < g_nRefs ? g_refs[r].name : "?");
            Log("[FP-set] size=%-5u %-10s off=0x%-4X  (x%u)", size, nm, off, kv.second);
        }
        g_fpDistinct.clear();
        // per-draw CB-binding census: which CB-size combos feed the scene draws?
        Log("[census] %zu distinct binding combos this frame (vs slots 0-5 | ds slots 0-3 | draws):", g_census.size());
        for (auto& kv : g_census) {
            const CensusRec& r = kv.second;
            bool has912 = false;
            for (UINT s : r.vs) if (s == kCamCBSize) has912 = true;
            for (UINT s : r.ds) if (s == kCamCBSize) has912 = true;
            Log("[census] vs=%u,%u,%u,%u,%u,%u ds=%u,%u,%u,%u draws=%u%s%s",
                r.vs[0],r.vs[1],r.vs[2],r.vs[3],r.vs[4],r.vs[5], r.ds[0],r.ds[1],r.ds[2],r.ds[3],
                r.draws, r.inst ? " INST" : "", has912 ? "" : "  <<< NO-912");
        }
        g_census.clear();
        // compute-stage CB census: 912 (=camera) or an unrewritten camera-sized CB feeding
        // screen-space compute passes is the target of this census.
        Log("[cscensus] %zu distinct compute CB-size combos (cs slots 0-5 | dispatches):", g_csCensus.size());
        for (auto& kv : g_csCensus) {
            const CensusRec& r = kv.second;
            bool has912 = false; for (UINT s : r.vs) if (s == kCamCBSize) has912 = true;
            Log("[cscensus] cs=%u,%u,%u,%u,%u,%u disp=%u%s",
                r.vs[0],r.vs[1],r.vs[2],r.vs[3],r.vs[4],r.vs[5], r.draws, has912 ? "  <912>" : "");
        }
        g_csCensus.clear();
    }

    // close an armed draw-stack capture frame: report each unique stack's draw count so the
    // dominant scene-render path (the ~hundreds-of-draws one) stands out from UI/shadow paths.
    if (g_drawStackActive.exchange(false)) {
        std::lock_guard<std::mutex> lk(g_dsMx);
        Log("[drawstk] frame done: %zu unique stacks", g_dsCount.size());
        for (auto& kv : g_dsCount) Log("[drawstk-count %016llX] draws=%u", (unsigned long long)kv.first, kv.second);
        g_dsCount.clear(); g_dsLogged.clear();
    }
    if (g_drawStackArm.exchange(false)) {
        { std::lock_guard<std::mutex> lk(g_dsMx); g_dsCount.clear(); g_dsLogged.clear(); }
        g_drawStackActive.store(true, std::memory_order_relaxed);
        Log("[drawstk] ARMED for next frame");
    }

    // latch per-frame counters
    g_lDrawIdx     = g_cDrawIdx.exchange(0);      g_lDraw     = g_cDraw.exchange(0);
    g_lDrawIdxInst = g_cDrawIdxInst.exchange(0);  g_lDrawInst = g_cDrawInst.exchange(0);
    g_lDispatch    = g_cDispatch.exchange(0);     g_lRTSet    = g_cRTSet.exchange(0);
    g_lUpdCB       = g_cUpdCB.exchange(0);        g_lMapCB    = g_cMapCB.exchange(0);
    g_lExec        = g_cExec.exchange(0);         g_lFinish   = g_cFinish.exchange(0);
    g_tidPresent.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_lVSSet       = g_cVSSet.exchange(0);        g_lPSSet    = g_cPSSet.exchange(0);
    uint32_t vcCalls = g_cGetViewCam.exchange(0);

    // refresh camera snapshot: prefer hook-captured cam, fall back to offset chain
    void* cam = g_viewCam.load(std::memory_order_relaxed);
    if (!cam) { cam = ChainViewCamera(); if (cam) g_viewCam.store(cam, std::memory_order_relaxed); }
    if (cam) {
        CamSnap cs{};
        if (ReadCamRaw(cam, &cs)) { g_cam = cs; g_camReadOk.fetch_add(1, std::memory_order_relaxed); }
        else                      { g_camReadFail.fetch_add(1, std::memory_order_relaxed); g_viewCam.store(nullptr, std::memory_order_relaxed); }
    }

    // M3a: optional-render-camera experiment (independent of VR/sway modes)
    if (g_orcMode.load(std::memory_order_relaxed) != 0) {
        bool ok = OrcTick();
        if ((f % 300) == 0)
            Log("[orc] mode=%d tick=%d readback=%p idx/f=%u disp/f=%u",
                g_orcMode.load(), (int)ok, OrcReadback(), g_lDrawIdx, g_lDispatch);
    }

    // M2 VR (AER) or M1 monitor sway — both drive g_curShift for the CB rewrite
    g_lRewrite = g_cRewrite.exchange(0);
    if (g_vrOn.load(std::memory_order_relaxed)) {
        VrFrame(sc);
        if ((f % 300) == 0)
            Log("[vr] eye=%d halfIPD=%.4f ws=%d%% rew/f=%u submits=%u running=%d",
                g_renderedEye, g_halfIPD, g_worldScale100.load(), g_lRewrite,
                g_vrSubmits.load(), (int)DL2Xr::IsRunning());
    } else if (g_injectOn.load(std::memory_order_relaxed)) {
        float amp = g_shiftCm.load(std::memory_order_relaxed) * 0.01f;
        g_curShift = g_swayOn.load(std::memory_order_relaxed)
                   ? amp * sinf((float)(f % 90) * (6.2831853f / 90.f))   // ~1.5s period @60fps
                   : amp;
        if (g_synthYaw.load(std::memory_order_relaxed)) {                // monitor head-look test
            float a = 0.35f * sinf((float)(f % 240) * (6.2831853f / 240.f));   // ±20° yaw, ~4s period
            g_headUseQ[0] = 0.f; g_headUseQ[1] = sinf(a*0.5f); g_headUseQ[2] = 0.f; g_headUseQ[3] = cosf(a*0.5f);
            g_headUseP[0] = g_headUseP[1] = g_headUseP[2] = 0.f;
            g_headValid.store(true, std::memory_order_relaxed);
        }
        if ((f % 300) == 0)
            Log("[inject] sway=%d shift=%dcm synthYaw=%d rewrites/frame=%u prodOrder=%d",
                (int)g_swayOn.load(), g_shiftCm.load(), (int)g_synthYaw.load(), g_lRewrite, g_prodOrder.load());
    } else g_curShift = 0.f;

    // dirty-flagged secondary CBs: poke per frame on the single-render paths (mode 2 pokes per
    // pass inside DoubleRender instead); restore the game's own content when injection turns off.
    bool injNow = g_vrOn.load(std::memory_order_relaxed) || g_injectOn.load(std::memory_order_relaxed);
    static bool s_prevInject = false;
    if (injNow && g_dblMode.load(std::memory_order_relaxed) != 2) PokeTrackedCBs(g_curShift);
    if (!injNow && s_prevInject) PokeRestore();
    s_prevInject = injNow;
    if (injNow && (f % 300) == 0)
        Log("[cbs] 300f: sweep pos=%u comb=%u comb0=%u rw0=%u objMV=%u fxcam=%u | up432=%u up176=%u mapCB/f=%u",
            g_cSwPos.exchange(0), g_cSwComb.exchange(0),
            g_cSwC0.exchange(0), g_cRw0.exchange(0), g_cObjMV.exchange(0), g_cFxCam.exchange(0),
            g_cUp432.exchange(0), g_cUp176.exchange(0), g_lMapCB);

    if (injNow && (f % 300) == 0)
        Log("[upcam] enabled=%d scopedNow=%d valid=%d apply=%u fail=%u sourceMatch=%u fallback=%u liveCam=%p",
            (int)g_upstreamCam.load(),(int)g_upLiveApplied.load(),(int)g_upInvValid.load(),
            g_cUpApply.load(),g_cUpFail.load(),g_cUpMatch.load(),g_cUpFallback.load(),g_upLiveCam);
    if (injNow && (f % 300) == 0)
        Log("[isoclear] replay=%d clearRTV L=%u R=%u clearDSV L=%u R=%u replayRTV=%u replayDSV=%u",
            (int)g_passClearReplay.load(),g_clearRtv0.exchange(0),g_clearRtv1.exchange(0),
            g_clearDsv0.exchange(0),g_clearDsv1.exchange(0),
            g_replayRtv.exchange(0),g_replayDsv.exchange(0));
    if (g_perFrameLine)
        Log("[f%llu] idx=%u inst=%u disp=%u RT=%u updCB=%u mapCB=%u vcCalls=%u cam=%p fov=%.1f rew=%u",
            (unsigned long long)f, g_lDrawIdx, g_lDrawIdxInst, g_lDispatch, g_lRTSet, g_lUpdCB, g_lMapCB,
            vcCalls, g_viewCam.load(), g_cam.valid ? g_cam.fov : -1.f, g_lRewrite);
    else if ((f % 300) == 0)
        Log("[hb f%llu] idx=%u disp=%u dbl=%d draws1=%u draws2=%u disp1=%u disp2=%u",
            (unsigned long long)f, g_lDrawIdx, g_lDispatch,
            g_dblMode.load(std::memory_order_relaxed),
            g_drawsIn1.load(std::memory_order_relaxed), g_drawsIn2.load(std::memory_order_relaxed),
            g_dispIn1.load(std::memory_order_relaxed), g_dispIn2.load(std::memory_order_relaxed));

    // arm fingerprint for NEXT frame if requested and matrices are available
    bool wantArm = g_fpArm.exchange(false);
    if (!wantArm && !g_autoArmed && f > 900 && g_lDrawIdx > 400 && g_cam.valid) { wantArm = true; g_autoArmed = true; Log("[FP] AUTO-ARM (gameplay detected)"); }
    if (wantArm) {
        if (g_cam.valid) {
            BuildRefs(g_cam);
            g_fpHits.store(0); g_fpScanned.store(0);
            g_fpActive.store(true, std::memory_order_relaxed);
            g_dump912.store(true, std::memory_order_relaxed);
            g_dumpFx.store(true, std::memory_order_relaxed);
            g_dumpSmallMask.store(0xF, std::memory_order_relaxed);   // dump 16/32/48/64B once each
            Log("[FP] ARMED for frame %llu with %d refs (fov=%.2f)", (unsigned long long)f + 1, g_nRefs, g_cam.fov);
        } else Log("[FP] arm requested but NO camera snapshot yet");
    }

    // keys (present thread)
    if (KeyPressed(VK_F1))  { bool n = !g_vrOn.load(); g_vrOn.store(n); if (!n) { g_curShift = 0.f; ClearUpstreamHeadCamera(); } Log("[key] F1 VR %s", n ? "ON" : "OFF"); }
    if (KeyPressed(VK_F2))  {
        if (g_vrOn.load()) { bool n = !g_head6dof.load(); g_head6dof.store(n); Log("[key] F2 6DOF head-tracking %s", n ? "ON" : "OFF"); }
        else { bool n = !g_injectOn.load(); g_injectOn.store(n); Log("[key] F2 monitor sway-inject %s", n ? "ON" : "OFF"); }
    }
    if (KeyPressed(VK_F3))  {
        if (g_vrOn.load()) { int v = g_worldScale100.fetch_add(5) + 5; Log("[key] worldScale=%d%%", v); }
        else { int v = g_shiftCm.fetch_add(5) + 5; Log("[key] shift=%dcm", v); }
    }
    if (KeyPressed(VK_F6))  {
        if (g_vrOn.load()) { int v = g_worldScale100.fetch_sub(5) - 5; if (v < 5) { v = 5; g_worldScale100.store(5); } Log("[key] worldScale=%d%%", v); }
        else { int v = g_shiftCm.fetch_sub(5) - 5; if (v < 0) { v = 0; g_shiftCm.store(0); } Log("[key] shift=%dcm", v); }
    }
    if (KeyPressed(VK_F5))  { DL2Xr::Recenter(); Log("[key] F5 recenter"); }
    if (KeyPressed(VK_INSERT)) { bool n=!g_bakeRot.load(); g_bakeRot.store(n); Log("[key] INS bake head ROTATION %s", n?"ON":"OFF"); }
    if (KeyPressed(VK_DELETE)) { bool n=!g_bakePos.load(); g_bakePos.store(n); Log("[key] DEL bake head POSITION %s", n?"ON":"OFF"); }
    if (KeyPressed(VK_HOME))   {
        if (g_vrOn.load()) { bool n=!g_tagHead.load(); g_tagHead.store(n); Log("[key] HOME pose-tag HEAD-TRACKED %s (off=world-locked tag)", n?"ON":"OFF"); }
        else {
            bool n=!g_synthYaw.load(); g_synthYaw.store(n);
            if (!n) { g_headUseQ[0]=g_headUseQ[1]=g_headUseQ[2]=0.f; g_headUseQ[3]=1.f; g_headValid.store(false); }
            Log("[key] HOME synthetic head-yaw sway %s (monitor 6DOF test)", n?"ON":"OFF");
        }
    }
    if (KeyPressed(VK_END))    { bool n=!g_rollInvert.load(); g_rollInvert.store(n); Log("[key] END ROLL sign inverted %s", n?"ON":"OFF"); }
    if (KeyPressed(VK_PRIOR))  { bool n=!g_poke432.load(); g_poke432.store(n); Log("[key] PGUP 432B CB poke %s", n?"ON":"OFF"); }
    if (KeyPressed(VK_NEXT))   { bool n=!g_poke176.load(); g_poke176.store(n); Log("[key] PGDN 176B CB poke %s", n?"ON":"OFF"); }
    if (KeyPressed(VK_NUMPAD1)) { bool n=!g_rwClassic.load(); g_rwClassic.store(n); Log("[key] NUM1 classic rewrite (view/invcam/comb@210 — buildings) %s", n?"ON":"OFF"); }
    if (KeyPressed(VK_NUMPAD0)) {
        int m=(g_fxKnockout.load(std::memory_order_relaxed)+1)%8;
        g_fxKnockout.store(m,std::memory_order_relaxed);
        Log("[key] NUM0 exact FX knockout mode=%d %s",m,FxKnockoutName(m));
    }
    if (KeyPressed(VK_NUMPAD2)) { bool n=!g_rwComb0.load();   g_rwComb0.store(n);   Log("[key] NUM2 comb0 rewrite (viewproj@000 — terrain/people) %s", n?"ON":"OFF"); }
    if (KeyPressed(VK_NUMPAD3)) { bool n=!g_rwSweep.load();   g_rwSweep.store(n);   Log("[key] NUM3 generic sweep (campos/rot/rel-viewproj) %s", n?"ON":"OFF"); }
    if (KeyPressed(VK_NUMPAD4)) { bool n=!g_rwObjMV.load();   g_rwObjMV.store(n);   Log("[key] NUM4 object MODELVIEW rewrite (people/zombies/vehicles) %s", n?"ON":"OFF"); }
    if (KeyPressed(VK_NUMPAD5)) { bool n=!g_rwFxCam.load();   g_rwFxCam.store(n);   Log("[key] NUM5 screen-space FX camera rewrite (reflections/fog) %s", n?"ON":"OFF"); }
    if (KeyPressed(VK_NUMPAD6)) { bool n=!g_diagYaw.load();   g_diagYaw.store(n);   Log("[key] NUM6 DIAG +30deg render-yaw (does lighting follow the 912?) %s", n?"ON":"OFF"); }
    if (KeyPressed(VK_NUMPAD7)) {
        bool n=!g_upstreamCam.load(); g_upstreamCam.store(n);
        if (!n) ClearUpstreamHeadCamera();
        g_orcMode.store(0);
        Log("[key] NUM7 upstream center-eye engine camera %s",n?"ON":"OFF");
    }
    if (KeyPressed(VK_NUMPAD8)) {
        bool n=!g_passClearReplay.load(); g_passClearReplay.store(n);
        Log("[key] NUM8 eye-1 render-target/depth clear replay %s",n?"ON":"OFF");
    }
    if (KeyPressed(VK_NUMPAD9)) {
        bool n=!g_rwCamPos.load(); g_rwCamPos.store(n);
        Log("[key] NUM9 targeted raw camera-position rewrite %s",n?"ON":"OFF");
    }
    if (KeyPressed(VK_F9))  {
        if (g_upstreamCam.exchange(false)) ClearUpstreamHeadCamera();
        int m = (g_orcMode.load() + 1) % 3;
        g_orcMode.store(m);
        if (m == 0) OrcClear();
        Log("[key] F9 optional-render-camera mode=%d (%s)", m, m==0 ? "OFF" : m==1 ? "live view cam" : "cloned +6.4cm cam");
    }
    if (KeyPressed(VK_F11)) {
        if (!g_origRenderA && !g_origRenderB) Log("[key] F11 ignored — no render-branch hook installed");
        else {
            int m = (g_dblMode.load() + 1) % 3;
            g_dblMode.store(m);
            if (m == 2) { g_vrOn.store(true); }        // same-frame needs the VR capture path live
            else ClearUpstreamHeadCamera();
            Log("[key] F11 double-render mode=%d (%s)", m, m==0 ? "OFF" : m==1 ? "2x same camera (stability trap)" : "2x L/R eye SAME-FRAME STEREO (VR auto-on)");
        }
    }
    if (KeyPressed(VK_F7))  {
        g_fpArm.store(true); g_fxPassArm.store(true);
        Log("[key] F7 -> arm fingerprint + exact L/R PS/CS effects-pass census");
    }
    if (KeyPressed(VK_F12)) { g_drawStackArm.store(true); Log("[key] F12 -> arm draw-stack capture"); }
    if (KeyPressed(VK_F8))  StatusDump(sc);
    if (KeyPressed(VK_F10)) { g_perFrameLine = !g_perFrameLine; Log("[key] perFrameLine=%d", (int)g_perFrameLine); }
    if (KeyPressed(VK_F4))  { g_camReadOk.store(0); g_camReadFail.store(0); g_autoArmed = false; Log("[key] counters reset"); }
}
static HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT s, UINT fl) { OnFrameBoundary(sc); return g_origPresent(sc, s, fl); }
static HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT s, UINT fl, const DXGI_PRESENT_PARAMETERS* pp) { OnFrameBoundary(sc); return g_origPresent1(sc, s, fl, pp); }
static HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    Log("[resize] buffers=%u %ux%u fmt=%d flags=0x%X (frame=%llu)", n, w, h, (int)fmt, flags, (unsigned long long)g_frame.load());
    ReleaseEyeTex();   // recreated at next VR capture with the new backbuffer desc
    return g_origResizeBuffers(sc, n, w, h, fmt, flags);
}

// ------------------------------------------------------------------ VEH crash fingerprinter
static LONG CALLBACK VehLogger(EXCEPTION_POINTERS* xp) {
    DWORD code = xp->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 8) {
        void* addr = xp->ExceptionRecord->ExceptionAddress;
        char m[192]; ModOf(addr, m, sizeof(m));
        Log("[VEH] code=0x%08X at %p (%s) tid=%lu frame=%llu fpActive=%d",
            code, addr, m, GetCurrentThreadId(), (unsigned long long)g_frame.load(), (int)g_fpActive.load());
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ------------------------------------------------------------------ init
static bool HookFn(void* target, void* det, void** orig, const char* name) {
    if (!target) { Log("[hook] %s: null target", name); return false; }
    if (MH_CreateHook(target, det, orig) != MH_OK || MH_EnableHook(target) != MH_OK) { Log("[hook] %s FAILED", name); return false; }
    char m[192]; ModOf(target, m, sizeof(m));
    Log("[hook] %s @ %s", name, m);
    return true;
}
static void InitProbe() {
    OpenLog();
    Log("[init] DL2PROBE M1 — eye-shift injection (sway test) on the 912B camera CB. PID=%lu", GetCurrentProcessId());
    { char exe[MAX_PATH]{}; GetModuleFileNameA(nullptr, exe, MAX_PATH); Log("[init] exe=%s", exe); }
    LoadRealDinput8();
    Log("[init] real dinput8 DirectInput8Create=%p", (void*)g_realDI8Create);
    AddVectoredExceptionHandler(1, VehLogger);

    // wait for the engine + a renderer to come up (engine dll is a static import of the exe's
    // dependency chain, but the renderer dll loads later during boot)
    for (int i = 0; i < 120; ++i) {
        if (GetModuleHandleA(ENGDLL) && (GetModuleHandleA("rd3d11_x64_rwdi.dll") || GetModuleHandleA("rd3d12_x64_rwdi.dll"))) break;
        Sleep(500);
    }
    HMODULE r11 = GetModuleHandleA("rd3d11_x64_rwdi.dll"), r12 = GetModuleHandleA("rd3d12_x64_rwdi.dll");
    Log("[init] modules: engine=%p rd3d11=%p rd3d12=%p", (void*)GetModuleHandleA(ENGDLL), (void*)r11, (void*)r12);
    if (!r11 && r12) Log("[init] !! game is on DX12 — this probe hooks DX11 only. Set RendererMode(\"d3d11\") in video.scr.");

    ResolveEngineExports();
    AobResolveLobby();

    if (MH_Initialize() != MH_OK) { Log("[init] MH_Initialize FAILED"); return; }

    // path A: hook the GetViewCamera export to capture the live camera
    if (pGetViewCamera)
        HookFn((void*)pGetViewCamera, (void*)HookGetViewCamera, (void**)&g_origGetViewCamera, "ILevel::GetViewCamera");

    // M3c/M3d: hook BOTH render branches of the tick fn — signature-guarded
    {
        static const uint8_t sigB[12] = { 0x48,0x89,0x5C,0x24,0x18,0x55,0x56,0x57,0x41,0x54,0x41,0x55 };
        HMODULE eng = GetModuleHandleA(ENGDLL);
        if (eng) {
            uint8_t* tA = (uint8_t*)eng + kRenderViewsRVA;      // 0x7EB120 (menu/simple path)
            if (memcmp(tA, kRenderViewsSig, sizeof(kRenderViewsSig)) == 0)
                HookFn(tA, (void*)HookedRenderA, (void**)&g_origRenderA, "engine::RenderA(+0x7EB120)");
            else Log("[hook] RenderA signature MISMATCH — skipped");
            uint8_t* tB = (uint8_t*)eng + 0x8324D0;             // gameplay path
            if (memcmp(tB, sigB, sizeof(sigB)) == 0)
                HookFn(tB, (void*)HookedRenderB, (void**)&g_origRenderB, "engine::RenderB(+0x8324D0)");
            else Log("[hook] RenderB signature MISMATCH — skipped");
        }
    }

    // D3D11 vtable hooks via dummy device (ReShade-safe: wrapper vtables still funnel here)
    HWND hwnd = CreateWindowExA(0, "STATIC", "dl2_probe_dummy", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferDesc.Width = 64; sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fls, 3,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, &got, &ctx);
    if (FAILED(hr) || !sc || !ctx) { Log("[init] dummy D3D11CreateDeviceAndSwapChain FAILED hr=0x%08X", (unsigned)hr); if (hwnd) DestroyWindow(hwnd); return; }

    { void** vt = *(void***)sc;
      HookFn(vt[VT_SC_Present],       (void*)HookPresent,       (void**)&g_origPresent,       "SC::Present");
      HookFn(vt[VT_SC_ResizeBuffers], (void*)HookResizeBuffers, (void**)&g_origResizeBuffers, "SC::ResizeBuffers");
      IDXGISwapChain1* sc1 = nullptr;
      if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1) {
          void** vt1 = *(void***)sc1;
          HookFn(vt1[VT_SC_Present1], (void*)HookPresent1, (void**)&g_origPresent1, "SC::Present1");
          sc1->Release();
      }
    }
    { void** vt = *(void***)dev;
      HookFn(vt[VT_DEV_CreateVS], (void*)HookCreateVS, (void**)&g_origCreateVS, "DEV::CreateVertexShader");
      HookFn(vt[VT_DEV_CreatePS], (void*)HookCreatePS, (void**)&g_origCreatePS, "DEV::CreatePixelShader");
      HookFn(vt[VT_DEV_CreateCS], (void*)HookCreateCS, (void**)&g_origCreateCS, "DEV::CreateComputeShader");
    }
    { void** vt = *(void***)ctx;
      HookFn(vt[VT_CTX_VSSetShader],  (void*)HookVSSetShader,  (void**)&g_origVSSetShader, "CTX::VSSetShader");
      HookFn(vt[VT_CTX_PSSetShader],  (void*)HookPSSetShader,  (void**)&g_origPSSetShader, "CTX::PSSetShader");
      HookFn(vt[VT_CTX_CSSetShader],  (void*)HookCSSetShader,  (void**)&g_origCSSetShader, "CTX::CSSetShader");
      HookFn(vt[VT_CTX_DrawIndexed],  (void*)HookDrawIndexed,  (void**)&g_origDrawIndexed, "CTX::DrawIndexed");
      HookFn(vt[VT_CTX_Draw],         (void*)HookDraw,         (void**)&g_origDraw,        "CTX::Draw");
      HookFn(vt[VT_CTX_DrawIdxInst],  (void*)HookDrawIdxInst,  (void**)&g_origDrawIdxInst, "CTX::DrawIndexedInstanced");
      HookFn(vt[VT_CTX_DrawInst],     (void*)HookDrawInst,     (void**)&g_origDrawInst,    "CTX::DrawInstanced");
      HookFn(vt[VT_CTX_Dispatch],     (void*)HookDispatch,     (void**)&g_origDispatch,    "CTX::Dispatch");
      HookFn(vt[VT_CTX_VSSetCB],      (void*)HookVSSetCB,      (void**)&g_origVSSetCB,     "CTX::VSSetConstantBuffers");
      HookFn(vt[VT_CTX_PSSetCB],      (void*)HookPSSetCB,      (void**)&g_origPSSetCB,     "CTX::PSSetConstantBuffers");
      HookFn(vt[VT_CTX_OMSetRTs],     (void*)HookOMSetRTs,     (void**)&g_origOMSetRTs,    "CTX::OMSetRenderTargets");
      HookFn(vt[VT_CTX_UpdateSubres], (void*)HookUpdateSubres, (void**)&g_origUpdateSubres,"CTX::UpdateSubresource");
      HookFn(vt[VT_CTX_ClearRTV],     (void*)HookClearRTV,     (void**)&g_origClearRTV,    "CTX::ClearRenderTargetView");
      HookFn(vt[VT_CTX_ClearDSV],     (void*)HookClearDSV,     (void**)&g_origClearDSV,    "CTX::ClearDepthStencilView");
      HookFn(vt[VT_CTX_Map],          (void*)HookMap,          (void**)&g_origMap,         "CTX::Map");
      HookFn(vt[VT_CTX_Unmap],        (void*)HookUnmap,        (void**)&g_origUnmap,       "CTX::Unmap");
      HookFn(vt[VT_CTX_ExecuteCommandList], (void*)HookExecCL,   (void**)&g_origExecCL,    "CTX::ExecuteCommandList");
      HookFn(vt[VT_CTX_DSSetCB],      (void*)HookDSSetCB,      (void**)&g_origDSSetCB,     "CTX::DSSetConstantBuffers");
      HookFn(vt[VT_CTX_CSSetCB],      (void*)HookCSSetCB,      (void**)&g_origCSSetCB,     "CTX::CSSetConstantBuffers");
      HookFn(vt[VT_CTX_FinishCommandList],  (void*)HookFinishCL, (void**)&g_origFinishCL,  "CTX::FinishCommandList");
    }
    Log("[init] DL2 VR: hooks installed (NUM3 broad sweep OFF, NUM9 raw campos OFF)");

    if (ctx) ctx->Release(); if (dev) dev->Release(); if (sc) sc->Release(); if (hwnd) DestroyWindow(hwnd);
}
static DWORD WINAPI InitThread(LPVOID) { InitProbe(); return 0; }

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
        // DL2 loads dinput8.dll TRANSIENTLY (LoadLibrary -> DirectInput8Create -> FreeLibrary).
        // Unpinned, the module gets unloaded mid-InitThread -> AV in unmapped code (crash-on-boot,
        // event log "DINPUT8.dll_unloaded", proven 2026-07-17). PIN so FreeLibrary is a no-op.
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)&__ImageBase, &self);
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
