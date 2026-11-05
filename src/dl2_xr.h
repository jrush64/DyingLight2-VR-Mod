#pragma once
#include <cstdint>
struct ID3D11Device;
struct ID3D11Texture2D;

// Dying Light 2 -> headset: a MINIMAL OpenXR stereo-submission module with hand-written ABI
// types (xr_types.h). This
// layer does ONLY: bring up an OpenXR session on the GAME's D3D11 device (XR_KHR_D3D11_enable), make
// two per-eye swapchains, locate the two eye views each frame, and submit two caller-provided eye
// textures as an XrCompositionLayerProjection. No DIBR / AER / menu / camera injection — the render
// side draws each eye at the located view pose and hands the two textures to SubmitEyes().
namespace DL2Xr {
    struct Pose { float orientation[4]; float position[3]; };  // XR frame: Y-up, -Z fwd, meters
    struct Fov  { float angleLeft, angleRight, angleUp, angleDown; };  // radians
    bool Init(ID3D11Device* device) noexcept;          // lazy XR bring-up on the game's device; false if no HMD/runtime
    bool IsRunning() noexcept;
    bool BeginFrame() noexcept;                        // xrWaitFrame+xrBeginFrame; false = skip rendering this frame
    bool GetEyeView(int eye, Pose* outPose, Fov* outFov) noexcept;  // located view for eye 0=L/1=R (call after BeginFrame)
    // Recenter-relative HEAD pose: midpoint position of the two eyes + head orientation. Meters,
    // XR axes (Y-up, -Z fwd, right-handed). Valid only after a BeginFrame that located views.
    bool GetHeadPose(Pose* outPose) noexcept;
    uint32_t EyeWidth() noexcept;                      // recommended per-eye swapchain width
    uint32_t EyeHeight() noexcept;
    // AER contract: each eye texture is submitted WITH the pose+fov it was RENDERED at (pose-tagging
    // is sacred — this is what keeps alternate-eye from ghosting). poses/fovs are caller-owned [2].
    void SubmitEyesPosed(ID3D11Texture2D* leftEye, ID3D11Texture2D* rightEye,
                         const Pose* poses, const Fov* fovs) noexcept;
    void SetEyeResolution(uint32_t w, uint32_t h) noexcept; // pin eye-swapchain res to the caller's source size (0=recommended)
    void SetSourceFormat(int64_t dxgiFormat) noexcept;      // backbuffer format family (call before Init; UNORM/sRGB pair auto)
    void Recenter() noexcept;
    void Shutdown() noexcept;
}
