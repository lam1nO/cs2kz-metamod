# Replay save prompt and practice seek

**Goal:** Save first, then offer playback of that exact UUID. Extend positional replay seek to practice, including practice without a running clock.
**Architecture:** Reuse cs2menus (E select, F exit) and existing replay load/session paths. Keep the current course, radius, skipped-tick and run-window guards.
**Authorization:** Owner requested both changes through canary without further questions on 29.10.2026. Russian title is supplied verbatim. Existing Yes and exit translations are reused; English title is a direct localization.

- [ ] Add and run failing eligibility tests (running timer, practice with stopped timer, neither, dead player).
- [ ] Implement alive + (timer running or practice) eligibility and successful-save menu using UUID item payload.
- [ ] Run host regression suite; independent review; CI native build.
- [ ] Release using current main KZ profile through kz-release to srv-5 only; verify applied, health, version and fleet unchanged.
