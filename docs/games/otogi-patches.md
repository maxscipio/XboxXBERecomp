# SDK patches

`sdk.patch` is `git diff origin/main` of the XboxXBERecomp checkout on branch `otogi`
(`baseline` from the mechassault port, plus the commits below). Apply it to an upstream
`TGP482/XboxXBERecomp` checkout with `git apply`.

Regenerate: `git add -N xdk\5455\*` then
`git diff origin/main --output=..\otogi-recomp\patches\sdk.patch`.

## Changes on top of `baseline`

| Commit | Change |
|--------|--------|
| `3d87dcb` | `xdk/5455/layout.toml`, `xdk/5455/signatures.toml`: XDK 5455 (Otogi: Myth of Demons, Europe) |
| `f712781` | 5455 signature fixes (`XPP_MemoryUnitType` pointer is at +0x39 in the unaligned 5455 table), new `IDirectSound_SetAllParameters` HLE (`runtime/xdk/dsound.cpp`, listener position/orientation) |
| next | `runtime/kernel/files.cpp`: reads on an overlapped (non-`FILE_SYNCHRONOUS_IO_*`) handle return `STATUS_PENDING` after completing the I/O, as the XAPI `ReadFile` / `GetOverlappedResult` pair expects. `runtime/xdk/dsound.cpp`: `SetLFO` HLE for buffers and streams. 5455 signatures for the stream/buffer `SetFormat`, `Release`, `SetPitch`, `SetLFO`, `SetEG`, `SetFilter`; `CreateSoundBuffer` is 0x128E97 and `CreateSoundStream` 0x128EBB (matches the 5659 signature) |
| next | `runtime/xdk/d3d8.cpp`: `D3DDevice_RunPushBuffer` applies the `D3DFixup` records; the present interval is kept and `app::Pace(interval)` / `app::SetEngineFps` (`runtime/app`) pace to 60 / interval. 5455 signatures for `CDevice_KickOff`, `D3D_SetFence`, `D3D_BlockOnTime`, `D3D_MakeRequestedSpace`, `D3DDevice_InsertFence`, `D3DDevice_RunPushBuffer` |
| next | `runtime/xdk/d3d8.cpp`: the `SetVertexShaderConstant1/1Fast/4/NotInlineFast` HLE write `1EA4`/`0B80` commands into a push buffer that is recording (device flag 4) instead of setting the constants, so a title's fixups find the placeholders |

| next | `runtime/gpu/{state.h,shaders.cpp,shaders.h,d3d12.cpp}`, `runtime/xdk/d3d8.cpp`: a vertex program input no stream supplies reads the current vertex attribute the title set with `SetVertexData*` (`State::vattr`, uploaded as `DrawConstants::vdef`) instead of a fixed black/white (same fix as bgda-recomp) |
| next | `runtime/gpu/shaders.cpp`: NV2A NaN rules as xemu has them (xemu PRs #1780/#2089/#2485): a NaN vertex color component reaches the combiners as 1 instead of `saturate`'s 0 (Otogi's skies and characters in other stages), and a NaN or infinite fog coordinate is no fog in the linear and exp modes, all fog in exp2 |
| next | `runtime/xdk/d3d8.cpp`: a draw recorded into a push buffer keeps only the render states that changed since the draw before (what the library's lazy flush pushes into the buffer); when it runs, the rest come from the device. Otogi records each model once and sets its fog (and lights) before every `RunPushBuffer`; snapshotting every state at record time left the whole level with the load-time "fog off, white", hence the black sky and unfogged forest |

### XDK 5455

No upstream signature set exists for 5455. The CDevice/D3D/DSOUND/XPP layout was derived by
comparing the 5659 library code with the XBE (displacements shifted by 0x414 from 0x770 to 0x96C
and by 0x410 above). `signatures.toml` was produced with `xbe_symbols --sign` from the
hand-verified `config/game.toml` (144 of 243 functions, 13 data symbols). The functions the game
does not link, and the DSOUND/Winsock/XNet entries, stay 0.
