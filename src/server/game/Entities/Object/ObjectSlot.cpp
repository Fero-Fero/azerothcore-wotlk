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
    uint32 const wordsPerSlot = valuesCount + maskWords;

    _storage = std::make_unique<uint32[]>(wordsPerSlot * 2);
    std::memset(_storage.get(), 0, wordsPerSlot * 2 * sizeof(uint32));

    for (uint32 i = 0; i < 2; ++i)
    {
        _slot[i].valuesCount = valuesCount;
        _slot[i].values = _storage.get() + i * wordsPerSlot;
        _slot[i].changesMask = _slot[i].values + valuesCount;
    }

    if (isUnit)
        EnsureAuraCapacity(valuesCount >= PLAYER_END ? INITIAL_PLAYER_AURA_CAPACITY : INITIAL_UNIT_AURA_CAPACITY);
}

void ObjectSlotPair::EnsureAuraCapacity(uint32 count)
{
    if (count <= _auraCapacity)
        return;

    uint32 capacity = std::max<uint32>(_auraCapacity, INITIAL_UNIT_AURA_CAPACITY);
    while (capacity < count)
        capacity *= 2;

    auto storage = std::make_unique<SlotAura[]>(capacity * 2);

    // Keep the readable slot's auras valid across the grow. The write slot is about to be refilled.
    for (uint32 i = 0; i < 2; ++i)
    {
        SlotAura* dst = storage.get() + i * capacity;
        if (_slot[i].auras && _slot[i].auraCount)
            std::copy_n(_slot[i].auras, std::min(_slot[i].auraCount, _auraCapacity), dst);
        _slot[i].auras = dst;
    }

    _auraStorage = std::move(storage);
    _auraCapacity = capacity;
}

SlotIndexAllocator* SlotIndexAllocator::instance()
{
    static SlotIndexAllocator instance;
    return &instance;
}

uint32 SlotIndexAllocator::Allocate()
{
    std::lock_guard<std::mutex> guard(_lock);
    if (!_free.empty())
    {
        uint32 index = _free.back();
        _free.pop_back();
        return index;
    }
    return _next++;
}

void SlotIndexAllocator::Release(uint32 index)
{
    if (index == ObjectSlotPair::NO_SLOT_INDEX)
        return;

    std::lock_guard<std::mutex> guard(_lock);
    _free.push_back(index);
}

uint32 SlotIndexAllocator::Capacity() const
{
    std::lock_guard<std::mutex> guard(_lock);
    return _next;
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

uint32 SlotReader::Health() const { return U32(UNIT_FIELD_HEALTH); }
uint32 SlotReader::MaxHealth() const { return U32(UNIT_FIELD_MAXHEALTH); }
uint8 SlotReader::Level() const { return uint8(U32(UNIT_FIELD_LEVEL)); }
uint32 SlotReader::Faction() const { return U32(UNIT_FIELD_FACTIONTEMPLATE); }
bool SlotReader::IsAlive() const { return s.deathState == uint8(DeathState::Alive); }
