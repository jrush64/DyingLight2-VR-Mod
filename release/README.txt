================================================================================
  DYING LIGHT 2 VR  (experimental, unfinished)
================================================================================

WHAT YOU NEED

  - Dying Light 2 Stay Human (Steam, PC).
  - A PC VR headset and an OpenXR runtime (SteamVR, Meta Quest Link,
    Virtual Desktop, etc.). Set the runtime you use as the active OpenXR runtime.
  - A strong GPU: the game is rendered twice per frame, once per eye.

INSTALLING

  1. Close the game.

  2. Start the game once without the mod and set the renderer to DirectX 11
     in the video settings (the mod does not work with DirectX 12). Close it.

  3. Find your Dying Light 2 folder. In Steam, right click the game, then
     Manage, then Browse local files. From there open  ph\work\bin\x64.

  4. Copy these files into that folder, next to DyingLightGame_x64_rwdi.exe:

         dinput8.dll         (from the builds folder)
         openxr_loader.dll   (from this folder)

  5. Start your VR runtime, then start the game normally from Steam.

USING IT

  Load into gameplay, then press F11 twice. This turns on the stereo
  double render and headset output.

  F11      cycles the double render: off / diagnostic / stereo (VR).
  F5       recenters the view.
  F2       turns head tracking on or off (while VR is on).
  F1       turns headset output on or off.
  F3 / F6  increases / decreases the world scale.
  END      inverts head roll, if tilting your head rolls the world
           the wrong way on your runtime.

  Moving can leave smearing and after-images on trees, shadows and
  reflections. Turning off DLSS / FSR / XeSS, frame generation, TAA and
  motion blur in the game's video settings may reduce it.

UNINSTALLING

  Delete dinput8.dll and openxr_loader.dll from ph\work\bin\x64
  (and dl2probe_log.txt and dl2_xr.log if they were created).

THIRD-PARTY

  openxr_loader.dll is the Khronos OpenXR loader, Apache License 2.0.
  See openxr_loader.LICENSE.txt.
