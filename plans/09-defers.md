# 09 — Defers

A write to a map or global container does not run inside a unit's update. It is appended as a plain-value defer and applied by the coordinator at the end of the tick (on this step, at the end of `Map::Update`; from step 12, at the seal). After this step, `_updatableObjectList`, `_objectsStore`, the grid cell lists, `marked_cells`, `m_scriptSchedule`, `i_objectsToRemove`, `WorldState`, and `Group` are edited only there.

## Files

- New `src/server/game/Maps/MapDefer.h`, `MapDefer.cpp` — `DeferKind`, payloads, `DeferBuffer`, `Map::ApplyDefers()`.
- `Map.h` — `DeferBuffer _defers;` (one per map on this step; per-core from step 14, routed by map pointer in the payload).
- `Map.cpp` — the writer sites listed below; `Map::Update` calls `ApplyDefers()` after `MoveAllDynamicObjectsInMoveList()` (line 520) and before `HandleDelayedVisibility()` (522).
- `Object.cpp`, `Unit.cpp`, `Creature.cpp`, `GameObject.cpp`, `Player.cpp`, `TemporarySummon.cpp`, `Group.cpp`, `WorldState.cpp`, `OutdoorPvP.cpp`, `Battleground.cpp`, `ScriptMgr`/`MapScripts.cpp` — the callers.

## Layout

```cpp
enum class DeferKind : uint8
{
    Relink,            // object moved to another cell
    GridLoad,          // EnsureGridLoaded(cell)
    AddToMap,          // summon / respawn / corpse created on a task: insert into store, grid, update list
    RemoveFromMap,     // AddObjectToRemoveList equivalent
    ScriptSchedule,    // Map::ScriptsStart
    CellMark,          // MarkNearbyCellsOf
    UpdateListAdd, UpdateListRemove,   // AddObjectToPendingUpdateList / _RemoveObjectFromUpdateList (gone in step 12)
    DynTreeInsert, DynTreeRemove,      // GameObjectModel insert/remove
    ZonePlayerCount,   // UpdatePlayerZoneStats
    GroupEndRoll,      // Group::EndRoll from a corpse
    GroupBroadcast,    // group looter / loot method packets from Kill
    BattlegroundEvent, // scores, HandleKillPlayer/Unit, EndBattleground
    OutdoorPvPEvent,   // HandleKill, capture point changes
    WorldStateWrite,   // sWorldState set/update
    DuelEnd, DeferredSpellHit,         // step 07
    MapChange,         // teleport / transport leaving the map (DelayedUpdate already does transports)
    TaskGroupJoin, TaskGroupLeave,     // vehicle seating and charm (step 12 task groups)
    PoolEvent, InstanceBind, GameEventAction,  // global singleton mutations (step 03 table)
    Spawn              // a due respawn: construct + AddToGrid in the spawn's grid item next beat (step 12 S1a)
};

// Which seal phase applies a kind (step 12): per-map kinds in S1 inside the map's seal task, cross-map kinds in S2 on the coordinator.
constexpr bool IsGlobalDefer(DeferKind k)
{
    switch (k)
    {
        case DeferKind::GroupEndRoll: case DeferKind::GroupBroadcast: case DeferKind::BattlegroundEvent:
        case DeferKind::OutdoorPvPEvent: case DeferKind::WorldStateWrite: case DeferKind::DuelEnd:
        case DeferKind::DeferredSpellHit: case DeferKind::MapChange: case DeferKind::PoolEvent:
        case DeferKind::InstanceBind: case DeferKind::GameEventAction:
            return true;
        default: return false;
    }
}

struct DeferHeader { Map* map; DeferKind kind; uint16 size; uint32 sequence; };

class DeferBuffer   // append-only, resident, one per core (step 14); drained at the seal in sequence order per map
{
public:
    template <typename T> void Append(Map* map, DeferKind kind, T const& payload);
    void DrainInto(std::vector<DeferHeader const*>& out);   // all entries, then caller sorts by (map, sequence)
    void Reset();
};
```

Payloads hold `ObjectGuid`, cell/grid coordinates, ids, and counters. A payload that needs the object at apply time resolves it with `ObjectAccessor`/`Map::GetWorldObject` at the seal, where everything is live and no task runs. The one exception is `AddToMap`, where the object was just constructed on a task and is not yet findable: the payload carries `WorldObject* obj` **as the sole owner** until the seal inserts it. It is the only pointer in any defer.

## Writer sites

| Today | Line | Becomes |
| --- | --- | --- |
| `Map::PlayerRelocation` — `RemoveFromGrid`, `EnsureGridLoaded`, `AddToGrid`, `UpdateObjectVisibility(false)` | 804–823 | `player->Relocate(x, y, z, o)`; `UpdatePositionData()`; `RelocatePassengers` stays (passengers are in the same task group, step 12); if the cell changed: `Append(Relink{guid, oldCell, newCell})` and, if the grid changed, `Append(GridLoad{newCell})`. No visibility call (step 10 owns it) |
| `Map::CreatureRelocation`, `GameObjectRelocation`, `DynamicObjectRelocation` | 826–900 | `Relocate`, position data as today; `Append(Relink…)` instead of `AddCreatureToMoveList`. The three move lists and `MoveAll*InMoveList` are deleted |
| `Map::AddToMap(T*)` from a task (summon, `Creature::Respawn` from AI, `GameObject::SummonGameObject`, corpse create) | various | The constructor and `Create` run on the task (reads of the summoner through its own fields: the summoner is the owner). `Append(AddToMap{obj})`. At apply: `AddToGrid`, `_objectsStore.Insert`, `AddObjectToPendingUpdateList`, `obj->AddToWorld()`, `UpdateObjectVisibility` for the new object = publish its first slot and let viewers pick it up next beat. `TempSummon::InitSummon`, `SetTempSummonType`, `AI()->IsSummonedBy`, `OnSummoned` hooks run at apply, not on the task |
| `Map::AddObjectToRemoveList(obj)` → `obj->CleanupsBeforeDelete(false)` inline | 1823–1831 | From a unit task: `obj->PrepareRemoval()` (step 06: the foreign half as records) then `Append(RemoveFromMap{guid, prepared = true})`; apply runs `CleanupsBeforeDelete(prepared)` which touches own fields and the own grid list only, so it can run inside a **grid task** (step 12 S1a). From the seal itself (S1b, map unload): `Append`/apply with `prepared = false` runs today's two-sided body in S1b. `RemoveAllObjectsInRemoveList` body is the apply |
| `Map::ScriptsStart` → `m_scriptSchedule.insert` | `MapScripts.cpp` | `Append(ScriptSchedule{tableId, scriptId, sourceGuid, targetGuid, delay})`; `ScriptsProcess` runs in the map epilogue (step 12) and on this step where it runs today |
| `Map::MarkNearbyCellsOf(player)` | inside the player loop | player task computes the cell ids and `Append(CellMark{cellIds…})`; apply does `resetMarkedCells()` once per map per tick before applying marks, then sets bits. `Creature::IsUpdateNeeded` (`Creature.cpp:4004`) reads the bitset one tick late |
| `AddObjectToPendingUpdateList`, `_RemoveObjectFromUpdateList` | 583–605 | `Append(UpdateListAdd/Remove{guid})`; apply edits `_updatableObjectList`. `UpdateNonPlayerObjects` walks the vector without the swap-remove (the recheck branch appends `UpdateListRemove` instead of removing in place) |
| `DynamicMapTree::insert/remove` from `GameObject::SetLootState`, `EnableCollision`, `Spell` (ice walls) | `GameObjectModel`, `MapCollisionData` | `Append(DynTreeInsert/Remove{goGuid})`; apply calls `_dynamicTree.insert(*model)`. `DynamicMapTree::update` stays in the epilogue |
| `UpdatePlayerZoneStats` | `Map.cpp` | `Append(ZonePlayerCount{oldZone, newZone})` |
| `Group::EndRoll` from `Creature::Update` corpse branch, `Loot` roll from `CMSG_LOOT_ROLL` (THREADUNSAFE, already world thread) | `Creature.cpp` | `Append(GroupEndRoll{groupGuid, corpseGuid})`; the corpse's task takes `_lootRollClaimed.exchange(true)` before appending so two members' tasks do not both end the roll |
| `Kill` → group looter broadcast, `Battleground::HandleKillPlayer/HandleKillUnit`, `UpdatePlayerScore`, `EndBattleground` | `Unit.cpp:13596…`, `Battleground.cpp` | `Append(GroupBroadcast{…})`, `Append(BattlegroundEvent{bgInstance, kind, killerGuid, victimGuid, …})` |
| `OutdoorPvP::HandleKill`, capture point `Update` from a GO task | `OutdoorPvP*.cpp` | `Append(OutdoorPvPEvent{…})`; `OutdoorPvPMgr::Update` already runs on the world thread |
| `sWorldState->Set*`, zone scripts writing `WorldState` from a creature task | `WorldState.cpp` | `Append(WorldStateWrite{index, value, zoneId})` |
| `Player::TeleportTo` across maps from a task (spell effect, GM command in a task) | `Player.cpp` | `Append(MapChange{playerGuid, destMap, pos, options})`; apply does today's `TeleportTo` body on the world thread. Same-map teleports are a `Relink` plus `Relocate` |
| `Duel` end from `Kill` / `DuelComplete` | `Player.cpp` | `Append(DuelEnd{…})` — both players written |
| `Vehicle::AddPassenger` / `RemovePassenger` (from `Unit::EnterVehicle`, `ExitVehicle`, `Vehicle::RemoveAllPassengers`), `Unit::SetCharmedBy` / `RemoveCharmedBy` | `Vehicle.cpp`, `Unit.cpp` | Today's body runs inline (both units are written: legal on this step, and legal under step 12 once they are one group — the *first* join is the exception, see below). Additionally `Append(TaskGroupJoin{rootGuid, memberGuid})` / `Append(TaskGroupLeave{memberGuid})`. Apply at the seal sets `_taskRoot`/`_taskMembers` (step 12). **First join**: the boarding unit is not yet in the group, so `EnterVehicle` on the passenger's task may not write the vehicle. The passenger appends the defer only; the seal's apply does `vehicle->GetVehicleKit()->AddPassenger(passenger, seat)` (seat map, `RelocatePassengers`, `OnAddPassenger` hooks, control packets) with both units live. Same for `SetCharmedBy`: the caster's task appends; the seal applies the full body. Boarding and charm therefore take effect at the next seal (≤ 1 beat), the same latency as any record. Leaving is appended by whichever side initiates and applied at the seal the same way, after which both run as separate tasks again |
| `sPoolMgr->UpdatePool` from despawn, `SpawnPool` from `ProcessRespawns` (map seal task) | `Creature.cpp`, `GameObject.cpp`, `Map.cpp` | `Append(PoolEvent{poolId, guid, typeId, action})`; S2 applies. Spawns it creates are inserted live (S2, no task runs) and published as newcomers |
| `InstanceMap::PermBindAllPlayers`, `Player::BindToInstance` from a boss kill or script | `InstanceScript.cpp`, `Map.cpp` | `Append(InstanceBind{mapId, instanceId, permanent, playerGuids})`; S2 applies through `sInstanceSaveMgr` |
| `sGameEventMgr->StartEvent/StopEvent` from a script | `GameEventMgr.cpp` | `Append(GameEventAction{eventId, start})`; S2 applies |
| `Map::ProcessRespawns` → `DoRespawn` → `Creature::Create`/`GameObject::Create` + `AddToMap` | `Map.cpp` 467–476 and `MapRespawn` | `ProcessRespawns` (S1b, step 12) only pops due entries and `Append(Spawn{spawnId, type, gridId})` routed to the spawn's grid (position from `CreatureData`/`GameObjectData`). The grid item of the **next** beat's link round constructs the object and `AddToGrid`s it; store insert, `AddToWorld`, and hooks go through the collected map-side path like any `AddToMap`. Fifty respawns after a wipe are fifty parallel creations instead of 2 ms in one map item. A respawn appears one beat later than today |

## Apply

Two drains. Per-map kinds are applied by the map (in step 12: inside its S1 seal task); global kinds are applied by the coordinator in S2.

```cpp
void Map::ApplyDefers()                                                 // S1, one call per map
{
    std::vector<DeferHeader const*> all; DeferBuffer::DrainFor(this, all);   // step 14: every core's buffer, filtered by map and !IsGlobalDefer
    std::stable_sort(all.begin(), all.end(), by sequence);
    resetMarkedCellsIfAnyMark(all);
    for (DeferHeader const* d : all) switch (d->kind) { … }
}

void BeatPool::ApplyGlobalDefers()                                      // S2, once
{
    std::vector<DeferHeader const*> all; DeferBuffer::DrainGlobal(all);      // IsGlobalDefer only
    std::stable_sort(all.begin(), all.end(), by sequence);
    for (DeferHeader const* d : all) switch (d->kind) { … }
    for each core: defers.Reset();
}
```

On this step (no beat loop yet) `Map::Update` calls both back to back on the one thread. `DrainFor` is a filtered walk of resident buffers; with many maps it is O(maps × entries) — acceptable up to a few hundred maps, and if `seal_tail` shows it, the writer side appends into a per-map bucket index instead (`std::vector<uint32> offsets` per `Map*`, built at append) so each map drains only its own.

### Grid affinity of each kind (for step 12 S1a)

Step 12 splits `Map::ApplyDefers` into per-grid tasks (S1a, two rounds) and a per-map residue (S1b). Every per-map kind is tagged now so the split is a routing change:

| Kind | Grid-local? | Applied in |
| --- | --- | --- |
| `Relink` | touches `oldCell`'s grid (unlink) and `newCell`'s grid (link) | S1a: unlink round on the old grid, link round on the new grid. Same-grid relinks do both in the link round on that grid |
| `GridLoad` | creates one grid and fills it | S1a link round, on the new grid's task; store inserts collected for S1b; `_gridLock` serialises `CreateGrid` per parent |
| `AddToMap{obj}` | inserts into one grid; store/accessor/update-list are map-wide | S1a link round for `AddToGrid`; store insert, `HashMapHolder`, `AddToWorld`, summon hooks collected into the grid task's `_pendingMapSide` and run in S1b |
| `RemoveFromMap{prepared=true}` | own grid list; store erase map-wide | S1a unlink round for `RemoveFromGrid` (the own-half cleanup already ran on the unit's task, step 06); store erase and `_freeAtSeal` push in S1b |
| `Spawn` | one grid | S1a link round: construct, `AddToGrid`; map-side tail collected for S1b |
| `RemoveFromMap{prepared=false}` | two-sided | S1b only |
| `ScriptSchedule`, `CellMark`, `UpdateListAdd/Remove`, `DynTreeInsert/Remove`, `ZonePlayerCount`, `TaskGroupJoin/Leave` | map-wide, cheap, O(few) per beat | S1b |

Payloads already carry the cell coordinates (`Relink`) or can compute the grid from the object's live position at apply, so no payload changes.

Order matters in two places: `Relink` before `RemoveFromMap` for the same guid (a removed object is unlinked by `RemoveFromMap` anyway, so apply `RemoveFromMap` last for that guid), and `AddToMap` before any `Relink` of the new object. Sorting by sequence gives this because the task that created the object appended `AddToMap` first.

Two `Relink`s of one object in one tick collapse to the last one: `Relink` apply reads the object's **current** live position (the owner wrote it) and computes the cell from that, ignoring the payload's `newCell` except for grid preloading.

## Check

1. Walk across a cell boundary: the relink is applied at tick end; `.debug slot` shows `cellId` change one tick after the position change. Visibility of a creature in the new cell appears one tick later (step 10 makes it the same tick as the scan).
2. Summon a pet, a totem, a guardian: each appears next tick; `IsSummonedBy` hooks fire; `_objectsStore` size changes only at tick end (debug counter of inserts outside `ApplyDefers` is 0).
3. Kill a creature in a group with group loot: one roll; `EndRoll` runs once.
4. Wintergrasp or any zone that writes `WorldState`: values update; no write outside `ApplyDefers`.
5. Board a vehicle, exit it; mind-control a creature, release it: each takes effect at tick end (one tick later than today), `_taskRoot` on both sides is set and cleared at apply (visible in `.debug slot`), `OnAddPassenger`/`OnRemovePassenger` hooks fire once.
6. Despawn a pooled creature and wait for the pool respawn: `PoolEvent` applied at tick end, no direct `sPoolMgr` write (debug `ASSERT_NOT_IN_TASK` in `UpdatePool`/`SpawnPool` quiet).
7. `.server info` in band.

## Must not

- No `Unit*` in a defer except `AddToMap::obj`, and that object has not been published.
- No defer applied inside an object's `Update`.
- No swap-remove on `_updatableObjectList` from inside the walk.
- No `UpdateObjectVisibility` from `PlayerRelocation`.

## Checklist

- [ ] All three `*Relocation` functions append `Relink`; move lists deleted.
- [ ] `AddToMap` from a task is a defer; summon hooks fire at apply.
- [ ] `AddObjectToRemoveList` from a task is `PrepareRemoval` + `RemoveFromMap{prepared}`; the two-sided `CleanupsBeforeDelete` body runs only for unprepared removals at the seal.
- [ ] `MarkNearbyCellsOf`, script schedule, update-list edits, dyn-tree edits, zone counts, group roll, battleground, outdoor PvP, `WorldState`, cross-map teleport, duel end are defers.
- [ ] `TaskGroupJoin`/`TaskGroupLeave` from vehicle seating and charm; first join applies the full `AddPassenger`/`SetCharmedBy` body at the seal.
- [ ] `PoolEvent`, `InstanceBind`, `GameEventAction` global defers; `IsGlobalDefer` lists every cross-map kind.
- [ ] `Spawn` defer from `ProcessRespawns`; no object construction in `ProcessRespawns` itself.
- [ ] `Map::ApplyDefers` (per map) and `ApplyGlobalDefers` (once) run after the object updates, each in sequence order.
- [ ] Debug counters for store/grid/list edits outside `ApplyDefers` read 0 after a play session.
