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

#ifndef ACORE_OBJECT_SLOT_H
#define ACORE_OBJECT_SLOT_H

#include "DBCStructure.h"
#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "SpellAuraDefines.h"
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

// One applied aura as a foreign reader sees it. Plain values only.
struct SlotAura
{
    uint32 spellId = 0;
    ObjectGuid casterGuid;
    uint8 effectMask = 0;              // AuraApplication::GetEffectMask()
    uint8 stackAmount = 0;             // Aura::GetStackAmount()
    uint8 charges = 0;                 // Aura::GetCharges()
    uint8 flags = 0;                   // SLOT_AURA_* bits
    int32 amount[MAX_SPELL_EFFECTS] = {};   // AuraEffect::GetAmount() per effect, 0 where the effect is absent
    int32 duration = 0;                // Aura::GetDuration()
    int32 maxDuration = 0;             // Aura::GetMaxDuration()
};

enum SlotAuraFlags : uint8
{
    SLOT_AURA_POSITIVE  = 0x01,        // AuraApplication::IsPositive()
    SLOT_AURA_PASSIVE   = 0x02,        // Aura::IsPassive()
    SLOT_AURA_SELF_CAST = 0x04         // caster == owner
};

// The readable copy of one WorldObject. Published by the owner after its Update; read by everyone else.
struct ObjectSlot
{
    uint32 beat = 0;                   // beat this slot was published in (debug cross-check)
    uint16 valuesCount = 0;            // == Object::m_valuesCount
    uint32* values = nullptr;          // valuesCount words, owned by ObjectSlotPair storage
    uint32* changesMask = nullptr;     // (valuesCount + 31) / 32 words; the owner's _changesMask at publish

    // WorldObject
    Position pos;                      // m_positionX/Y/Z/O
    uint32 phaseMask = 0;
    uint32 zoneId = 0;
    uint32 areaId = 0;
    uint32 cellId = 0;                 // CellCoord::GetId() of pos, for isCellMarked-style checks

    // Unit only (zero for others)
    uint32 movementFlags = 0;          // m_movementInfo.flags
    uint16 movementFlags2 = 0;         // m_movementInfo.flags2
    uint32 unitState = 0;              // Unit::m_state
    uint8 deathState = 0;              // Unit::m_deathState
    uint8 isInCombat = 0;              // Unit::IsInCombat()
    uint32 lastSanctuaryTime = 0;      // Unit::m_lastSanctuaryTime
    ObjectGuid victimGuid;             // GetVictim() ? GetVictim()->GetGUID() : Empty
    ObjectGuid charmerOrOwnerGuid;     // GetCharmerOrOwnerGUID()
    uint32 auraCount = 0;
    SlotAura* auras = nullptr;         // auraCount entries, capacity in the pair

    // Movement update block pre-serialised for viewers (step 10 fills it; empty until then)
    uint16 movementBlockSize = 0;
    uint8* movementBlock = nullptr;
};

// Two slots and a stamp. The owner writes the slot opposite to the stamped one, then advances the stamp.
// The stamp moves only when this object publishes, so a reader of the stamped slot never races the writer.
class ObjectSlotPair
{
public:
    static constexpr uint32 NO_SLOT_INDEX = ~0u;

    ObjectSlotPair() = default;
    ObjectSlotPair(ObjectSlotPair const&) = delete;
    ObjectSlotPair& operator=(ObjectSlotPair const&) = delete;

    // One allocation for both slots: values*2 and masks*2. Aura arrays start at 64 (unit) or 0 and grow by doubling.
    void Init(uint16 valuesCount, bool isUnit);
    [[nodiscard]] bool IsInitialized() const { return _storage != nullptr; }

    ObjectSlot& WriteSlot() { return _slot[(_publishedBeat.load(std::memory_order_relaxed) + 1) & 1]; }
    [[nodiscard]] ObjectSlot const& ReadSlot() const
    {
        return _slot[_publishedBeat.load(std::memory_order_acquire) & 1];
    }
    void Commit(uint32 beat) { _publishedBeat.store(beat, std::memory_order_release); }
    [[nodiscard]] uint32 PublishedBeat() const { return _publishedBeat.load(std::memory_order_relaxed); }

    // Grows the aura arrays of both slots when count exceeds capacity. Never shrinks. Called from FillSlot only.
    void EnsureAuraCapacity(uint32 count);
    [[nodiscard]] uint32 AuraCapacity() const { return _auraCapacity; }

    // Dense per-process index for the record log's head arrays (step 06). NO_SLOT_INDEX until AddToWorld.
    [[nodiscard]] uint32 SlotIndex() const { return _slotIndex; }
    void SetSlotIndex(uint32 index) { _slotIndex = index; }

private:
    ObjectSlot _slot[2];
    std::atomic<uint32> _publishedBeat{0};
    uint32 _slotIndex = NO_SLOT_INDEX;
    std::unique_ptr<uint32[]> _storage;        // values and masks for both slots
    std::unique_ptr<SlotAura[]> _auraStorage;  // aura arrays for both slots
    uint32 _auraCapacity = 0;
};

// Hands out dense slot indices. Taken at add/remove only, which happen between beats or at the seal.
class SlotIndexAllocator
{
public:
    static SlotIndexAllocator* instance();

    uint32 Allocate();
    void Release(uint32 index);
    [[nodiscard]] uint32 Capacity() const;

private:
    std::vector<uint32> _free;
    uint32 _next = 0;
    mutable std::mutex _lock;
};

#define sSlotIndexAllocator SlotIndexAllocator::instance()

// Slot accessors with the same semantics as the Object getters. No ASSERT: slots are always readable.
struct SlotReader
{
    ObjectSlot const& s;

    explicit SlotReader(ObjectSlot const& slot) : s(slot) { }

    [[nodiscard]] uint32 U32(uint16 i) const { return s.values[i]; }
    [[nodiscard]] int32 I32(uint16 i) const { return int32(s.values[i]); }
    [[nodiscard]] float F(uint16 i) const { float f; std::memcpy(&f, &s.values[i], sizeof(float)); return f; }
    [[nodiscard]] uint64 U64(uint16 i) const { return uint64(s.values[i]) | (uint64(s.values[i + 1]) << 32); }
    [[nodiscard]] ObjectGuid Guid(uint16 i) const { return ObjectGuid(U64(i)); }
    [[nodiscard]] uint8 Byte(uint16 i, uint8 off) const { return (s.values[i] >> (off * 8)) & 0xFF; }
    [[nodiscard]] bool Flag(uint16 i, uint32 f) const { return (s.values[i] & f) != 0; }

    [[nodiscard]] bool HasAura(uint32 spellId, ObjectGuid caster = ObjectGuid::Empty) const;
    [[nodiscard]] SlotAura const* FindAura(uint32 spellId, ObjectGuid caster = ObjectGuid::Empty) const;
    // Sum of amount[i] over slot auras whose effect i applies the given aura type.
    [[nodiscard]] int32 TotalAuraModifier(AuraType type) const;

    [[nodiscard]] uint32 Health() const;
    [[nodiscard]] uint32 MaxHealth() const;
    [[nodiscard]] uint8 Level() const;
    [[nodiscard]] uint32 Faction() const;
    [[nodiscard]] bool IsAlive() const;
};

#endif
