# Burnout recomp: changes made to get it running

How the European Xbox Burnout (title ID 0x41430006, XDK 1.0.4361) went from an ISO to a playable native build with video, sound, controller and a setup screen. Paths are relative to this folder. This list is rebuilt from my session notes. Items marked *(from notes)* are summarised from checkpoints rather than re-checked in the code today.

## 1. Setup and toolchain

- Extracted `default.xbe` from the ISO and scaffolded the game project from the SDK template (Xbox-XBERecomp).
- Installed LLVM 23.1.3 and VS 2022 BuildTools, and used Capstone, Ghidra 12.1.4 and JDK 21 for disassembly work.
- The SDK ships only XDK 5849 data, but Burnout's 7 libraries are all 4361. Using a 4361 installer you supplied (licensing flagged, local-only):
  - matched 632 library function names to addresses in `default.xbe`;
  - derived the 4361 data layout, `sdk\xdk\4361\layout.toml`, from disassembly and relocations. Only anchors verified against the game's code are kept, and a missing key traps when used;
  - wrote the game's symbol map, `config\game.toml`, from those matches.
- Matching initially missed the DirectSound members: C++-mangled names were dropped, and COMDAT section symbols at offset 0 cut function bodies short. A second matcher that used only external symbols as boundaries found them.
- Disc check: a stray loose `assets\default.xbe` (left by my launch script) made the disc check fail. `assets\` now holds only the ISO, and `default.xbe` is staged only for a build and removed afterwards.

## 2. Kernel, disc, save, relaunch

- Mounted the XISO as the game's disc. The disc-authentication IOCTL `0x4D014` is deliberately **not** faked.
- Save game: mounted the save and utility drives (`MountDefaults`, `XMountUtilityDrive`) *(from notes)*.
- `XLaunchNewImageA` passes through to the runtime's relaunch.
- Allocator: `AllocContiguous` allocates top-down, which fixed contiguous-memory exhaustion *(from notes)*.
- USB: the XDK's host-controller enumeration is stubbed in `src\game\hooks.cpp` (`HCD_EnumHardware`), because the runtime does not emulate the controller at `0xFED00000`.

## 3. Controller

- `XGetDevices` HLE and `XPP_GamepadType = 0x175534` in `config\game.toml`, so the game detects the pad and the controller prompt advances.
- `kPadMapping` in `sdk\runtime\core\input.cpp`: an SDL mapping for the DirectInput pad you use, which SDL does not know.

## 4. Recompiler

- `sdk\tools\recomp\codegen.cpp`: the flags are saved at `ret` and reloaded after `call`. The C runtime's `sin`, `cos` and `tan` return `arg + 1` through the flags, so without this the camera and car matrices were NaN.

## 5. Direct3D (HLE and renderer)

- Fixed start-up crashes in the default-state and device-state setup (wrong 4361 addresses and layout keys), and replaced the guest push-segment slow path.
- Fixed P8 (palette) textures drawing as gray blocks, and the blend state and ordering of the splash and video alpha *(from notes)*.
- Fixed garbled 3D: the HLE render-state table overlapped the vertex stream table. The stream table is now placed correctly, and there are a pusher and control block, a bounded `SetComplex` mirror, and `Walk(Dev(kDevPush))` in the draw calls. `CDevice_KickOff` and `D3DDevice_SetIndices` are no-ops.
- Transform constants: an `XfLoad` handler for the NV2A pushbuffer methods, plus `UpdateViewportConstants` (viewport constants `c-38` and `c-37`).
- **4361 render-state numbering** (`sdk\runtime\xdk\d3d8.cpp`, layout key `RenderState_Numbering = 4361`): the game numbers render states differently from the 5849 table the renderer assumes. The guest-to-host maps are:
  - deferred block: guest 82 ↔ host 92;
  - material sources: guest 96–99 ↔ host 110–113;
  - ambient: guest 105 ↔ host 115;
  - points: guest 106–113 ↔ host 116–123;
  - complex block: starts at guest 117 ↔ host 136.
  `TwoSidedLighting` no longer enables the back-colour path. This fixed the black road and buildings, and colours are correct.
- Cube-face tiles are copied by quads into cube surfaces, and `BindCubeTargets` in `sdk\runtime\gpu\d3d12.cpp` binds them for sampling.
- **Reflections:** the game sets its pixel shaders through `D3DDevice_SetPixelShaderProgram`, which builds `{1, 0, definition}` and jumps into `SetPixelShader`. The runtime could not read that handle record because `PixelShader_Definition` (`0x8`) was missing in `sdk\xdk\4361\layout.toml`. Adding it makes the car draws use the combiner shaders that sample the cube map and the fresnel ramp.

## 6. Sound (DirectSound)

- The runtime already has a host mixer (`sdk\runtime\xdk\dsound.cpp`, `sdk\runtime\audio\xaudio2.cpp`). It only activates for addresses listed in `config\game.toml`, and nearly all of the DirectSound ones were unset.
- Mapped the 4361 addresses: the `IDirectSoundBuffer`, `IDirectSound` and `CDirectSoundStream` wrappers, the stream vtables, `DirectSoundCreateStream`, `CreateSoundBuffer` and others. Added `SetEG` and `SetMode`, and the new layout keys (`CDirectSoundStream_Vtable`, `Vtable2`, `EffectsImage_*`).
- `DirectSoundCreate` calls the guest's `DSound_GetObject` (`0x159861`), because a fabricated host object crashed the constructor chain.
- The APU and DSP MMIO has no host device behind it, so the DSP busy-waits and submit spins are replaced (`DspAckLoop`) and the handshakes cleared.
- The library's own start-up code calls the same wrappers that are HLE'd, with buffers it made itself. `BUFFER` methods therefore fall back to the original recompiled body for non-host buffers (`GUEST_ORIG`). Without this `DirectSoundCreate` failed and the game later called with a null `this`.
- Music and effects now play.

## 7. Host features added this week

- **Esc quits** (`sdk\runtime\app\main.cpp`), unless the F1 window is open, which Esc closes instead.
- **Controller:** set up in the F1 window. The game's own F9 controller screen (`pad_mapping.cpp`, ported from the Baldur's Gate recomp) is removed. The SDK turns SDL's XInput on, so wired Xbox-style pads are recognised without game code. The earlier launcher and its start screen stay removed; display options are in F1.
- **FPS overlay:** F10 toggles it (`src\common\overlay.cpp`). It is saved as `show_fps`, and is also a row on the F1 Game page.
- **Settings window (F1):** the SDK's own (`settings_window = true`): display, aspect ratio (4:3, 16:9, fill the screen), anti-aliasing (FXAA, SMAA, MSAA), sharpening, CRT, audio, controller and Quit (Back + Start together on a pad opens it). It replaced the game's own F8 panel, FXAA pass (`enhance.cpp`) and F12 quit dialog. The 3D view widens by itself at 16:9: the game's own widescreen path draws it, checked in a race at 4:3 and 16:9.
- **21:9** (`src\common\game_info.cpp`), checked on a 16:9 display (21:9 band with bars) in a menu and a race:
  - `OnInit` sets `app::g_aspect` to 21:9 and `app::g_wide_2d` when the aspect setting is "fill the screen" and the output is narrower than 21:9. The renderer then widens its targets and keeps the 2D in a centred 16:9 part;
  - the game widens its own 3D through its widescreen path, which reads the 16:9 float at `0x189ACC`; `.loaded` patches that float to the real aspect (`OnLoaded`);
  - the SDK change: `app::g_wide_2d` (`runtime\app\app.h`, `settings.cpp` `FillsScreen`), and `D3D12Backend::HudSqueeze` treats a depth test that always passes as no test, since Burnout draws its HUD and menu text that way.
  - `RenderScale` (`runtime\app\settings.cpp`) sizes the render targets by the letterboxed picture's height, not the output's: 21:9 on a 16:9 display rendered 2520x1080 where 1920x823 is shown, and held about 54 fps on an Iris Xe; it now holds 60, like 16:9 (about 59.8 average, GPU about 31% busy, so the remaining dips are CPU side).
  - Not checked: a real 21:9 or wider monitor, the pause menu, other tracks. The Vulkan backend has no `g_wide_2d` handling.
- **Frame rate** (30, 60, 120; default 60), a row on the F1 Game page saved as `frame_rate` (an index into 30, 60, 120):
  - the game steps once per Swap with a fixed dt, a float at `0x664C8C` (1/60 s, or 1/50 s when `sub_00074176` finds the PAL flag at `0x8A7834`) and whole milliseconds at `0x664C88`. `graphics::ApplyGameFps` (`src\common\graphics.cpp`) rewrites those two, and only when they hold the game's own or our earlier value. The `.rdata` constants 1/60 and 1/50 are shared with other code, so they are not touched;
  - `app::g_step_rate` makes `Pace()` hold to the step rate, and `app::g_show_every` makes `D3DDevice_Swap` present one step in N (30 steps at 60 and shows every second one). The choice replaces the SDK's frame rate cap;
  - 120 is experimental: anything that counts whole frames runs faster.
- Memory experiment: a 16 MB upload ring cut memory from about 343 MB to 195 MB but stuttered, so it was reverted to 64 MB. See `TODO.md`.

## 7b. SDK update

- The game now builds against the main SDK repo (`xberecomp-ref`, `sdk\` is a junction to it), which already carries all the Burnout SDK work. The earlier `sdk\` checkout, with its local edits, is kept as `sdk.old-local`.
- The SDK was fast-forwarded from `65e959d` to upstream `f7b13fd` (129 commits). The uncommitted Burnout changes were stashed and re-applied, with conflicts resolved in `memory.cpp`, `input.cpp`, `d3d12.cpp`, `dsound.cpp` and `d3d8.cpp`. A backup of the previous patch is outside the repo, and `stash@{0}` in `sdk` is still held as a safety net.
- `config\game.toml` `[xbe]` now needs `library = "Game.dll"`.
- Merge fixes: a duplicate `BUFFER(SetEG, 2)` in `dsound.cpp`, the `Walk` function header in `d3d8.cpp`, and a `<cstdio>` include removed from `x86_ops.h` (the freestanding `Game_code` build has no such header).
- Hooks added to the SDK for the frame-rate and FXAA work (the same ones `bgda-recomp` carries in `patches\sdk.patch`): `gpu::Extension::Present`, `Backend::Present(front, show)` so a step can go unshown, and `app::g_step_rate` and `g_show_every`.
- Not ported: Dark Alliance's SSAO and soft shadows. They rest on that game's depth projection and shadow map, which have not been measured for Burnout.

## 8. Dead ends worth remembering

- Treating 5849 layout values as valid for 4361 was wrong; only verified 4361 offsets are used.
- Broad XBE sweeps were invalid until Capstone was run with `skipdata=True`.
- Fabricating DirectSound parent objects, or skipping the guest factory, crashed the constructor.
- Many camera NaN, tiled-viewport and cube-atlas probes were temporary diagnostics. All probes are removed.

## 9. Files touched

| Area | Files |
|---|---|
| Config and layout | `config\game.toml`, `sdk\xdk\4361\layout.toml`, `sdk\xdk\4361\*` |
| Recompiler | `sdk\tools\recomp\codegen.cpp` |
| D3D8 and renderer | `sdk\runtime\xdk\d3d8.cpp`, `sdk\runtime\xdk\layout.h`, `sdk\runtime\gpu\d3d12.cpp` |
| Sound | `sdk\runtime\xdk\dsound.cpp` |
| Input and app | `sdk\runtime\core\input.cpp`, `input.h`, `sdk\runtime\app\main.cpp`, `app.h` |
| Game hooks and UI | `src\game\hooks.cpp`, `src\common\game_info.cpp`, `overlay.cpp`, `graphics.cpp`, `enhance.cpp` (and their headers) |

## Licensing

`config\game.toml` symbols and `sdk\xdk\4361\*` derive from the leaked XDK 4361 installer. Settle the licensing before committing or sharing anything. Nothing is committed yet.
