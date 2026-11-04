# SDK patch

`sdk.patch` is the single diff against the unmodified XboxXBERecomp checkout (`git apply patches/sdk.patch` from its root).
Regenerate it with `git add -N xdk/5659 && git diff --output=<path>` in the SDK.

| Area | Files | Why |
|------|-------|-----|
| XDK 5659 data | `xdk/5659/{layout,signatures}.toml` | Layout and function signatures for the XDK build Crimson Skies links. |
| setjmp continuations | `tools/recomp/{analysis.*,codegen_emit.cpp,main.cpp}` | The title longjmps; the recompiler needs the `[xbe] setjmp` addresses to emit the resume points. |
| Log flush | `runtime/core/log.cpp` | Warnings and errors are flushed at once, so they survive a crash. |
| Directory listing | `runtime/kernel/files.cpp` | `NtQueryDirectoryFile` with an empty FileName lists everything; it was matched literally, so the title saw an empty disc and showed "problem with the disc". |
| DirectSound | `runtime/xdk/dsound.cpp` | HLE for `IDirectSoundBuffer::SetNotificationPositions`, `SetPitch`, `SetMode`, `SetRolloffFactor`. Unhandled, the recompiled XDK code ran on host objects and crashed. |
| 5659 layout fix | `xdk/5659/layout.toml` | `CDevice_VertexShader` is `0x790` (the shader object; `0x794` is the handle). With `0x794`, native `0x2024A0` read a null object and crashed on mission input (e.g. right trigger). |
| FVF shader signature | `xdk/5659/signatures.toml` | Adds `D3D_FvfShader` (the immediate in `SetVertexShader`) and `D3DDevice_DrawIndexedVerticesUP`. |
| DrawIndexedVerticesUP | `runtime/xdk/d3d8.cpp` | New HLE. The native code wrote pushbuffer commands the SDK ignores and used device fields it does not maintain, crashing in `rep movsd` a few seconds into a mission. |
| Dynamic textures | `runtime/xdk/d3d8.cpp`, `runtime/gpu/d3d12.cpp` | A text texture the title rewrites between draws showed its last contents in every draw (menus all read "BACK"). `Lock2DSurface` and `D3D_BlockOnResource` now wait for draws still sampling the resource, and a texture that changed recently is re-checked at every bind, not once per frame. |
| Shader cache | `runtime/gpu/d3d12.cpp`, `runtime/app/{app.h,settings.cpp}` | Compiled DXBC (`shader_cache/shaders.bin`) persists across runs, so a shader is compiled once ever, not once per run. New `shader_cache` setting (default on). A D3D12 pipeline library was tried and crashed the Intel driver on the second run, so it is not included. |
| Visibility test wait | `runtime/gpu/{render_thread,d3d12}.cpp` | A title that polls `GetVisibilityTestResult` (with `D3D_SetFence` in the loop) waited for a report that only `Present()` delivered, so the main thread spun forever mid-mission. `Fence()` now submits the open batch with a Flush after a `GetReport`, and `Flush()` delivers the reports. |
| Present hook | `runtime/gpu/{ext.h,d3d12.cpp}` | `Extension::Present(Context&, Target&, DXGI_FORMAT&)` runs before the back buffer is bound and may return a replacement picture; `BlitFrame` shows it. The game uses it for post-process anti-aliasing. |
| setjmp resume loop | `runtime/cpu/{dispatch.cpp,x86_ops.h}`, `tools/recomp/codegen_emit.cpp` | `x86_resume` ran the continuation inside a catch handler, so a second longjmp to the same `setjmp` frame (the game restarts its main loop this way) escaped as an unhandled `X86Resume` (E06D7363) and killed the process. It now loops and catches nested resumes whose `esp` is below the frame. |
| Step rate and shown frames | `runtime/app/{app.h,main.cpp}`, `runtime/gpu/{backend.h,d3d12.cpp,render_thread.cpp}`, `runtime/xdk/d3d8.cpp` | From the Dark Alliance port. `app::g_step_rate` makes `Pace` hold the title's step rate instead of the fps cap; `app::g_show_every` shows one `Swap` in N (`Backend::Present(front, show)` skips the back buffer draw and the DXGI present). For a title that steps once per `Swap` and has no clock. Unused until the game sets them. |

Based on upstream `f7b13fd`.

Game-side fixes that are not in the SDK live in `config/game.toml` (symbols, `setjmp`, and a `[[function]]` for 0x185110, a function reached only through a data table).
