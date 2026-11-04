# SDK patch

`sdk.patch` is the single diff against the unmodified XboxXBERecomp checkout, upstream `91146ff` (`git apply patches/sdk.patch` from its root).
Regenerate it with `git add -N xdk && git diff origin/main --output=<path>` in the SDK (branch `baseline`).

| Area | Files | Why |
|------|-------|-----|
| XDK 4928 / 5659 data | `xdk/{4928,5659}/{layout,signatures}.toml` | Layouts and function signatures for the builds of Dark Alliance and Crimson Skies. MechAssault itself uses the stock 4831 data. |
| setjmp/longjmp | `tools/recomp/{analysis.cpp,analysis.h,codegen_emit.cpp,main.cpp}`, `runtime/cpu/dispatch.cpp` | `[xbe] setjmp` config key and the nested resume loop. MechAssault uses it for the D3DX image loaders. |
| DirectSound | `runtime/xdk/dsound.cpp` | Buffer/stream HLE, packet completion reports the full size, F3 voice and stream-event rings. |
| Kernel | `runtime/kernel/{xboxkrnl,objects,files,kernel.h}` | `ReadFileEx` APC dispatcher (export 232), FIFO mutant hand-off, empty directory mask, file-call ring. Files: `NtSetInformationFile` rename closes and reopens the host handle (Windows refuses to rename an open file; MechAssault's `filecopy.tmp` -> `fbd13caa.cache` step). `NtReadFile`/`NtWriteFile` return `STATUS_PENDING` (with the transfer, status block and event already complete) on handles opened without `FILE_SYNCHRONOUS_IO_*`; MechAssault's cache copier only decodes the next chunk when ReadFile reports `ERROR_IO_PENDING`. |
| Vertex shader size | `runtime/xdk/d3d8.cpp` | `CreateVertexShader` writes the instruction count at record+8; `DrawIndexedVerticesUP` HLE. |
| Point sprites | `runtime/gpu/{shaders.cpp,shaders.h,d3d12.cpp}` | `oPts` size and `POINTSPRITEENABLE` texture coordinates (overlaps with upstream's own point sprite change `12d1351`: check). |
| GPU / diagnostics | `runtime/gpu/{d3d12.cpp,render_thread.cpp,ext.h}`, `runtime/app/{main,settings}.cpp`, `runtime/core/log.cpp` | Dynamic-texture waits, visibility fence, shader cache, `Extension::Present`, transform dump in the F3 capture, log flush. `XBE_LOG=1` makes a release build log every level (flushed per line); the crash log also dumps the memory and vtables behind esi/edi/ebx/ebp. |

Game-side fixes that are not in the SDK live in `config/game.toml` and `src/game/hooks.cpp`.
