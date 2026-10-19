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
#include "ObjectGuid.h"
#include <array>
#include <atomic>
#include <vector>

class Map;
class Player;
class WorldObject;

// Step 12: one process-wide beat replaces Map::Update and the map rotation. A beat is U1 (one task per due task
// root: inbox, update, publish), U2 (one view item per in-world player: scan, build, send), S1b (one seal task
// per map: defers, transports, epilogue) and S2 (the coordinator alone: global defers, resets, frees). At one
// core the world thread runs every item itself; step 14 adds workers to the same loop.

// A plain ring of work items. Pushed by the owning core, popped from the front by the worker loop.
template <typename T>
struct WorkRing
{
    std::vector<T> items;
    uint32 head = 0;

    void push(T item) { items.push_back(item); }
    // Next live item; erased slots (T{}) are skipped. T{} when drained.
    T pop()
    {
        while (head < items.size())
            if (T item = items[head++])
                return item;
        return T{};
    }
    [[nodiscard]] uint32 size() const { return uint32(items.size()) - head; }
    [[nodiscard]] bool empty() const { return head >= items.size(); }
    void clear() { items.clear(); head = 0; }
    // Removes every occurrence (an object freed while scheduled).
    void erase(T item)
    {
        for (uint32 i = head; i < items.size(); ++i)
            if (items[i] == item)
                items[i] = T{};
    }
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
    bool heavy = false;
};

struct PoolClock
{
    uint64 wall = 0;        // RunBeat entry to the next RunBeat entry
    uint64 sumTask = 0;     // Σ task bodies (U1)
    uint64 task = 0;        // U1 phase wall
    uint64 tail = 0;        // longest U1 item
    uint64 view = 0;        // U2 phase wall
    uint64 viewTail = 0;
    uint64 grid = 0;        // S1a (no grid items on this step)
    uint64 gridTail = 0;
    uint64 sealMap = 0;     // S1b phase wall
    uint64 sealTail = 0;
    uint64 sealSer = 0;     // S2
    uint64 inbox = 0;
    uint64 update = 0;
    uint64 publish = 0;
    uint32 tasks = 0;
    uint32 globalDefers = 0;
};

class BeatPool
{
public:
    static BeatPool* instance();

    void Init(uint8 cores);

    // One beat: U1, U2, S1b, S2. Called by MapMgr::Update when the interval elapsed.
    void RunBeat(uint32 diff);

    // Puts a task root onto the calling core's next ring for the next beat, once per beat (CAS on
    // WorldObject::_scheduledBeat). Called by the republish rule, the wake (a record to a target), the seal for
    // newcomers, and AddPlayerToMap.
    void Schedule(WorldObject* root);
    void Wake(WorldObject* target);
    // Called from ~WorldObject: an object freed while it sits in a ring.
    void Unschedule(WorldObject* obj);

    // A map that joined between beats: its players and its updatable list get their first tasks.
    void SeedMap(Map* map);

    [[nodiscard]] uint32 HeavyThresholdNs() const { return _heavyThresholdNs; }
    [[nodiscard]] uint32 Beat() const;
    [[nodiscard]] PoolClock const& LastClock() const { return _lastClock; }
    [[nodiscard]] std::vector<SlowTask> const& LastSlowTasks() const { return _slowTasksLast; }
    [[nodiscard]] bool InBeat() const { return _inRunBeat; }

private:
    BeatPool() = default;

    enum class Phase : uint8 { Unit, View, MapSeal };

    struct CoreState
    {
        WorkRing<WorldObject*> current;
        WorkRing<WorldObject*> next;
        WorkRing<WorldObject*> heavy;
        std::vector<Player*> viewItems;           // U2 items produced by this core's U1 tasks
        std::vector<Map*> sealItems;              // S1b items
        uint32 completed = 0;
        uint64 tTask = 0, tInbox = 0, tUpdate = 0, tPublish = 0, tView = 0;
        uint64 tailTask = 0, tailView = 0, tailSeal = 0;
        uint32 tasks = 0;
        std::array<uint32, 64> histogram{};       // log2(ns) buckets of this beat's task durations
    };

    void RunPhase(Phase phase, uint32 diff, uint64& wall);
    void WorkerLoop(CoreState& me, Phase phase, uint32 diff);
    WorldObject* PopHeavy();

    void TaskBody(WorldObject* root, uint32 diff, CoreState& me);
    void UpdateOne(WorldObject* obj, WorldObject* root, uint32 diff);
    void QueueView(Player* player, CoreState& me);
    void ViewBody(Player* viewer, CoreState& me);
    void MapSealTask(Map* map, uint32 diff, CoreState& me);
    void SealSerial(uint32 diff);

    void CollectMaps(std::vector<Map*>& out);
    void RecordSlowTask(SlowTask const& task);
    uint32 MedianTaskNs() const;
    void ResetClock();
    void MaybeLogClock(uint32 diff);

    std::vector<CoreState> _cores;
    std::vector<Map*> _maps;                      // this beat's maps, collected before U1
    std::vector<uint32> _slotIndicesToReturn;     // released in S2 (WorldObject::RemoveFromWorld)
    std::vector<SlowTask> _slowTasks;             // ten largest of the running beat
    std::vector<SlowTask> _slowTasksLast;
    PoolClock _clock;
    PoolClock _lastClock;
    uint64 _lastBeatStart = 0;
    uint64 _lastLogMs = 0;
    uint32 _heavyThresholdNs = 0;
    bool _inRunBeat = false;

    friend class WorldObject;
};

#define sBeatPool BeatPool::instance()

#endif
