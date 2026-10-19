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

#include "CrowdBuffer.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectSlot.h"
#include "Player.h"
#include <thread>

void CrowdBuffer::Plan(std::vector<ObjectGuid> const& players)
{
    _active = true;
    _targets = players;
    _count = 0;
    _index.clear();
    _ready.store(false, std::memory_order_relaxed);
}

void CrowdBuffer::Build(Map& map, uint32 beat)
{
    if (_entries.size() < _targets.size())
        _entries.resize(_targets.size());
    _index.reserve(_targets.size());

    for (ObjectGuid const& guid : _targets)
    {
        Player* player = ObjectAccessor::GetPlayer(&map, guid);
        if (!player || !player->IsInWorld())
            continue;

        ObjectSlot const& s = player->ReadSlot();
        if (s.beat != beat || !s.AnyChange())
            continue;

        // Stealth / invisibility: what a viewer sees depends on its detection; never shared.
        if (s.vis.stealth.flags || s.vis.invisibility.flags)
            continue;

        Entry& e = _entries[_count];
        e.guid = guid;
        player->BuildPublicValuesBlock(s, e.cached);
        _index[guid] = _count++;
    }

    _ready.store(true, std::memory_order_release);
}

void CrowdBuffer::WaitReady() const
{
    // Bounded: a missing crowd item must not park every view chunk (and then BeatPool::Stop's join).
    for (uint32 n = 0; n < 10000 && !_ready.load(std::memory_order_acquire); ++n)
        std::this_thread::yield();
}

BuildValuesCachedBuffer const* CrowdBuffer::Find(ObjectGuid guid) const
{
    auto it = _index.find(guid);
    return it != _index.end() ? &_entries[it->second].cached : nullptr;
}

std::size_t CrowdBuffer::HeapBytes() const
{
    std::size_t bytes = _targets.capacity() * sizeof(ObjectGuid) + _index.bucket_count() * sizeof(void*);
    for (Entry const& e : _entries)
        bytes += sizeof(Entry) + e.cached.buffer.capacity();
    return bytes;
}
