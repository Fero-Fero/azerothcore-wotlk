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

#include "BeatPool.h"
#include "BeatContext.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectSlot.h"
#include "Player.h"
#include "UnitRecordLog.h"
#include "WorldSession.h"
#include <algorithm>
#include <bit>
#include <chrono>

namespace
{
    uint64 NowNs()
    {
        return uint64(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    constexpr uint64 LOG_BEAT_OVER_MS = 20;
    constexpr uint64 LOG_EVERY_MS = 5000;
    constexpr std::size_t SLOW_TASKS_KEPT = 10;
}

BeatPool* BeatPool::instance()
{
    static BeatPool instance;
    return &instance;
}

void BeatPool::Init(uint8 cores)
{
    _cores.clear();
    _cores.resize(std::max<uint8>(cores, 1));
    _slowTasks.reserve(SLOW_TASKS_KEPT + 1);
    _slowTasksLast.reserve(SLOW_TASKS_KEPT + 1);
}

uint32 BeatPool::Beat() const
{
    return BeatContext::beat.load(std::memory_order_relaxed);
}

void BeatPool::Schedule(WorldObject* root)
{
    if (_cores.empty())
        return;

    uint32 const target = Beat() + 1;
    uint32 expected = root->_scheduledBeat.load(std::memory_order_relaxed);
    for (;;)
    {
        if (expected == target)
            return;
        if (root->_scheduledBeat.compare_exchange_weak(expected, target, std::memory_order_relaxed))
            break;
    }

    _cores[std::min<std::size_t>(BeatContext::core, _cores.size() - 1)].next.push(root);
}

void BeatPool::Wake(WorldObject* target)
{
    Schedule(target->TaskRoot());
}

void BeatPool::Unschedule(WorldObject* obj)
{
    for (CoreState& core : _cores)
    {
        core.current.erase(obj);
        core.next.erase(obj);
        core.heavy.erase(obj);
        // Called from ~WorldObject: compare as the base, the derived part is already gone.
        for (Player*& item : core.viewItems)
            if (item && static_cast<WorldObject*>(item) == obj)
                item = nullptr;
    }
    obj->_scheduledBeat.store(0, std::memory_order_relaxed);
}

void BeatPool::SeedMap(Map* map)
{
    for (MapReference const& ref : map->GetPlayers())
        if (Player* player = ref.GetSource())
            Schedule(player->TaskRoot());

    for (WorldObject* obj : map->_updatableObjectList)
        Schedule(obj->TaskRoot());
    for (WorldObject* obj : map->_pendingAddUpdatableObjectList)
        Schedule(obj->TaskRoot());
}

void BeatPool::CollectMaps(std::vector<Map*>& out)
{
    out.clear();
    sMapMgr->CollectAllMaps(out);
}

// ---- the beat ----

void BeatPool::RunBeat(uint32 diff)
{
    uint64 const t0 = NowNs();
    _clock.wall = _lastBeatStart ? t0 - _lastBeatStart : 0;
    _lastBeatStart = t0;
    _inRunBeat = true;

    // The beat advances first: handles resolved under the old number are never reused inside this one, and
    // the new beat's write logs are reset while last beat's stay readable for the inboxes applied in U1.
    uint32 const beat = BeatContext::beat.fetch_add(1, std::memory_order_relaxed) + 1;
    sRecordLogs->BeginBeat(beat);

    CollectMaps(_maps);
    for (Map* map : _maps)
        if (!map->_beatSeeded)
        {
            map->_beatSeeded = true;
            SeedMap(map);
        }

    BeatContext::inBeat.store(true, std::memory_order_release);

    // U1 — unit tasks
    for (CoreState& core : _cores)
    {
        std::swap(core.current, core.next);
        core.next.clear();
    }
    RunPhase(Phase::Unit, diff, _clock.task);

    // U2 — view items, reading this beat's slots
    RunPhase(Phase::View, diff, _clock.view);

    // S1b — one seal task per map
    for (std::size_t i = 0; i < _maps.size(); ++i)
        _cores[i % _cores.size()].sealItems.push_back(_maps[i]);
    RunPhase(Phase::MapSeal, diff, _clock.sealMap);

    BeatContext::inBeat.store(false, std::memory_order_release);

    // S2 — the coordinator alone
    SealSerial(diff);

    _inRunBeat = false;
}

void BeatPool::RunPhase(Phase phase, uint32 diff, uint64& wall)
{
    uint64 const t0 = NowNs();
    for (CoreState& core : _cores)
        core.completed = 0;

    // Step 14: wake the workers here; at one core the coordinator drains every ring itself.
    for (CoreState& core : _cores)
        WorkerLoop(core, phase, diff);

    wall = NowNs() - t0;
}

WorldObject* BeatPool::PopHeavy()
{
    for (CoreState& core : _cores)
        if (WorldObject* root = core.heavy.pop())
            return root;
    return nullptr;
}

void BeatPool::WorkerLoop(CoreState& me, Phase phase, uint32 diff)
{
    switch (phase)
    {
        case Phase::Unit:
            for (;;)
            {
                // Heavy first: every core's heavy ring before anything else (longest-processing-time first).
                WorldObject* root = PopHeavy();
                if (!root)
                    root = me.current.pop();
                if (!root)
                    break;
                TaskBody(root, diff, me);
                ++me.completed;
            }
            me.current.clear();
            break;
        case Phase::View:
            for (Player* viewer : me.viewItems)
                if (viewer)
                {
                    ViewBody(viewer, me);
                    ++me.completed;
                }
            break;
        case Phase::MapSeal:
            for (Map* map : me.sealItems)
            {
                MapSealTask(map, diff, me);
                ++me.completed;
            }
            me.sealItems.clear();
            break;
    }
}

// ---- U1 ----

void BeatPool::TaskBody(WorldObject* root, uint32 diff, CoreState& me)
{
    // The ring held it since last beat; it may have been grouped under another root, or left the world.
    root->_scheduledBeat.store(0, std::memory_order_relaxed);
    if (!root->IsTaskRoot() || !root->IsInWorld())
        return;

    uint64 const t0 = NowNs();

    // Inbox for the whole group first, so a member's records are visible to the root's update and vice versa.
    {
        TaskScope scope(root, root);
        root->ApplyInbox();
    }
    for (WorldObject* member : root->_taskMembers)
    {
        TaskScope scope(member, root);
        member->ApplyInbox();
    }
    uint64 const t1 = NowNs();

    // Vehicle / charmer first: it moves the members.
    UpdateOne(root, root, diff);
    // A member can leave the group inside the update (RemoveFromWorld dissolves it); walk a copy.
    thread_local std::vector<WorldObject*> members;
    members = root->_taskMembers;
    for (WorldObject* member : members)
        if (member->TaskRoot() == root)
            UpdateOne(member, root, diff);
    uint64 const t2 = NowNs();

    if (root->IsInWorld())
    {
        TaskScope scope(root, root);
        root->PublishSlot();
    }
    for (WorldObject* member : root->_taskMembers)
        if (member->IsInWorld())
        {
            TaskScope scope(member, root);
            member->PublishSlot();
        }
    uint64 const t3 = NowNs();

    // View work (scan + packet) is phase U2, so it reads this beat's slots.
    if (Player* player = root->ToPlayer())
        if (player->IsInWorld())
            QueueView(player, me);
    for (WorldObject* member : root->_taskMembers)
        if (Player* player = member->ToPlayer())
            if (player->IsInWorld())
                QueueView(player, me);

    uint32 const ns = uint32(std::min<uint64>(t3 - t0, 0xFFFFFFFFu));
    root->_lastTaskNs = ns;
    bool const heavy = _heavyThresholdNs && ns > _heavyThresholdNs;

    // Republish
    if (root->IsInWorld() && !root->IsRemoving())
    {
        if (root->IsDueNextBeat())
        {
            uint32 const target = Beat() + 1;
            uint32 expected = root->_scheduledBeat.load(std::memory_order_relaxed);
            if (expected != target && root->_scheduledBeat.compare_exchange_strong(expected, target, std::memory_order_relaxed))
                (heavy ? me.heavy : me.next).push(root);
        }
        else if (!root->IsPlayer())
        {
            // Dormant: leave the map's updatable list so a viewer's scan can re-add (and reschedule) it.
            TaskScope scope(root, root);
            if (Map* map = root->FindMap())
                map->RemoveObjectFromMapUpdateList(root);
        }
    }

    // Clock
    me.tInbox += t1 - t0;
    me.tUpdate += t2 - t1;
    me.tPublish += t3 - t2;
    me.tTask += t3 - t0;
    me.tailTask = std::max<uint64>(me.tailTask, t3 - t0);
    ++me.tasks;
    ++me.histogram[std::min<uint32>(ns ? 32 - std::countl_zero(ns) : 0, 63)];

    SlowTask slow;
    slow.guid = root->GetGUID();
    slow.entry = root->GetEntry();
    slow.mapId = root->GetMapId();
    slow.ns = ns;
    slow.inboxNs = uint32(t1 - t0);
    slow.updateNs = uint32(t2 - t1);
    slow.publishNs = uint32(t3 - t2);
    slow.heavy = heavy;
    RecordSlowTask(slow);
}

void BeatPool::UpdateOne(WorldObject* obj, WorldObject* root, uint32 diff)
{
    TaskScope scope(obj, root);

    if (Player* player = obj->ToPlayer())
    {
        if (!player->IsInWorld())
            return;

        // Map-side packets (PROCESS_THREADSAFE and late PROCESS_INPLACE) and the player's update run together.
        if (WorldSession* session = player->GetSession())
        {
            MapSessionFilter filter(session);
            session->Update(diff, filter);
        }

        if (!player->IsInWorld())
            return;

        player->Update(diff);

        // Cells around the player (and its viewpoint) stay marked so nearby creatures remain due.
        if (Map* map = player->FindMap())
        {
            map->MarkNearbyCellsOf(player);
            if (WorldObject* viewPoint = player->GetViewpoint())
                if (viewPoint->IsCreature() || viewPoint->IsDynamicObject())
                    map->MarkNearbyCellsOf(viewPoint);
        }
    }
    else if (obj->IsInWorld())
        obj->Update(diff);
}

void BeatPool::QueueView(Player* player, CoreState& me)
{
    me.viewItems.push_back(player);
}

// ---- U2 ----

void BeatPool::ViewBody(Player* viewer, CoreState& me)
{
    if (!viewer->IsInWorld())
        return;

    uint64 const t0 = NowNs();

    // Grids a scanning viewer may reach are loaded outside its task (grid loads write the map).
    Map* map = viewer->FindMap();
    if (map && viewer->View().scanDue)
        map->LoadGridsInRange(*viewer, MAX_VISIBILITY_DISTANCE);

    {
        TaskScope scope(viewer, viewer->TaskRoot());
        viewer->RunViewPhase();
    }

    uint64 const dt = NowNs() - t0;
    me.tView += dt;
    me.tailView = std::max<uint64>(me.tailView, dt);
}

// ---- S1b ----

void BeatPool::MapSealTask(Map* map, uint32 diff, CoreState& me)
{
    uint64 const t0 = NowNs();

    SealTaskScope scope(map);
    map->SealApply(diff);
    map->BeatEpilogue(diff);

    me.tailSeal = std::max<uint64>(me.tailSeal, NowNs() - t0);
}

// ---- S2 ----

void BeatPool::SealSerial(uint32 diff)
{
    uint64 const t0 = NowNs();

    // Cross-map defers (group, battleground, pool, instance bind, map change, ...): after every map sealed.
    uint32 globalDefers = 0;
    for (Map* map : _maps)
        globalDefers += map->ApplyGlobalDefers();

    // Instances that emptied: unload and free between any two tasks, never inside a phase.
    sMapMgr->DestroyExpiredInstances(diff);

    // Slot indices of everything that left the world this beat: no record can address them any more.
    for (uint32 index : _slotIndicesToReturn)
        if (index != ObjectSlotPair::NO_SLOT_INDEX)
            sSlotIndexAllocator->Release(index);
    _slotIndicesToReturn.clear();

    for (CoreState& core : _cores)
        core.viewItems.clear();

    // Heavy threshold for the next beat: 8x the median task of this one.
    if (uint32 median = MedianTaskNs())
        _heavyThresholdNs = median * 8;

    BeatContext::FlushForeignReads();

    _clock.sealSer = NowNs() - t0;
    _clock.globalDefers = globalDefers;
    MaybeLogClock(diff);
    ResetClock();
}

// ---- clock ----

void BeatPool::RecordSlowTask(SlowTask const& task)
{
    if (_slowTasks.size() >= SLOW_TASKS_KEPT && task.ns <= _slowTasks.back().ns)
        return;

    auto pos = std::upper_bound(_slowTasks.begin(), _slowTasks.end(), task,
        [](SlowTask const& a, SlowTask const& b) { return a.ns > b.ns; });
    _slowTasks.insert(pos, task);
    if (_slowTasks.size() > SLOW_TASKS_KEPT)
        _slowTasks.pop_back();
}

uint32 BeatPool::MedianTaskNs() const
{
    uint64 total = 0;
    std::array<uint64, 64> buckets{};
    for (CoreState const& core : _cores)
        for (std::size_t i = 0; i < buckets.size(); ++i)
        {
            buckets[i] += core.histogram[i];
            total += core.histogram[i];
        }

    if (!total)
        return 0;

    uint64 seen = 0;
    for (std::size_t i = 0; i < buckets.size(); ++i)
    {
        seen += buckets[i];
        if (seen * 2 >= total)
            return i ? (1u << std::min<std::size_t>(i, 31)) : 1;
    }
    return 0;
}

void BeatPool::ResetClock()
{
    _lastClock = _clock;
    for (CoreState& core : _cores)
    {
        _lastClock.sumTask += core.tTask;
        _lastClock.inbox += core.tInbox;
        _lastClock.update += core.tUpdate;
        _lastClock.publish += core.tPublish;
        _lastClock.tasks += core.tasks;
        _lastClock.tail = std::max(_lastClock.tail, core.tailTask);
        _lastClock.viewTail = std::max(_lastClock.viewTail, core.tailView);
        _lastClock.sealTail = std::max(_lastClock.sealTail, core.tailSeal);

        core.tTask = core.tInbox = core.tUpdate = core.tPublish = core.tView = 0;
        core.tailTask = core.tailView = core.tailSeal = 0;
        core.tasks = 0;
        core.histogram.fill(0);
    }
    _slowTasksLast.swap(_slowTasks);
    _slowTasks.clear();
    _clock = PoolClock();
}

void BeatPool::MaybeLogClock(uint32 /*diff*/)
{
    PoolClock c = _clock;
    uint64 sumTask = 0, tail = 0, viewTail = 0, sealTail = 0, inbox = 0, update = 0, publish = 0;
    uint32 tasks = 0;
    for (CoreState const& core : _cores)
    {
        sumTask += core.tTask;
        inbox += core.tInbox;
        update += core.tUpdate;
        publish += core.tPublish;
        tasks += core.tasks;
        tail = std::max(tail, core.tailTask);
        viewTail = std::max(viewTail, core.tailView);
        sealTail = std::max(sealTail, core.tailSeal);
    }

    uint64 const wallMs = c.wall / 1000000;
    if (wallMs < LOG_BEAT_OVER_MS)
        return;

    uint64 const nowMs = uint64(GameTime::GetGameTimeMS().count());
    if (_lastLogMs && nowMs - _lastLogMs < LOG_EVERY_MS)
        return;
    _lastLogMs = nowMs;

    auto us = [](uint64 ns) { return ns / 1000; };
    uint64 const coord = c.grid + c.sealMap + c.sealSer;
    LOG_INFO("maps.pool", "pool timing: cores={} tasks={} wall={}us sum_task={}us task={}us idle=0 tail={}us view={}us view_tail={}us "
        "grid={}us grid_tail={}us seal_map={}us seal_tail={}us seal_ser={}us coord={}us claim=0 inbox={}us update={}us publish={}us global_defers={}",
        _cores.size(), tasks, us(c.wall), us(sumTask), us(c.task), us(tail), us(c.view), us(viewTail),
        us(c.grid), us(c.gridTail), us(c.sealMap), us(sealTail), us(c.sealSer), us(coord), us(inbox), us(update), us(publish), c.globalDefers);
}
