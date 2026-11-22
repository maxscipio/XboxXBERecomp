# Game workarounds kept out of the SDK

Fixes a port needs in its own `src/game/hooks.cpp` that are not worth building into the SDK: they are specific to the XDK build or the title, or too small and blunt to be a generic feature. Add an entry when a port carries one, so the next port that hits the same symptom finds it here.

| Workaround | Seen in (XDK) | Symptom | Fix |
|------------|---------------|---------|-----|
| Skip USB host controller enumeration | Burnout (4361) | The XDK's USB stack maps the host controller at `0xFED00000`, which the runtime does not emulate, so `HCD_EnumHardware` hangs or faults. Input already comes through the runtime's own path. | Replace `HCD_EnumHardware` in `src/game/hooks.cpp` with a stub that returns at once. It is cdecl with no arguments, so it only pops the return address: `X86_FUNC(GUEST(HCD_EnumHardware)) { ctx.esp += 4; }` |
| XInput back on for pads without an SDL mapping | Burnout (4361), any title with a PowerA/Xbox-style wired pad | `input.cpp` turns SDL's XInput off, so such a pad shows up as a raw DirectInput joystick that SDL has no mapping for, and the game sees no controller. | In `GameInfo::init`, call `SDL_SetHintWithPriority(SDL_HINT_XINPUT_ENABLED, "1", SDL_HINT_OVERRIDE)`; the override beats the SDK's plain `SDL_SetHint`, which runs later. The title's CMake gives that source SDL's include path. |
