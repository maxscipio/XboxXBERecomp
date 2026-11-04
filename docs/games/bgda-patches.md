# SDK patch

`sdk.patch` is the single diff against the unmodified XboxXBERecomp checkout, upstream `f7b13fd` (`git apply patches/sdk.patch` from its root).
Regenerate it with `git add -N xdk/4928 && git diff origin/main --output=<path>` in the SDK.

| Area | Files | Why |
|------|-------|-----|
| XDK 4928 data | `xdk/4928/{layout,signatures}.toml` | Layout (identical to 4831, verified against the disassembly) and function signatures for the XDK build this title links. |
| DirectSound streams | `runtime/xdk/dsound.cpp` | HLE for `IDirectSound_CreateSoundStream` (the early API creates a stream through the device) and `CDirectSoundStream_SetPitch`. Unhandled, the recompiled XDK code ran on host objects. |
| Kernel APC | `runtime/kernel/xboxkrnl.cpp` | Export 232 `NtUserIoApcDispatcher`, used by `ReadFileEx` as the APC routine of `NtReadFile`; calls the guest completion routine. `RtlNtStatusToDosError` shares a `DosError` helper. |
| Vertex shader size | `runtime/xdk/d3d8.cpp` | `CreateVertexShader` writes the instruction count at record+8, where the inlined `GetVertexShaderSize` reads it. Games that chain `LoadVertexShader` slots with it otherwise load every program at slot 0 (black screen). |
| Point sprites | `runtime/gpu/{shaders.cpp,shaders.h,d3d12.cpp}` | Points were drawn as one console pixel, ignoring `oPts` and the point state. The vertex shader now passes its size (`oPts`, or `D3DRS_POINTSIZE` when not written) to the point geometry shader, which clamps it to min/max and, with `D3DRS_POINTSPRITEENABLE`, emits 0..1 sprite texture coordinates. Fixes the loading screen's flame particles. |
| DirectSound packet completion | `runtime/xdk/dsound.cpp` | `Complete` reports the full packet size for flushed packets too: the title only frees a flushed stream slot when completed == size, so NPC voices went silent after skipping a line. |
| Mutant hand-off | `runtime/kernel/objects.cpp` | A freed mutant goes to the longest blocked wait (first in the object's waiter list, upstream's per-wait queue) instead of to whoever runs first. The game's render thread releases and retakes a mutex every frame and starved its loader (20 s loads). |
| F3 diagnostics | `runtime/xdk/dsound.cpp`, `runtime/kernel/files.cpp`, `runtime/app/main.cpp` | F3 also dumps every DirectSound voice, the last stream events (with guest caller) and the last file opens/reads. |
| Capture transform dump | `runtime/gpu/d3d12.cpp` | F3 captures print, for the first big draw of each vertex shader, the viewport and world-matrix constants (`hw0-3`, `hw96-100`) at full precision; used to read the game's projection for SSAO. |
| Current vertex attributes | `runtime/gpu/{state.h,shaders.cpp,shaders.h,d3d12.cpp}`, `runtime/xdk/d3d8.cpp` | A vertex program input no stream supplies now reads the current vertex attribute the title set with `SetVertexData*` (`State::vattr`, uploaded as `DrawConstants::vdef`) instead of a fixed black/white. The water shader takes its diffuse so; it drew black. |
| Step rate and shown frames | `runtime/app/{app.h,main.cpp}`, `runtime/gpu/{backend.h,d3d12.cpp,render_thread.cpp}`, `runtime/xdk/d3d8.cpp` | `app::g_step_rate` makes `Pace` hold the title's step rate instead of the fps cap; `app::g_show_every` shows one `Swap` in N (`Backend::Present(front, show)` skips the back buffer draw and the DXGI present). For a title that steps once per `Swap` and has no clock. |
Based on upstream `f7b13fd`. The Dark Alliance changes also rest on the Crimson Skies common patches (the `gpu::Extension` hooks the QoL layer
uses), which `sdk.patch` carries.

Game-side fixes that are not in the SDK live in `config/game.toml` (symbols).
