# MechAssault: game-specific patches

Patches for the MechAssault port (`ma-recomp`) that the SDK does not carry. Apply with `git apply` from the `ma-recomp` root.

| Patch | Against | What it is |
|-------|---------|------------|
| `own-aspect-and-crt.patch` | `ma-recomp` `11e0415` | The port's own picture aspect (4:3 / 16:9 / 21:9), "original rendering" with a CRT look, and the game's culling frustum hook (`sub_000381B0`, the camera's 4:3 planes). It drives the SDK's `app::g_aspect` and the `Extension` hooks `SplitQuads`, `Geometry`, `Clip`, `Display` and `ShaderHooks::Position`. |

The SDK now has the generic parts as settings: **Console look** (4:3 at the game's own resolution) and the **CRT filter**, and the F1 aspect setting handles ultrawide. Apply this patch only if the SDK's ultrawide shows the game wrongly, for example 3D culled at the sides, since the game's own frustum is 4:3. It sets `app::g_aspect`, which makes the SDK leave the aspect to the port.
