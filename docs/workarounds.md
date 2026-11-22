# Game workarounds kept out of the SDK

Fixes a port needs in its own `src/game/hooks.cpp` that are not worth building into the SDK: they are specific to the XDK build or the title, or too small and blunt to be a generic feature. Add an entry when a port carries one, so the next port that hits the same symptom finds it here.

| Workaround | Seen in (XDK) | Symptom | Fix |
|------------|---------------|---------|-----|
| Skip USB host controller enumeration | Burnout (4361) | The XDK's USB stack maps the host controller at `0xFED00000`, which the runtime does not emulate, so `HCD_EnumHardware` hangs or faults. Input already comes through the runtime's own path. | Replace `HCD_EnumHardware` in `src/game/hooks.cpp` with a stub that returns at once. It is cdecl with no arguments, so it only pops the return address: `X86_FUNC(GUEST(HCD_EnumHardware)) { ctx.esp += 4; }` |
