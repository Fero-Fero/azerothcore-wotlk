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

#include "UnitRecordLog.h"
#include "BeatContext.h"
#include "Errors.h"
#include "Log.h"
#include "Object.h"
#include <algorithm>
#include <cstring>

char const* RecordKindName(RecordKind kind)
{
    switch (kind)
    {
        case RecordKind::SpellHit:          return "SpellHit";
        case RecordKind::SpellLanded:       return "SpellLanded";
        case RecordKind::DeferredSpellHit:  return "DeferredSpellHit";
        case RecordKind::MeleeHit:          return "MeleeHit";
        case RecordKind::MeleeLanded:       return "MeleeLanded";
        case RecordKind::PeriodicLanded:    return "PeriodicLanded";
        case RecordKind::AreaAuraApply:     return "AreaAuraApply";
        case RecordKind::AreaAuraRemove:    return "AreaAuraRemove";
        case RecordKind::ThreatBackRef:     return "ThreatBackRef";
        case RecordKind::CombatRef:         return "CombatRef";
        case RecordKind::CombatEnd:         return "CombatEnd";
        case RecordKind::AuraRemove:        return "AuraRemove";
        case RecordKind::Knockback:         return "Knockback";
        case RecordKind::ThreatRemove:      return "ThreatRemove";
        case RecordKind::MinionUnlink:      return "MinionUnlink";
        case RecordKind::GameObjectUnlink:  return "GameObjectUnlink";
        case RecordKind::VisibilityLink:    return "VisibilityLink";
        case RecordKind::VisibilityUnlink:  return "VisibilityUnlink";
        case RecordKind::Wake:              return "Wake";
        case RecordKind::Threat:            return "Threat";
        case RecordKind::DamageDealt:       return "DamageDealt";
        case RecordKind::HealDone:          return "HealDone";
        case RecordKind::KillingBlow:       return "KillingBlow";
        case RecordKind::KillCredit:        return "KillCredit";
        case RecordKind::ProcActor:         return "ProcActor";
        default:                            return "Unknown";
    }
}

void UnitRecordLog::Init(uint8 coreIndex, uint32 slotCapacity, std::size_t bytes)
{
    _core = coreIndex;
    _block = bytes;
    _bytes.assign(bytes, 0);
    _size = 0;
    _sequence = 0;
    _spills = 0;
    _head.assign(slotCapacity, TargetHead());
}

void UnitRecordLog::Reset()
{
    _size = 0;
    _sequence = 0;
}

void UnitRecordLog::GrowHeads(uint32 slotCapacity)
{
    if (slotCapacity > _head.size())
        _head.resize(slotCapacity);
}

void UnitRecordLog::Spill(std::size_t needed)
{
    // One extra resident block per spill. A spill every beat means the reserve is short: raise it.
    std::size_t grown = _bytes.size() + std::max(_block, needed);
    _bytes.resize(grown);
    ++_spills;
    LOG_WARN("maps.records", "Record log core {} spilled to {} bytes ({} spills); raise the reserve.",
        _core, grown, _spills);
}

void UnitRecordLog::AppendRaw(ObjectGuid source, WorldObject* target, RecordKind kind, void const* payload, std::size_t size)
{
    uint32 const idx = target->SlotIndex();
    // A target without a slot index is not in the world: nothing would ever apply the record.
    if (idx == ObjectSlotPair::NO_SLOT_INDEX)
        return;
    if (idx >= _head.size())
        GrowHeads(std::max<uint32>(idx + 1, uint32(_head.size() * 2)));

    uint32 const beat = BeatContext::beat.load(std::memory_order_relaxed);
    TargetHead& h = _head[idx];

    RecordHeader hdr;
    hdr.target = target->GetGUID();
    hdr.source = source;
    hdr.nextInTarget = h.beat == beat ? h.offset : RecordHeader::NO_RECORD;
    hdr.sequence = _sequence++;
    hdr.size = uint16(size);
    hdr.kind = kind;
    hdr.pad = 0;

    std::size_t const off = _size;
    std::size_t const total = RECORD_HEADER_SIZE + RecordAlignUp8(size);
    if (off + total > _bytes.size())
        Spill(total);
    _size = off + total;

    std::memcpy(_bytes.data() + off, &hdr, sizeof(hdr));
    if (size)
        std::memcpy(_bytes.data() + off + RECORD_HEADER_SIZE, payload, size);

    bool const first = (h.beat != beat);
    h.beat = beat;
    h.offset = uint32(off);

    // One shared write per (core, target, beat). The target's inbox exchange is the only other writer.
    if (first)
        target->OnFirstRecordOfBeat(_core);
}

RecordLogSet* RecordLogSet::instance()
{
    static RecordLogSet instance;
    return &instance;
}

void RecordLogSet::Init(uint8 cores, std::size_t bytesPerLog)
{
    ASSERT(cores > 0 && cores <= 64, "RecordLogSet supports 1..64 cores (writtenBy is a 64-bit mask)");
    _cores = cores;
    uint32 const slots = std::max<uint32>(sSlotIndexAllocator->Capacity(), 1024);
    for (auto& parity : _logs)
    {
        parity = std::vector<UnitRecordLog>(cores);
        for (uint8 core = 0; core < cores; ++core)
            parity[core].Init(core, slots, bytesPerLog);
    }
}

void RecordLogSet::BeginBeat(uint32 beat)
{
    for (UnitRecordLog& log : _logs[beat & 1])
        log.Reset();
}

UnitRecordLog& RecordLogSet::WriteLog()
{
    if (!_cores)
        Init(1);   // a task ran before MapMgr::Initialize (unit tests): one core
    ASSERT(BeatContext::core < _cores, "record log core {} out of range ({} cores)", BeatContext::core, _cores);
    return _logs[BeatContext::beat.load(std::memory_order_relaxed) & 1][BeatContext::core];
}

UnitRecordLog const& RecordLogSet::ReadLog(uint8 core) const
{
    return _logs[(BeatContext::beat.load(std::memory_order_relaxed) - 1) & 1][core];
}

void RecordLogSet::Wake(ObjectGuid source, WorldObject* target)
{
    WakeRecord wake{};
    Append(source, target, RecordKind::Wake, wake);
}

uint32 RecordLogSet::PendingCount() const
{
    uint32 count = 0;
    for (UnitRecordLog const& log : _logs[BeatContext::beat.load(std::memory_order_relaxed) & 1])
        count += log.Count();
    return count;
}
