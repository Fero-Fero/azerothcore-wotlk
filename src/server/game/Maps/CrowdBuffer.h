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

#ifndef ACORE_CROWD_BUFFER_H
#define ACORE_CROWD_BUFFER_H

#include "Define.h"
#include "ObjectGuid.h"
#include "UnitUtils.h"
#include <atomic>
#include <unordered_map>
#include <vector>

class Map;

// Step 17 (optional, `MapUpdate.CrowdBuffer`): in a cell with >= THRESHOLD players every viewer builds the same
// public values block of every other player from the same slot, N² serialisations per beat. One CrowdBuffer per
// such cell builds each player's public block (UF_FLAG_PUBLIC only, from this beat's slot) once, first in U2
// (heavy ring), and the viewers' chunks copy it and patch the per-viewer indices (PatchValuesUpdate: aura
// state, dynamic flags, unit flags, bytes2, faction, script-tracked). A viewer that would see more than the
// public fields (itself, a raid member, an owner, empathy), or a target that is stealthed or invisible, takes
// the per-viewer path as before. Players only: creatures have fewer viewers and more per-viewer indices.
//
// Owner (step 16 table): Map::_crowdBuffers, allocated in S2 when the cell first crosses the threshold, reset
// in S2 with the capacity kept, inactive (not freed) when the cell empties, freed with the map.
class CrowdBuffer
{
public:
    static constexpr uint32 THRESHOLD = 32;

    explicit CrowdBuffer(uint32 cellId) : _cellId(cellId) { }

    // S2: the cell's players for the next beat. Clears last beat's blocks (capacity kept), ready = false.
    void Plan(std::vector<ObjectGuid> const& players);
    void Deactivate() { _active = false; _targets.clear(); _ready.store(true, std::memory_order_release); }
    [[nodiscard]] bool Active() const { return _active; }
    [[nodiscard]] uint32 CellId() const { return _cellId; }

    // U2, the crowd item: builds the public block of every planned player whose slot changed this beat.
    void Build(Map& map, uint32 beat);

    // U2, a viewer chunk: the crowd item started before any chunk (heavy ring), so this is a short wait.
    void WaitReady() const;
    [[nodiscard]] BuildValuesCachedBuffer const* Find(ObjectGuid guid) const;

    // Step 16: resident bytes, for `.debug memory`.
    [[nodiscard]] std::size_t HeapBytes() const;
    [[nodiscard]] uint32 Built() const { return _count; }

private:
    struct Entry
    {
        ObjectGuid guid;
        BuildValuesCachedBuffer cached{ 256 };
    };

    uint32 _cellId;
    bool _active = false;
    std::vector<ObjectGuid> _targets;
    std::vector<Entry> _entries;                        // _count used, the rest keep their buffers
    uint32 _count = 0;
    std::unordered_map<ObjectGuid, uint32> _index;      // guid -> entry, rebuilt per beat
    std::atomic<bool> _ready{ true };
};

#endif
