# AWR recorded checkpoint index: canary implementation plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Correct the replay-to-AWR checkpoint index conversion and deploy the reviewed native fix to KZ canary only.

**Architecture:** Keep replay files one-based and `awr::Frame` zero-based. Normalize once at the existing TickData→Frame adapter, using a small pure helper shared with host regressions. Keep coordinate fallback for undo and missing checkpoints.

**Tech Stack:** C++17 host tests; existing Steam Runtime CI build; kz-release and orchestrator.

**Spec:** Approved conversation and diagnostic report `outputs/2026-10-27-awr-kiwiconvulsion.md` in the parent task directory. User explicitly requested completion through canary on 27 October 2026.

## Global constraints

- Base on c9245de704d87aa8757528811db35c742662ceb9, preserving shared-mapvote and all current canary configuration.
- No replay format change; no normal timer, score or physics changes.
- No new or modified player-facing copy.
- No early/fleet promotion, Workshop upload or mass AWR recalculation.
- Recalculation after future promotion is limited to the top ten PB per map/course/mode; API candidate selection and queue preparation are a separate following stage.
- Read RCON status and player list immediately before rollout; do not interrupt an occupied server without explicit authorization.
- Full native builds run in CI, not locally. Reports use Markdown; no secrets in logs.

## Review focus

- Recorded checkpoint 1 must select the first checkpoint, including when a later pass comes within 16 units of it.
- Repeated arrivals must join back to checkpoint placement rather than leave failed attempts.
- A map teleport near a checkpoint must not become a false cut anchor.
- Zero/negative recorded checkpoint identifiers must use coordinate fallback, including INT32_MIN without overflow.
- Practice freezing, splice arrivals, pause subtraction, undo and other course/mode behavior must remain covered by existing regressions.

## Task 1: Normalize recording boundary and prove the failure

Files: `src/kz/replays/awr_cut.h`, `src/kz/replays/playback.cpp`, `tests/awr_recording_index_test.cpp`, this plan and `CYBER.md`.

- [x] Compile/run the existing host suite as baseline.
- [x] Extract the existing identity conversion into `CheckpointIndexFromRecording(int32_t)` and call it from the adapter, without changing behavior.
- [x] Add boundary and consumer regressions using one-based recorded checkpoint numbers. The two failed-attempt fixtures must produce one full cut anchored to checkpoint placement, with method `a`.
- [x] Run the new executable and observe a runtime assertion failure with identity conversion.
- [x] Implement positive-number minus one; non-positive becomes -1. Run new and existing host suites.
- [x] Run the downloaded SVOking replay through the production helper and actual cut algorithm; require 654789 ms and matching positions at both reported joins.
- [ ] Obtain independent Sol/medium review, address actionable findings, commit and push the native branch.

## Task 2: Build, deploy, verify canary

- [ ] Read current main pin/profile and live canary status; retain all other plugins and configs.
- [ ] Run canonical kz-release from main with the reviewed immutable fork SHA (dry_run first if canary occupied).
- [ ] Confirm full native CI success, artifact SHA/ref, registered profile, actual applied version and health.
- [ ] Confirm RCON native version, shared-mapvote capability and preserved relevant config.
- [ ] If the diagnostic replay is locally cached, use read-only `kz_awr_debug` on an empty server and verify the new cut without writing central AWR values.
- [ ] Save final Markdown report with rollout, commits, checks and remaining soak/recalculation limitations.

## Authorization and status

User approved the approach in conversation and explicitly requested canary deployment. The work proceeds inline; independent review is required before native push/release. This plan does not grant promotion or historical recalculation authority.
