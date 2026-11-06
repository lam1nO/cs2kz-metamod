# CKZ/KZT prediction implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Backport the three upstream prediction fixes into both movement modes and deploy only KZ canary.
**Architecture:** Preserve deployed native ancestry and SDK. Keep the mode methods aligned with upstream while adapting schema access and preserving KZT input/perf physics.
**Tech Stack:** C++17, Python host harness, Steam Runtime CI, orchestrator rollout.
**Spec:** docs/superpowers/specs/2026-11-06-strafe-prediction-design.md

## Global Constraints

- Base 75ec25df/cyb.342; no full upstream merge or local native build.
- Preserve KZT real input times, perf grid and acceleration; no product copy changes.
- Only KZ canary; manual gate, unchanged early/fleet, exact applied payload baseline.

## Review Focus

- Full prediction slots must retain a current half-tick event and no duplicate event.
- Near-ground, noclip, cleanup and teleport must not inject ground-friction changes.
- Pending/newly consumed angle changes bypass interpolation; normal turns still interpolate.
- Removing latch resets must preserve KZT press-time collection and perf classification.
- Release must preserve other components and never advance early/fleet automatically.

### Task 1: Backport and regression tests

**Files:** tests/movement_prediction_test.py; src/kz/mode/kz_mode_{ckz,kzt}.{cpp,h}; src/sdk/services.h; src/sdk/entity/ccsplayerpawn.h.
**Interfaces:** Existing OnPhysicsSimulate/Post, OnSetupMove, InterpolateViewAngles, Cleanup, OnTeleport; add CanUseFrictionSplit/ClearFrictionSplit and server-angle state per mode.
- [ ] Write a host harness extracting actual production method bodies; demonstrate failures for occupied slots, prediction coverage, teleport angle and jump latch regressions on the old code.
- [ ] Adapt the three upstream fixes and current-SDK schema accessors. Run `python3 tests/movement_prediction_test.py`; expected all CKZ/KZT scenarios PASS.
- [ ] Compile/run every existing standalone host test with g++ C++17; expected all PASS. Commit code and evidence.

### Task 2: Review and native build

**Files:** review report in task outputs; fork branch; CI native artifact.
**Interfaces:** Reviewed immutable fork SHA consumed by build/release.
- [ ] Independent Sol/medium reviewer checks full diff, physics invariants and real SDK compatibility; address material findings with tests.
- [ ] Push reviewed branch and run cs2kz-build dry_build for immutable SHA; expected native CI SUCCESS and exact artifact ref.

### Task 3: Canary deployment and verification

**Files:** Cyber pin/lock/profile bump through release channel; task outputs release report.
**Interfaces:** Current orchestrator profile registration and rollout Start; no Promote.
- [ ] Verify current registry, image/config/pins, players and manual gate; capture all non-canary versions.
- [ ] Build/publish new immutable profile preserving payload, register and start KZ rollout only on canary.
- [ ] Check applied manifest, native CKZ/KZT load/cvars and safe available runtime movement checks; observe 30-minute health soak.
- [ ] Compare non-canary pins/applied unchanged; document precise results and remaining real-player verification limits.
