# 14 — Workers

Add cores to the step 12 loop. Nothing about what a task does changes; this step is the deque, the claim, the steal, the completion count, the wake, per-worker nav queries, and the removal of every lock and shared atomic from the task path. `MapUpdate.PhaseThreads ≥ 2` starts `key − 1` worker threads once; the world thread is core 0.

## Files

- `BeatPool.h/.cpp` — `WorkDeque` becomes a Chase–Lev deque; `Start`, `Stop`, `WorkerMain`, `Steal`, `WaitCompletion`, wake; `RunPhase` for all five phases on the pool.
- `Object.h` — `WorldObject` gains `alignas(8) std::atomic<uint8> _claimed{0};` and `std::atomic<uint32> _scheduledBeat{0};`.
- `MapMgr.cpp` — `Start(key)` after world load, `Stop()` in `UnloadAll`.
- `PathGenerator.cpp`, `MapCollisionData.cpp` — per-worker query.
- `MapTree.h/.cpp`, `SharedNavMesh.h`, `MapGridManager.h/.cpp` — the step 02 read-side locks and counters are deleted (§Geometry below).
- `UnitRecordLog`, `DeferBuffer` — one per core (already structured that way).

## Deques

```cpp
class WorkDeque           // Chase–Lev, single owner pushes/pops bottom, thieves steal top
{
public:
    void push(WorkItem);               // owner only; grows the ring at the seal if full (never mid-beat: capacity is high-water + slack, checked at seal)
    WorkItem pop();                    // owner only
    WorkItem steal();                  // any core
    size_t size() const;
private:
    std::atomic<int64> _top, _bottom;
    std::atomic<WorkItem*> _buf; size_t _cap;
};
```

`WorkItem` is a tagged 16-byte value: `{WorldObject* root}` in U1, `{Player* viewer, uint32 chunk}` in U2, `{Map*, uint16 grid, uint8 round}` in S1a, `{Map*}` in S1b. Use the fenced formulation from Lê, Pop, Cohen, Nardelli, *Correct and Efficient Work-Stealing for Weak Memory Models* (PPoPP 2013), with the `seq_cst` fence in `pop` and the `acquire`/`seq_cst` pair in `steal` exactly as published. Do not hand-derive the memory orders; every known bug in this deque is a misplaced fence.

Only `current` is concurrent (owner pops, thieves steal). `next` is single-producer: the owner pushes republishes, and `Wake` pushes onto the *calling* core's own `next`. `next` therefore needs no atomics at all and is a plain ring; the swap at beat start hands it to `current`.

Capacity policy: at the seal, if any `next` deque is above 75% of its capacity, double it (allocation at the seal is allowed; mid-beat growth is not). A `push` that would overflow mid-beat (from `Wake`) spills onto a per-core `std::vector<WorldObject*> _wakeOverflow` that the seal merges into `next`.

## Claim

```cpp
bool WorldObject::TryClaim() { uint8 e = 0; return _claimed.compare_exchange_strong(e, 1, std::memory_order_acquire, std::memory_order_relaxed); }
void WorldObject::Unclaim() { _claimed.store(0, std::memory_order_release); }
```

A unit can be in two deques at once only through `Wake` (writer A pushes B onto A's `next`; B's own task also pushed B onto B's core's `next`). `Wake` first does `_scheduledBeat.compare_exchange_strong(expected = beat, beat + 1)`; if the exchange fails the unit is already scheduled and `Wake` does nothing. The task body sets `_scheduledBeat = beat + 1` before pushing itself. The claim flag is the second line of defence: a duplicate handle that loses the claim is dropped and does not count toward `published` (the loser decrements a per-core `_dropped` counter that `WaitCompletion` subtracts).

## Steal

```cpp
WorkItem BeatPool::PopHeavy(CoreState& me)                       // §Heavy first (step 12): every core's heavy ring, own first
{
    if (WorkItem w = me.heavy.pop()) return w;
    for (CoreState* v : me.stealOrder) if (WorkItem w = v->heavy.steal()) return w;
    return {};
}

WorkItem BeatPool::Steal(CoreState& me)
{
    for (CoreState* v : me.stealOrder) if (WorkItem w = v->current.steal()) return w;   // same-socket cores first, then remote
    return {};
}
```

`stealOrder` is built once at `Start`: the cores sharing this core's NUMA node (from `GetNumaNodeProcessorMask` / `numa_node_of_cpu`, or all cores when there is one node) in a per-core random rotation, followed by the remote cores in a random rotation. One full pass, then give up. A core that gives up adds its `completed` and parks. Two-level order keeps steals on the local socket while there is local work; it changes nothing on a single socket.

Heavy items: `heavy` rings are drained by every core before any `current`. They are tiny (a handful of entries) so the extra `steal()` attempts per pop are a few cache-line reads; `PopHeavy` is skipped once the per-beat heavy count (`Σ heavy.size()`, read once at phase start) has been consumed (per-core counter).

## Completion

```cpp
void BeatPool::WaitCompletion()
{
    uint32 want = published.load(relaxed);
    for (;;)
    {
        uint32 got = 0; for (CoreState& c : cores) got += c.completed.load(acquire) - c.dropped.load(acquire);
        if (got == want) return;
        _completionSem.acquire();      // a parking worker releases it once after its final add; spurious wakes just re-sum
    }
}
```

No global in-flight atomic: `published` is written once before the wake; the per-core counters are each written by one core on their own cache line.

`tail` for the clock: the coordinator records the time the second-to-last core parked and the time the sum matched.

## Wake and park

Workers park on a `std::counting_semaphore _beatSem{0}`; `WakeWorkers()` does `_beatSem.release(key − 1)` once per beat. A worker's loop:

```cpp
void BeatPool::WorkerMain(uint32 idx)
{
    CoreState& me = cores[idx];
    LoginDatabase.WarnAboutSyncQueries(true); CharacterDatabase...; WorldDatabase...;   // as MapUpdater::WorkerThread did
    while (!_stop.load(acquire))
    {
        _beatSem.acquire();
        if (_stop.load(acquire)) break;
        WorkerLoop(me);                                 // step 12, with Steal live
        _completionSem.release();
    }
}
```

No wake on push. No condition variable broadcast. `Stop()` sets `_stop`, releases the semaphore `key − 1` times, joins. Join happens once, at shutdown.

## Idle accounting

A core that finds nothing to pop or steal while `completed + dropped < published` is idle: it adds `now − lastTaskEnd` to `tIdle` before parking. `idle` in the clock is Σ `tIdle / cores`.

## Geometry: delete the read-side locks

After step 12 `MapUpdater` runs no map, so the configuration step 02 protected (`MapUpdate.Threads > 1` with maps on their own threads) no longer exists. Tiles install only inside a map seal task (S1), and in S1 no unit task runs. A `shared_lock` or `fetch_add` on the query path would still be an RMW on one cache line shared by every core doing line of sight or pathing — that line would bounce at every call and `sum_task` would rise with the key. So this step removes them:

- `GEOMETRY_READ_GUARD(mutex)` (step 02) expands to nothing in release and to a debug-only `ASSERT(!StaticMapTree::_writing.load(relaxed))` check. `StaticMapTree::_mutex` is deleted; `LoadMapTile`/`UnloadMapTile`/`UnloadMap`/`InitMap` set and clear a debug-only `std::atomic<bool> _writing` and `ASSERT(BeatContext::InSealTask() || !BeatContext::inBeat)`.
- `SharedNavMesh::EnterQuery`/`LeaveQuery` become no-ops in release (`NavMeshQueryScope` keeps its RAII shape so no caller changes); in debug they still count so `Write` can `ASSERT(_queries == 0)` instead of waiting. `Write` asserts `InSealTask() || !inBeat`.
- `MapGridManager::GetGrid`, `IsGridCreated`, `IsGridLoaded` call the unlocked helpers directly — the grid array mutates only in `CreateGrid`/`UnloadGrid`, both seal-only or between beats. `_gridLock` stays in `CreateGrid` only, because two instances of one map id are two S1 tasks that can both create a parent grid at once.
- `StaticMapTree::QueryLocation` (copy-out, step 02) stays: it is the right API regardless of locking.

Invariant 8 of the overview (no geometry lock in a task) is now literally true: there is nothing to take.

## Per-worker nav queries

`PathGenerator` today uses `MMapData::GetNavMeshQuery()` (one per map). Replace with:

```cpp
dtNavMeshQuery const* BeatPool::NavQueryFor(SharedNavMesh const& mesh)
{
    CoreState& me = Current();                           // thread_local CoreState*
    auto it = me.navQueries.find(mesh.UnsafeGet());
    if (it == me.navQueries.end())
    {
        ManagedNavMeshQuery q(dtAllocNavMeshQuery());
        q->init(mesh.UnsafeGet(), 1024);                  // same maxNodes as MMapData::CreateNavMeshQuery
        it = me.navQueries.emplace(mesh.UnsafeGet(), std::move(q)).first;
    }
    return it->second.get();
}
```

`UnsafeGet` is safe because tiles install only in S1. `navQueries` is per core, so a map seal task that paths (a script in `ScriptsProcess`) uses its core's query too. When a map unloads (`MapMgr::UnloadAll`, instance destroy — both in S2 or between beats), the coordinator calls `ForEachCore(c => c.navQueries.erase(mesh))` — allowed because no task runs. `MMapData::_navMeshQuery` is deleted.

## Phases U2, S1a, S1b on the pool

All five phases of step 12 use the same `RunPhase`: publish, wake, `WorkerLoop`, completion wait. Nothing phase-specific is added to the deque or the wake. What this step adds per phase:

**U2 — view chunks.** `QueueView` (step 12) sets `chunkCount > 1` for a crowded viewer:

```cpp
uint32 n = p->GetObjectVisibilityContainer().GetVisibleWorldObjectsMap()->size();
uint32 byCount = (n + VIEW_CHUNK_TARGETS - 1) / VIEW_CHUNK_TARGETS;                 // VIEW_CHUNK_TARGETS = 64
uint32 byTime  = p->_lastViewNs > heavyThresholdNs ? uint32(p->_lastViewNs / heavyThresholdNs) + 1 : 1;
vw.chunkCount = std::clamp(std::max(byCount, byTime), 1u, cores.size());
vw.chunkData.resize(vw.chunkCount);                                                  // capacity retained; resize is a no-op after the first beat
```

Chunk *c* builds values blocks for buckets `[c·B/K, (c+1)·B/K)` of the viewer's `unordered_map` (`B = bucket_count()`), which is read-only until the last chunk's `ViewCommit`. Chunks of one viewer land on different cores through the normal deque/steal path; items are pushed heavy-first (`_lastViewNs`). `view_tail` then is one chunk of the most crowded viewer, not the viewer.

**S1a — grid items.** `PublishGridItems` (step 12) spreads `(map, grid)` items by `(mapId·64 + gridId) % cores` and heavy-first by `_lastGridNs`. Two instances of one map id loading the same parent grid serialise on `_gridLock` inside `CreateGrid`; everything else in a grid task touches only that grid's lists and the objects routed to it, so the two rounds need no lock. `grid_tail` is the busiest grid's list edits.

**S1b — map items.** One `WorkItem{Map*}` per map, spread by `id % cores` so the two continents and the largest battleground do not start on the same core; steal balances the rest. `seal_tail` is the busiest map's residue (store edits, transports, respawns, scripts, hooks), none of which grows with moving units.

A worker in U2, S1a, or S1b sees `inBeat == true` with `owner == nullptr`. `MayAccess` handles it through `sealMap`/`sealGrid` (step 04); a U2 item sets `owner` to the viewer as a U1 task would.

## Per-beat path budget

Pathfinding is the single most expensive thing a creature's task can do, and a far path across a continent is the usual U1 straggler after players in crowds. Detour's sliced API bounds it:

```cpp
// PathGenerator, replacing the one-shot findPath
dtStatus s = q->initSlicedFindPath(startPoly, endPoly, startPos, endPos, &_filter);
int done = 0; s = q->updateSlicedFindPath(sWorld->getIntConfig(CONFIG_PATH_ITER_BUDGET), &done);        // default 512 node expansions
if (dtStatusInProgress(s)) { s = q->finalizeSlicedFindPathPartial(_pathPolyRefs, &_polyLength, MAX_PATH_LENGTH); _type |= PATHFIND_INCOMPLETE; }
else                        s = q->finalizeSlicedFindPath(_pathPolyRefs, &_polyLength, MAX_PATH_LENGTH);
```

`finalizeSlicedFindPathPartial` returns the path to the explored node closest to the goal; `PathGenerator` already handles `PATHFIND_INCOMPLETE` (`DT_PARTIAL_RESULT` today) by letting the movement generator walk it and re-path on arrival, now from closer, with fresh budget. No slice state is kept across beats, so the per-worker `dtNavMeshQuery` is reused freely. 512 expansions complete every path shorter than roughly 150–200 polys outright, which is almost every path a creature takes; only cross-zone paths become two or three legs. Behaviour change (recorded in 00): a creature chasing across a zone follows a slightly less optimal route in the first legs. `.debug slowtasks` shows `path` as a split field so the budget can be tuned against `tail`.

Same budget for `findStraightPath` smoothing (`MAX_POINT_PATH_LENGTH` already caps it) and for `raycast` (bounded by polygon count, no change).

## Grid visitors

`Cell::VisitObjects` and the grid searchers walk live `GridRefMgr` lists. They are read-only during a beat because relinks, inserts, and removes are seal-only. Visitors read visited objects through `ReadSlot()` (steps 07–10 changed the hot ones; step 13 found the rest). No lock is added to the lists.

## `Start`

`MapMgr::Start(uint32 key)` is called from `World::SetInitialWorldSettings` after maps are loaded and before the world loop. `key = getIntConfig(CONFIG_MAPUPDATE_PHASE_THREADS)`; `cores.resize(key)`; threads `key − 1`; each thread sets `thread_local CoreState* tlsCore`. Thread names `beat-worker-N`.

**First touch.** `CoreState`'s big buffers (record log, defer buffer, deque rings, heavy ring, `viewItems`, nav queries) are allocated and zero-filled **by the worker thread itself** in `WorkerMain` before its first `_beatSem.acquire()`, not by the coordinator in `Start`. On a NUMA box the OS then places them on the worker's node. Resizes at the seal are done by the coordinator and re-touched by the owner on its next beat; they are rare.

**Affinity.** `MapUpdate.PhaseAffinity` (optional config, default empty) is a CPU list; when set, worker *i* is pinned to the *i*-th CPU in the list and the world thread to the first. Default leaves the scheduler alone. It exists so a two-socket box can be run as one socket for the step 15 comparison.

## Checks

1. Key = 2, three characters in one spot: all see each other (`VisibilityLink` across cores), a hit lands next beat on whichever core owns the target, `sum_task` within noise of key 1 for the same scene. Clean shutdown.
2. Key = 2, debug build, the step 13 procedure again: zero asserts. Any assert here is a site step 13 missed because it needed two cores to reach (e.g. a steal landing a unit on a different core than its pet).
3. Key = 4, four characters on two maps (two in a city, two in Deadmines): `tasks` includes both maps; `coord` under budget; a party across the two maps works.
4. Key = 13 on an empty server: `idle` ≈ `wall` (nothing to do), `coord` under budget, no spin.
5. A debug counter of `Wake` pushes that failed the `_scheduledBeat` CAS and of claims lost: both small and non-growing.
6. Key = 2 then 4, twenty creatures pathing toward a player across a doorway (LoS and navmesh on every task): `sum_task` flat across the two keys. If it rises, a shared line is still on the geometry path — `perf c2c` / VTune memory access on the hot loop names it.
7. Key = 4, Deadmines ×2 plus a continent: `seal_map` ≈ `seal_tail`, `grid` ≈ `grid_tail`, neither the sum; `seal_ser` under 100 µs.
8. Key = 2, a player in a vehicle crossing a cell boundary; a mind-controlled creature walked by its controller: group stays on one core, `MayAccess` asserts zero in debug.
9. Key = 4, one character standing in a spot with 300 visible objects (a city hub with bots, or `.npc add` ×300): `QueueView` produces ≥ 4 chunks; `view_tail` is ≈ `view / 4`, not ≈ `view`; the client receives one `SMSG_UPDATE_OBJECT` per beat with the same block count as at key 1; debug: no chunk touched the visible map before commit.
10. Key = 4, a creature pathing from Goldshire to Stormwind gate (`.npc follow` on a far creature): `.debug slowtasks` shows `path` under the budget on every beat, the creature arrives, and the path has at most three legs (debug counter of `PATHFIND_INCOMPLETE` per creature).
11. Key = 13, fifty creatures die in one beat (AoE on a pack): their removes appear as grid items in S1a, `grid_tail` is one grid, `seal_tail` does not move, and every attacker's inbox has one `ThreatRemove` per dead creature.
12. Two-socket box only: `stealOrder` lists local cores first (debug print at `Start`); `sum_task` at `cores − 1` vs pinned to one socket recorded for step 15.

## Must not

- No `std::thread` created or joined inside a beat.
- No wake on push, no condition variable broadcast.
- No global in-flight atomic.
- No lock, `shared_lock`, or shared atomic RMW on the geometry or grid query path. Debug counters only.
- No `MapUpdater` running maps.
- No worker calling `Query`, `DirectExecute`, `LoadGrid`, `LoadAllCellsInGrid`.
- No per-map work in `SealSerial`.

## Checklist

- [ ] Chase–Lev deques (Lê et al. 2013 fences), growth at the seal only, wake overflow vector; `next` is a plain ring.
- [ ] Claim CAS + `_scheduledBeat` CAS dedupe; dropped count subtracted from completion.
- [ ] Steal random start, one round.
- [ ] Per-core padded `completed`; `published` fixed before wake; semaphore wake once per beat.
- [ ] `tIdle`, `tail` recorded.
- [ ] `StaticMapTree::_mutex` deleted, `SharedNavMesh` counters debug-only, `MapGridManager` readers unlocked, `_gridLock` on `CreateGrid` only.
- [ ] Per-worker nav queries keyed by mesh pointer, freed on map unload in S2; `MMapData::_navMeshQuery` deleted.
- [ ] U2 view chunks (`VIEW_CHUNK_TARGETS`, `_lastViewNs`), bucket ranges, last-chunk commit; `view_tail` recorded.
- [ ] S1a grid items and S1b map items on the pool; `grid_tail`, `seal_tail` recorded.
- [ ] `PopHeavy` across all cores before `current`; two-level `stealOrder` (local node first).
- [ ] Per-beat path budget via sliced Detour API, `CONFIG_PATH_ITER_BUDGET`, `path` split in `.debug slowtasks`.
- [ ] First-touch allocation of per-core buffers in `WorkerMain`; optional `MapUpdate.PhaseAffinity`.
- [ ] `Start` once after load, `Stop` joins at shutdown.
- [ ] Checks 1–12 passed and written in the exit note.
