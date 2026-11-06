# CKZ/KZT prediction backport

Owner instruction 06.11.2026: adapt upstream 538da92, ab012a1, 9e84def, check CKZ and KZT, deploy only KZ canary. This is authorization to implement and deploy this concrete backport; no additional design approval is needed.

Approach: targeted backport onto deployed native 75ec25df/cyb.342, retaining the current SDK and all local movement/replay/menu changes. A full upstream merge would replace unrelated systems. Merely changing cvars would omit the scheduling and teleport fixes.

Apply upstream half-tick prediction scheduling and conservative air-only friction-stash trace to CKZ and KZT. Clear injected prediction state on cleanup and teleport. Add schema accessors required by these fixes using the current SDK collection mechanism. Track consumed/pending server angle changes in both modes and avoid interpolating server-originated angles.

Remove server-only legacy jump latch resets from both modes. Apply the upstream prediction-compatible jump penalty value (-1.267) in CKZ and KZT; retain KZT's real input press timestamps and its own perf window/grid, height, speed, prestrafe and acceleration behavior. CKZ keeps its existing input quantization; KZT keeps real subtick times.

Tests execute production method bodies with host engine stubs to cover full scheduling slots, air/ground/near-ground trace guards, cleanup/teleport, server-angle acknowledgements, jump latch behavior and KZT input timestamps. Run existing host tests and build through CI (no local native SDK build). Independent Sol/medium review before push/release. No product texts change.

Deployment must use current applied KZ canary payload as base, preserve runtime/config/other pins and fallback, and retain a manual promotion gate. Capture non-canary baseline and compare after applied/health/30-minute soak. No early/fleet promotion, direct pin changes, or occupied-server gameplay testing without owner authorization. Runtime plugin loading and host simulation must be distinguished from real-player input validation.
