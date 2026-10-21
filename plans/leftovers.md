# Still open

`worldserver` builds. These are the tasks that are not in the tree yet, or that need a logged-in client.

## 1. Spell-hit caster audit

`SpellScript::GetCaster()` still returns `Unit*`. Land-side hit hooks can still write the live caster, which asserts once the caster is foreign.

Change the types, then fix the compiler errors. Do not edit handler bodies before the type change.

- `GetCaster()` returns `UnitView` in every hook.
- `GetCasterUnit()` returns `Unit*` and asserts it is not a hit hook (`BeforeHit`, `OnHit`, `AfterHit`, `OnEffectHit`, `OnEffectHitTarget`). Launch hooks (`OnCheckCast`, `OnCast`, `BeforeCast`, `AfterCast`, target selects) call `GetCasterUnit()`.
- `CreatureAI::SpellHit` / `SpellHitTarget`: the other unit is a `UnitView`.
- `AuraEffect::CalculateAmount` / `RecalculateAmount`: the target argument is `UnitView const&`.
- `AuraScript::GetCaster()` on proc and periodic hooks becomes a view. Launch and apply hooks that run on the caster's task keep a unit accessor with the same assert.

`UnitView` has no `CastSpell`, `AddAura`, `SetHealth`, `GetMotionMaster`, or `ToPlayer`. Each error is one of:

- a read the view already has → call it on the view;
- a missing slot field → add it and fill it from the owner;
- a caster write from a hit hook → `Feedback().AddFollowUp`;
- both units must be live (teleport, summon player, duel, taxi, bind) → `SpellMgr::IsDeferredHit`, with the spell id and the reason recorded here.

Land `Spell` stores the caster as a handle that may be null. `CasterView()` on a null handle is an empty view. Do not keep a `WorldObject* m_caster` that hit handlers can dereference.

Separate builds, so each error list stays readable: `GetCaster()` first, then the AI hooks, then `CalculateAmount`, then `AuraScript::GetCaster()`.

Launch-side target reads are still live and assert when the target is foreign: `SpellHitResult`, `SpellTakenCritChance`, `CalculateAOEDamageReduction`, `IsImmunedTo*`. Give them `UnitView` overloads only for fields the slot already has after the audit. Immunity is already on the slot.

Leave until a measurement asks for it: `ProcEventInfo::GetActor()` stays `Unit*`; the land `Spell` stays a stack object; health stays the batch in `ApplyRecords`.

Already handled, do not redo: Raise Dead (46584) is deferred, Val'kyr guardian smite uses `directSelfHeal`, Lay on Hands uses `scriptValue` plus a follow-up.

## 2. Grid tasks (S1a)

Workers, S1b, and S2 already run. `grid` and `grid_tail` are 0. Respawn construction is a `Spawn` defer applied by the next map seal, not by a grid item. `RemoveListedObject` still destroys creatures inside the seal.

### Route at append

Grid-local kinds: `Relink`, `AddToMap`, `RemoveFromMap`, `GridLoad`, `CellMark`, `DynTreeInsert`, `DynTreeRemove`, `Spawn`.

At `AppendDefer`, compute the grid from the payload (relink: old and new cell; spawn or add: the object or `CreatureData` position; grid load: the cell). Append into `Map::GridPending(grid)`, one vector per core. Set a bit in a per-map 64×64 bitset (`fetch_or` on the word) the first time a grid is touched that beat.

`PublishGridItems` walks the bitset. One work item per touched grid per round. Spread with `id % cores`, heavy grids first by `_lastGridNs`.

### Two rounds

A relink removes from grid A and inserts into grid B. One round would have both tasks writing the object's `GridReference`.

- Unlink round: each grid removes what leaves it.
- Link round: each grid inserts what enters it, runs `GridLoad`, and constructs `Spawn`s.

Same-grid relinks do both in the link round. `GridBody` may touch its cell lists, the objects on its pending lists, and a `mapSide` vector of store and accessor edits. It may not touch `_objectsStore`, `HashMapHolder`, or another grid's objects. `MayAccess` in a grid task is true only for objects on that pending list.

`grid` is the sum of both rounds. `grid_tail` is the longest grid item, stored as `_lastGridNs`.

### Removals

`PrepareRemoval` on the owner's task already splits own and foreign halves. The grid item for a prepared removal does `RemoveFromGrid` only and asserts `_removing`. An unprepared removal stays two-sided in S1b.

`Map::DeleteFromWorld` already queues the `delete`. After this step the seal only drains `mapSide` (store, accessor, `AddToWorld`, summon hooks). The free stays in `SealSerial`.

`LoadGridsInRange` for a scanning viewer stays in `BeatPool::PreloadViewGrids`, on the coordinator between U1 and U2. Move it only if `grid_tail` says grid loads dominate. It must not move onto a unit task.

Done when `pool timing` shows non-zero `grid` / `grid_tail` on a continent with movers, fifty deaths in one beat show up as grid items without moving `seal_tail`, and a wipe respawn constructs on a grid item the next beat.

## 3. Needs a logged-in client

No source edits during a soak run except a crash fix, and that fix ends the run. `MapUpdate.PhaseThreads` is the key. Do not set it to the core count.

### Debug boot

Debug build, `PhaseThreads = 1`. Idle continent, then one character (talk, quest, loot, vendor, mount, taxi, bank, mail), then two characters (duel, party, trade, inspect, follow across a cell, death and resurrect, ship), then a pack with a pet, a patrol, an escort. Mail and trade stay between beats.

Each new assert is a slot read, a record, a defer, or a deferred spell. A sync query from a session packet inside the player's task is the intended assert: switch that call to `AsyncQuery` / `Execute`, resolve the player by guid in the callback, and record the opcode here.

### Worker checks

| Check | Key | Pass |
| --- | --- | --- |
| 1 | 2 | Three characters see each other. A hit lands on the target's next beat. `sum_task` within noise of key 1. Clean shutdown. |
| 2 | 2 | The debug-boot procedure again. Zero asserts. |
| 3 | 4 | City plus Deadmines. Both maps in `tasks`. A party across them works. |
| 4 | empty, high key | `idle` ≈ `wall`. `coord` under budget. No spin. |
| 5 | any | `wake_dup` and lost claims stay small and flat. |
| 6 | 2 then 4 | Twenty creatures pathing through a door. `sum_task` flat. |
| 7 | 4 | `seal_map` ≈ `seal_tail`, `grid` ≈ `grid_tail`. `seal_ser` under 100 µs. Needs section 2. |
| 8 | 2 | Vehicle across a cell; mind control. One task group, zero `MayAccess` asserts. |
| 9 | 4 | One character, ~300 visible objects. At least 4 view chunks. `view_tail` ≈ `view / 4`. One `SMSG_UPDATE_OBJECT` per beat. |
| 10 | 4 | Long path. `path` stays inside `MapUpdate.PathIterBudget`. At most three incomplete legs. |
| 11 | high | Fifty deaths. Grid items, `grid_tail` is one grid, `seal_tail` still. Needs section 2. |
| 12 | two sockets only | `stealOrder` is local-node first. Record `sum_task` at `cores − 1` and pinned to one socket. |

### Soak

Config: `ItemDelete.Method = 0`, `ItemDelete.Vendor = 0`. Wide runs also `PreloadAllNonInstancedMapGrids = 1`. One change per run. 15 minutes. Clean shutdown.

Record `.server info` mean / p95 / p99 / max, characters online, both thread keys, every `pool timing` field, and process memory at start, steady, after logout, and shutdown.

| Run | Setup |
| --- | --- |
| A | Key 1, two Deadmines, one zoning while the other fights |
| B | Key 1, two characters, one spot. Melee next beat, spell feedback the beat after, one `EndRoll`, `coord < 1 ms` |
| C2 | Key 2, three characters. `sum_task` within noise of B. One debug session with zero asserts |
| Side cases | At the C2 key: party split city / Deadmines; Menethil–Theramore ship; pet on a mob the owner is not targeting; a vehicle quest; mind control 30 s then release; Wintergrasp siege engine with driver and gunner |
| Cell | Same crowd, keys 1, half, `cores − 1`. `wall` falls, `sum_task` flat, `seal_ser` flat, `coord` under 10% of `MapUpdateInterval` |
| Pair | Eastern Kingdoms and Kalimdor, key 1 then full. Both maps in `tasks` the same beat |
| Outdoor | Wintergrasp or Scourge Invasion during Cell |

`.debug procchain` during B and C2: the histogram and every spell at depth ≥ 4. A common rotation at that depth goes inline at land instead of another record.

Also write `.server info` for an empty server and for one character in a city, `PhaseThreads` 1.

### Memory

From Cell and C2: steady − start is accounted for by `.debug memory`; after logout the process does not fall below steady minus the players' own view buffers and slot pairs; three steady 15-minute windows differ by less than 1%. Windows: C2 under Application Verifier for 15 minutes, zero leaks at shutdown from the step 04–14 files.

### Crowd buffer

Only if Cell shows `view` dominating `wall` with chunking on. Same Cell, `MapUpdate.CrowdBuffer` off then on. Record `view`, `view_tail`, `wall`, and `crowd_copied` / `crowd_built`.

Keep it only if `view` and `wall` fall and `crowd_copied` is most of the public player blocks in cells of ≥ 32. A small `crowd_copied` (one raid in the cell) drops the step; leave the config default off.

Visibility check with the buffer on: one character stealthed, one in another phase, one who taps a mob. All three must match the buffer-off client.
