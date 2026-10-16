# 01 — Phase clock inside today's `Map::Update`

Baseline measurement of the single-threaded tick before anything is moved. This clock lives only until step 12 retires `Map::Update`; it is deleted there. It is still worth landing first because steps 05–11 change the single-thread path (slot copy, record apply, slot-built packets) and this clock is the only way to see whether those steps made today's tick slower.

No gameplay change. No new lock. One config key.

## Files

- `src/server/game/World/WorldConfig.h` — add `CONFIG_MAPUPDATE_PHASE_TIMING` immediately above `MAX_NUM_SERVER_CONFIGS` (line 522). Values above the insertion point are array indexes in `ConfigValueCache`; do not insert beside `CONFIG_NUMTHREADS`.
- `src/server/game/World/WorldConfig.cpp` — next to line 575:

```cpp
SetConfigValue<bool>(CONFIG_MAPUPDATE_PHASE_TIMING, "MapUpdate.PhaseTiming", false, ConfigValueCache::Reloadable::Yes);
```

- `src/server/apps/worldserver/worldserver.conf.dist` — under the `MapUpdate.Threads` block:

```
#    MapUpdate.PhaseTiming
#        Description: Record per-phase durations inside Map::Update and log a
#                     summary for slow maps. 0 leaves Map::Update unchanged.
#        Default:     0 - (Off)
#                     1 - (On, measurement only)

MapUpdate.PhaseTiming = 0
```

- New `src/server/game/Maps/MapPhaseTiming.h` and `.cpp`. Included from `Map.h` only.
- `src/server/game/Maps/Map.h` — one value member `MapPhaseTiming _phaseTiming;`, in-class initialised, not in the constructor initialiser list, no heap, no mutex.
- `src/server/game/Maps/Map.cpp` — `Map::Update` (line 436): the local clock and the `nextPhase` calls, nothing else.

## Types

```cpp
enum class MapUpdatePhase : uint8
{
    DynamicTree = 0, Sessions, Respawn, Players, NonPlayers,
    ObjectUpdates, Scripts, Movement, Visibility, Tail, Count
};

struct MapPhaseTiming
{
    uint32 lastMs[uint8(MapUpdatePhase::Count)] = {};
    uint32 windowSumMs[uint8(MapUpdatePhase::Count)] = {};
    uint32 windowSamples = 0;
    uint32 lastLogMs = 0;

    void Add(MapUpdatePhase phase, uint32 ms);
    void MaybeLog(Map const& map, uint32 wallMs);   // defined in the .cpp; the only function that logs
};

class MapPhaseClock
{
public:
    MapPhaseClock(MapPhaseTiming* sink, Map const& map);
    void Next(MapUpdatePhase phase);   // closes the open phase into _tickMs, opens the new one
    void Abandon();                    // destructor then does nothing
    ~MapPhaseClock();                  // Add() every phase incl. the open one, then MaybeLog()
private:
    MapPhaseTiming* _sink;
    Map const* _map;
    MapUpdatePhase _phase;
    bool _open = false;
    bool _abandoned = false;
    uint32 _tickMs[uint8(MapUpdatePhase::Count)] = {};
    std::chrono::steady_clock::time_point _mark, _wallStart;
};
```

`Next` on a null sink returns before `steady_clock::now()`.

## Wiring in `Map::Update`

At the top of the function:

```cpp
std::optional<MapPhaseClock> phases;
if (sWorld->getBoolConfig(CONFIG_MAPUPDATE_PHASE_TIMING))
    phases.emplace(&_phaseTiming, *this);
auto nextPhase = [&](MapUpdatePhase p) { if (phases) phases->Next(p); };
```

Call `nextPhase` immediately before each existing statement. Do not reorder or wrap anything.

| Phase | Immediately before (today's line) |
| --- | --- |
| `DynamicTree` | `_mapCollisionData.GetDynamicTree().update(t_diff)` inside `if (t_diff)` (438) |
| `Sessions` | first `for (m_mapRefIter ...)` loop (442) |
| `Respawn` | `Events.Update(t_diff)` (458) |
| `Players` | second `for (m_mapRefIter ...)` loop (482). `MarkNearbyCellsOf` stays inside this phase |
| `NonPlayers` | `UpdateNonPlayerObjects(t_diff)` (506) |
| `ObjectUpdates` | `SendObjectUpdates()` (508) |
| `Scripts` | `if (!m_scriptSchedule.empty())` (511) |
| `Movement` | `MoveAllCreaturesInMoveList()` (518) |
| `Visibility` | `HandleDelayedVisibility()` (522) |
| `Tail` | `UpdatePlayersRedirectKickEvent(t_diff)` (524) |

The `if (!t_diff)` branch (460) calls `HandleDelayedVisibility()` and returns. Insert `if (phases) phases->Abandon();` before that `return`. A zero-diff tick is never logged.

## `MaybeLog` rules

- `wallMs < 20` → return, no sample.
- Fewer than 5000 ms since `lastLogMs` → accumulate, return.
- Otherwise accumulate, log one line, reset the window.

```
map phase timing: map={} inst={} players={} updatable={} wall={} dyn={} sess={} rsp={} plr={} npc={} upd={} scr={} mov={} vis={} tail={}
```

Each phase value is `windowSumMs[phase] / windowSamples`. `players` is `m_mapRefMgr.getSize()`, `updatable` is `_updatableObjectList.size()`, both read const. If the ten phases differ from `wall` by more than 1 ms, log a second line `map phase timing mismatch: map={} wall={} sum={}`. Do not `ASSERT`.

## Checks

1. Key absent and key = 0: boot, login, `.server info` unchanged. No `map phase timing` line. `git diff` on `Map.cpp` is the local clock, the `nextPhase` calls, and one `Abandon`.
2. Key = 1, empty world: no line (maps are under 20 ms). Lower the floor locally to see one well-formed line, then restore it.
3. Key = 1 on any crowd: the ten numbers sum to `wall` within 1 ms.

## Must not

- No change to update order, lists, visits, targets, or movement.
- No clock inside the object loops.
- No `METRIC_TIMER` (compiled out under `WITHOUT_METRICS`).
- The key stays 0 in any config used for play.

## Checklist

- [x] Config key above `MAX_NUM_SERVER_CONFIGS`, default `false` in both places, reloadable.
- [x] `MapPhaseTiming.h` included from `Map.h` only.
- [x] `Map::Update` statements unchanged and in the same order.
- [x] Zero-diff path calls `Abandon`.
- [x] Log format string exact.
- [ ] Baseline numbers for an empty server and for whatever crowd is available are written into this file's exit note before step 05 starts.

## Exit note

(empty-server and crowd baselines go here)
