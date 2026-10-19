# 15 — Soak

Run the server after steps 01–14 are in the build. No source edit, no SQL, no e2e test. A data race is a crash: save the dump, both config values, the character count, the run name, and stop that run.

Pass criterion for the project: `wall` falls across three pool widths while `sum_task` stays flat, `seal_ser` stays flat as units grow, and `coord` stays under 10% of `MapUpdateInterval`. `task` (the divided number) is never the pass.

Do not set `PhaseThreads` to the core count. On a 14-core machine the top run is 13.

On a two-socket machine, also record which socket the world thread runs on and whether workers span both. Cross-socket steals and `VisibilityLink` records cost a remote cache line each; if `sum_task` at `cores − 1` is above `sum_task` at one socket's worth of cores, that is the NUMA ceiling, not a bug in the plan. Pin with `taskset`/`Start-Process -Affinity` to one socket for the comparison run.

## Records per run

`.server info` (mean, p95, p99, max), characters online, `MapUpdate.Threads`, `MapUpdate.PhaseThreads`, the `pool timing` lines (all fields), process memory at start / steady / after logout / at shutdown (step 16 uses these).

Config for every run: `ItemDelete.Method = 0`, `ItemDelete.Vendor = 0`, `PreloadAllNonInstancedMapGrids = 1` for the wide runs. Turn off any bot smart-scale so the population is the number you set.

## What a crash is

Process abort; a hang that does not shut down; a debug `ASSERT` (foreign access, sync query in a task, slot overflow); a result one thread cannot produce (a corpse rolled twice, a unit freed while another task is in its `Update`, a transport passenger left behind, a hit that never lands, a `VisibilityLink` without its viewer). A slower `.server info` is a number, not a crash.

## Runs, in order

One change per run. Clean shutdown between runs. 15 minutes minimum (one `PlayerSaveInterval`).

**A — many instances, key 1.** `MapUpdater` no longer runs maps after step 12, so this is the step 02 regression under the beat loop: two Deadmines instances, one zoning (grid loads at the seal against a mesh and tree the other instance is querying) while the other fights. A crash is step 02 or step 12.

**B — one spot, key 1.** Two characters. Melee lands next beat; a spell's feedback the beat after; a kill in a group reaches `EndRoll` once; `coord < 1 ms`. A failure stops the pass before C.

**C2 — one spot, key 2.** Three characters. All three see each other. Hit on target's packet after the next seal. `sum_task` within noise of B. Debug build for one of the 15-minute sessions: zero asserts.

During B and C2, `.debug procchain` (a debug counter in `ApplySpellLanded`/`ProcActorSide` that tags each record with the depth of the chain that produced it: a hit is depth 1, a proc-triggered cast landing is depth 2, its proc depth 3…) prints the histogram of chain depths and the longest chain seen. Depth ≥ 4 at the beat interval is 40 ms of player-visible delay; list the spells that reach it. If a common rotation does (e.g. a shadow priest's proc chain), that spell's trigger goes inline at land (same task, legal because the triggered cast's target is the proccing unit itself or the original target already on the task) rather than through a new record — the step 07 `FollowUpCast` list.

**Side cases at the key C2 allowed:**

- Party split across a city and a Deadmines instance for the whole run; one member rolls a corpse the other cannot see.
- Menethil → Theramore ship with a passenger aboard through the map change.
- A pet class with the pet fighting a mob the owner is not targeting.
- A vehicle quest.
- Mind control of a creature for 30 seconds, then release (task group forms and dissolves at the seal; `tasks` drops by one while charmed).
- A Wintergrasp siege engine with a driver and a passenger gunner crossing a cell boundary and killing a creature (vehicle task group of three; kill credit to both players).

**Cell — crowded.** Hundreds in one spot if the population exists, with a fight of about 80. Three runs: key 1, key 7 (half), key 13 (full). Same spot, same count, same duration. Record `wall`, `sum_task`, `idle`, `tail`, `coord` at each width.

**Pair — two continents.** Players on Eastern Kingdoms and Kalimdor. Key 1 then key 13. Both maps appear in `tasks` in the same beat.

**Outdoor world state.** During the cell run, Wintergrasp with a battle up or the Scourge Invasion event.

**Later machine.** Any box with more cores repeats Cell at its own `cores − 1`. `wall` falls again.

## Decision

| Result | Meaning | Where it goes |
| --- | --- | --- |
| A crashes | geometry or seal-time grid load | 02 or 12 |
| B: hit misses the next beat, `coord` over budget, hang | single-core beat loop wrong | 12 |
| C2 asserts | a foreign access two cores reach | 13 |
| C2 crashes without assert | record log, claim, or wake | 06 / 14 |
| C2 `sum_task` ≫ B | something serialises inside a task | 14; the largest split field names it |
| Cell: `wall` falls, `sum_task` flat, `coord` under budget | pass | record the three widths |
| Cell: `wall` flat, `sum_task` rises | shared line or lock inside a task | 14, by field |
| Cell: `wall` flat, `coord` grows, `seal_ser` grows | something per unit or per map leaked into S2 | 12: it belongs in S1b, S1a, or a unit task |
| Cell: `seal_map` ≈ `seal_tail` and growing with units | per-unit work leaked into S1b (object construction, a summon hook that scans, a module `OnMapUpdate`) | 12 residue table: route it to a grid item or a unit task; name the hook |
| Cell: `seal_tail` spikes on wipe-respawn or wall destruction | `ProcessRespawns` constructing, or an uncapped tree rebalance | 09 `Spawn` defer / 12 rebalance cap not in effect |
| Cell: `grid` ≈ `grid_tail` and large | one grid's item dominates | a grid item is list unlinks/links plus `Spawn` constructions; if a mass death shows here, a cleanup escaped `PrepareRemoval` (06); if a mass respawn shows here, that is the expected place and `grid` ≫ `grid_tail` should hold |
| Cell: `grid` ≫ `grid_tail`, or `seal_map` ≫ `seal_tail`, or `view` ≫ `view_tail` | a phase is not spreading | 14: item-to-core assignment or steal; check `idle` for that phase |
| Cell: `sum_task` rises with the key on a pathing-heavy scene | a shared line on the geometry path survived | 14: `perf c2c` names it; the guard macro should be empty in release |
| Cell: `tail` large, `.debug slowtasks` names a unit, `heavy` flag not set on it | heavy threshold wrong or the task was not heavy last beat (first-beat spike) | 12: threshold rule; a one-off spike is not a bug |
| Cell: `tail` large, `heavy` flag set | a task that is heavy even started first | split it: `path` split → budget (14); `update` split → that creature's AI; a boss with a 200-entry threat list → `ThreatMgr` update cost, consider updating threat order every other beat |
| Cell: `view_tail` large | one viewer's chunk is still too long | 14: lower `VIEW_CHUNK_TARGETS`; then 17 |
| Cell: `view` dominates `wall` | the per-viewer build itself is the cost, spread or not | 17 |
| Cell: `view` dominates `wall` after chunking | per-viewer build | 17 |
| Pair: one map has no tasks | not publishing | 12 |
| Pair: `wall` falls, both maps in `tasks` | pass | record |
| Side case fails | name it | the owning step; never raise the key to get past it |

## Checklist

- [ ] A, B, C2, side cases recorded with clean shutdowns.
- [ ] Debug-build C2 session with zero asserts.
- [ ] Cell at 1, half, full: `wall` fell, `sum_task` flat, `seal_ser` flat, `coord` under budget — or the named field was sent back.
- [ ] `.debug procchain` histogram recorded; spells at depth ≥ 4 listed.
- [ ] NUMA note if the machine has two sockets.
- [ ] Pair at 1 and full with both maps in `tasks`.
- [ ] Outdoor world state during the cell.
- [ ] Memory numbers handed to step 16.
- [ ] No source change was made during the soak.

## Status

**Not run.** This session had no build tree (`var/build` empty, no `worldserver` binary anywhere under the
repo, no MSVC on the path) and the soak needs a server with 2–300 characters online for 15-minute runs. Nothing
below the "Runs" heading was executed; the checklist is untouched on purpose.

**Prerequisites landed before the soak (source edits are forbidden once it starts):**

- `.debug procchain` did not exist; the B / C2 runs read it. `RecordHeader::pad` became `depth`: the log stamps
  every record with `RecordChain::applying + 1`, and both apply loops (`WorldObject::ApplyRecords`,
  `Unit::ApplyRecords`) set `applying` to the record's depth for the duration of its applier, so a record emitted
  while applying a depth-*d* record is depth *d*+1. A task's own action is depth 1. Counters are per core on their
  own line (`RecordChain::CoreStats`, relaxed, written by the owning core only): a 16-bucket histogram, the
  deepest chain, and a 16-entry (spell id → max depth) table for records at depth ≥ 4 (`SpellHit`,
  `DeferredSpellHit`, `SpellLanded`, `PeriodicLanded`, `ProcActor`). `.debug procchain` prints
  `depth:count` pairs, the deepest, the spells at ≥ 4 by name, and resets. Inline landings (self hit, reflected,
  the seal's deferred hits) do not deepen the chain: they add no beat.
- `Player::ViewCommit` under concurrent U2 (plan 14 exit note): pet removal deferred to the owner's next U1,
  target-side visibility writes under a one-byte spinlock.

**How to run it when a build exists.** Build `RelWithDebInfo` for every run and one `Debug` for the C2 assert
session. `MapUpdate.PhaseThreads` is the key; `MapUpdate.Threads` stays at its default (it only loads).
`Warden.Enabled = 0` if characters come from the AzerothGhost harness. Per run, record into the table below and
stop at the first crash as defined above.

| Run | Key | Chars | `.server info` mean / p95 / p99 / max | `pool timing` (wall, task, sum_task, tail, idle, view, view_tail, seal_map, seal_tail, seal_ser, coord, barrier, path, claim, wake_dup) | Mem start / steady / after logout / shutdown | Result |
| --- | --- | --- | --- | --- | --- | --- |
| A | 1 | 2 (two Deadmines) | | | | |
| B | 1 | 2 | | | | |
| C2 | 2 | 3 | | | | |
| C2 debug | 2 | 3 | | | | asserts: |
| Cell | 1 / 7 / 13 | n | | | | |
| Pair | 1 / 13 | | | | | |

`.debug procchain` after B and after C2: histogram line and the depth ≥ 4 spell list go here verbatim.

**What this session cannot decide and the first run will:** the plan 13 / 14 exit notes list the asserts a debug
boot is expected to hit first (foreign `CanSeeVendor` conditions, `CreatureGroup::LeaderStartedMoving`,
`OnPatchValuesUpdate`, the `StaticMapTree` `_writing` guard across sibling instances, `Unschedule` inside a
beat). Those are run-A / run-C2 material, not soak failures of the kind the Decision table routes.
