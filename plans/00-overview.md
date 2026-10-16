# Finalized plan: one process-wide pool, one task per due unit

This directory is the implementation order. Each file is one step. Steps are built in numeric order, each boots and plays before the next starts, and none of them keeps a compatibility path for the old behaviour. When the last step is in, the pieces fit together without an adapter: there is no flag that restores inline cross-unit writes, no second timing model, and no `Map::Update`.

Line numbers in these files are from the 2026-10-16 tree. Nothing described here is in `src/` yet.

## The end state

- `MapMgr::Update` runs one **beat** every `MapUpdateInterval` (default 10 ms). A beat is a fixed set of **tasks**, one per due unit on every map, pulled from per-core work-stealing deques by `cores − 1` workers. The world thread is one of those workers and is also the **coordinator**.
- A task owns exactly one **task group** for its duration: normally a single `WorldObject`; for a vehicle, the vehicle and its seated passengers; for a charm, the charmer and the charmed unit. It writes those objects' live fields and nothing else's. Every other object is read through its **slot**: a copy of its values block, position, movement flags, and aura list, published by that object's own task at the end of its last beat.
- A write to another unit is a **record** appended to the writer's per-core log and applied by the target at the start of its next task, in source-guid order. The main kinds are `SpellHit` (launch on the caster, land on the target) and `SpellLanded` (the target's feedback to the caster).
- A beat is **five phases** on the same pool, each a publish → work → completion wait: **U1** unit tasks (update, records, defers, slot publish); **U2** view items (each player's visibility scan and update packet, built from *this* beat's slots, chunked across cores for a crowded viewer); **S1a** grid items (relinks, inserts, removes of grid cell lists, two rounds: unlink then link — grids are disjoint); **S1b** map items (map-wide containers, transports, respawns, scripts, epilogue — maps are disjoint); **S2** on the coordinator alone (cross-map defers, buffer resets, newcomers, frees). S2 is the only serial part of a beat and does no per-unit, per-grid, or per-map work.
- A write to a map container is a **defer**, routed at append time to the grid (S1a), the map (S1b), or the coordinator (S2) that applies it. A unit that removes itself emits the foreign half of its cleanup as records on its own task, so its seal-time removal is grid-local.
- Within each phase, items are scheduled **heavy first**: an item whose last run was long starts before the short ones, so the phase's tail is hidden behind the bulk (longest-processing-time rule, ≤ 4/3 of optimal).
- Objects removed in beat *k* are freed in S2 of seal *k*. Nothing holds a pointer across a seal, so there is no version chain and no two-seal delay.
- Geometry (vmap tree, navmesh tiles, dynamic tree) changes only in S1a/S1b, when no unit task runs. The query path takes **no lock and performs no shared atomic RMW** — step 02's locks are scaffolding and step 14 deletes them. Each worker has its own `dtNavMeshQuery` per mesh, and a path search spends at most a fixed node budget per beat (Detour sliced API; far paths are walked in legs).
- Global singletons are read-only in a task, or guarded by a rarely-taken mutex, or mutated through an S2 defer (step 03 table). Nothing a task does per beat touches a shared cache line except the target's `_writtenBy` word and the claim byte.
- A task never calls `Query` or `DirectExecute`.

The pass criterion for the whole project is the `pool timing` line: `wall` falls as the key grows while `sum_task` and `seal_ser` stay flat and `coord` stays under 10% of `MapUpdateInterval`. A number divided by the core count is not a pass.

Why this scales: the serial fraction of a beat is S2 plus five barrier waits, all independent of unit, grid, and map count (together well under 1% of a 10 ms interval). Everything that grows with the world — unit updates, hits, visibility, packets, pathing, relinks, spawns, deaths, respawns, scripts — runs in a phase that spreads across the pool. The remaining floor of a beat is the longest *indivisible* item in each phase: one unit's `Update` without its view work and with a bounded path step (~50–300 µs), one viewer's chunk (~100 µs), one grid's list unlinks and links (~100 ns each: a hundred deaths in one grid is ~10 µs because the dying units cleaned themselves up on their own tasks), one map's residue (store edits, transports, popped respawns, scripts, a capped tree rebalance — none of it constructs objects or scales with moving units). Each is a constant of the content, not of the population, each is named in the clock (`tail`, `view_tail`, `grid_tail`, `seal_tail`), and each starts first in its phase.

## Order

| # | File | Step | Thread model after this step |
| --- | --- | --- | --- |
| 01 | `01-phase-clock.md` | Ten-phase clock inside today's `Map::Update`, baseline only | unchanged |
| 02 | `02-shared-geometry.md` | `StaticMapTree` and navmesh safe for `MapUpdate.Threads > 1` behind one guard macro (scaffolding, deleted in 14); `QueryLocation` copy-out; grid slot publication | unchanged |
| 03 | `03-write-classification.md` | Reference list of every write in a tick: Own / Record / Defer; global singleton table | no code |
| 04 | `04-config-and-atomic-guids.md` | `MapUpdate.PhaseThreads` key, `BeatContext` (owner, root, sealMap), atomic item and map GUIDs | unchanged |
| 05 | `05-object-slot.md` | Slot pair on every `WorldObject`, published after `Update` | single thread |
| 06 | `06-record-log.md` | Per-core record log, head chains, `writtenBy` mask, inbox apply, the non-spell kinds, prepared removal | single thread, hits land next tick |
| 07 | `07-spell-hit-split.md` | `SpellHitRecord` / `SpellLandedRecord`, `UnitView`, launch and land split, `Deferred` hits | single thread |
| 08 | `08-melee-periodic-procs.md` | `MeleeHitRecord`, periodic ticks, proc halves on records | single thread |
| 09 | `09-defers.md` | `MapDefer` buffer; per-map and global kinds; relink, insert, remove, scripts, cell marks, `EndRoll`, `WorldState`, task-group join/leave, pool/instance/game-event at tick end | single thread |
| 10 | `10-visibility-and-packets.md` | Visibility scan and update packets per viewer from slots as `ViewScan`/`ViewBuild`/`ViewCommit`; `VisibilityLink` record | single thread |
| 11 | `11-sync-sql.md` | `recoveryItem` enqueues; preload creates the map off the worker | single thread |
| 12 | `12-beat-loop-single-core.md` | `BeatPool` on `MapMgr`; `Map::Update` retired; task groups; five phases U1 / U2 / S1a / S1b / S2; heavy-first; `pool timing`; key = 1 | one core, beat loop |
| 13 | `13-foreign-access-audit.md` | Debug `ASSERT` on foreign live reads and writes and on singleton mutators; audit at key 1 | one core |
| 14 | `14-workers.md` | Workers, two-level steal, claim, completion counters, all phases on the pool, view chunks, path budget, per-worker nav queries, geometry locks deleted, first-touch; key ≥ 2 | pool |
| 15 | `15-soak.md` | Runs and gates | — |
| 16 | `16-memory-owners.md` | Owner table for every allocation in 01–14 | — |
| 17 | `17-crowd-buffer-optional.md` | Per-cell packet buffer, only if `view` dominates after chunking | pool |

Steps 05–11 change timing (a hit lands on the target's next tick) but not threading. They are rehearsed on today's `Map::Update`. Step 12 replaces `Map::Update` with the beat loop on one core. Step 14 adds cores. Step 13 sits between them because the audit is cheaper at key 1 with the `ASSERT` on than at key 2 with a crash dump.

## Glossary

| Term | Meaning |
| --- | --- |
| Beat | One `MapMgr::Update` pass that runs every due unit once. Period `MapUpdateInterval`. |
| Task | One claimed task group in U1: apply inbox, update, append records, publish slot, queue view items, republish. |
| View item | One player (or one chunk of a crowded player's visible set) in U2: scan and build the update packet from this beat's slots; the last chunk commits and sends. |
| Grid item | One `(map, grid)` in S1a with pending grid-local defers; two rounds, unlink then link. |
| Heavy ring | Per-core ring of items whose last run exceeded 8× the median; drained by every core before anything else in the phase. |
| Task group | The set of objects one task owns. Normally one object. A vehicle base with its seated passengers, or a charmer with its charmed unit, form a group whose **root** is the vehicle or charmer; members are not due on their own. Membership changes at the seal (`TaskGroupJoin`/`Leave` defers). |
| Due | A root whose group contains a player, or a unit with `IsUpdateNeeded()`, or a unit with a non-empty inbox (wake). |
| Slot | `ObjectSlot`: values block, changes mask, position, movement flags, aura list. Two per object plus a `publishedBeat` stamp. |
| Record | Plain-value message to one target guid, in a per-core append-only log. Applied at the start of the target's next task. |
| Defer | Plain-value message applied at the seal, routed at append: grid-local kinds to a grid item (S1a), map-wide kinds to the map item (S1b), cross-map kinds to the coordinator (S2). |
| Seal | The three phases after U2. **S1a**: grid items on the pool, unlink round then link round. **S1b**: one item per map on the pool (collected map-side work, map-wide defers, `DelayedUpdate`, epilogue). **S2**: coordinator alone (global defers, buffer resets, newcomers, frees, slot indices, heavy threshold, clock). Budget for all three plus barriers: 10% of the interval; S2 must not grow with units, grids, or maps. |
| Prepared removal | A unit removing itself emits `ThreatRemove`, `CombatEnd`, `MinionUnlink`, `GameObjectUnlink`, `VisibilityUnlink` on its own task, so its seal-time cleanup writes nothing but itself and its grid list. |
| Owner | The `WorldObject` on the stack of the current task (`BeatContext::owner`); `BeatContext::root` is its group root; `BeatContext::sealMap` (and `sealGrid` in S1a) is set instead in a seal item. |
| Foreign access | Reading or writing a live field of an object that is not the owner, during a beat. Debug `ASSERT`. |
| `UnitView` | Read-only view over a unit's slot. `CasterView` is the `UnitView` of the caster inside land-side code. |
| Key | `MapUpdate.PhaseThreads`. 1 runs the beat loop on the world thread alone. `cores − 1` is the target. |

## Invariants every step must keep

1. A unit's live fields have one writer per beat: the task of its group. Enforced in debug by `BeatContext::MayAccess`.
2. A record or defer holds guids, ids, and plain values. Never `Unit*`, `Spell*`, `Aura*`, `Item*` (sole exception: `AddToMap::obj`, unpublished, owned by the defer).
3. Records apply at the start of the target's next task, sorted by `(source guid, sequence)`. Same at key 1 and key 13.
4. Containers on `Map`, `MapMgr`, `ObjectAccessor`, and `Group` change only at the seal or between beats on the world thread. During U1 and U2 they are read-only, so live pointers are valid handles. A grid's cell lists change only in that grid's S1a item; a map's other containers only in that map's S1b item or in S2.
5. Objects removed in beat *k* are freed in S2 of seal *k*.
6. No per-beat allocation of a slot, a log, a deque, or a nav query. Buffers stay at high-water size.
7. No `std::thread::join` inside a beat. No broadcast wake on a push path. No global in-flight atomic.
8. A task does not call `Query`, `DirectExecute`, `LoadGrid`, `LoadAllCellsInGrid`. The geometry and grid query paths contain no lock and no shared atomic RMW in release builds.
9. Position fields (`Position::m_positionX/Y/Z/O`) are the one tolerated foreign read without a slot: the owner is the only writer, the storage is never freed during a beat, and a torn read is bounded by one beat of movement. Everything else goes through the slot. Divisions by a distance derived from a foreign position are clamped.
10. Nothing in S2 is per unit, per grid, or per map. Anything that scales with the world runs in U1, U2, S1a, or S1b.
11. A global singleton is mutated from a task only through its own rarely-taken mutex (step 03 *mutex* rows) or an S2 defer. Everything else is `ASSERT_NOT_IN_TASK`.
12. A removal requested on a unit's own task is prepared there — foreign half as records, own half inline — and the seal does only the grid unlink and the store erase. Only S1b may run a two-sided cleanup. S1b constructs no objects: respawns and summons are created in grid items.
13. A U2 item writes nothing but its own `ViewWork` until the last chunk commits. A path search spends at most `CONFIG_PATH_ITER_BUDGET` node expansions per beat.
14. Every phase drains the heavy rings first. The threshold is 8× the previous beat's median item time.

## Behaviour changes accepted by this plan

These are not bugs. They are written down so a tester does not file them.

- A spell or melee hit lands on the target's next beat (≤ 10 ms later), and its caster-side effects (procs, rage, combat state, kill reward) the beat after that.
- Map-side session packets (`PROCESS_THREADSAFE`: movement, cast, swing, selection) are handled once per beat instead of once per world tick. Latency rises from ~1 ms to ≤ `MapUpdateInterval`.
- DoT and HoT amounts, periodic amplitude, and the caster-side half of aura duration are fixed at application (launch), not recomputed per tick from the live caster.
- Combo points are taken at launch from the primary target's rolled miss condition; a land-side immune or evade no longer refunds them unless the feedback says so (step 07 makes it say so).
- `MarkNearbyCellsOf` and `IsUpdateNeeded` see cell marks one beat late.
- `mapUpdateStep` class rotation is gone. Continents, battlegrounds, and dungeons all publish into the same beat.
- Boarding a vehicle, leaving it, charming a unit, and releasing it take effect at the next seal (≤ 1 beat later than today). Once formed, a vehicle or charm group has no server-side lag between its members.
- Pool respawns, instance binds, and game-event starts triggered from a task apply in S2 of the same beat, so a pooled respawn appears one beat later than today.
- Every timed respawn appears one beat (≤ 10 ms) after its timer fires: `ProcessRespawns` schedules, the next beat's grid items create.
- A dying unit removes its own auras, motion, and events in the beat it dies rather than at the end of the tick; nothing observable changes except that its final slot (dead, no auras) is visible to viewers in that same beat.
- The dynamic collision tree rebalances at most once per 16 changed models or 32 beats; line of sight through a just-destroyed wall is correct immediately, the query is merely slightly slower until the next rebalance.
- Visibility and update packets are built from the **same** beat's slots (phase U2), so a viewer sees a change in the beat it happened — fresher than today's end-of-tick `SendObjectUpdates` for objects updated earlier in the tick, and never staler.
- A dying unit's attackers lose it from their threat and combat lists on their next task (≤ 1 beat), not at the moment of death. Their AI's evade/target-switch therefore happens one beat later than today.
- A creature's path search is capped per beat; a path longer than the budget is walked in two or three legs with a re-plan at each, which can be a slightly less direct route across a zone.

## Pool clock

Emitted from the coordinator, same rate rule as the step 01 clock (skip a beat under 20 ms, at most one line per 5 s):

```
pool timing: cores={} tasks={} wall={} sum_task={} task={} idle={} tail={} view={} view_tail={} grid={} grid_tail={} seal_map={} seal_tail={} seal_ser={} coord={} claim={} inbox={} update={} publish={}
```

| Field | Definition |
| --- | --- |
| `cores` | Pool size including the coordinator |
| `tasks` | Unit tasks published this beat (U1) |
| `wall` | Wake to wake |
| `sum_task` | Sum of U1 task durations across all cores |
| `task` | `sum_task / cores` (informational only) |
| `idle` | Time a core spent with nothing to pop or steal while another core still ran an item, summed then divided by cores |
| `tail` | Longest single U1 task |
| `view`, `view_tail` | Wall of U2; longest single view item (one chunk of the most crowded viewer) |
| `grid`, `grid_tail` | Wall of both S1a rounds; longest single grid item |
| `seal_map`, `seal_tail` | Wall of S1b; longest single map item |
| `seal_ser` | Wall of S2 — the only serial part of the beat |
| `coord` | `grid + seal_map + seal_ser` + the five barrier waits |
| `claim`, `inbox`, `update`, `publish` | Split of `sum_task`; `.debug slowtasks` adds `path` per unit |

| Reading | Meaning |
| --- | --- |
| `wall` falls, `sum_task` flat, `seal_ser` flat, `coord` under budget | Linear stretch |
| `wall` flat, `sum_task` rises | A shared line or lock inside a task. Find it in the split; on a pathing scene, the geometry path |
| `wall` flat, `seal_ser` grows with units | Per-unit, per-grid, or per-map work leaked into S2. Move it to S1a/S1b or a unit task |
| `seal_map` ≈ `seal_tail` and growing with units | Per-unit work leaked into S1b. Route it to a grid item or prepare it on the unit |
| A phase wall ≫ its tail | That phase is not spreading. Item-to-core assignment or steal |
| A phase wall ≈ its tail | One item dominates that phase. `.debug slowtasks` names it; split it (path budget, view chunk size, AI cost) |
| `tasks` below the core count | Not enough due units, or a map not publishing |
| `view` dominates `wall` after chunking | Per-viewer packet build itself dominates. Step 17 |

## Master checklist

- [ ] 01 phase clock
- [ ] 02 shared geometry
- [x] 03 write classification (reference, no code)
- [x] 04 config key, `BeatContext`, atomic GUIDs
- [x] 05 object slot
- [ ] 06 record log
- [ ] 07 spell hit split
- [ ] 08 melee, periodic, procs
- [ ] 09 defers
- [ ] 10 visibility and packets from slots
- [ ] 11 sync SQL
- [ ] 12 beat loop at key 1, task groups, S1/S2 seal
- [ ] 13 foreign-access and singleton audit at key 1
- [ ] 14 workers at key ≥ 2, S1 on the pool, geometry locks deleted
- [ ] 15 soak: `wall` falls across three widths, `seal_ser` flat
- [ ] 16 memory owner table complete
- [ ] 17 crowd buffer (only if `view` dominates after chunking)
