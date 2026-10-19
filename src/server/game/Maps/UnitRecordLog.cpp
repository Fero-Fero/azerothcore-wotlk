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
#include "CombatRecords.h"
#include "Errors.h"
#include "Log.h"
#include "Object.h"
#include "SpellHitRecord.h"
#include <algorithm>
#include <atomic>
#include <cstring>

namespace RecordChain
{
    thread_local uint8 applying = 0;

    namespace
    {
        struct alignas(64) CoreStats
        {
            std::atomic<uint32> histogram[MAX_DEPTH] = {};
            std::atomic<uint8> maxDepth{0};
            std::atomic<uint32> deepSpell[MAX_DEEP_SPELLS] = {};
            std::atomic<uint8> deepDepth[MAX_DEEP_SPELLS] = {};
        };

        CoreStats stats[64];

        // The spell a record is about, for the depth >= DEEP_FROM list. Zero for kinds without one.
        uint32 SpellOf(RecordKind kind, void const* payload, std::size_t size)
        {
            switch (kind)
            {
                case RecordKind::SpellHit:
                    return size >= sizeof(SpellHitRecord) ? static_cast<SpellHitRecord const*>(payload)->spellId : 0;
                case RecordKind::DeferredSpellHit:
                    return size >= sizeof(DeferredSpellHitRecord) ? static_cast<DeferredSpellHitRecord const*>(payload)->hit.spellId : 0;
                case RecordKind::SpellLanded:
                    return size >= sizeof(SpellLandedRecord) ? static_cast<SpellLandedRecord const*>(payload)->spellId : 0;
                case RecordKind::PeriodicLanded:
                    return size >= sizeof(PeriodicLandedRecord) ? static_cast<PeriodicLandedRecord const*>(payload)->spellId : 0;
                case RecordKind::ProcActor:
                    return size >= sizeof(ProcActorRecord) ? static_cast<ProcActorRecord const*>(payload)->procSpellId : 0;
                default:
                    return 0;
            }
        }
    }

    void Count(RecordKind kind, uint8 depth, void const* payload, std::size_t size)
    {
        CoreStats& me = stats[std::min<uint8>(BeatContext::core, 63)];
        me.histogram[std::min<uint8>(depth, MAX_DEPTH - 1)].fetch_add(1, std::memory_order_relaxed);
        if (depth > me.maxDepth.load(std::memory_order_relaxed))
            me.maxDepth.store(depth, std::memory_order_relaxed);

        if (depth < DEEP_FROM)
            return;
        uint32 const spellId = SpellOf(kind, payload, size);
        if (!spellId)
            return;

        // Own core only writes here: a plain find-or-insert into a small fixed table.
        for (uint8 i = 0; i < MAX_DEEP_SPELLS; ++i)
        {
            uint32 const at = me.deepSpell[i].load(std::memory_order_relaxed);
            if (at == spellId)
            {
                if (depth > me.deepDepth[i].load(std::memory_order_relaxed))
                    me.deepDepth[i].store(depth, std::memory_order_relaxed);
                return;
            }
            if (!at)
            {
                me.deepSpell[i].store(spellId, std::memory_order_relaxed);
                me.deepDepth[i].store(depth, std::memory_order_relaxed);
                return;
            }
        }
    }

    Report Collect(bool reset)
    {
        Report report;
        for (CoreStats& core : stats)
        {
            for (uint8 d = 0; d < MAX_DEPTH; ++d)
                report.histogram[d] += reset ? core.histogram[d].exchange(0, std::memory_order_relaxed)
                                             : core.histogram[d].load(std::memory_order_relaxed);
            uint8 const max = reset ? core.maxDepth.exchange(0, std::memory_order_relaxed)
                                    : core.maxDepth.load(std::memory_order_relaxed);
            report.maxDepth = std::max(report.maxDepth, max);

            for (uint8 i = 0; i < MAX_DEEP_SPELLS; ++i)
            {
                uint32 const spellId = reset ? core.deepSpell[i].exchange(0, std::memory_order_relaxed)
                                             : core.deepSpell[i].load(std::memory_order_relaxed);
                uint8 const depth = reset ? core.deepDepth[i].exchange(0, std::memory_order_relaxed)
                                          : core.deepDepth[i].load(std::memory_order_relaxed);
                if (!spellId)
                    continue;
                auto it = std::find_if(report.deepSpells.begin(), report.deepSpells.end(),
                    [spellId](auto const& e) { return e.first == spellId; });
                if (it == report.deepSpells.end())
                    report.deepSpells.emplace_back(spellId, depth);
                else
                    it->second = std::max(it->second, depth);
            }
        }
        std::sort(report.deepSpells.begin(), report.deepSpells.end(),
            [](auto const& a, auto const& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
        return report;
    }
}

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
        case RecordKind::VisibilityPoke:    return "VisibilityPoke";
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
    _highWater = std::max(_highWater, _size);
    _size = 0;
    _sequence = 0;
}

void UnitRecordLog::Free()
{
    std::vector<uint8>().swap(_bytes);
    std::vector<TargetHead>().swap(_head);
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
    hdr.depth = uint8(std::min<uint32>(uint32(RecordChain::applying) + 1, 255));
    RecordChain::Count(kind, hdr.depth, payload, size);

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

void RecordLogSet::Shutdown()
{
    for (auto& parity : _logs)
    {
        for (UnitRecordLog& log : parity)
            log.Free();
        parity.clear();
        parity.shrink_to_fit();
    }
    _cores = 0;
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
