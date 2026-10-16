# 04 — Config key, `BeatContext`, atomic GUIDs

Three small independent changes that every later step relies on. Nothing reads the key yet.

## Slice 1 — `MapUpdate.PhaseThreads`

Files: `WorldConfig.h`, `WorldConfig.cpp`, `worldserver.conf.dist`.

`CONFIG_MAPUPDATE_PHASE_THREADS` goes immediately above `MAX_NUM_SERVER_CONFIGS` (`WorldConfig.h:522`), below the step 01 key.

```cpp
SetConfigValue<uint32>(CONFIG_MAPUPDATE_PHASE_THREADS, "MapUpdate.PhaseThreads", 1, ConfigValueCache::Reloadable::No,
    [](uint32 const& v) { return v >= 1 && v <= 128; }, "1..128");
```

```
#    MapUpdate.PhaseThreads
#        Description: Cores that run unit tasks. 1 runs the beat loop on the
#                     world thread alone and starts no worker thread. Set it
#                     to the core count minus 1; the world thread is one of
#                     them. On a 14-core machine that is 13. Tasks come from
#                     every map in one process-wide pool.
#        Default:     1
#                     2..128 - (pool size, including the world thread)

MapUpdate.PhaseThreads = 1
```

Not reloadable: the pool is created once at startup (step 14).

## Slice 2 — `BeatContext`

New `src/server/game/Maps/BeatContext.h`. Included from `Object.h`. No dependency on `Map.h`.

```cpp
struct BeatContext
{
    // The object whose task is running on this thread. nullptr outside a task
    // (coordinator at the seal, world thread between beats, loading thread).
    static thread_local Object* owner;

    // Root of the task group the running task belongs to (step 12: a vehicle and its seated
    // passengers, a charmer and its charmed unit, run as one task). Equals owner for an ungrouped unit.
    static thread_local WorldObject* root;

    // The map whose seal task is running on this thread (step 12 seal phases S1a/S1b). nullptr otherwise.
    static thread_local Map* sealMap;
    // In an S1a grid task: the grid; MayAccess then allows only objects the router tagged for that grid. -1 in S1b.
    static thread_local int32 sealGrid;

    // True from pool wake to the end of the parallel seal phase. Set and cleared by the coordinator (step 12).
    static std::atomic<bool> inBeat;

    // Current beat number. Written by the coordinator before the wake, read with relaxed loads.
    static std::atomic<uint32> beat;

    static bool InTask() { return owner != nullptr; }
    static bool InSealTask() { return sealMap != nullptr; }
    static bool MayAccess(Object const* o)
    {
        // Outside a beat everything is single-threaded.
        if (!inBeat.load(std::memory_order_relaxed)) return true;
        // Unit phase: the owner, or any member of the owner's task group.
        if (owner) return owner == o || (o->IsWorldObject() && static_cast<WorldObject const*>(o)->TaskRoot() == root);
        // Seal phase S1a: a grid task may touch only the objects routed to its grid this beat.
        if (sealMap && sealGrid >= 0) return o->IsWorldObject() && static_cast<WorldObject const*>(o)->_pendingInGrid == sealGrid && static_cast<WorldObject const*>(o)->GetMap() == sealMap;
        // Seal phase S1b: a map seal task may touch anything on its own map (maps are disjoint, no unit task runs).
        if (sealMap) return !o->IsWorldObject() || static_cast<WorldObject const*>(o)->GetMap() == sealMap;
        return false;
    }
};

// RAII: sets owner (and root) for the duration of a task body or a group member's sub-update.
struct TaskScope
{
    Object* prevOwner; WorldObject* prevRoot;
    TaskScope(Object* o, WorldObject* r) : prevOwner(BeatContext::owner), prevRoot(BeatContext::root) { BeatContext::owner = o; BeatContext::root = r; }
    ~TaskScope() { BeatContext::owner = prevOwner; BeatContext::root = prevRoot; }
};

struct SealTaskScope
{
    explicit SealTaskScope(Map* m) { BeatContext::sealMap = m; BeatContext::sealGrid = -1; }
    ~SealTaskScope() { BeatContext::sealMap = nullptr; }
};
struct GridTaskScope
{
    GridTaskScope(Map* m, int32 g) { BeatContext::sealMap = m; BeatContext::sealGrid = g; }
    ~GridTaskScope() { BeatContext::sealMap = nullptr; BeatContext::sealGrid = -1; }
};
```

`WorldObject::TaskRoot()` returns `_taskRoot` (step 12; `this` for an ungrouped object). `inBeat` stays `false` until step 12 flips it, so `MayAccess` is `true` everywhere until then. Step 13 adds the `ASSERT`s that call it. Step 05 uses `BeatContext::beat` as the slot stamp.

A debug-only macro, defined here and used by later steps:

```cpp
#ifdef ACORE_DEBUG
#define ASSERT_OWNED(obj) ASSERT(BeatContext::MayAccess(obj), "foreign access to {} from task of {}", (obj)->GetGUID().ToString(), BeatContext::owner ? BeatContext::owner->GetGUID().ToString() : "none")
// For mutators of global singletons (step 03 table, step 13): legal between beats and in a map seal task, never in a unit task.
#define ASSERT_NOT_IN_TASK() ASSERT(!BeatContext::InTask(), "global mutation from task of {}", BeatContext::owner->GetGUID().ToString())
#else
#define ASSERT_OWNED(obj) ((void)0)
#define ASSERT_NOT_IN_TASK() ((void)0)
#endif
```

## Slice 3 — atomic GUID counters

Two counters are `_nextGuid++` with no lock and are taken from inside unit updates (item create in `Spell::DoCreateItem`, summons in `Map::GenerateLowGuid`).

`src/server/game/Entities/Object/ObjectGuid.h:287–316`:

```cpp
class ObjectGuidGeneratorBase
{
public:
    ObjectGuidGeneratorBase(ObjectGuid::LowType start = 1) : _nextGuid(start) { }
    virtual void Set(ObjectGuid::LowType val) { _nextGuid.store(val, std::memory_order_relaxed); }
    ObjectGuid::LowType GetNextAfterMaxUsed() const { return _nextGuid.load(std::memory_order_relaxed); }
protected:
    alignas(64) std::atomic<ObjectGuid::LowType> _nextGuid;
};

template<HighGuid high>
ObjectGuid::LowType ObjectGuidGenerator<high>::Generate()
{
    ObjectGuid::LowType v = _nextGuid.fetch_add(1, std::memory_order_relaxed);
    if (v >= ObjectGuid::GetMaxCounter(high) - 1)
        HandleCounterOverflow(high);   // existing fatal path
    return v;
}
```

`Map::GenerateLowGuid` uses the same generator type per map, so it becomes atomic by the same change. Mail IDs and pet numbers already take a mutex and are untouched.

## Checks

1. Boot with the key absent and with `MapUpdate.PhaseThreads = 1`. Login works. A value of 0 or 200 is rejected at startup with the validator message.
2. `BeatContext::owner` is `nullptr` everywhere; nothing else observes the header yet.
3. Create two items; summon two creatures. Distinct GUIDs. `.server info` unchanged.

## Checklist

- [x] Key above `MAX_NUM_SERVER_CONFIGS`, default 1, validator 1..128, not reloadable, dist comment names cores − 1 and 13.
- [x] `BeatContext.h` has no `Map.h` include; `inBeat` defaults false.
- [x] `_nextGuid` atomic on its own cache line; `Set`/`GetNextAfterMaxUsed` relaxed.
- [x] No other file changed.
