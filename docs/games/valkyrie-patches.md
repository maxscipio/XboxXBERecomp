# SDK patches

Gun Valkyrie (XDK 4134, title 49470017). Merged from `..\valkyrie\XboxXBERecomp` (branch `valkyrie`, on top of `ref`).

| Area | Files | Why | Origin |
|------|-------|-----|--------|
| XDK 4134 data | `xdk/4134/layout.toml`, `xdk/4134/signatures.toml` | New XDK build. Layout verified against the Valkyrie code; signatures are a proxy derived from Burnout (4361) because no 4134 libs exist | valkyrie |
| Pad recognition | `runtime/core/input.cpp` | `SDL_HINT_JOYSTICK_DIRECTINPUT` is "0": with "1" an Xbox One pad shows up as a raw DirectInput joystick SDL has no mapping for. Burnout's `kPadMapping` is no longer used by default; a game that needs DirectInput pads must set the hint back | valkyrie |
| XInputPoll HLE | `runtime/xdk/xapi.cpp` | The guest `XInputPoll` reads the handle as an XPP device object and fails on the HLE handle, so the title closed and reopened the pad every ~100 ms and never called `XInputGetState`. Map it with `XInputPoll = <addr>` in `game.toml` (`SYM_XInputPoll`); `XInputClose` needs mapping the same way | valkyrie |
| Depth buffer size | `runtime/gpu/{d3d12,vulkan}.cpp` | `BindTargets` dropped a depth buffer whose size differed from the colour target's. A depth buffer larger than the target is valid (it covers the top left corner), so only a smaller one is dropped. Fixed distant terrain vanishing while thrusting (a 512x384 distortion pass under a 640x480 depth buffer) | valkyrie |
