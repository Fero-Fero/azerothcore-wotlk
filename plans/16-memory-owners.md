# 16 — Memory owners

Every allocation introduced in steps 01–14 has exactly one owner and one free site. This step is the table, the three fixes the table forces, and the measurement that proves resident memory is flat across the soak.

## Owner table

| Allocation | Owner | Allocated | Freed | Grows? |
| --- | --- | --- | --- | --- |
| `ObjectSlotPair` (step 05) | the `WorldObject`, by value; one heap block for both slots (`Init`) | `AddToWorld` | with the object | the aura array has a capacity set in `Init`; when `FillSlot` needs more it marks the pair for regrowth and the **seal** reallocates the block (never mid-beat); `.debug slot` counts regrowths |
| `UnitRecordLog` byte buffer (step 06) | `CoreState`, one per core | `Start` | `Stop` | capacity = high-water × 1.5, resized at the seal only; reset (not freed) each beat |
| `TargetHead` array (step 06) | `CoreState` | `Start`; indexed by slot index | `Stop` | sized to the slot-index allocator's high-water, grown at the seal |
| `DeferBuffer` (step 09) | `CoreState` | `Start` | `Stop` | same policy as the record log |
| `WorkDeque` ring (step 14) | `CoreState` | `Start` | `Stop` | doubled at the seal when > 75% |
| `_wakeOverflow` vector (step 14) | `CoreState` | lazily | `Stop` | `clear()` keeps capacity |
| `dtNavMeshQuery` per (core, mesh) (step 14) | `CoreState::navQueries` | first path on that mesh from that core | map unload at the seal; `Stop` | one per mesh per core, bounded by maps × cores |
| Land-side `Spell` (step 07) | per-core `SpellPool` free list | first use | `Stop` | pool high-water; `Spell` is ~1 KB, hundreds per core is fine |
| `SpellHitRecord` / `SpellLandedRecord` bodies | inside the record log bytes | — | — | — |
| `AddToMap` defer carrying a `WorldObject*` (step 09) | the defer is the sole owner until `ApplyDefers` hands it to the map | task | seal; if the map is gone (`IsUnloadPending`), the seal deletes the object | no |
| `RemoveFromMap` free (step 12) | `Map::_freeAtSeal` | S1 of seal k (the remove applied) | S2 of seal k (`FreeRemoved`, `delete`) | bounded by removes per beat |
| `Map::_newcomers` (step 12) | `Map` | S1 (objects created by `ProcessRespawns`, `AddToMap` defers) | drained in S2 (`PublishNewcomers`); vector keeps capacity | no |
| `_taskMembers` vector per root (step 12) | the root `WorldObject` | first `TaskGroupJoin` | with the object; `shrink_to_fit` not called (seats ≤ 8) | no |
| `heavy` ring (step 12) | `CoreState` | `Start` (first touch by the worker) | `Stop` | tiny; doubled at the seal like the others |
| `viewItems` vector (step 12) | `CoreState` | lazily | `Stop` | `clear()` keeps capacity |
| `ViewWork` per player (step 10): `chunkData[]`, `delta` | `Player` | with the player; `chunkData` resized to `chunkCount` (≤ cores) | with the player | each `UpdateData` capped at 64 KB with `shrink_to_fit` above that in S2 |
| `GridPending` per (map, grid): per-core vectors + `mapSide` collections (step 12) | `Map::_gridPending[grid]` | first defer routed to that grid | map unload | `clear()` keeps capacity; touched-grid bitset is 64 words inline |
| Task-time histogram (64 buckets) for the heavy threshold (step 12) | `CoreState` | `Start` | `Stop` | fixed |
| `CrowdBuffer` per crowded cell (step 17) | `Map` | S2, first threshold crossing | map unload | high-water |
| `UpdateData` per player (step 10) | `Player::_updateData` | with the player | with the player | buffers `clear()`, capacity retained; capped at 64 KB with `shrink_to_fit` above that at the seal |
| `ObjectSlot::movementBlock` bytes (step 10) | inside the pair's block, capacity set in `Init` (`MovementInfo` max encoded size + transport + spline header, ~256 B) | with the pair | with the pair | no; spline point data is not copied (viewers read it from the live `movespline`, which only the owner writes mid-beat — a tolerated read, same class as position) |
| Slot index (step 05) | `SlotIndexAllocator` free list | `AddToMap` | S2 of the seal that freed the object (`ReturnSlotIndices` after `FreeRemoved`) | high-water; never shrinks |
| `dtNavMeshQuery` per (core, mesh) — see row above; `MMapData::_navMeshQuery` is deleted in step 14 | — | — | — | — |
| `MapPhaseClock` (step 01) | deleted in step 12 | — | — | — |
| `CoreState` itself | `BeatPool::cores` | `Start` | `Stop` | fixed by key |

## Fixes the table forces

1. **`MMapMgr::LoadTile` failed `fread`.** In `MMapMgr.cpp` the `fread(data, fileHeader.size, 1, file) != 1` branch frees the `FILE*` but today also calls `dtFree(data)` — verify; if it does not, add it. The step 02 `SharedNavMesh::Write` path must not leak the tile buffer when `addTile` fails either: `dtFree(data)` on `dtStatusFailed(status)`, which the existing code does.
2. **Slot index of a dead target.** A record appended at beat k to a target removed at seal k is in a per-core log that S2 of seal k resets before `ReturnSlotIndices` runs; `ApplyInbox` is never called for the removed object. The order inside S2 is therefore fixed: log reset → `FreeRemoved` → `ReturnSlotIndices`. The allocator asserts in debug that an index returned in beat k is not handed out before beat k+1 starts (`_freedBeat[idx] < beat`).
3. **Land-side `Spell` returned to the pool on every exit path.** `Land()` is wrapped in a scope guard that calls `SpellPool::Release(this)`; `Spell::~Spell` is not run for pooled spells, so `InitFromHit` must reset every field `Spell` reads during land (`m_UniqueTargetInfo`, `m_spellAura`, `m_hitTriggerSpells`, `m_appliedMods`, `m_damage`, `m_healing`, script lists via `LoadScripts` → `delete` old scripts first).

## Measurement

From the soak numbers (step 15): resident memory at start, steady, after all characters log out, and at shutdown, for runs B, C2, and Cell at full key.

Pass: steady − start is explained by per-core buffers at high-water (sum of the "Grows?" column reported by `.debug memory`, a new command that prints each `CoreState`'s capacities and the `SpellPool` size); after logout the number does not fall below steady minus the players' own `UpdateData` and slot pairs — that is, the per-core buffers are retained, not leaked; three consecutive 15-minute windows at steady population differ by less than 1%.

In a debug build run C2 under ASan (Linux) or Application Verifier (Windows) for 15 minutes: zero leaks at shutdown attributable to the files in steps 04–14.

## Checklist

- [x] Every row in the table has a free site in code and a comment naming the owner at the allocation. (rows that exist in the build; see Status for the rows whose allocation was never made)
- [x] `.debug memory` prints the capacities.
- [x] Fixes 1–3 landed. (3 is moot: there is no pooled land-side `Spell`; see Status)
- [ ] Soak memory numbers pass the three criteria. (needs the step 15 runs, not run)
- [ ] ASan / Verifier run clean. (needs a build)

## Status

Done statically (no build). The table above was written against the plan's design; the build differs in places,
so the row-by-row result is:

| Row | In the build | Owner comment / free site |
| --- | --- | --- |
| `ObjectSlotPair` | yes | `ObjectSlot.h` at `Init`. **Changed:** the aura and movement arrays are now one allocation *per slot* (`_auraStorage[2]`, `_movementStorage[2]`), so `EnsureAuraCapacity` / `EnsureMovementCapacity` only ever replace the write slot's array. The old code reallocated one block for both slots mid-beat and freed the previous one while a view chunk on another core could be walking the published slot's `auras` — a use-after-free the plan's "regrow at the seal" was meant to avoid. Per-slot arrays make a mid-beat grow legal without the seal: the write slot was last readable two beats ago. `Regrowths()` and `HeapBytes()` added; `.debug slot` prints both. |
| `UnitRecordLog` bytes, `TargetHead` | yes, but owned by `RecordLogSet` (singleton), not `CoreState` | comment on `RecordLogSet`; `Shutdown()` added and called from `MapMgr::UnloadAll` after the pool stopped; `HighWater()` / `HeadCapacity()` for the command. Growth is on spill, not ×1.5 at the seal. |
| `DeferBuffer` | yes: `Map::_defers` is a `DeferBufferSet`, one buffer per core per map, not per `CoreState` | comment on `DeferBuffer` and `Map::_defers`; freed with the map; `Capacity()` / `HighWater()` / `Allocated()` added |
| `WorkDeque` ring | yes | `CoreState` comment; `Fill` swaps the phase list in, no growth rule needed |
| `_wakeOverflow` | no such thing (`next` is a vector) | — |
| `dtNavMeshQuery` per (core, mesh) | yes | `CoreState::navQueries`; `ForgetNavMesh` from `~MapCollisionData` |
| Land-side `Spell` pool | **no** — `Unit::ApplySpellHit` builds the land `Spell` on the stack, so its destructor runs on every exit path | fix 3 has nothing to apply to |
| `AddToMap` defer owning a `WorldObject*` | yes | **fix added:** `Map::DropPendingAddToMapDefers()` from `Map::UnloadAll` deletes objects of never-applied `AddToMap` defers (expired instance / shutdown with defers pending) |
| `RemoveFromMap` free | yes, as `CoreState::freeList`, freed in `SealSerial` | comment |
| `Map::_newcomers`, `GridPending`, `Player::_updateData`, `MapPhaseClock` | do not exist (S1a not built; newcomers are scheduled directly; `UpdateData` lives in `ViewWork`) | — |
| `CrowdBuffer` per crowded cell | yes (step 17, built after this audit) | `Map::_crowdBuffers`, allocated in S2 at the first threshold crossing, inactive but retained when the cell empties, freed with the map; `_entries` keep their `ByteBuffer`s at high-water; `.debug memory` prints allocated / active / KB |
| `_taskMembers` | yes | comment at the member |
| `heavy`, `viewItems`, histogram, `CoreState` | yes | `CoreState` comment; alive to process exit (objects freed in `UnloadAll` still return slot indices through it after `Stop`) |
| `ViewWork` | yes | comment at `Player::_view`; **cap added:** `UpdateData::Compact(64 KB)` after every commit (new `Capacity()` on `UpdateData`, `capacity()` on `ByteBuffer`) |
| Slot index | yes | **fix 2:** the free list is a beat-tagged deque; `Allocate` takes the oldest release only if `freedBeat < beat`, else a fresh index. S2 order is free list → `DestroyExpiredInstances` → return indices; the parity-*k* log is still read in beat *k*+1, and the new holder of a reused index never has the dead object's `_writtenBy` bits, so *k*+1 is the right gate. `FreeCount()` added. |

**Fix 1:** `MMapMgr::LoadTile` leaked the tile buffer on a short `fread`; `dtFree(data)` added. `SharedNavMesh::Write`
already frees on a failed `addTile`.

**`.debug memory`** prints process RSS (Windows `GetProcessMemoryInfo`, Linux `/proc/self/statm`), every core's
list and deque capacities and nav-query count, both record-log parities (bytes, heads, high-water, spills), slot
indices handed out / in use / free, per-map defer buffers (allocated, resident, high-water), and for in-world
players the `ViewWork` buffers and slot-pair bytes with the regrowth count.

**Not done:** the soak measurement and the ASan / Verifier run both need a running server (plan 15 Status).
