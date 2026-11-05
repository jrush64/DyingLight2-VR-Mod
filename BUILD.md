# Building Dying Light 2 VR

## Prerequisites

- Windows 10/11, x64.
- Visual Studio 2022 (Community, Professional, Enterprise, or the standalone Build Tools)
  with the **"Desktop development with C++"** workload installed. `build.bat` finds it
  automatically via `vswhere.exe`.
- Nothing else. MinHook is vendored in `third_party/`. The OpenXR types the mod needs are
  hand-written in `src/xr_types.h`, so no OpenXR SDK is required.

## Build

```
scripts\build.bat
```

Output: `builds\dinput8.dll` (and `builds\dinput8.map`, useful for resolving crash addresses).

The build is a single `cl.exe` invocation (no solution, no CMake). It compiles the mod sources
together with the vendored MinHook sources and links the CRT statically (`/MT`), so the DLL needs
no separate VC++ redistributable in the game folder. The real system `dinput8.dll` and the
OpenXR loader are both loaded at runtime with `LoadLibrary`.

## Deploying

Copy these files into `<Dying Light 2 install>\ph\work\bin\x64\`, next to
`DyingLightGame_x64_rwdi.exe`, while the game is closed:

- `builds\dinput8.dll` (just built)
- `release\openxr_loader.dll`: an x64 build of the official
  [Khronos OpenXR loader](https://github.com/KhronosGroup/OpenXR-SDK), not built by this repo

The game must use its **DirectX 11** renderer. Select it in the game's video settings, or set
`RendererMode("d3d11")` in `Documents\dying light 2\out\settings\video.scr`.

Logs are written beside the DLL: `dl2probe_log.txt` (proxy, hooks, camera, stereo) and
`dl2_xr.log` (OpenXR). The useful log lines are listed in `docs/TECHNICAL_NOTES.md`.

## Third-party components

| Component | Location | License |
|---|---|---|
| MinHook | `third_party/minhook` | BSD 2-Clause (see its `LICENSE.txt`) |
| OpenXR loader | `release/openxr_loader.dll` (prebuilt binary) | Apache 2.0 (`release/openxr_loader.LICENSE.txt`) |
