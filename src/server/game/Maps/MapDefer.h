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

#ifndef ACORE_MAP_DEFER_H
#define ACORE_MAP_DEFER_H

#include "BeatContext.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <type_traits>
#include <vector>

class Map;
class WorldObject;

// Step 09: a write to a map or global container does not run inside a unit's update. It is appended as a
// plain-value defer and applied at the end of Map::Update (from step 12: at the seal). Payloads hold guids,
// packed cells, ids and counters. The one pointer is AddToMap::obj: the object is not yet findable.

enum class DeferKind : uint8
{
    Relink = 0,            // object moved to another cell
    GridLoad,              // EnsureGridLoaded(cell)
    AddToMap,              // summon / respawn / corpse created on a task
    RemoveFromMap,         // AddObjectToRemoveList equivalent
    ScriptSchedule,        // Map::ScriptsStart
    CellMark,              // MarkNearbyCellsOf
    UpdateListAdd,
    UpdateListRemove,
    DynTreeInsert,
    DynTreeRemove,
    ZonePlayerCount,       // UpdatePlayerZoneStats
    GroupEndRoll,          // Group::EndRoll from a corpse
    GroupBroadcast,        // group looter / loot method packets from Kill (payload reserved)
    BattlegroundEvent,     // scores, HandleKillPlayer/Unit
    OutdoorPvPEvent,       // HandleKill
    WorldStateWrite,       // sWorldState set/update (payload reserved; remaining row)
    DuelEnd,
    DeferredSpellHit,      // step 07
    MapChange,             // teleport / transport leaving the map
    TaskGroupJoin,         // vehicle seating and charm (step 12)
    TaskGroupLeave,
    PoolEvent,
    InstanceBind,
    GameEventAction,
    Spawn                  // due respawn: construct + AddToGrid next beat (step 12 S1a)
};

[[nodiscard]] constexpr bool IsGlobalDefer(DeferKind k)
{
    switch (k)
    {
        case DeferKind::GroupEndRoll:
        case DeferKind::GroupBroadcast:
        case DeferKind::BattlegroundEvent:
        case DeferKind::OutdoorPvPEvent:
        case DeferKind::WorldStateWrite:
        case DeferKind::DuelEnd:
        case DeferKind::DeferredSpellHit:
        case DeferKind::MapChange:
        case DeferKind::PoolEvent:
        case DeferKind::InstanceBind:
        case DeferKind::GameEventAction:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] char const* DeferKindName(DeferKind kind);

struct DeferHeader
{
    Map* map = nullptr;
    uint32 sequence = 0;
    uint16 size = 0;
    DeferKind kind = DeferKind::Relink;
    uint8 pad = 0;
};

static constexpr std::size_t DEFER_HEADER_SIZE = sizeof(DeferHeader);
static_assert(DEFER_HEADER_SIZE % 8 == 0, "DeferHeader is 8-aligned so payloads stay aligned");

inline constexpr std::size_t DeferAlignUp8(std::size_t n)
{
    return (n + 7u) & ~std::size_t(7);
}

template <typename T>
[[nodiscard]] T const& DeferPayload(uint8 const* payload)
{
    static_assert(std::is_trivially_copyable_v<T>, "defer payloads are plain values");
    return *reinterpret_cast<T const*>(payload);
}

struct RelinkDefer
{
    ObjectGuid guid;
    uint32 oldCell = 0;     // Cell::data.All
    uint32 newCell = 0;
};

struct GridLoadDefer
{
    uint32 cell = 0;        // Cell::data.All
    uint8 objects = 1;      // 1: EnsureGridLoaded (terrain + objects); 0: EnsureGridCreated (terrain only)
};

struct AddToMapDefer
{
    WorldObject* obj = nullptr;     // sole owner until the seal inserts it; the only pointer in any defer
    uint8 checkTransport = 0;
};

struct RemoveFromMapDefer
{
    ObjectGuid guid;
    uint8 prepared = 1;
};

struct ScriptScheduleDefer
{
    void const* scripts = nullptr;  // ScriptMapMap const*; table is immortal. If set, apply calls ScriptsStart.
    void const* script = nullptr;   // ScriptInfo const*; one ScriptCommandStart. Mutually exclusive with scripts.
    uint32 id = 0;
    uint32 delay = 0;
    ObjectGuid source;
    ObjectGuid target;
};

constexpr uint8 MAX_CELL_MARKS = 64;    // 5x5 cells around a player, with room for a viewpoint

struct CellMarkDefer
{
    uint16 count = 0;
    uint32 cellIds[MAX_CELL_MARKS] = {};
};

struct UpdateListDefer
{
    ObjectGuid guid;
};

struct DynTreeDefer
{
    ObjectGuid goGuid;
};

struct ZonePlayerCountDefer
{
    uint32 oldZone = 0;
    uint32 newZone = 0;
};

struct GroupEndRollDefer
{
    ObjectGuid groupGuid;
    ObjectGuid corpseGuid;
};

struct BattlegroundEventDefer
{
    uint32 bgInstanceId = 0;
    uint8 kind = 0;                 // 0 = HandleKillPlayer, 1 = HandleKillUnit
    ObjectGuid killer;
    ObjectGuid victim;
};

struct OutdoorPvPEventDefer
{
    ObjectGuid killer;
    ObjectGuid victim;
};

struct DuelEndDefer
{
    ObjectGuid player;
    ObjectGuid opponent;
    uint8 type = 0;                 // DuelCompleteType
};

struct MapChangeDefer
{
    ObjectGuid player;
    uint32 mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float o = 0.0f;
    uint32 options = 0;
    ObjectGuid target;
    uint8 newInstance = 0;
};

struct TaskGroupJoinDefer
{
    ObjectGuid root;
    ObjectGuid member;
    int8 seatId = -1;
    uint8 charm = 0;                // 0 = vehicle, 1 = charm
};

struct TaskGroupLeaveDefer
{
    ObjectGuid member;
    uint8 charm = 0;
};

struct PoolEventDefer
{
    uint32 poolId = 0;
    ObjectGuid::LowType spawnId = 0;
    uint8 typeId = 0;               // TYPEID_UNIT / TYPEID_GAMEOBJECT
};

struct InstanceBindDefer
{
    uint32 mapId = 0;
    uint32 instanceId = 0;
    uint8 permanent = 0;
    ObjectGuid player;
};

struct GameEventActionDefer
{
    uint16 eventId = 0;
    uint8 start = 0;
    uint8 overwrite = 0;
};

struct SpawnDefer
{
    ObjectGuid::LowType spawnId = 0;
    uint8 type = 0;                 // SpawnObjectType
};

static_assert(std::is_trivially_copyable_v<RelinkDefer>, "defer payloads are plain values");
static_assert(std::is_trivially_copyable_v<GridLoadDefer>, "defer payloads are plain values");
static_assert(std::is_trivially_copyable_v<AddToMapDefer>, "defer payloads are plain values");
static_assert(std::is_trivially_copyable_v<RemoveFromMapDefer>, "defer payloads are plain values");
static_assert(std::is_trivially_copyable_v<ScriptScheduleDefer>, "defer payloads are plain values");
static_assert(std::is_trivially_copyable_v<CellMarkDefer>, "defer payloads are plain values");
static_assert(std::is_trivially_copyable_v<UpdateListDefer>, "defer payloads are plain values");
static_assert(std::is_trivially_copyable_v<DynTreeDefer>, "defer payloads are plain values");
static_assert(std::is_trivially_copyable_v<ZonePlayerCountDefer>, "defer payloads are plain values");
static_assert(std::is_trivially_copyable_v<GroupEndRollDefer>, "defer payloads are plain values");
static_assert(std::is_trivially_copyable_v<BattlegroundEventDefer>, "defer payloads are plain values");
static_assert(std::is_trivially_copyable_v<OutdoorPvPEventDefer>, "defer payloads are plain values");
static_assert(std::is_trivially_copyable_v<DuelEndDefer>, "defer payloads are plain values");
static_assert(std::is_trivially_copyable_v<MapChangeDefer>, "defer payloads are plain values");
static_assert(std::is_trivially_copyable_v<TaskGroupJoinDefer>, "defer payloads are plain values");
static_assert(std::is_trivially_copyable_v<TaskGroupLeaveDefer>, "defer payloads are plain values");
static_assert(std::is_trivially_copyable_v<PoolEventDefer>, "defer payloads are plain values");
static_assert(std::is_trivially_copyable_v<InstanceBindDefer>, "defer payloads are plain values");
static_assert(std::is_trivially_copyable_v<GameEventActionDefer>, "defer payloads are plain values");
static_assert(std::is_trivially_copyable_v<SpawnDefer>, "defer payloads are plain values");

// Append-only byte buffer of defers written by one map in one tick. Resident: reserved once, grows only on
// spill. Owner (step 16 table): Map::_defers, a DeferBufferSet with one buffer per core; allocated on the first
// append from that core, reset (capacity kept) at every seal, freed with the map.
class DeferBuffer
{
public:
    static constexpr std::size_t DEFAULT_BYTES = 256 * 1024;

    void Init(std::size_t bytes = DEFAULT_BYTES);

    template <typename T>
    void Append(Map* map, DeferKind kind, T const& payload)
    {
        static_assert(std::is_trivially_copyable_v<T>, "defer payloads are plain values");
        static_assert(sizeof(T) <= 0xFFFF, "defer payload too large");
        AppendRaw(map, kind, &payload, sizeof(T));
    }

    void DrainInto(std::vector<DeferHeader const*>& out) const;
    void Reset();

    [[nodiscard]] std::size_t Size() const { return _size; }
    [[nodiscard]] std::size_t Capacity() const { return _bytes.size(); }
    [[nodiscard]] std::size_t HighWater() const { return _highWater; }
    [[nodiscard]] uint32 Count() const { return _sequence; }
    [[nodiscard]] uint32 Spills() const { return _spills; }

private:
    void AppendRaw(Map* map, DeferKind kind, void const* payload, std::size_t size);
    void Spill(std::size_t needed);

    std::vector<uint8> _bytes;
    std::size_t _size = 0;
    std::size_t _highWater = 0;
    std::size_t _block = 0;
    uint32 _sequence = 0;
    uint32 _spills = 0;
};

// Step 14: one DeferBuffer per core for one map. Tasks of one map run on every core at once, so each core
// appends to its own buffer; the seal drains them all and orders by sequence (per-core order is kept, cross-core
// order was never defined). Buffers allocate on their first append, so an idle core costs a few words.
class DeferBufferSet
{
public:
    static constexpr uint8 MAX_CORES = 64;

    template <typename T>
    void Append(Map* map, DeferKind kind, T const& payload)
    {
        _buffers[std::min<uint8>(BeatContext::core, MAX_CORES - 1)].Append(map, kind, payload);
    }

    void DrainInto(std::vector<DeferHeader const*>& out) const
    {
        for (DeferBuffer const& buffer : _buffers)
            if (buffer.Size())
                buffer.DrainInto(out);
    }

    void Reset()
    {
        for (DeferBuffer& buffer : _buffers)
            if (buffer.Size() || buffer.Count())
                buffer.Reset();
    }

    [[nodiscard]] uint32 Count() const
    {
        uint32 count = 0;
        for (DeferBuffer const& buffer : _buffers)
            count += buffer.Count();
        return count;
    }

    // Step 16: resident bytes, the largest fill any seal drained, and how many cores ever appended.
    [[nodiscard]] std::size_t Capacity() const
    {
        std::size_t bytes = 0;
        for (DeferBuffer const& buffer : _buffers)
            bytes += buffer.Capacity();
        return bytes;
    }
    [[nodiscard]] std::size_t HighWater() const
    {
        std::size_t bytes = 0;
        for (DeferBuffer const& buffer : _buffers)
            bytes += buffer.HighWater();
        return bytes;
    }
    [[nodiscard]] uint8 Allocated() const
    {
        uint8 n = 0;
        for (DeferBuffer const& buffer : _buffers)
            n += buffer.Capacity() ? 1 : 0;
        return n;
    }

private:
    std::array<DeferBuffer, MAX_CORES> _buffers;
};

#endif
