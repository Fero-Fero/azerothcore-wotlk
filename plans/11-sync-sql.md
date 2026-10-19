# 11 — Synchronous SQL off task paths

A task that blocks in `Query` or `DirectExecute` parks a core for the round trip, and `wall` grows by it. `Execute`, `CommitTransaction`, `AsyncQuery`, `AsyncCommitTransaction`, and `DelayQueryHolder` enqueue and return. `Query` and `DirectExecute` take a sync connection and wait.

No schema change. No file under `data/sql/`.

## What already enqueues (leave as is)

| Call | API |
| --- | --- |
| `Player::SaveToDB` (`PlayerStorage.cpp:7238`), `UpdateAdditionalSaves` | `CommitTransaction` |
| `Pet::SavePetToDB` | `CommitTransaction` |
| Mute expiry in `Player::Update` | `LoginDatabase.Execute` |
| `InstanceScript::SaveToDB` | `CharacterDatabase.Execute` |
| Creature and gameobject respawn times | `CharacterDatabase.Execute` |

Building `SaveToDB`'s transaction is CPU on the task and shows under `update`. If it grows faster than the rest of `update` with player count, move the statement build to a defer and let a map epilogue task enqueue it. Not in this step.

## What blocks on a task path

Only one site: `WorldSession::recoveryItem` → `CharacterDatabase.Query(CHAR_INS_RECOVERY_ITEM)` (an insert whose result is discarded), reached from `HandleDestroyItemOpcode` and `HandleSellItemOpcode` (`ItemDelete.Vendor`). Both are `PROCESS_INPLACE`; a packet still queued when the player's task runs `session->Update` handles it on the task. Gated by `ItemDelete.Method` and quality/level thresholds; both keys default to 0.

Everything else that blocks runs on the world thread between beats or at load and stays: `Map::LoadRespawnTimes`/`LoadCorpseData` from `CreateBaseMap`/`CreateInstance` (`MSG_MOVE_WORLDPORT_ACK` is `PROCESS_THREADUNSAFE`), `Guild::AddMember`, `CMSG_MESSAGECHAT` commands, `PoolGroup<Quest>::SpawnObject`, mail expiry, `CharacterCache::RefreshCacheEntry`, startup loads.

## Slice 1 — `recoveryItem` enqueues

Files: `src/server/database/Database/Implementation/CharacterDatabase.cpp`, `src/server/game/Handlers/ItemHandler.cpp`.

```cpp
PrepareStatement(CHAR_INS_RECOVERY_ITEM,
    "INSERT INTO recovery_item (Guid, ItemEntry, Count, DeleteDate) VALUES (?, ?, ?, UNIX_TIMESTAMP())",
    CONNECTION_ASYNC);
```

`recoveryItem` calls `CharacterDatabase.Execute(stmt)` and returns `true` as before. The statement already copies the owner counter, entry, and count; it holds no `Item*`.

## Slice 2 — preload creates the map off the worker

Files: `World.cpp`, `MapUpdater.cpp`.

`MapPreloadRequest::call` (`MapUpdater.cpp:65–71`) calls `sMapMgr->CreateBaseMap(_mapId)` on a `MapUpdater` worker, which queries respawns and corpses. Change the preload loop in `World` to call `CreateBaseMap` on the loading thread first, then `schedule_map_preload(mapId)`; `call` fetches the existing map with `sMapMgr->FindBaseMap` and only runs `LoadAllGrids`. If the map is missing, log and return.

## Slice 3 — guard

Debug-only: `DatabaseWorkerPool::GetFreeConnection` already logs a stack when `WarnAboutSyncQueries` is on. Add `ASSERT(!BeatContext::InTask())` in debug builds next to it. From step 12 on, any sync query reached from a task is a crash in debug, not a slow beat.

## Checks

1. `ItemDelete.Method = 0`: destroy and sell still remove the item. `Method = 1`, destroy a rare at item level ≥ 80: a `recovery_item` row appears; the handler did not wait. Sell writes a row only with `Vendor = 1` too.
2. `PreloadAllNonInstancedMapGrids = 1`, `MapUpdate.Threads > 0`: boot reaches world-ready with continent grids resident; `MapPreloadRequest::call` has no `CreateBaseMap`.

## Checklist

- [x] `CHAR_INS_RECOVERY_ITEM` is `CONNECTION_ASYNC`; `recoveryItem` uses `Execute`.
- [x] Preload: `CreateBaseMap` on the loading thread, `LoadAllGrids` on the worker.
- [x] Debug `ASSERT(!InTask())` in `GetFreeConnection`.
- [x] No `data/sql` change.

## Status

Not built since these edits (the user builds). `codestyle-cpp.py` passes. Not played.

### Done

- `CharacterDatabase.cpp`: `CHAR_INS_RECOVERY_ITEM` → `CONNECTION_ASYNC`. `WorldSession::recoveryItem` enqueues with
  `CharacterDatabase.Execute(stmt)` and returns `true` as before.
- `World.cpp` preload loop: `CreateBaseMap` on the loading thread for every non-instanceable map (skipped on
  `nullptr`), then `schedule_map_preload` when the updater is active, `LoadAllGrids` inline otherwise.
  `MapPreloadRequest::call` fetches the map with `FindBaseMap` and only loads the grids; logs an error and
  returns if the map is missing.
- Guard: the database library cannot see `BeatContext`, so `DatabaseWorkerPool` gained a debug-only static
  predicate `SetSyncQueryForbiddenGuard(bool (*)())`; `GetFreeConnection` asserts it is unset or false
  (`"Synchronous query on a unit task path"`), next to the existing `WarnAboutSyncQueries` stack dump.
  `World::SetInitialWorldSettings` installs `[]() { return BeatContext::InTask(); }` on the three pools right after
  `sMapMgr->Initialize()`, so the startup loads before it are not guarded.

### Open

- `ModuleDatabasePool::GetFreeConnection` has no guard (modules' own pools).
- The predicate fires for any sync query under a `TaskScope`, including session packets handled in the player's
  task (`Map::Update` sessions phase); in a debug build that is the intended crash, and the sites it finds are
  this step's follow-ups.
