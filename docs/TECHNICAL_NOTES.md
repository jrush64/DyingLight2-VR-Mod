# Technical notes

How the mod works, which engine addresses it uses, and which approaches were tried and failed,
so whoever continues does not repeat them. Addresses are RVAs into `engine_x64_rwdi.dll` from
the Steam build of Dying Light 2 Stay Human, game version 1.28.3, DirectX 11 renderer
(`rd3d11_x64_rwdi.dll`). The install has no anti-cheat binaries (only the Epic Online Services
SDK); a proxy DLL loads without trouble in single player.

## Architecture

```
DyingLightGame_x64_rwdi.exe
  └─ engine_x64_rwdi.dll  imports exactly one dinput8 function: DirectInput8Create
       └─ dinput8.dll (this mod)  ── forwards DirectInput8Create to the real system32 dinput8.dll
            ├─ dl2_probe.cpp  engine export resolution, MinHook on engine + D3D11/DXGI vtables,
            │                 same-frame double render, per-eye constant-buffer rewrite,
            │                 6DOF eye construction, hotkeys, diagnostics, logging
            ├─ dl2_xr.cpp/.h  OpenXR instance/session on the game's D3D11 device
            │                 (XR_KHR_D3D11_enable), two eye swapchains, pose-tagged submission
            └─ xr_types.h     hand-written OpenXR 1.0 ABI subset (no SDK, loader LoadLibrary'd)
```

**The module pins itself** (`GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_PIN)`) in `DllMain`. The
game loads `dinput8.dll` transiently (LoadLibrary → DirectInput8Create → FreeLibrary); without
the pin the init thread runs in unmapped memory and the game crashes on boot.

D3D11 hooks are installed on the vtables of a throwaway device/swapchain created at init.

### The engine's camera API

`engine_x64_rwdi.dll` exports its camera interface by mangled name, so most camera access needs
no patterns:

- `?GetViewCamera@ILevel@@QEBAPEAVIBaseCamera@@XZ` (hooked; the game calls it hundreds of times per
  frame, which gives the live `ILevel*` and `IBaseCamera*`)
- `IBaseCamera::GetViewMatrix` (mtx34), `GetProjectionMatrix` / `GetCombinedMatrix` (mtx44),
  `GetInvCameraMatrix` (mtx34), `GetFOV`, `GetAspect`, `GetClipNear/Far`, plus setters
  (`SetPosition`, `SetInvCameraMatrix`, `FromForwardUpPos`, ...) and
  `ILevel::SetOptionalRenderCamera`.
- A second path, as found by the open-source EGameTools project: CLobbySteam static via the AOB
  `48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ?? 48 83 C0` → `+0xF8` CGame → `+0x380` CLevel → `+0x20`
  LevelDI (= ILevel). Offsets have moved between patches; the export hook is preferred.

Never cache the `IBaseCamera*`: it changes on area and menu transitions.

### Render structure (measured)

- Single-threaded immediate context: no deferred contexts, no command lists, ~1000–6000 draws
  and ~100 compute dispatches per frame, all on one thread together with Present.
- All constant buffers are streamed with `Map(WRITE_DISCARD)/Unmap`; `UpdateSubresource` is
  never used for CBs.
- Scene passes run at the upscaler's internal resolution (DLSS/FSR via Streamline when enabled)
  and are upscaled to the backbuffer.

### Same-frame stereo (the shipping path, F11 → mode 2)

- **`engine+0x8324D0` ("RenderB", gameplay)** renders a complete frame **and calls Present**.
  The simulation update lives in its caller (`engine+0x8043E0`). Calling RenderB twice
  therefore renders the same world state twice. `engine+0x7EB120` ("RenderA", menus/simple path)
  gets the same treatment. Both are hooked only if a 12-byte prologue signature matches.
- `DoubleRender`: refresh the camera snapshot, freeze one head pose for both eyes, then
  pass 0 with eye shift −halfIPD, pass 1 with +halfIPD. `g_renderSlotEye` tells every other hook
  which eye is being drawn.
- Each pass's nested Present is captured in the Present hook: the backbuffer is copied into that
  eye's texture, tagged with the pose and FOV it was rendered with. After pass 1 both are
  submitted to OpenXR in one `xrEndFrame` (one XR frame spans both presents).
- FOV tag comes from the game's own projection (`atan(1/m00)`, `atan(1/m11)`). IPD comes from the
  located OpenXR views (accepted between 30 and 90 mm).

### Per-eye camera: the 912-byte camera CB

One fixed-layout 912-byte CB carries the scene camera, re-uploaded before almost every pass
(~200 uploads/frame). Layout (verified byte-exact against the engine API):

| offset | content |
|-------:|---------|
| 0x000 | **main view-projection**, reversed-Z infinite far, TAA jitter baked in |
| 0x040 | view (mtx34) |
| 0x080 / 0x090 | unprojection constants |
| 0x0A0 | inverse camera (camera→world, mtx34; columns = right, up, forward, position) |
| 0x0D0 | camera position (float3) |
| 0x110–0x1A0 | scene-dependent: shadow-cascade matrices or inverse-camera copies |
| 0x1D0 | projection |
| 0x210 | view-projection (comb = P·V, row-major) |
| 0x250 | view (second copy) |
| 0x280 | inverse camera (second copy) |
| 0x2B0 | projection (jittered) |
| 0x2F0 | current translation-free view-projection |
| 0x330 | **previous-frame** translation-free view-projection |
| 0x370 | sun direction |
| 0x380 | time |

At Unmap the hook rebuilds the eye camera and writes:

- **classic** (NUM1): view, view2, inverse cameras at 0xA0/0x280, comb = P·V' with the game's
  own P (jitter kept).
- **comb0** (NUM2): the 0x000 matrix right-multiplied by the rigid world delta
  `W = invcam_center · view_eye`. This preserves the reversed-Z convention and jitter exactly.
  It fixed terrain; it never matched an API fingerprint because of the reversed-Z form.
- **Sweep912** (NUM3): generic match-and-replace of camera fields at unknown offsets. **Off by
  default**: it corrupts the image and tree billboards.

A perspective gate on the projection block keeps shadow/ortho/UI uploads untouched.

### Per-eye camera: dynamic objects

People, zombies and vehicles do not read the camera CB for their transform: the CPU bakes
`MODELVIEW = V · model` into per-object CBs (128–4096 bytes). `ObjCamRewrite` (NUM4, on): for
each affine 3×4 block Y in the buffer, if another block equals `V_game ∘ Y`, replace it with
`V_eye ∘ Y` (delta form, jitter-preserving). The center camera comes from the 912 CB content
cached before rewrite (`CacheCbCam`) and refreshed at the start of each double render
(`RefreshCamSnap`), so the world and object paths share one camera. Using the frame-boundary
camera instead made dynamic objects drift while walking.

### 6DOF

`Build6DofEye` takes the live camera basis (R, U, F, C from invcam 0xA0) and the frozen HMD pose,
and builds the eye basis. OpenXR app axes map to the game camera as appX→R, appY→U, −appZ→F.
That mapping is a reflection: it inverts yaw and pitch but preserves roll, so only the x/y
quaternion components are negated. An earlier full conjugate also flipped roll, which made the
world roll with the head at double rate. END flips roll sign only, as an escape hatch for other
runtimes. The submitted eye is tagged with exactly the baked pose (orientation = head quat,
position = head pos + IPD along head-right), so the compositor only corrects latency.

## Keys

| Key | Action |
|---|---|
| F11 | cycle double render: 0 off, 1 two renders with the same camera (stability test), **2 same-frame stereo** (turns VR on) |
| F1 | headset output on/off |
| F2 | 6DOF on/off (VR on); monitor sway-inject (VR off) |
| F5 | recenter |
| F3 / F6 | world scale ±5 % (VR on); sway shift ±5 cm (VR off) |
| INSERT / DELETE | bake head rotation / position into the render (default on) |
| HOME | VR: pose tag head-tracked vs world-locked. Flat: synthetic yaw sway through the full 6DOF path, to see head-locked geometry on a monitor without a headset |
| END | invert head-roll sign |
| NUM1 / NUM2 / NUM3 | 912 rewrite layers: classic (on) / comb0 (on) / Sweep912 (**off**, corrupts) |
| NUM4 | dynamic-object modelview rewrite (on; needed or NPCs head-lock) |
| NUM5 | screen-space FX camera rewrite (on; effectively a no-op, see below) |
| NUM6 | diagnostic: +30° yaw on the rendered camera only (does lighting follow the 912?) |
| NUM7 | experimental: center-eye pose into the engine's real camera before rendering (off) |
| NUM8 | experimental: replay eye-0 RT/depth clears in eye 1 (off; no visible effect) |
| NUM9 | experimental: raw camera-position rewrite (off; corrupts the image) |
| NUM0 | cycle exact-shader knockout for FX isolation |
| PGUP / PGDN | old 432 B / 176 B CB poke (off; those are per-object buffers, not camera) |
| F9 | `SetOptionalRenderCamera` experiment |
| F7 | arm one-frame CB fingerprint, census, 912/FX/small-CB dumps (also auto-arms once in gameplay) |
| F8 / F10 / F4 / F12 | status dump / per-frame log line / reset counters / draw call-stack capture |

## Log lines

`dl2probe_log.txt` next to the DLL:

- `[hook]` / `[init]` / `[eng]` / `[aob]`: hook installs, signature mismatches, export resolution.
- `[hb f…] … dbl=2 draws1=… draws2=… disp1=… disp2=…`: per-eye draw and dispatch counts. In
  stereo both eyes should be roughly equal (draws ≈ 6000, dispatches ≈ 100).
- `[cbs] 300f: sweep pos/comb/comb0 rw0 objMV fxcam …`: rewrite counters every 300 frames.
  `objMV` collapsing in bursts means dynamic objects are head-locking.
- `[vr] eye=… halfIPD=… submits=… running=…`: XR submission state.
- `[census]`, `[cscensus]`, `[dump912]`, `[objpair]`, `[fxdump]`, `[smalldump]`: one-shot
  analysis dumps from F7.

`dl2_xr.log`: OpenXR bring-up. `SESSION RUNNING` means frames are reaching the runtime. If no HMD
is found at start, `xrGetSystem` is polled every 2 s for about 5 minutes.

## Approaches that failed (do not repeat)

- **Blind writes to 0x110–0x1A0.** Early builds wrote the eye inverse camera there because a
  fingerprint once showed invcam copies. In other scenes those slots hold shadow-cascade
  matrices that skinned shaders read, so NPCs broke. Rule for this engine: verify a CB field's
  content every write; offsets are not stable semantics.
- **Broad 912 matching (Sweep912) and the raw camera-position write at 0x0D0 (NUM3/NUM9).** Both
  corrupt the image and tree billboards. Any further lighting/reflection work should target one
  identified shader pass with its own toggle.
- **432 B / 176 B "secondary camera CBs".** They are per-object CBs (~1850 uploads/frame);
  poking them per pass did nothing for head-lock.
- **"Screen-space FX read a dedicated camera CB (the 320 B)."** Decoded: it holds global/GI
  constants (grid size, clipmap, streaming position, GI volume). No view-projection. Small CBs
  (16/32/48/64 B) hold camera position only, TAA jitter, or identity; no camera orientation.
- **"Deferred lighting runs once per frame, not per eye."** False: compute dispatches double per
  eye.
- **"Lighting rides the 912 camera."** False: with NUM6 (+30° render yaw) the geometry swings
  but lighting and shadows stay put, so lighting's camera is reconstructed elsewhere.
- **Engine-level eye cameras.** `ILevel::SetOptionalRenderCamera` with a cloned, shifted camera
  (F9) and writing the render-side camera slot (`impl(+0x38)+0x13A8`) did not give a usable
  per-eye render. Putting the head pose into the real camera with `SetInvCameraMatrix` (NUM7) is
  rejected by the game's camera implementation.
- **Alternate-eye rendering** (one eye per game frame) worked as a first headset image but is
  superseded by same-frame stereo.

## Open problems and where to go next

1. **Temporal ghosting while moving** (trees, shadows, reflections melt). Most likely DLSS/TAA:
   the eyes alternate L, R, L, R into one history, so each eye is reconstructed against the other
   eye's previous frame. Not yet confirmed. **First test:** DLSS/FSR/XeSS off (native),
   frame generation off, anti-aliasing off or SMAA (not TAA), motion blur off, then move around.
   If that clears it, options are:
   - Ship with temporal off (possibly forced from the mod by blocking DLSS init or forcing the AA
     setting).
   - Per-eye history: intercept the Streamline/DLSS evaluate (`slEvaluateFeature` in
     `sl.interposer`) or its resource binds and give each eye its own history.
   - Write the **previous eye's** view-projection into 0x330 (it is not rewritten in the
     default configuration) so camera motion vectors are correct per eye; or set it equal to the current eye
     to neutralise camera motion vectors.
   - Render both eyes into separate targets before one Present (large rework).
2. **Moving lights and reflections** do not follow the eye camera. Start from the clean baseline
   and find the exact pass with F7 dumps and NUM0 knockouts. Volume-light pixel shaders were seen
   to subtract a center-eye camera position held in their own CB0; an unreleased experiment that
   patched it per draw was not confirmed to help.
3. **Blurry image.** Undiagnosed. Check the upscaler's internal resolution, the eye swapchain
   resolution (currently the backbuffer size), and sRGB handling.
4. **FOV fill, VR HUD, motion controllers, performance**: not started.
