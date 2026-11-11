# KZ nominate server budget

Goal: keep map-grid writes out of unbounded command/SQL callbacks on the simulation thread.

Evidence: GG1 already sends at most 10 console commands per tick; each native batch applies all operations immediately. Fill Apply writes up to 400 PB/WR tiles in one callback. Each layout setter scans networked vectors. This is a verified unbounded path; its contribution to the reported live stall still requires runtime timing.

Design: a native round-robin queue shared by all external-menu slots, at most 128 work units and 1 ms per simulation frame. One unit is a bridge operation or one PB/WR tile (at most seven writes); the time limit is checked between units, so one unit may overrun. Keep engine/schema accesses on the game thread. Drain bridge writes before a slot's PB/WR. Closing/recycling/disconnecting clears pending writes. Preserve XML, text, commands, PB/WR calculations, SQL and input behavior.

- [ ] Add standalone scheduler regressions: operation and elapsed budgets, fairness, idle queues, FIFO and cancellation.
- [ ] Queue native bridge operations; hook a single global simulation-frame drain.
- [ ] Change PB/WR callbacks to schedule incremental tile writes; reset pending state on all existing reset paths.
- [ ] Run scheduler tests, existing applicable host tests and full native compilation in CI (no local SDK/toolchain install).
- [ ] Independent review before push; retain patch and limitations if live access is unavailable.

Review focus: close/reopen during pending batch; disconnect and slot reuse; mode changes/WR invalidation; entity recycling during apply; multiple simultaneous menus and total server budget. Live acceptance: measure frame time while one player runs and another opens full/filtered nominate, including warm cache and repeat opens. Canary only; no early/fleet promotion.

## Indexed external writer (2026-11-11)

Add a cache owned by each ExtState, never by an SDK overlay: string → intern index and packed pair → state entry index. Rebuild a lookup on first access or collection count mismatch, retain first duplicate, update expected counts on successful append. Both bridge operations and PB/WR tiles use the same actual writer. DestroyExt resets the cache on close/open/recycle/disconnect; identity checks also reset after a changed layout/state address. No persistent element pointers survive AddToTail. Preserve owner full notifications on string append, state full on entry append, indexed element notifications on update, explicit false classes, empty values and unset behavior.

Host regression starts RED without the writer header. ASan/UBSan writer and queue, fill-step and quickmenu-position pass. Writer first+repeat 400-tile scenario visits 800 existing elements (200 tiles: 400), versus approximately 642k visits for the prior linear scans in the same scenario (excluding append internals). Fake collections force vector reallocations. Full SDK compilation and real gameplay remain CI/runtime acceptance work, not host-test claims.

Raise the shared operation cap 32 → 128 with the same 1000µs deadline and round-robin fairness. The faster writer should spend fewer frames resending growing state vectors; one unit can still cross the deadline. Owner requested no SQL, UI copy, layout or gameplay-rule changes. Independent review is required before push/release; canary only.
