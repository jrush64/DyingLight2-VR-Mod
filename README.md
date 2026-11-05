# Dying Light 2 VR

An unfinished VR mod for Dying Light 2 Stay Human (Steam, x64, Techland C-Engine, DirectX 11
renderer), built as a `dinput8.dll` proxy loaded next to `DyingLightGame_x64_rwdi.exe`. It renders
each frame twice, once per eye, from the same simulation state, applies head tracking to the game
camera, and submits both eyes to any OpenXR runtime (SteamVR, Meta Quest Link, Virtual Desktop,
etc.) on the game's own D3D11 device.

⚠️ Experimental, **unfinished** community mod. Not affiliated with Techland. Use at your own risk.
Development has stopped, and the source is published so that others can pick it up. See
[What doesn't work yet](#what-doesnt-work-yet) and
[`docs/TECHNICAL_NOTES.md`](docs/TECHNICAL_NOTES.md).

Licensed under [GPL-3.0](LICENSE).

## What it does

The first four items have been confirmed in headset testing.

- **Real geometric stereo, same frame.** The engine's gameplay render function (which renders and
  presents one complete frame) is called twice per game tick, once per eye. Both eyes show the
  same world state; it is not alternate-eye rendering and not reprojection. Draws and compute
  dispatches both run fully in each eye's pass.
- **Per-eye camera.** The engine's 912-byte camera constant buffer (view, inverse camera,
  view-projection, the main reversed-Z view-projection) is rewritten per eye as it is uploaded,
  keeping the game's own projection and TAA jitter. People, zombies and vehicles, whose
  model-view matrices are baked on the CPU into per-object constant buffers, are rewritten too, so
  they stay locked to the world instead of moving with the head.
- **Head tracking.** Head yaw, pitch and roll from the headset are baked into the rendered eye
  cameras, and each eye is submitted to OpenXR tagged with the exact pose it was rendered with.
  Rotation in all three axes is correct and comfortable. Head position (leaning) is applied the
  same way but was not tested separately.
- **Image and billboard corruption fixed.** The broad camera-buffer sweeps that corrupted the image
  and tree billboards are off in this build.
- **Recenter** (F5) and a world-scale adjustment (F3/F6).
- **Fails safe.** The two engine render functions it hooks have byte-signature checks; on a
  different game build the hook is skipped and the game runs as normal.

## What doesn't work yet

- **Ghosting when moving.** Trees, shadows and reflections smear ("melt") and leave after-images
  while the player moves; standing still is clean. The likely cause is temporal reconstruction
  (DLSS / TAA history blending the left-eye frame into the right-eye frame and back), but this has
  **not been confirmed** by testing with DLSS, frame generation and TAA turned off. That test is
  the first next step. Turning those settings off yourself may help.
- **Moving lights and reflections are not fixed.** Lighting and reflections do not follow the
  per-eye camera correctly.
- **Blurry image.** The headset image is softer than it should be. The cause is not diagnosed
  (candidates: the upscaler's internal resolution, and the flat game FOV being stretched across
  the headset's view).
- **No FOV fill.** Each eye is the game's normal flat frame at the game's FOV.
- **No motion controllers.** Play with a gamepad or keyboard and mouse.
- **No VR-specific HUD/UI placement.** The HUD and menus are drawn into each eye at their flat
  screen positions.
- **Performance.** Every frame is rendered twice.
- **DirectX 11 only.** The DX12 renderer is not supported.
- **Developer controls only.** There is no menu or config file; stereo is turned on with a
  keypress, and the source still contains the diagnostic toggles used during development.
- Developed against game version 1.28.3 (Steam). Other versions may refuse the hook or break the
  constant-buffer layout assumptions.

## Building

See [BUILD.md](BUILD.md). In short: install Visual Studio 2022 with the C++ workload, then run
`scripts\build.bat`. All dependencies are vendored in `third_party/`.

## Installing

See [`release/README.txt`](release/README.txt).

## Layout

| Path | What's there |
|---|---|
| `src/` | Mod source: dinput8 proxy, engine/D3D11 hooks, per-eye camera rewrite, same-frame double render and 6DOF (`dl2_probe.cpp`); OpenXR session and submission (`dl2_xr.cpp/.h`); hand-written OpenXR ABI types (`xr_types.h`) |
| `scripts/` | `build.bat` |
| `third_party/` | Vendored MinHook, used by the build |
| `docs/` | `TECHNICAL_NOTES.md`: architecture, engine addresses, approaches that failed and why |
| `release/` | End-user install notes, OpenXR loader, its license |
| `builds/` | A prebuilt `dinput8.dll` (and its `.map`) |

## License

GPL-3.0 - see [LICENSE](LICENSE). Copyright (C) 2026 Halcyon.

Vendored dependencies keep their own licenses: MinHook (BSD-2-Clause), under `third_party/` with
its license file. The shipped `openxr_loader.dll` is the Khronos OpenXR loader (Apache-2.0,
license in `release/`).

The engine instance chain (a byte pattern and three offsets) was first found by the open-source
EGameTools project. No EGameTools code is included.
