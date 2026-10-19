# 12 — Beat loop on one core

Replace `Map::Update` and the `mapUpdateStep` rotation with one process-wide beat on `MapMgr`. At `MapUpdate.PhaseThreads = 1` the world thread runs every task itself. This step introduces the deque, the task body, task groups, the two-phase seal (S1 per map, S2 serial), and the pool clock. Step 14 adds workers to the same loop and runs S1 on them; nothing in this step is single-thread-only, and nothing that grows with the world is placed in S2.

## Files

- New `src/server/game/Maps/BeatPool.h`, `BeatPool.cpp` — `BeatPool`, `BeatScheduler`, `WorkDeque`, per-core state, `RunPhase`, `GridBody`, `MapSealTask`, `SealSerial`.
- `MapMgr.h/.cpp` — `MapMgr::Update` (251) becomes the beat driver; `m_updater`, `i_timer[4]`, `mapUpdateStep` removed.
- `Map.h/.cpp` — `Map::Update` (436) deleted; `Map::BeatEpilogue(uint32 diff)`, `Map::SealApply()`, `_freeAtSeal`, `_newcomers` added; `UpdateNonPlayerObjects`, `_pendingAddUpdatableObjectList` handling moved as described.
- `Object.h/.cpp` — `WorldObject::_taskRoot`, `_taskMembers`, `TaskRoot()`, `IsTaskRoot()`; `Vehicle.cpp`, `Unit.cpp` charm — `TaskGroupJoin`/`Leave` appends (step 09 added the defer kinds; this step applies them to scheduling).
- `MapInstanced.cpp` — `MapInstanced::Update` deleted; child maps publish their own tasks.
- `MapUpdater.h/.cpp` — kept only for `schedule_map_preload` and `schedule_lfg_update`; `schedule_update` and `MapUpdateRequest` deleted.
- `World.cpp:1254` — `sMapMgr->Update(diff)` unchanged as the call site.
- `Main.cpp:584` — `WorldUpdateLoop` unchanged. The loop still runs `World::Update` every `MinWorldUpdateTime` (1 ms); the beat runs inside it when the interval has elapsed.
- `MapPhaseTiming.*` from step 01 deleted; the config key removed.

## Timing model

```cpp
void MapMgr::Update(uint32 diff)
{
    _lfg(diff);                                   // sLFGMgr->Update(diff, 1) on this thread; the MapUpdater LFG task goes away
    _beatAccum += diff;
    if (_beatAccum < _beatInterval) return;        // _beatInterval = MapUpdateInterval; world loop keeps spinning at 1 ms for sessions etc.
    uint32 beatDiff = _beatAccum; _beatAccum = 0;
    _pool.RunBeat(beatDiff);
}
```

Players and creatures receive the same `diff` = `beatDiff`. Players were updated every world tick before; now every beat. `World::UpdateSessions` (THREADUNSAFE packets, logins, logouts) still runs every world tick on this thread between beats, so the containers it edits (`m_mapRefMgr`, `HashMapHolder<Player>`, `Group`) change only between beats.

## Per-core state

```cpp
struct CoreState
{
    alignas(64) std::atomic<uint32> completed{0};   // tasks finished by this core this phase
    WorkDeque current, next;                         // WorkItem handles, Chase–Lev in step 14; plain ring here
    WorkDeque heavy;                                 // roots whose last task was long: drained by every core before `current` (§Heavy first)
    UnitRecordLog log;                               // step 06
    DeferBuffer defers;                              // step 09
    std::unordered_map<dtNavMesh const*, ManagedNavMeshQuery> navQueries;   // step 14 fills
    std::vector<WorkItem> viewItems;                 // U2 items produced by this core's U1 tasks (players, chunked in step 14)
    uint64 tTask, tClaim, tInbox, tUpdate, tPublish, tView, tIdle;          // clock, ns
};

struct BeatPool
{
    std::vector<CoreState> cores;                    // size = key; cores[0] is the coordinator (world thread)
    std::atomic<uint32> published{0};                // items published this phase (fixed before the wake)
    void RunBeat(uint32 diff);
    void TaskBody(WorldObject* root, uint32 diff, CoreState& me);           // U1
    void ViewBody(Player* viewer, uint32 chunk, CoreState& me);             // U2
    void GridBody(Map* map, GridCoord grid, SealRound round, CoreState& me); // S1a
    void MapSealTask(Map* map, uint32 diff);                                 // S1b
    void SealSerial(uint32 diff);                                            // S2
};
```

A beat is five phases, each a publish → work → completion-wait on the same pool:

| Phase | Items | Writes |
| --- | --- | --- |
| **U1** unit | one per due task root | the group's live fields; records; defers; slot publish |
| **U2** view | one per in-world player (step 14: one per chunk of a crowded player's visible set) | the viewer's `ViewWork`; on commit the viewer's visible set, `VisibilityLink/Unlink` records, its packet |
| **S1a** grid | one per grid with pending grid-local defers, two rounds (unlink, link) | that grid's cell lists; pending map-side work collected per task |
| **S1b** map | one per map | map-wide containers, transports, respawns, scripts, epilogue hooks, two-sided removals |
| **S2** serial | coordinator alone | global defers, buffer resets, newcomers, frees, slot indices, clock |

Five completion waits (U1→U2, U2→S1a-unlink, unlink→link, link→S1b, S1b→S2) at roughly 10–30 µs each: well under 1% of the interval. Only S2 is serial. Everything that grows with the world — unit updates, hits, visibility, pathing, relinks, spawns — is in a phase that spreads.

`BeatScheduler::Wake(WorldObject*)` (called from `UnitRecordLog::Append`, step 06) pushes **the target's task root** onto the **calling core's** `next` and sets `root->_scheduledBeat = beat + 1` with a relaxed CAS so several writers push once.

## Task groups

A vehicle and its seated passengers, and a charmer and the unit it charms, are one task. They write each other's fields constantly (`Vehicle::RelocatePassengers`, seat maps, `m_charmer`/`m_charmed`, charm AI, `PossessAI` reading the possessor's input), and modelling that as records would put the passenger one beat behind the vehicle on the server every beat. Grouping costs parallelism only for that handful of units.

```cpp
class WorldObject
{
    WorldObject* _taskRoot = this;                  // the object whose task updates this one; written only at the seal
    std::vector<WorldObject*> _taskMembers;         // non-empty on a root with members; written only at the seal
public:
    WorldObject* TaskRoot() const { return _taskRoot; }
    bool IsTaskRoot() const { return _taskRoot == this; }
};
```

- Root of a seated unit = the vehicle base (`GetVehicleBase()`); root of a charmed unit = the charmer; nested (a charmer seated in a vehicle) resolves to the top: at the seal, follow `_taskRoot` until fixed point, bounded depth 3.
- Membership changes are **defers** (`TaskGroupJoin{rootGuid, memberGuid}`, `TaskGroupLeave{memberGuid}`, step 09) appended by `Vehicle::AddPassenger`/`RemovePassenger` and `Unit::SetCharmedBy`/`RemoveCharmedBy`, and applied at the seal: set `_taskRoot`, edit `_taskMembers`, and (join) drop the member from the due set / (leave) push the member as a newcomer. The boarding or charm itself takes effect at that seal — one beat, the same latency as any record — and thereafter the group runs with zero lag.
- The seat map, `m_charmer`, `m_charmed`, `m_Controlled` for the charmed unit, and `RelocatePassengers` are then plain own writes inside the group's task. The `Charm` and `Passenger` record kinds of step 06 are not needed.
- Pets, guardians, and totems are **not** grouped: they are common, their coupling to the owner is through spells and threat (records), and grouping them would halve the parallelism of every hunter and warlock.
- Transports stay on the seal (`DelayedUpdate`), where moving their passengers is legal because no unit task runs.

## Due set and publication

At the first beat, and whenever a map is created, the coordinator seeds: every player in `m_mapRefMgr` and every object in `_updatableObjectList` of every map onto `cores[i].next` round-robin. From then on **tasks republish themselves**:

```cpp
bool WorldObject::IsDueNextBeat() const
{
    if (!IsTaskRoot()) return false;                 // a member is updated by its root's task
    if (IsPlayer()) return true;
    if (IsUpdateNeeded() || _writtenBy.load(relaxed) != 0) return true;   // IsUpdateNeeded: Creature.cpp:3999, GameObject.cpp:3095, DynamicObject.cpp:282, WorldObject 3248
    for (WorldObject* m : _taskMembers) if (m->IsPlayer() || m->IsUpdateNeeded() || m->_writtenBy.load(relaxed) != 0) return true;
    return false;
}
```

`_updatableObjectList` and the recheck timer (`Map.cpp:540–581`) are replaced by this rule: a creature that stops being needed simply does not republish and has no task until a record wakes it or `AddObjectToPendingUpdateList` (now a defer, step 09) pushes it at the seal. `IsUpdateNeeded`'s `isCellMarked` read is one beat late (step 09); `IsInCombat` and `_visiblePlayersMap.empty()` are own fields.

Newcomers (`AddToMap` defers, respawns from `ProcessRespawns`, players entering the map) are pushed by the coordinator at the seal onto the `next` deque with the fewest entries.

## Heavy first

The beat ends when the last task ends. A long task that starts late is the tail; the same task started first is hidden behind the short ones. Every `WorldObject` keeps `uint32 _lastTaskNs` (written by its own task). At republish:

```cpp
if (root->IsDueNextBeat())
{
    root->_scheduledBeat = beat + 1;
    (root->_lastTaskNs > heavyThresholdNs ? me.heavy : me.next).push(root);
}
```

`heavyThresholdNs` is recomputed by the coordinator in S2 as 8× the median `_lastTaskNs` of this beat's tasks (median from a 64-bucket log-scale histogram the cores fill; no sort). In `WorkerLoop` every core first drains every core's `heavy` ring (steal order, step 14) and only then its own `current`. This is longest-processing-time-first scheduling: the heavy few start at t = 0 on separate cores and the short many fill in behind them; the beat's makespan is within 4/3 of optimal regardless of how the heavy ones are distributed. No barrier, no sort, one branch per republish.

The same rule applies to S1a grid items (`_lastGridNs` per grid) and U2 view items (`_lastViewNs` per player, step 14 chunks by it).

`.debug slowtasks` prints the ten largest `_lastTaskNs` of the last beat with type, entry, map, and the `inbox/update/publish` split, so the clock names the straggler.

## The beat

```cpp
void BeatPool::RunBeat(uint32 diff)
{
    BeatContext::beat.fetch_add(1, relaxed);
    BeatContext::inBeat.store(true, release);

    // U1 — unit tasks
    for (CoreState& c : cores) std::swap(c.current, c.next);
    RunPhase(Phase::Unit, diff);                     // publish = Σ current.size() + Σ heavy.size(); wake; WorkerLoop(cores[0]); WaitCompletion()

    // U2 — view tasks, reading this beat's slots
    for (CoreState& c : cores) for (WorkItem w : c.viewItems) c.current.push(w);
    RunPhase(Phase::View, diff);

    // S1a — grid tasks, two rounds
    PublishGridItems(SealRound::Unlink);  RunPhase(Phase::Grid, diff);
    PublishGridItems(SealRound::Link);    RunPhase(Phase::Grid, diff);

    // S1b — one task per map
    for (Map* m : allMaps) cores[m->GetId() % cores.size()].current.push(WorkItem{m});
    RunPhase(Phase::MapSeal, diff);

    BeatContext::inBeat.store(false, release);
    SealSerial(diff);                                // S2
}

void BeatPool::RunPhase(Phase p, uint32 diff)
{
    auto t0 = now();
    for (CoreState& c : cores) c.completed.store(0, relaxed);
    published.store(Σ c.current.size() + Σ c.heavy.size(), relaxed);
    phase = p;
    WakeWorkers();                                   // no-op at key 1 (step 14)
    WorkerLoop(cores[0]);                            // the coordinator works too
    WaitCompletion();                                // sum(completed) == published; at key 1 it already is
    phaseWall[p] = now() - t0;
}

void BeatPool::WorkerLoop(CoreState& me)
{
    for (;;)
    {
        WorkItem w = PopHeavy(me);                   // §Heavy first: every core's heavy ring before anything else (used in U1 and S1a)
        if (!w) w = me.current.pop();
        if (!w) w = Steal(me);                       // step 14; empty here
        if (!w) break;
        if (!w.TryClaim()) continue;                 // step 14; always true here
        switch (phase)
        {
            case Phase::Unit:    TaskBody(w.root, diff, me); break;
            case Phase::View:    ViewBody(w.viewer, w.chunk, me); break;
            case Phase::Grid:    GridBody(w.map, w.grid, w.round, me); break;
            case Phase::MapSeal: MapSealTask(w.map, diff); break;
        }
        me.completed.fetch_add(1, release);
    }
}
```

### Task body

```cpp
void BeatPool::TaskBody(WorldObject* root, uint32 diff, CoreState& me)
{
    auto t0 = now();
    // Inbox for the whole group first, so a member's records are visible to the root's update and vice versa.
    { TaskScope s(root, root); root->ApplyInbox(); }                                   // step 06; records from last beat, source-guid order
    for (WorldObject* m : root->_taskMembers) { TaskScope s(m, root); m->ApplyInbox(); }
    auto t1 = now();
    UpdateOne(root, root, diff);                                                        // vehicle / charmer first: it moves the members
    for (WorldObject* m : root->_taskMembers) UpdateOne(m, root, diff);
    auto t2 = now();
    { TaskScope s(root, root); root->PublishSlot(); root->ClearUpdateMask(false); }     // step 05, step 10
    for (WorldObject* m : root->_taskMembers) { TaskScope s(m, root); m->PublishSlot(); m->ClearUpdateMask(false); }
    auto t3 = now();
    // View work (scan + packet) is phase U2, so it reads this beat's slots: queue one item per in-world player in the group.
    if (Player* p = root->ToPlayer()) if (p->IsInWorld()) QueueView(p, me);
    for (WorldObject* m : root->_taskMembers) if (Player* p = m->ToPlayer()) if (p->IsInWorld()) QueueView(p, me);
    root->_lastTaskNs = uint32(t3 - t0);
    if (root->IsDueNextBeat()) { root->_scheduledBeat = beat + 1; (root->_lastTaskNs > heavyThresholdNs ? me.heavy : me.next).push(root); }
    root->Unclaim();                                                                    // step 14
    me.tInbox += t1 - t0; me.tUpdate += t2 - t1; me.tPublish += t3 - t2; me.tTask += t3 - t0;
}

void BeatPool::QueueView(Player* p, CoreState& me)
{
    ViewWork& vw = p->_viewWork;
    vw.chunkCount = 1; vw.chunksDone.store(0, relaxed);                                 // step 14: chunkCount from visible-set size and _lastViewNs
    for (uint32 c = 0; c < vw.chunkCount; ++c) me.viewItems.push_back(WorkItem::View(p, c));
}

void BeatPool::ViewBody(Player* viewer, uint32 chunk, CoreState& me)                   // U2
{
    TaskScope scope(viewer, viewer->TaskRoot());
    auto t0 = now();
    ViewWork& vw = viewer->_viewWork;                                                   // step 10
    vw.slotBeat = beat;
    if (chunk == 0) viewer->ViewScan(vw);                                               // grid scan from this beat's slots → chunkDelta (objects entering / leaving view)
    viewer->ViewBuild(vw, chunk);                                                       // values blocks for bucket range `chunk` of the current visible map (read-only until commit)
    if (vw.chunksDone.fetch_add(1, acq_rel) + 1 == vw.chunkCount) viewer->ViewCommit(vw);   // last finisher: create/out-of-range blocks for the deltas, set edits, VisibilityLink/Unlink records, concatenate, send
    auto t1 = now();
    viewer->_lastViewNs = uint32(t1 - t0); me.tView += t1 - t0;
}

void BeatPool::UpdateOne(WorldObject* obj, WorldObject* root, uint32 diff)
{
    TaskScope scope(obj, root);                      // BeatContext::owner = obj, root = root
    if (Player* p = obj->ToPlayer())
    {
        if (p->IsInWorld())
        {
            MapSessionFilter f(p->GetSession());
            p->GetSession()->Update(diff, f);        // PROCESS_THREADSAFE + late PROCESS_INPLACE, Map.cpp:449–450 moved here
            p->Update(diff);                          // Map.cpp:489
        }
    }
    else if (obj->IsInWorld())
        obj->Update(diff);                           // Map.cpp:557/578
}
```

For an ungrouped unit `_taskMembers` is empty and the body is the plain sequence inbox → update → publish → queue view item → republish; the packet itself is built in U2. Inside the group, `MayAccess` allows every member to touch every other member (step 04), which is what `Vehicle::RelocatePassengers` and the charm code need; the `TaskScope` per sub-update keeps `owner` accurate for records (`source` guid) and for the `ASSERT` messages.

`session->Update` inside the task: `WorldSession::Update` (562) with `MapSessionFilter` processes queued packets; handlers that write the player are own writes; cast/swing produce records. `WorldSession::Update` also runs `ProcessQueryCallbacks`, which can complete an async query callback that writes the player: own write, fine. Logout is `PROCESS_THREADUNSAFE` and stays in `UpdateSessions`.

An object whose map is `nullptr` (removed between publish and claim) is skipped: `IsInWorld()` check.

### Seal

The seal is three phases. **S1a** is per grid, two rounds, on the pool: the only seal work that grows with the number of moving or dying units is editing grid cell lists (relink, insert, remove), and a grid's lists are disjoint from every other grid's. **S1b** is per map on the pool: maps are disjoint, so each map's remaining defers, transports, respawns, scripts, and epilogue run concurrently with every other map's. **S2** is the cross-map remainder on the coordinator alone. Only S2 is serial, and it is O(cross-map defers + cores), not O(units) or O(maps). At key 1 the coordinator runs every phase serially; nothing in them is single-thread-only.

```cpp
// S1a — publish one item per (map, grid) that has pending grid-local work this round
void BeatPool::PublishGridItems(SealRound round)
{
    for (Map* m : allMaps) for (GridCoord g : m->GridsWithPendingDefers(round))             // per-map bitset of touched grids, see §Routing
        (m->_lastGridNs[g] > heavyThresholdNs ? cores[0].heavy : cores[(m->GetId() * 64 + g.id) % cores.size()].current).push(WorkItem::Grid(m, g, round));
}

void BeatPool::GridBody(Map* m, GridCoord g, SealRound round, CoreState& me)
{
    GridTaskScope scope(m, g);                                                                  // BeatContext::sealMap = m, sealGrid = g
    auto t0 = now();
    GridPending& gp = m->GridPending(g);
    if (round == SealRound::Unlink)
    {
        for (RelinkDefer const& d : gp.relinksOut) d.obj->RemoveFromGrid();                    // old cell list in this grid
        for (RemoveDefer const& d : gp.removes)   { ASSERT(d.obj->_removing); d.obj->RemoveFromGrid(); gp.mapSide.freeAtSeal.push_back(d.obj); }   // own-half cleanup already ran on the unit's task (step 06): this is one list unlink
    }
    else
    {
        if (gp.load) m->GetGridManager().CreateAndLoadGrid(g, gp.mapSide);                      // under _gridLock per parent; LoadAllCellsInGrid inserts into this grid only, map-side work collected
        for (RelinkDefer const& d : gp.relinksIn)  d.obj->AddToGrid(m->GetGrid(g), d.obj->GetPosition());   // new cell list in this grid; same-grid relinks did RemoveFromGrid here first
        for (AddDefer const& d : gp.adds)         { d.obj->AddToGrid(...); gp.mapSide.added.push_back(d.obj); }
        for (SpawnDefer const& d : gp.spawns)     { WorldObject* o = m->ConstructFromSpawn(d.spawnId, d.type); if (o) { o->AddToGrid(...); gp.mapSide.added.push_back(o); } }   // respawns create here, in parallel (step 09)
    }
    m->_lastGridNs[g] = uint32(now() - t0);
}

// S1b — one task per map
void BeatPool::MapSealTask(Map* m, uint32 diff)
{
    SealTaskScope scope(m);                           // BeatContext::sealMap = m; MayAccess allows anything on m
    m->SealApply(diff);
    m->BeatEpilogue(diff);
}

void Map::SealApply(uint32 diff)
{
    for (GridPending& gp : _gridPending)              // map-side half of what the grid tasks did
    {
        for (WorldObject* o : gp.mapSide.added)   { _objectsStore.Insert(o); o->AddToWorld(); summon hooks; _newcomers.push_back(o); }   // AddToMap tail
        for (WorldObject* o : gp.mapSide.freeAtSeal) { _objectsStore.Remove(o); o->RemoveFromWorld(); _freeAtSeal.push_back(o); }
        gp.Reset();
    }
    ApplyMapDefers();                                 // step 09 S1b kinds: RemoveFromMap{prepared=false} (two-sided body), ScriptSchedule, CellMark, UpdateListAdd/Remove, DynTreeInsert/Remove, ZonePlayerCount, TaskGroupJoin/Leave
    DelayedUpdate(diff);                              // Map.cpp:1807: transports (their passenger relinks are applied here directly: no grid task runs)
}

// S2 — coordinator alone
void BeatPool::SealSerial(uint32 diff)
{
    auto t0 = now();
    ApplyGlobalDefers();                              // cross-map only: GroupEndRoll, GroupBroadcast, BattlegroundEvent, OutdoorPvPEvent, WorldStateWrite, MapChange, DuelEnd, DeferredSpellHit, PoolEvent, InstanceBind, GameEventAction (step 09)
    for (CoreState& c : cores) { c.log.Reset(); c.defers.Reset(); c.viewItems.clear(); }
    PublishNewcomers();                               // Map::_newcomers from S1b, onto the least-loaded next
    FreeRemoved();                                    // Map::_freeAtSeal: delete now
    ReturnSlotIndices();
    heavyThresholdNs = 8 * MedianTaskNs();            // §Heavy first
    _sealSerNs = now() - t0;
    MaybeLogPoolTiming(diff);
}
```

**Routing defers to grids.** `DeferBuffer::Append` for a grid-local kind (step 09 table) computes the grid at append time from the payload (`Relink`: old and new cell → one or two grids; `AddToMap`/`RemoveFromMap{prepared}`: the object's current position; `GridLoad`: the cell) and appends into `map->GridPending(g)` directly — a per-grid, per-core bucket (`GridPending` holds one small vector per core so appends are uncontended; the grid task reads all cores' vectors). `GridsWithPendingDefers` is then a per-map bitset of touched grids, set with a relaxed `fetch_or` at append (64 × 64 grids = 4096 bits = 64 words per map; one RMW per *first* touch of a grid per beat, not per defer). The coordinator's `PublishGridItems` walks the bitset: O(touched grids), not O(defers).

**Why two rounds.** A cross-grid relink removes from grid A's list and inserts into grid B's; if A's and B's tasks ran in the same round they would both touch the object's intrusive `GridReference`. Unlink round: every grid removes what leaves it. Link round: every grid inserts what enters it. Same-grid relinks do both in the link round. Grid loads are in the link round because `LoadAllCellsInGrid` inserts.

**What a grid task may touch.** Its own grid's cell lists, the objects it relinks/adds/removes (their `GridReference` and, for removes, their own fields via `CleanupsBeforeDelete(prepared)`), and its `GridPending.mapSide` collection vectors. Not `_objectsStore`, not `HashMapHolder`, not any other object. `MayAccess` with `sealGrid` set allows exactly the objects in that grid's pending lists (debug: a `_pendingInGrid` byte set by the router).

```cpp
void Map::BeatEpilogue(uint32 diff)
{
    _mapCollisionData.GetDynamicTree().update(diff);  // 438; rebalance capped: see §Residue
    Events.Update(diff);                              // 458
    if (!sWorld->getBoolConfig(CONFIG_RESPAWN_FORCE_COMPATIBILITY_MODE)) { respawn timer; ProcessRespawns(); }   // 467–476; pops due entries → Spawn defers only (step 09), constructs nothing
    if (!m_scriptSchedule.empty()) { i_scriptLock = true; ScriptsProcess(); i_scriptLock = false; }   // 511–516
    UpdatePlayersRedirectKickEvent(diff); UpdateWeather(diff); UpdateExpiredCorpses(diff);                // 524–527
    sScriptMgr->OnMapUpdate(this, diff);              // 529
    METRIC_VALUE(...)                                  // 531–537
}
```

What a map seal task (S1b) may and may not do:

- It may touch any object on its own map directly (`MayAccess` via `sealMap`). `ScriptsProcess` command bodies run live (what `i_scriptLock` assumed), `OnMapUpdate` hooks run live, an unprepared removal's two-sided `CleanupsBeforeDelete` edits other units' threat and combat lists. `ProcessRespawns` constructs nothing: it emits `Spawn` defers for next beat's grid items.

**Residue budget.** What is left in S1b must not grow with moving, dying, or spawning units; the table is the contract:

| S1b work | Scales with | Bound |
| --- | --- | --- |
| Collected map-side adds/removes (store, `HashMapHolder`, `AddToWorld`, summon hooks) | spawns + deaths this beat | ~100–300 ns each; 100 deaths ≈ 30 µs. Summon hooks (`IsSummonedBy`, `OnSummoned`) are script code and the one item here that can be slow; a hook that paths or scans is a `.debug slowtasks` finding |
| `ProcessRespawns` | due respawns | pops only; ~50 ns each |
| `DelayedUpdate` transports | transports × passengers | a ship with 20 passengers ≈ 20 µs; few per map |
| `ScriptsProcess` | due script commands | rare; a command that summons goes through the collected path |
| `DynamicMapTree::update` | GO models changed since last rebalance | **capped**: rebalance only when `_dirtyCount ≥ 16` or every 32 beats, whichever first; otherwise the tree is queried with stale balance, which is correct (BIH queries do not depend on balance for correctness). A Wintergrasp wall sequence therefore costs one ~1 ms rebalance per few beats instead of one per wall |
| `UpdateWeather`, `UpdateExpiredCorpses`, redirect-kick, `OnMapUpdate`, metrics | zones / corpses / modules | constant; a module whose `OnMapUpdate` walks all players is a module bug, visible as `seal_tail` on that map |
- It may not touch an object on another map or a global singleton's mutable state (the same `MayAccess`; singleton mutators carry `ASSERT(!inBeat)` per step 13). Anything cross-map that today's epilogue does — a respawn that touches `sPoolMgr`, a boss kill in `ScriptsProcess` binding players through `sInstanceSaveMgr`, a script that teleports a player to another map — appends a global defer (`PoolEvent`, `InstanceBind`, `MapChange`, step 09) and S2 applies it.
- Tile installation into the shared `StaticMapTree` and `dtNavMesh` happens in S1a grid tasks (`GridLoad`). Two instances of the same map id, or two grids of one map, can load at once; `MapGridManager::_gridLock` (step 02 slice 4) serialises `CreateGrid` per parent and `LoadMapTile`/`LoadTile` are inside it. No unit task runs, so readers need no lock (step 14 removes them).
- Objects it removes go onto `Map::_freeAtSeal`; newcomers go onto `Map::_newcomers`. S2 drains both.

Sessions of players not in world (loading screen) are updated by `UpdateSessions` as today.

### Removes

A removal requested from a unit's own task is **prepared** there (`PrepareRemoval()`, step 06: the foreign half as `ThreatRemove`, `CombatEnd`, `MinionUnlink`, `GameObjectUnlink`, `VisibilityUnlink` records, and the own half — aura removal, motion clear, events, own lists — inline, then `_removing = true` and a final slot publish), then `RemoveFromMap{guid, prepared = true}` is appended and routed to the object's grid. The grid task (S1a unlink round) does `RemoveFromGrid` only and collects the object for S1b, which erases it from `_objectsStore` and `HashMapHolder` and pushes it onto `Map::_freeAtSeal`. The per-death cost at the seal is therefore two list/hash operations, whatever the unit was. A removed object that is a task root dissolves its group in S1b (members get `_taskRoot = this` and go to `_newcomers`); a removed member is erased from its root's `_taskMembers`. An unprepared removal (map unload, S1b script despawn) runs today's two-sided `RemoveAllObjectsInRemoveList` body (`Map.cpp:1833–1860`) in S1b.

`FreeRemoved()` in S2 deletes. By then no record can reference the object: records appended in beat *k* were reset in this same S2 before the free, a record to it in beat *k* was only possible while the store still held it, and `ReturnSlotIndices` (after the free) clears `head[]` for its index.

## Pool clock

```
pool timing: cores={} tasks={} wall={} sum_task={} task={} idle={} tail={} view={} view_tail={} grid={} grid_tail={} seal_map={} seal_tail={} seal_ser={} coord={} claim={} inbox={} update={} publish={}
```

`wall` = this `RunBeat` entry to the next. `sum_task` = Σ `tTask` (U1 only). `idle`, `tail` are zero at key 1. Phase walls: `view` (U2), `grid` (both S1a rounds), `seal_map` (S1b), `seal_ser` (S2). Phase tails — the longest single item: `tail` (U1), `view_tail` (U2; the most crowded viewer), `grid_tail` (S1a; the busiest grid), `seal_tail` (S1b; the busiest map). `coord` = `grid + seal_map + seal_ser` + the five barrier waits. `seal_ser` is the only serial number and must stay flat as units grow; every `*_tail` is the thing to split when its phase's wall is pinned to it. Skip beats under 20 ms; at most one line per 5 s; windowed means as in step 01.

## World thread between beats

Order inside `World::Update` is unchanged: `UpdateSessions` (1227) → LFG (1248) → `MapMgr::Update` (1254) → battlegrounds, outdoor PvP, `WorldState`, etc. All of those run with `inBeat == false` and `owner == nullptr`, so they may touch anything, as today. `sBattlegroundMgr->Update` ending a battleground removes players from a map between beats: allowed.

## Checks

1. Boot at key 1. Login. Walk, cast, fight a creature, die, resurrect, enter Deadmines, take a ship. Everything in steps 05–11 still holds (hit next beat, feedback the beat after, relink at seal).
2. `pool timing` appears when a beat exceeds 20 ms. With two characters `coord < 1 ms` and `seal_ser < 100 µs`.
3. `.server info`: the world update mean includes one beat per `MapUpdateInterval`; compare against the step 01 baseline for the same population and write both into the exit note.
4. An idle creature far from players publishes no task (`tasks` count on an empty continent equals active objects: transports, active-flagged creatures, players).
5. Hit an idle creature with a ranged spell: it has a task next beat (wake), aggroes, and chases.
6. Board a vehicle (Wintergrasp siege engine or any Icecrown quest vehicle): the player stops appearing in `tasks` as its own entry at the next seal; the vehicle moves and the player moves with it with no server-side lag (`.debug slot` on both shows the same beat stamp and consistent positions). Exit: the player is a task again at the next seal. Mind-control a creature: same, with the player as root.
7. Debug: a counter of global-defer entries per beat (`seal_ser` work) stays near zero on a quiet server and does not scale with creature count during the Deadmines run.
8. Visibility is zero-lag: a character casts a buff; the second character's client receives the aura update in the **same** beat's U2 packet (`.debug slot` on the target and the packet log on the viewer show the same beat stamp). A creature walking into view appears in the same beat its slot first falls inside the viewer's range.
9. Kill a creature that had three attackers and a pet: `ThreatRemove`/`CombatEnd`/`MinionUnlink` records are visible in `.debug inbox` on each of them next beat; the corpse's `RemoveFromMap` is applied by a grid task (debug counter of grid-task removes); nothing two-sided ran in S1b for it. AoE-kill a pack of 30 in one grid: `grid_tail` for that beat stays under 20 µs (the own-half cleanup ran on each creature's own task in U1, visible in `sum_task`, not in `grid`).
9b. Wipe a dungeon and wait for the respawn timer: respawns arrive one beat after `ProcessRespawns` pops them, as `Spawn` items in S1a (`grid` shows the burst spread across grids); `seal_tail` for that map does not spike (debug: `ProcessRespawns` constructed 0 objects).
9c. `.gm` destroy and rebuild a Wintergrasp wall segment repeatedly: `DynamicMapTree::update` rebalances at most once per 16 dirty models / 32 beats (debug counter), LoS through the wall is correct between rebalances.
10. Walk a character across a grid boundary: `grid` phase shows two grid items (unlink on the old, link on the new) and `_objectsStore` was not touched by either (debug counter).
11. `.debug slowtasks` after a boss pull names the boss; the following beats show it in the `heavy` ring (debug flag in the output) and `tail` does not grow with it.
12. Shutdown is clean: `MapMgr::UnloadAll` runs between beats; no task is running.

## Must not

- No `Map::Update`, no `mapUpdateStep`, no `MapUpdateRequest`.
- No sleep inside `MapMgr::Update`; pacing is the world loop's.
- No serial scan of all objects for inbox apply; wake and republish are the only ways onto a deque.
- No defer applied during a U1 or U2 item.
- No class rotation: every map's due units are in the same beat.
- Nothing O(units) in `SealSerial`. Anything per map belongs in S1b; anything per grid belongs in S1a.
- No grid task touching `_objectsStore`, `HashMapHolder`, or an object outside its pending lists.
- No map seal task touching another map's objects or a global singleton's mutable state without a global defer.
- No `CleanupsBeforeDelete(prepared = true)` doing anything but `RemoveFromGrid`; the own-half cleanup runs in `PrepareRemoval` on the unit's task.
- No object construction in S1b (`ProcessRespawns` emits `Spawn` defers; script summons go through `AddToMap` defers applied next beat).
- No S1b work that scales with moving, dying, or spawning units beyond the per-object store/accessor edits in the residue table.
- No view item writing anything but its `ViewWork` until commit.

## Checklist

- [x] `MapMgr::Update` accumulates to `MapUpdateInterval` and runs one beat; LFG update moved to the world thread.
- [x] Per-core state struct, swap of `current`/`next`, `published` fixed before each phase starts; `RunPhase` shared by all five phases.
- [x] `TaskBody`: group inbox, root then members `UpdateOne` with `TaskScope(obj, root)`, publish, clear mask, `_lastTaskNs`, view items queued, republish root only (heavy ring when over threshold).
- [x] U2: `ViewBody` with `ViewScan` (chunk 0) / `ViewBuild` / last-chunk `ViewCommit`; `slotBeat = beat`. (One item per player: `Player::RunViewPhase` does scan, build 0 and commit; chunking stays a step 14 matter.)
- [x] `_taskRoot` / `_taskMembers` on `WorldObject`; `TaskGroupJoin/Leave` applied in S1b; removal dissolves or detaches.
- [x] `IsDueNextBeat` = root ∧ (player ‖ `IsUpdateNeeded` ‖ pending records, for self or any member); `Wake` pushes the root onto `next`.
- [x] Heavy ring per core, `heavyThresholdNs` from the S2 median, `PopHeavy` before `current`, `.debug slowtasks`.
- [ ] S1a: grid routing at append (`GridPending` per grid per core, touched-grid bitset), unlink round then link round, `GridBody` collects map-side work; `_lastGridNs`.
- [x] S1b: `SealApply` applies the collected map-side work, then map-wide defers, `DelayedUpdate`, `BeatEpilogue`; one task per map under `SealTaskScope`.
- [x] S2: global defers, log/defer/view reset, newcomers, free, slot index return, heavy threshold, clock. Nothing per unit or per map. (Newcomers are scheduled in S1b when the map drains its pending list; frees still happen in S1b's `DelayedUpdate`.)
- [ ] `PrepareRemoval` on own-task removals does both halves and sets `_removing`; grid item does `RemoveFromGrid` only; unprepared removals two-sided in S1b.
- [ ] `ProcessRespawns` emits `Spawn` defers; grid link round constructs; `ConstructFromSpawn` shared with `ObjectGridLoader`.
- [ ] `DynamicMapTree::update` rebalance capped (16 dirty / 32 beats); residue table checked against `seal_tail` in the exit note.
- [x] `BeatEpilogue` carries lines 438, 458, 467–476, 511–516, 524–537 of the old `Map::Update`.
- [x] `inBeat` true from the U1 wake to the end of S1b.
- [x] Clock has `view`, `view_tail`, `grid`, `grid_tail`, `seal_map`, `seal_tail`, `seal_ser`.
- [x] Step 01 clock and key removed.
- [ ] Exit note has the `.server info` comparison.

## Status

### Done

- `Maps/BeatPool.h/.cpp` (`sBeatPool`): `CoreState` per core (`current` / `next` / `heavy` rings, `viewItems`, `sealItems`, clocks, 64-bucket log histogram), `RunBeat(diff)` = beat++ and `BeginBeat`, map list, seed of maps that joined, `inBeat` on, U1 → U2 → S1b, `inBeat` off, S2. `RunPhase`/`WorkerLoop` are the shape step 14 adds workers to; at one core the world thread drains every ring itself.
- `TaskBody`: skips non-roots and objects out of world, group inbox, `UpdateOne` for root then members (copy of the member list; a member that left the group inside the update is skipped), publish root and members, queue view items for in-world players, `_lastTaskNs`, histogram, republish via `IsDueNextBeat` with CAS on `_scheduledBeat` (heavy ring when over `heavyThresholdNs`), dormant non-player roots leave the map update list so a viewer's scan re-adds them. `UpdateOne` for a player = `WorldSession::Update(MapSessionFilter)` + `Player::Update` + `MarkNearbyCellsOf` (and viewpoint); otherwise `WorldObject::Update`.
- U2 `ViewBody`: `LoadGridsInRange` when a scan is due, then `Player::RunViewPhase` under `TaskScope(p, p->TaskRoot())`. `Map::RunViewPhase` deleted.
- `Map::SealApply(diff)` (S1b, under `SealTaskScope`): scripts, move lists, map-local `ApplyDefers`, pending updatable list drained and each newcomer's root scheduled, `DelayedUpdate` (transports, remove list). `Map::BeatEpilogue(diff)` (virtual): dynamic tree, `Events`, respawn timer, redirect kicks, weather, corpses, `OnMapUpdate`, deferred spell hits, metrics. `InstanceMap::BeatEpilogue` adds `instance_data->Update`.
- S2 `SealSerial`: `ApplyGlobalDefers` per map (now returns the count), `MapMgr::DestroyExpiredInstances` (new `MapInstanced::DestroyExpiredInstances`, the old `CanUnload`/`DestroyInstance` loop), slot index return (`WorldObject::RemoveFromWorld` pushes to `_slotIndicesToReturn` instead of releasing), view item reset, heavy threshold = 8× histogram median, `FlushForeignReads`, clock line `pool timing: …` (≤1 per 5 s when wall ≥ 20 ms), top-10 slow tasks for `.debug slowtasks`.
- `WorldObject`: `_taskMembers`, `_scheduledBeat`, `_lastTaskNs`, `IsTaskRoot`, `TaskMembers`, `JoinTaskGroup` (resolves to the top root ≤3 levels, moves own members along), `LeaveTaskGroup` (reschedules the member), `IsDueNextBeat`. `Map::ApplyDefer` applies `TaskGroupJoin`/`TaskGroupLeave`. `RemoveFromWorld` dissolves a root's group or detaches a member. `~WorldObject` calls `Unschedule` when `_scheduledBeat != 0` (rings and view items are scrubbed; `WorkRing::pop` skips the holes). `OnFirstRecordOfBeat` wakes via `sBeatPool->Wake`. `Map::AddPlayerToMap` schedules the player.
- `MapMgr::Update` is the accumulator (`_beatAccum` / `_beatInterval`, `SetMapUpdateInterval` feeds it); `sLFGMgr->Update(diff, 1)` on the world thread each tick. `CollectAllMaps`, `DestroyExpiredInstances`. `i_timer`, `mapUpdateStep`, the type rotation and the `t_diff == 0` session ticks are gone. `Map::Update`, `InstanceMap::Update`, `MapInstanced::Update`/`DelayedUpdate`, `UpdateNonPlayerObjects`, `ApplyInboxes`, `_updatableObjectListRecheckTimer` deleted. `MapUpdater` keeps only the grid preload request (`MapUpdateRequest`, `LFGUpdateRequest`, `schedule_update`, `schedule_lfg_update` removed).
- Step 01 `MapPhaseTiming.h/.cpp`, `_phaseTiming`, `CONFIG_MAPUPDATE_PHASE_TIMING` and the `MapUpdate.PhaseTiming` conf block removed.

### Open

- S1a is not in this step: there are no grid items, map-local defers (relink, grid load, cell marks, update list, dyn tree) are applied per map in S1b. `grid` / `grid_tail` read 0.
- Frees are still inline in S1b (`RemoveListedObject` inside `DelayedUpdate`), not collected for S2; `PrepareRemoval` two-sided removals are unchanged from step 09.
- `ProcessRespawns` still constructs inline in `BeatEpilogue`; no `Spawn` defers from it yet.
- `DynamicMapTree::update` rebalance is uncapped.
- Grid loads for a scanning viewer run inline in U2 (`LoadGridsInRange`), as in step 10.
- `Unschedule` is a linear scan over the rings (step 14 gives rings a stable index).
- `inBeat` is now true during U1–S1b, so `BeatContext::MayAccess` asserts that were latent since step 09 may surface in debug builds.
- `sBeatPool->Init(1)`: a single core regardless of `MapUpdate.Threads`; the updater threads only serve startup grid preloads.
- `.server info` comparison not done (not played).

### Behaviour

- Every map is updated every beat (`MapUpdateInterval`); no more continents / BG / instance rotation and no session-only ticks between map updates. Map-side packets wait for the player's task like everything else.
- Dormancy is decided per task from `IsDueNextBeat`, not every 30 s: a creature with no marked cell, no inbox and not active leaves the update list right after its task and comes back when a viewer's scan collects it or a record wakes it. Cell marks are refreshed every beat by every player task.
- Objects that leave the world keep their slot index until S2 of that beat.
- Not built since these edits (the user builds). `codestyle-cpp.py` passes. Not played.
