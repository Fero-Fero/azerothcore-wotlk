# 02 — Shared geometry safe for `MapUpdate.Threads > 1`

The vmap tree (`StaticMapTree`) and the navmesh (`dtNavMesh`) are shared by every instance of one map id. Today's `MapUpdater` runs instance maps concurrently when `MapUpdate.Threads > 1`, and a child map creating a grid loads tiles into those shared objects while a sibling instance is pathing or checking line of sight. This step closes that race.

These locks are **scaffolding for steps 02–11**, while today's `MapUpdater` still runs instance maps on their own threads. Step 12 retires `MapUpdater` and makes tile installation seal-only, and step 14 **deletes the read-side locks and counters**: a `shared_lock` or `fetch_add` on the line-of-sight and pathing path is an RMW on a cache line shared by every core, and under the pool it would make `sum_task` grow with the key. To keep that deletion a small diff, every read-side lock in this step goes through one macro and one RAII type, and no caller names the mutex directly:

```cpp
// MapTree.h — step 14 redefines this to a debug-only assert
#define GEOMETRY_READ_GUARD(m) std::shared_lock<std::shared_mutex> _geometryGuard(m)
```

`NavMeshQueryScope` (slice 2) is the navmesh equivalent. What survives step 14 unchanged is the structural work: `QueryLocation` copy-out, `SharedNavMesh` as the one holder per map id, the unlocked `MapGridManager` helpers, and `_gridLock` on `CreateGrid`.

Line of sight, height, and path answers do not change.

## The race

`MapGridManager::CreateGrid` holds `_gridLock` (a `std::mutex`, `MapGridManager.h:58`) across `LoadTerrain`. A child map calls `parent->EnsureGridCreated`; the parent (instance id 0) calls `LoadVMapTile` and `LoadMMapTile`, which write the one `StaticMapTree` and the one `dtNavMesh` for that map id. `Map::isInLineOfSight`, `getHeight`, `GetLocationInfo`, and `PathGenerator` read those objects without taking `_gridLock`.

`WorldModel::IntersectRay` is `const` and models come from `WorldModelStore::AcquireModelInstance`, already locked. No change there.

## Lock order

1. `MapGridManager::_gridLock` (grid create).
2. `StaticMapTree::_mutex` exclusive (`LoadMapTile`, `UnloadMapTile`, `UnloadMap`, `InitMap`), taken inside `LoadTerrain`, which is inside 1.
3. `SharedNavMesh::_mutex` exclusive (`MMapMgr::LoadTile` on the shared object), also inside 1.

Readers take 2 or 3 shared and never take 1. Nothing takes 1 while holding 2 or 3. None of these are recursive.

## Slice 1 — `std::shared_mutex` in `StaticMapTree`

Files: `src/common/Collision/Maps/MapTree.h`, `MapTree.cpp`, `src/server/game/Maps/MapCollisionData.cpp`.

- Add `mutable std::shared_mutex _mutex;` to `StaticMapTree`.
- `std::unique_lock` in `LoadMapTile`, `UnloadMapTile`, `UnloadMap`, `InitMap`.
- `GEOMETRY_READ_GUARD(_mutex)` in the public readers `isInLineOfSight`, `GetObjectHitPos`, `getHeight`, `GetLocationInfo`. `GetIntersectionTime` stays unlocked; it is only called from those.
- `GetLocationInfo` fills a `LocationInfo` holding `hitModel` / `hitInstance` that `StaticVMapCollisionData::GetAreaAndLiquidData` uses after return. Add `StaticMapTree::QueryLocation(...)` returning a plain struct copied out under the shared lock: hit flag, ground Z, liquid type, liquid level, liquid present, WMO id, ADT id, root id, MOGP flags, model instance id. `GetAreaAndLiquidData` calls it and keeps no pointer. `GetLocationInfo` stays as the locked wrapper for callers that only need the bool.
- `GetModelInstances` returns interior pointers. No server caller is added.
- `src/tools/mmaps_generator` keeps using `LoadMapTile` on its own tree; the mutex is uncontended there. No special case.

## Slice 2 — `SharedNavMesh`

New `src/common/Collision/Management/SharedNavMesh.h`:

```cpp
class SharedNavMesh
{
public:
    explicit SharedNavMesh(std::shared_ptr<dtNavMesh> mesh);

    // Read side: bump the in-flight count, return the mesh, caller pairs with LeaveQuery().
    dtNavMesh const* EnterQuery() const;   // _queries.fetch_add(1, acq_rel); return _mesh.get();
    void LeaveQuery() const;               // _queries.fetch_sub(1, release)

    // Write side: exclusive lock, then spin/yield until _queries == 0, then fn(dtNavMesh*).
    template <typename F> auto Write(F&& fn);

    dtNavMesh const* UnsafeGet() const { return _mesh.get(); }   // seal-only use in step 14

private:
    std::shared_ptr<dtNavMesh> _mesh;
    mutable std::shared_mutex _mutex;
    mutable std::atomic<uint32> _queries{0};
};

struct NavMeshQueryScope          // RAII around EnterQuery/LeaveQuery
{
    SharedNavMesh const& mesh; dtNavMesh const* ptr;
    explicit NavMeshQueryScope(SharedNavMesh const& m) : mesh(m), ptr(m.EnterQuery()) {}
    ~NavMeshQueryScope() { mesh.LeaveQuery(); }
};
```

`Write` acquires `_mutex` exclusively first so no new `EnterQuery` can start a long path after the writer arrived? No: `EnterQuery` does not take `_mutex`, by design, so a path never touches the mutex cache line. The writer waits on the count. A reader that arrives while the writer is waiting still enters, and the writer waits for it too. Tile loads happen on grid creation, which is rare, so the writer starving is not a practical problem; if it ever is, add a `_writerWaiting` flag that `EnterQuery` spins on.

The header does not include `Map.h`. No config flag; the count is always on.

## Slice 3 — server callers

Files: `MMapMgr.h/.cpp`, `MapCollisionData.h/.cpp`, `PathGenerator.h/.cpp`, `cs_mmaps.cpp`, `cs_misc.cpp`.

- `MMapMgr::LoadTile(dtNavMesh*, ...)` is unchanged (the generator links it). Add `LoadTile(SharedNavMesh&, uint32 mapId, int32 x, int32 y)` that calls `Write` and then the raw overload.
- `MMapData::_navMesh` (`MapCollisionData.h:67`) becomes `std::shared_ptr<SharedNavMesh>`. Children copy the parent's pointer in the `MapCollisionData` constructor, so there is one mutex per map id.
- `GetNavMesh()` (`MapCollisionData.h:62`) is removed. Add `SharedNavMesh* GetSharedNavMesh()`. Every game caller is migrated in this slice.
- `PathGenerator` stores `SharedNavMesh*` (`PathGenerator.h:148`). Each public method that touches the mesh or `_navMeshQuery` opens a `NavMeshQueryScope` for its body and passes `scope.ptr` to the private helpers. `HaveTile` and the other private methods assume the scope is open. The constructor does not enter.
- `dtNavMeshQuery` (`ManagedNavMeshQuery _navMeshQuery`, `MapCollisionData.h:69`) stays one per map until step 14. It is created inside a `NavMeshQueryScope`.
- GM commands inspect the mesh inside a scope and drop the pointer before returning.

## Slice 4 — grid slot publication

Files: `src/server/game/Grids/MapGridManager.h/.cpp`.

- `_gridLock` becomes `mutable`. Still `std::mutex`, not recursive.
- Add private unlocked helpers `GetGridUnlocked`, `IsGridCreatedUnlocked`, `IsGridLoadedUnlocked` with today's bodies. Public `GetGrid`, `IsGridCreated`, `IsGridLoaded` lock, call the helper, unlock.
- `CreateGrid` already holds the lock and must call the unlocked helpers. The `unique_ptr` assignment stays after `LoadTerrain`.
- `LoadGrid`: hold `_gridLock` only to read the slot and set the loaded flag; release before `LoadAllCellsInGrid` (a spawn inside can re-enter `LoadGrid`; the already-loaded early return handles it). `UnloadGrid` likewise releases before walking cells.
- `_createdGridsCount`, `_loadedGridsCount` are read under the lock.

This does not make `Cell::VisitObjects` safe against a grid that is loading. That race is closed by step 12 (grids load only at the seal).

## Checks

1. After slice 1: single-thread login, a doorway blocks, a road passes, `.server info` in band.
2. After slice 3: no remaining game caller of raw `GetNavMesh()`. `src/tools/` has no diff. One scripted path has the same point count as before.
3. After slice 4: two Deadmines instances created a few seconds apart, one zoning while the other is in combat, for 15 minutes with `MapUpdate.Threads = 2`. No crash, no double-load log from `LoadMapTile`.

## Must not

- No preload-all flag, no per-instance mesh copy, no mutex on `WorldModel` or the dynamic tree.
- No second `dtNavMeshQuery` per map (step 14).
- No edit under `src/tools/`.
- No off switch for the locks before step 14, and no caller that names `_mutex` or `_queries` outside the guard macro and `NavMeshQueryScope`.

## Checklist

- [x] `StaticMapTree` readers shared, writers exclusive, `GetIntersectionTime` unlocked.
- [x] `QueryLocation` copies plain fields; `GetAreaAndLiquidData` keeps no model pointer.
- [x] `EnterQuery`/`LeaveQuery` pair around every `PathGenerator` public method and every LOS/height call that reaches the mesh. No raw `dtNavMesh*` stored past `LeaveQuery`.
- [x] `Write` waits for `_queries == 0`.
- [x] One `SharedNavMesh` per map id; children share the parent's pointer.
- [x] `_gridLock` mutable, helpers unlocked, `CreateGrid` uses helpers, `LoadGrid`/`UnloadGrid` release before walking cells.
- [x] Lock order grid → tree → navmesh, nothing takes grid second.
