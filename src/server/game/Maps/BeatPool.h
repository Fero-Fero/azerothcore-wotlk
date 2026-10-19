/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ACORE_BEAT_POOL_H
#define ACORE_BEAT_POOL_H

#include "Define.h"
#include "MMapMgr.h"
#include "ObjectGuid.h"
#include <array>
#include <atomic>
#include <memory>
#include <semaphore>
#include <thread>
#include <unordered_map>
#include <vector>

class Map;
class Player;
class SharedNavMesh;
class WorldObject;
class dtNavMesh;
class dtNavMeshQuery;

// Step 12: one process-wide beat replaces Map::Update and the map rotation. A beat is U1 (one task per due task
// root: inbox, update, publish), U2 (one view item per in-world player, chunked for a crowded one), S1b (one
// seal task per map: defers, transports, epilogue) and S2 (the coordinator alone: global defers, resets, frees).
// Step 14: `MapUpdate.PhaseThreads` cores run every phase but S2; the world thread is core 0 and runs the
// same worker loop as the others. Nothing about what a task does changes with the core count.

// The 16-byte tagged value a core pops. ptr is a WorldObject* (U1 root), Player* (U2 viewer) or Map* (S1b,
// and the step 17 crowd item, chunk = cell id).
struct WorkItem
{
    enum Kind : uint8 { None, Unit, View, MapSeal, Crowd };

    void* ptr = nullptr;
    uint32 chunk = 0;
    Kind kind = None;
    uint8 pad[3]{};

    // Object items store the WorldObject subobject's address so ~WorldObject can match them (Unschedule).
    static WorkItem UnitTask(WorldObject* root) { WorkItem w; w.ptr = root; w.kind = Unit; return w; }
    static WorkItem ViewTask(WorldObject* viewer, uint32 chunk) { WorkItem w; w.ptr = viewer; w.chunk = chunk; w.kind = View; return w; }
    static WorkItem SealTask(Map* map) { WorkItem w; w.ptr = map; w.kind = MapSeal; return w; }
    static WorkItem CrowdTask(Map* map, uint32 cellId) { WorkItem w; w.ptr = map; w.chunk = cellId; w.kind = Crowd; return w; }

    explicit operator bool() const { return kind != None; }
};
static_assert(sizeof(WorkItem) == 16, "WorkItem is a 16-byte value");

// Chase–Lev deque over a fixed array (Lê, Pop, Cohen, Nardelli, PPoPP 2013; fences as published). The owner
// pops the bottom, thieves steal the top. Every phase's items are complete before the phase starts (U1 items
// come from last beat's `next`, U2 from U1, S1b from the map list), so the array is filled by the owner in
// Fill() and never pushed to while a thief can run; the only concurrent operations are pop and steal. Items
// are plain values that no one writes during the phase.
class WorkDeque
{
public:
    // Owner, before the phase: takes the items (src is left empty with its capacity), top = 0, bottom = n.
    void Fill(std::vector<WorkItem>& src);
    // Owner only.
    WorkItem pop();
    // Any core. Returns WorkItem{} when the deque is empty (a lost race is retried internally).
    WorkItem steal();
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t capacity() const { return _items.size(); }

private:
    std::vector<WorkItem> _items;
    alignas(64) std::atomic<int64> _top{0};
    alignas(64) std::atomic<int64> _bottom{0};
};

struct SlowTask
{
    ObjectGuid guid;
    uint32 entry = 0;
    uint32 mapId = 0;
    uint32 ns = 0;
    uint32 inboxNs = 0;
    uint32 updateNs = 0;
    uint32 publishNs = 0;
    uint32 pathNs = 0;
    bool heavy = false;
};

struct PoolClock
{
    uint64 wall = 0;        // RunBeat entry to the next RunBeat entry
    uint64 sumTask = 0;     // Σ task bodies (U1)
    uint64 task = 0;        // U1 phase wall
    uint64 tail = 0;        // longest U1 item
    uint64 idle = 0;        // Σ per-core idle inside the parallel phases / cores
    uint64 view = 0;        // U2 phase wall
    uint64 viewTail = 0;    // longest U2 item (one chunk of the most crowded viewer)
    uint64 grid = 0;        // S1a (no grid items yet: plan 12 left S1a open)
    uint64 gridTail = 0;
    uint64 sealMap = 0;     // S1b phase wall
    uint64 sealTail = 0;    // busiest map
    uint64 sealSer = 0;     // S2
    uint64 barrier = 0;     // Σ time between the second-to-last core parking and the completion sum matching
    uint64 inbox = 0;
    uint64 update = 0;
    uint64 publish = 0;
    uint64 path = 0;        // Σ PathGenerator time inside U1 tasks
    uint32 tasks = 0;
    uint32 globalDefers = 0;
    uint32 claimsLost = 0;  // duplicate handles dropped by the claim CAS
    uint32 wakeDups = 0;    // Wake calls whose _scheduledBeat CAS found the root already scheduled
    uint32 pathIncomplete = 0;
    uint32 crowdCells = 0;  // step 17: crowd items run this beat
    uint32 crowdBuilt = 0;  // public blocks the crowd items built
    uint32 crowdCopied = 0; // viewer appends served from a crowd block instead of a build
};

class BeatPool
{
public:
    static BeatPool* instance();

    // Sizes the per-core state. Called once from MapMgr::Initialize before any map exists.
    void Init(uint8 cores);
    // Starts cores − 1 worker threads. Called once after the world loaded; Stop joins them at shutdown.
    void Start();
    void Stop();
    [[nodiscard]] bool Started() const { return !_threads.empty(); }
    [[nodiscard]] uint8 Cores() const { return uint8(_cores.size()); }

    // One beat: U1, U2, S1b, S2. Called by MapMgr::Update when the interval elapsed.
    void RunBeat(uint32 diff);

    // Puts a task root onto the calling core's next list for the next beat, once per beat (CAS on
    // WorldObject::_scheduledBeat). Called by the republish rule, the wake (a record to a target), the seal for
    // newcomers, and AddPlayerToMap.
    void Schedule(WorldObject* root);
    void Wake(WorldObject* target);
    // Called from ~WorldObject: an object freed while it sits in a list. Legal between beats and in S2 only.
    void Unschedule(WorldObject* obj);
    // An object removed inside a beat: freed by the coordinator in S2 (Map::DeleteFromWorld).
    void FreeAtSealSerial(WorldObject* obj);
    // A slot index of an object that left the world this beat: returned to the allocator in S2.
    void ReturnSlotIndex(uint32 index);

    // A map that joined between beats: its players and its updatable list get their first tasks.
    void SeedMap(Map* map);

    // Per-worker Detour query for the running core (dtNavMeshQuery is not thread-safe; tiles install only in
    // seal tasks, when no unit task runs, so the mesh pointer is read unguarded).
    dtNavMeshQuery const* NavQueryFor(SharedNavMesh const& mesh);
    // Map unload (S2 or between beats): every core forgets its query on that mesh.
    void ForgetNavMesh(SharedNavMesh const& mesh);
    // PathGenerator adds its time here; TaskBody splits it out of the task's duration.
    static void AddPathNs(uint64 ns) { _pathNs += ns; }
    static void CountPathIncomplete() { ++_pathIncomplete; }
    // Step 17: Player::ViewBuild served one append from a crowd block; ViewBody folds it into the core.
    static void CountCrowdCopy() { ++_crowdCopied; }

    // Step 16: resident capacities of one core's buffers, for `.debug memory`.
    struct CoreMemory
    {
        uint8 core = 0;
        std::size_t current = 0, heavy = 0;             // deque arrays (items)
        std::size_t next = 0, heavyNext = 0;            // next-beat lists (items)
        std::size_t viewItems = 0, sealItems = 0;       // phase lists (items)
        std::size_t crowdItems = 0;                     // step 17 crowd items planned in S2 (items)
        std::size_t freeList = 0, slotReturns = 0;      // S2 lists (entries)
        std::size_t navQueries = 0;                     // dtNavMeshQuery objects held (one per mesh touched)
        std::size_t slowTasks = 0;
    };
    [[nodiscard]] std::vector<CoreMemory> Memory() const;

    [[nodiscard]] uint32 HeavyThresholdNs() const { return _heavyThresholdNs; }
    [[nodiscard]] uint32 Beat() const;
    [[nodiscard]] PoolClock const& LastClock() const { return _lastClock; }
    [[nodiscard]] std::vector<SlowTask> const& LastSlowTasks() const { return _slowTasksLast; }
    [[nodiscard]] bool InBeat() const { return _inRunBeat; }

private:
    BeatPool() = default;

    enum class Phase : uint8 { Unit, View, MapSeal };

    // Owner (step 16 table): BeatPool::_cores, one per core, allocated in Init and alive until the process
    // exits (objects freed during MapMgr::UnloadAll still return slot indices through it after Stop). Every
    // vector below is cleared, never shrunk, so each sits at its high-water; the deques take their arrays from
    // the lists at phase start (Fill swaps, no allocation once both sides reached their high-water). Workers
    // first-touch their own state in WorkerMain. `.debug memory` prints the capacities.
    struct CoreState
    {
        uint8 index = 0;

        WorkDeque current;                        // this phase's items
        WorkDeque heavy;                          // U1: this beat's heavy roots; U2: this beat's crowd items
        std::vector<WorkItem> next;               // U1 roots for the next beat, pushed by this core only
        std::vector<WorkItem> heavyNext;
        std::vector<WorkItem> crowdItems;         // step 17: U2 crowd items planned by the coordinator in S2
        std::vector<WorkItem> viewItems;          // U2 items produced by this core's U1 tasks
        std::vector<WorkItem> sealItems;          // S1b items
        std::vector<CoreState*> stealOrder;       // local NUMA node first, then remote, each in a random rotation

        std::unordered_map<dtNavMesh const*, MMAP::ManagedNavMeshQuery> navQueries;
        std::vector<SlowTask> slowTasks;          // ten largest of the running beat on this core
        std::vector<uint32> slotIndicesToReturn;
        std::vector<WorldObject*> freeList;

        uint64 tTask = 0, tInbox = 0, tUpdate = 0, tPublish = 0, tPath = 0, tView = 0, tIdle = 0;
        uint64 tailTask = 0, tailView = 0, tailSeal = 0;
        uint64 lastTaskEnd = 0;
        uint64 parkedAt = 0;
        uint32 tasks = 0;
        uint32 heavyLeft = 0;                     // this beat's heavy items not yet consumed, as seen by this core
        uint32 claimsLost = 0, wakeDups = 0, pathIncomplete = 0;
        uint32 crowdCells = 0, crowdBuilt = 0, crowdCopied = 0;
        std::array<uint32, 64> histogram{};       // log2(ns) buckets of this beat's task durations

        // Written by this core only; each on its own line so the completion sum reads cold lines, not bouncing ones.
        alignas(64) std::atomic<uint32> completed{0};
        alignas(64) std::atomic<uint32> dropped{0};
    };

    static CoreState& Current();

    void WorkerMain(uint32 idx);
    void BuildStealOrders();
    void ApplyAffinity(uint32 idx);

    void RunPhase(Phase phase, uint32 diff, uint64& wall);
    void WorkerLoop(CoreState& me);
    void RunItem(WorkItem const& item, CoreState& me);
    WorkItem PopHeavy(CoreState& me);
    WorkItem Steal(CoreState& me);
    void WaitCompletion(uint64& barrierNs);

    void TaskBody(WorldObject* root, uint32 diff, CoreState& me);
    void UpdateOne(WorldObject* obj, WorldObject* root, uint32 diff);
    void QueueView(Player* player, CoreState& me);
    void ViewBody(Player* viewer, uint32 chunk, CoreState& me);
    void CrowdBody(Map* map, uint32 cellId, CoreState& me);
    void PreloadViewGrids();
    void PlanCrowdItems();
    void MapSealTask(Map* map, uint32 diff, CoreState& me);
    void SealSerial(uint32 diff);

    void CollectMaps(std::vector<Map*>& out);
    void RecordSlowTask(CoreState& me, SlowTask const& task);
    uint32 MedianTaskNs() const;
    void ResetClock();
    void MaybeLogClock(uint32 diff);

    std::vector<std::unique_ptr<CoreState>> _cores;
    std::vector<std::thread> _threads;
    std::vector<uint32> _affinity;                // MapUpdate.PhaseAffinity: CPU per core, empty = scheduler
    std::vector<Map*> _maps;                      // this beat's maps, collected before U1
    std::vector<SlowTask> _slowTasksLast;
    PoolClock _clock;
    PoolClock _lastClock;
    uint64 _lastBeatStart = 0;
    uint64 _lastLogMs = 0;
    uint32 _heavyThresholdNs = 0;
    uint32 _heavyThisBeat = 0;
    bool _inRunBeat = false;

    // Phase handoff: written by the coordinator before the wake, read by the workers after it.
    Phase _phase = Phase::Unit;
    uint32 _diff = 0;
    uint32 _published = 0;
    std::atomic<bool> _stop{false};
    std::counting_semaphore<> _beatSem{0};
    std::counting_semaphore<> _completionSem{0};

    static inline thread_local uint64 _pathNs = 0;
    static inline thread_local uint32 _pathIncomplete = 0;
    static inline thread_local uint32 _crowdCopied = 0;
    std::vector<uint32> _crowdCells;              // S2 scratch: a map's crowded cells

    friend class WorldObject;
};

#define sBeatPool BeatPool::instance()

#endif
