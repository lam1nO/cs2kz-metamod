# KZ nominate server budget

Goal: keep map-grid writes out of unbounded command/SQL callbacks on the simulation thread.

Evidence: GG1 already sends at most 10 console commands per tick; each native batch applies all operations immediately. Fill Apply writes up to 400 PB/WR tiles in one callback. Each layout setter scans networked vectors. This is a verified unbounded path; its contribution to the reported live stall still requires runtime timing.

Design: a native round-robin queue shared by all external-menu slots, at most 32 work units and 1 ms per simulation frame. One unit is a bridge operation or one PB/WR tile (at most seven writes); the time limit is checked between units, so one unit may overrun. Keep engine/schema accesses on the game thread. Drain bridge writes before a slot's PB/WR. Closing/recycling/disconnecting clears pending writes. Preserve XML, text, commands, PB/WR calculations, SQL and input behavior.

- [ ] Add standalone scheduler regressions: operation and elapsed budgets, fairness, idle queues, FIFO and cancellation.
- [ ] Queue native bridge operations; hook a single global simulation-frame drain.
- [ ] Change PB/WR callbacks to schedule incremental tile writes; reset pending state on all existing reset paths.
- [ ] Run scheduler tests, existing applicable host tests and full native compilation in CI (no local SDK/toolchain install).
- [ ] Independent review before push; retain patch and limitations if live access is unavailable.

Review focus: close/reopen during pending batch; disconnect and slot reuse; mode changes/WR invalidation; entity recycling during apply; multiple simultaneous menus and total server budget. Live acceptance: measure frame time while one player runs and another opens full/filtered nominate, including warm cache and repeat opens. Canary only; no early/fleet promotion.
