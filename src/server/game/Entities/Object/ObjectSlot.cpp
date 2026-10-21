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

#include "ObjectSlot.h"
#include "BeatContext.h"
#include "Errors.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"
#include "UpdateFields.h"
#include <algorithm>
#include <mutex>

namespace
{
    constexpr uint32 INITIAL_UNIT_AURA_CAPACITY = 16;
    constexpr uint32 INITIAL_PLAYER_AURA_CAPACITY = 64;
}

void ObjectSlotPair::Init(uint16 valuesCount, bool isUnit)
{
    uint32 const maskWords = (valuesCount + 31) / 32;
    _valuesWords = valuesCount + maskWords;

    _storage = std::make_unique<uint32[]>(_valuesWords * 2);
    std::memset(_storage.get(), 0, _valuesWords * 2 * sizeof(uint32));

    for (uint32 i = 0; i < 2; ++i)
    {
        _slot[i].valuesCount = valuesCount;
        _slot[i].values = _storage.get() + i * _valuesWords;
        _slot[i].changesMask = _slot[i].values + valuesCount;
    }

    if (isUnit)
    {
        // Both slots at the initial capacity, so the first beats grow nothing.
        uint32 const initial = valuesCount >= PLAYER_END ? INITIAL_PLAYER_AURA_CAPACITY : INITIAL_UNIT_AURA_CAPACITY;
        for (uint32 i = 0; i < 2; ++i)
        {
            _auraStorage[i] = std::make_unique<SlotAura[]>(initial);
            _auraCapacity[i] = initial;
            _slot[i].auras = _auraStorage[i].get();
        }
    }
}

// The write slot is the only one touched: its previous content is about to be overwritten by FillSlot, and the
// published slot (which a view chunk or a foreign reader may be walking right now) keeps its own array. Growth
// is therefore legal mid-beat (step 16); the old array is freed here because nobody can hold it: the write slot
// was last read two beats ago, before this object's previous publish moved the stamp.
void ObjectSlotPair::EnsureAuraCapacity(uint32 count)
{
    uint32 const w = WriteIndex();
    if (count <= _auraCapacity[w])
        return;

    uint32 capacity = std::max<uint32>(_auraCapacity[w], INITIAL_UNIT_AURA_CAPACITY);
    while (capacity < count)
        capacity *= 2;

    _auraStorage[w] = std::make_unique<SlotAura[]>(capacity);
    _auraCapacity[w] = capacity;
    _slot[w].auras = _auraStorage[w].get();
    _slot[w].auraCount = 0;
    ++_regrowths;
}

void ObjectSlotPair::EnsureMovementCapacity(uint32 bytes)
{
    uint32 const w = WriteIndex();
    if (bytes <= _movementCapacity[w])
        return;

    uint32 capacity = std::max<uint32>(_movementCapacity[w], 64);
    while (capacity < bytes)
        capacity *= 2;

    _movementStorage[w] = std::make_unique<uint8[]>(capacity);
    _movementCapacity[w] = capacity;
    _slot[w].movementBlock = _movementStorage[w].get();
    _slot[w].movementBlockSize = 0;
    ++_regrowths;
}

std::size_t ObjectSlotPair::HeapBytes() const
{
    std::size_t bytes = _storage ? std::size_t(_valuesWords) * 2 * sizeof(uint32) : 0;
    for (uint32 i = 0; i < 2; ++i)
        bytes += std::size_t(_auraCapacity[i]) * sizeof(SlotAura) + _movementCapacity[i];
    return bytes;
}

SlotIndexAllocator* SlotIndexAllocator::instance()
{
    static SlotIndexAllocator instance;
    return &instance;
}

uint32 SlotIndexAllocator::Allocate()
{
    uint32 const beat = BeatContext::beat.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> guard(_lock);
    // Oldest release first; an index freed in this beat is skipped (its record heads are still addressable),
    // a fresh one is taken instead. The list is in release order, so the front is the oldest.
    if (!_free.empty() && _free.front().beat < beat)
    {
        uint32 const index = _free.front().index;
        _free.pop_front();
        ASSERT(index < _next);
        return index;
    }
    return _next++;
}

void SlotIndexAllocator::Release(uint32 index)
{
    if (index == ObjectSlotPair::NO_SLOT_INDEX)
        return;

    std::lock_guard<std::mutex> guard(_lock);
    _free.push_back(Freed{ index, BeatContext::beat.load(std::memory_order_relaxed) });
}

uint32 SlotIndexAllocator::Capacity() const
{
    std::lock_guard<std::mutex> guard(_lock);
    return _next;
}

uint32 SlotIndexAllocator::FreeCount() const
{
    std::lock_guard<std::mutex> guard(_lock);
    return uint32(_free.size());
}

SlotAura const* SlotReader::FindAura(uint32 spellId, ObjectGuid caster) const
{
    for (uint32 i = 0; i < s.auraCount; ++i)
    {
        SlotAura const& a = s.auras[i];
        if (a.spellId == spellId && (caster.IsEmpty() || a.casterGuid == caster))
            return &a;
    }
    return nullptr;
}

bool SlotReader::HasAuraEffect(uint32 spellId, uint8 effIndex) const
{
    SlotAura const* aura = FindAura(spellId);
    return aura && (aura->effectMask & (1u << effIndex));
}

bool SlotReader::HasAura(uint32 spellId, ObjectGuid caster) const
{
    return FindAura(spellId, caster) != nullptr;
}

int32 SlotReader::TotalAuraModifier(AuraType type) const
{
    int32 total = 0;
    for (uint32 i = 0; i < s.auraCount; ++i)
    {
        SlotAura const& a = s.auras[i];
        SpellInfo const* info = sSpellMgr->GetSpellInfo(a.spellId);
        if (!info)
            continue;

        for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
            if ((a.effectMask & (1 << e)) && info->Effects[e].ApplyAuraName == type)
                total += a.amount[e];
    }
    return total;
}

bool SlotReader::HasAuraType(AuraType type) const
{
    for (uint32 i = 0; i < s.auraCount; ++i)
    {
        SlotAura const& a = s.auras[i];
        SpellInfo const* info = sSpellMgr->GetSpellInfo(a.spellId);
        if (!info)
            continue;

        for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
            if ((a.effectMask & (1 << e)) && info->Effects[e].ApplyAuraName == type)
                return true;
    }
    return false;
}

bool SlotReader::HasAuraTypeWithCaster(AuraType type, ObjectGuid caster) const
{
    for (uint32 i = 0; i < s.auraCount; ++i)
    {
        SlotAura const& a = s.auras[i];
        if (a.casterGuid != caster)
            continue;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(a.spellId);
        if (!info)
            continue;

        for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
            if ((a.effectMask & (1 << e)) && info->Effects[e].ApplyAuraName == type)
                return true;
    }
    return false;
}

uint32 SlotReader::AuraStateForTarget(ObjectGuid target) const
{
    uint32 auraStates = U32(UNIT_FIELD_AURASTATE) & ~uint32(PER_CASTER_AURA_STATE_MASK);
    for (uint32 i = 0; i < s.auraCount; ++i)
    {
        SlotAura const& a = s.auras[i];
        if (a.casterGuid != target)
            continue;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(a.spellId);
        if (!info)
            continue;

        // Same registration rule as Unit::_ApplyAura into m_auraStateAuras.
        if (AuraStateType state = info->GetAuraState())
            if ((1 << (state - 1)) & PER_CASTER_AURA_STATE_MASK)
                auraStates |= (1 << (state - 1));
    }
    return auraStates;
}

uint32 SlotReader::Health() const { return U32(UNIT_FIELD_HEALTH); }
uint32 SlotReader::MaxHealth() const { return U32(UNIT_FIELD_MAXHEALTH); }
uint8 SlotReader::Level() const { return uint8(U32(UNIT_FIELD_LEVEL)); }
uint32 SlotReader::Faction() const { return U32(UNIT_FIELD_FACTIONTEMPLATE); }
bool SlotReader::IsAlive() const { return s.deathState == uint8(DeathState::Alive); }
