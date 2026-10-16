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

#ifndef ACORE_BEAT_CONTEXT_H
#define ACORE_BEAT_CONTEXT_H

#include "Define.h"
#include <atomic>

class Object;
class WorldObject;
class Map;

// Included from Object.h. No Map.h dependency: sealMap is only ever compared as a pointer.
// MayAccess lives in BeatContext.cpp because it reads WorldObject members.
struct BeatContext
{
    // The object whose task is running on this thread. nullptr outside a task
    // (coordinator at the seal, world thread between beats, loading thread).
    static inline thread_local Object* owner = nullptr;

    // Root of the task group the running task belongs to (step 12: a vehicle and its seated
    // passengers, a charmer and its charmed unit, run as one task). Equals owner for an ungrouped unit.
    static inline thread_local WorldObject* root = nullptr;

    // The map whose seal task is running on this thread (step 12 seal phases S1a/S1b). nullptr otherwise.
    static inline thread_local Map* sealMap = nullptr;
    // In an S1a grid task: the grid. MayAccess then allows only objects the router tagged for that grid. -1 in S1b.
    static inline thread_local int32 sealGrid = -1;

    // True from pool wake to the end of the parallel seal phase. Set and cleared by the coordinator (step 12).
    static inline std::atomic<bool> inBeat{false};

    // Current beat number. Written by the coordinator before the wake, read with relaxed loads.
    // Starts at 1 so the first beat's record heads (initialised to ~0u) never match it.
    static inline std::atomic<uint32> beat{1};

    // Index of the core this thread runs on: 0 for the world thread, 1.. for map update workers.
    // Selects the record log a task appends to and the bit it sets in a target's writtenBy mask.
    static inline thread_local uint8 core = 0;

    // Foreign live reads seen by this thread (debug builds only). Flushed into foreignReadsTotal at the end
    // of every map update so .debug foreignreads sees every thread's count.
    static inline thread_local uint64 foreignReads = 0;
    static inline std::atomic<uint64> foreignReadsTotal{0};

    static bool InTask() { return owner != nullptr; }
    static bool InSealTask() { return sealMap != nullptr; }

    // True when the running thread may read or write o's live fields. Always true outside a beat.
    static bool MayAccess(Object const* o);

    // True inside a task when o is in the world and outside the owner's task group: a write to it must be a
    // record. False outside a task (world thread between beats, seal) where everything is written inline.
    static bool IsForeign(WorldObject const* o);

    // Debug-only foreign-read counter. Excludes the owner's charmer/charmed unit and vehicle/passengers,
    // which step 12 runs in the owner's task group and step 13 does not audit.
    static void CountForeignRead(Object const* o);
    static void FlushForeignReads();
};

// RAII: sets owner (and root) for the duration of a task body or a group member's sub-update.
struct TaskScope
{
    Object* prevOwner;
    WorldObject* prevRoot;

    TaskScope(Object* o, WorldObject* r) : prevOwner(BeatContext::owner), prevRoot(BeatContext::root)
    {
        BeatContext::owner = o;
        BeatContext::root = r;
    }

    ~TaskScope()
    {
        BeatContext::owner = prevOwner;
        BeatContext::root = prevRoot;
    }
};

struct SealTaskScope
{
    explicit SealTaskScope(Map* m)
    {
        BeatContext::sealMap = m;
        BeatContext::sealGrid = -1;
    }

    ~SealTaskScope()
    {
        BeatContext::sealMap = nullptr;
    }
};

struct GridTaskScope
{
    GridTaskScope(Map* m, int32 g)
    {
        BeatContext::sealMap = m;
        BeatContext::sealGrid = g;
    }

    ~GridTaskScope()
    {
        BeatContext::sealMap = nullptr;
        BeatContext::sealGrid = -1;
    }
};

#ifdef ACORE_DEBUG
#define ASSERT_OWNED(obj) ASSERT(BeatContext::MayAccess(obj), "foreign access to {} from task of {}", \
    (obj)->GetGUID().ToString(), BeatContext::owner ? BeatContext::owner->GetGUID().ToString() : "none")
// Legal between beats and in a map seal task, never inside a unit task (step 03, step 13).
#define ASSERT_NOT_IN_TASK() ASSERT(!BeatContext::InTask(), "global mutation from task of {}", \
    BeatContext::owner->GetGUID().ToString())
#else
#define ASSERT_OWNED(obj) ((void)0)
#define ASSERT_NOT_IN_TASK() ((void)0)
#endif

#endif
