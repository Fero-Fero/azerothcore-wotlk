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

#include "MapDefer.h"
#include "Log.h"
#include <algorithm>
#include <cstring>

char const* DeferKindName(DeferKind kind)
{
    switch (kind)
    {
        case DeferKind::Relink:             return "Relink";
        case DeferKind::GridLoad:           return "GridLoad";
        case DeferKind::AddToMap:           return "AddToMap";
        case DeferKind::RemoveFromMap:      return "RemoveFromMap";
        case DeferKind::ScriptSchedule:     return "ScriptSchedule";
        case DeferKind::CellMark:           return "CellMark";
        case DeferKind::UpdateListAdd:      return "UpdateListAdd";
        case DeferKind::UpdateListRemove:   return "UpdateListRemove";
        case DeferKind::DynTreeInsert:      return "DynTreeInsert";
        case DeferKind::DynTreeRemove:      return "DynTreeRemove";
        case DeferKind::ZonePlayerCount:    return "ZonePlayerCount";
        case DeferKind::GroupEndRoll:       return "GroupEndRoll";
        case DeferKind::GroupBroadcast:     return "GroupBroadcast";
        case DeferKind::BattlegroundEvent:  return "BattlegroundEvent";
        case DeferKind::OutdoorPvPEvent:    return "OutdoorPvPEvent";
        case DeferKind::WorldStateWrite:    return "WorldStateWrite";
        case DeferKind::DuelEnd:            return "DuelEnd";
        case DeferKind::DeferredSpellHit:   return "DeferredSpellHit";
        case DeferKind::MapChange:          return "MapChange";
        case DeferKind::TaskGroupJoin:      return "TaskGroupJoin";
        case DeferKind::TaskGroupLeave:     return "TaskGroupLeave";
        case DeferKind::PoolEvent:          return "PoolEvent";
        case DeferKind::InstanceBind:       return "InstanceBind";
        case DeferKind::GameEventAction:    return "GameEventAction";
        case DeferKind::Spawn:              return "Spawn";
        default:                            return "Unknown";
    }
}

void DeferBuffer::Init(std::size_t bytes)
{
    _block = bytes;
    _bytes.assign(bytes, 0);
    _size = 0;
    _sequence = 0;
    _spills = 0;
}

void DeferBuffer::Reset()
{
    _size = 0;
    _sequence = 0;
}

void DeferBuffer::Spill(std::size_t needed)
{
    std::size_t grown = _bytes.size() + std::max(_block, needed);
    _bytes.resize(grown);
    ++_spills;
    LOG_WARN("maps.defers", "Defer buffer spilled to {} bytes ({} spills); raise the reserve.", grown, _spills);
}

void DeferBuffer::AppendRaw(Map* map, DeferKind kind, void const* payload, std::size_t size)
{
    DeferHeader hdr;
    hdr.map = map;
    hdr.sequence = _sequence++;
    hdr.size = uint16(size);
    hdr.kind = kind;
    hdr.pad = 0;

    std::size_t const off = _size;
    std::size_t const total = DEFER_HEADER_SIZE + DeferAlignUp8(size);
    if (_bytes.empty())
        Init();
    if (off + total > _bytes.size())
        Spill(total);
    _size = off + total;

    std::memcpy(_bytes.data() + off, &hdr, sizeof(hdr));
    if (size)
        std::memcpy(_bytes.data() + off + DEFER_HEADER_SIZE, payload, size);
}

void DeferBuffer::DrainInto(std::vector<DeferHeader const*>& out) const
{
    std::size_t off = 0;
    while (off < _size)
    {
        DeferHeader const* hdr = reinterpret_cast<DeferHeader const*>(_bytes.data() + off);
        out.push_back(hdr);
        off += DEFER_HEADER_SIZE + DeferAlignUp8(hdr->size);
    }
}
