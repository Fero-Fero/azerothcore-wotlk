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
#include "SharedDefines.h"
#include "SpellAuraDefines.h"
#include <atomic>
#include <cstddef>
#include <cstring>
#include <deque>
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

// A FlaggedValuesArray32 as plain values (ObjectSlot.h cannot include Object.h).
template <uint8 N>
struct SlotFlaggedValues
{
    uint32 flags = 0;
    int32 values[N] = {};

    [[nodiscard]] bool HasFlag(uint32 i) const { return (flags & (1u << i)) != 0; }
    [[nodiscard]] int32 GetValue(uint32 i) const { return values[i]; }
};

enum SlotVisibilityFlags : uint16
{
    SLOT_VIS_NEVER_VISIBLE          = 0x0001,   // WorldObject::IsNeverVisible()
    SLOT_VIS_INVISIBLE_DUE_DESPAWN  = 0x0002,   // WorldObject::IsInvisibleDueToDespawn()
    SLOT_VIS_ARENA_SPECTATOR        = 0x0004,   // Player::IsSpectator() on an arena map
    SLOT_VIS_GAME_MASTER            = 0x0008,   // Player::IsGameMaster()
    SLOT_VIS_GM_ACCOUNT             = 0x0010,   // WorldSession::IsGMAccount()
    SLOT_VIS_SUMMONER_ONLY          = 0x0020,   // TempSummon::IsVisibleBySummonerOnly()
    SLOT_VIS_TRANSPORT_OR_BUILDING  = 0x0040,   // GameObject: IsTransport() || IsDestructibleBuilding()
    SLOT_VIS_GHOST                  = 0x0080,   // Player: isDead() && GetHealth() > 0
    SLOT_VIS_HAS_STEALTH_AURA       = 0x0100,   // Unit::HasStealthAura()
    SLOT_VIS_IN_FLIGHT              = 0x0200    // Unit::IsInFlight()
};

// One entry of ThreatManager::_redirectInfo (Misdirection, Tricks of the Trade) as the attacker's threat
// target reads it (step 13: AddThreat on the victim's task must not read the attacker's threat manager).
struct SlotThreatRedirect
{
    ObjectGuid guid;
    uint32 pct = 0;
};
constexpr uint32 MAX_SLOT_THREAT_REDIRECTS = 4;

// What CanSeeOrDetect reads from the target (step 10). Filled for every WorldObject; unit/player parts are
// zero for the rest.
struct SlotVisibility
{
    uint16 flags = 0;                                   // SlotVisibilityFlags
    uint8 teamId = 0;                                   // Player::GetTeamId()
    SlotFlaggedValues<TOTAL_STEALTH_TYPES> stealth;
    SlotFlaggedValues<TOTAL_STEALTH_TYPES> stealthDetect;
    SlotFlaggedValues<TOTAL_INVISIBILITY_TYPES> invisibility;
    SlotFlaggedValues<TOTAL_INVISIBILITY_TYPES> invisibilityDetect;
    SlotFlaggedValues<TOTAL_SERVERSIDE_VISIBILITY_TYPES> serverSideVisibility;
    SlotFlaggedValues<TOTAL_SERVERSIDE_VISIBILITY_TYPES> serverSideVisibilityDetect;
    ObjectGuid groupGuid;                               // Player::GetGroup() guid, for IsGroupVisibleFor
    ObjectGuid summonerGuid;                            // TempSummon::GetSummonerGUID()
    ObjectGuid vehicleBaseGuid;                         // Unit::GetVehicleBase() guid
    ObjectGuid ownerGuid;                               // Unit/GameObject GetOwnerGUID()
    ObjectGuid farsightGuid;                            // Player: PLAYER_FARSIGHT
    float combatReach = 0.0f;                           // Unit::GetCombatReach()
    uint8 level = 0;                                    // getLevelForTarget() without target adjustments
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
    uint8 threatRedirectCount = 0;     // ThreatManager::_redirectInfo, in order, capped
    SlotThreatRedirect threatRedirects[MAX_SLOT_THREAT_REDIRECTS];

    // Viewer builds (step 13 leftovers). Live members of a foreign unit are not readable from U2.
    uint32 transformSpell = 0;         // Unit::m_transform
    uint8 controlledByPlayer = 0;      // Unit::m_ControlledByPlayer
    ObjectGuid lootRecipient;          // Creature::m_lootRecipient
    uint32 lootRecipientGroup = 0;     // Creature::m_lootRecipientGroup

    // Visibility inputs for a viewer's CanSeeOrDetect (step 10)
    SlotVisibility vis;

    // Movement update block pre-serialised for viewers (step 10): BuildMovementUpdate with updateFlag,
    // never UPDATEFLAG_SELF (a viewer builds its own create block live).
    uint16 updateFlag = 0;             // the flags the block was serialised with
    uint16 movementBlockSize = 0;
    uint8* movementBlock = nullptr;    // movementBlockSize bytes, capacity in the pair

    [[nodiscard]] bool AnyChange() const
    {
        for (uint32 i = 0, n = (valuesCount + 31) / 32; i < n; ++i)
            if (changesMask[i])
                return true;
        return false;
    }
    [[nodiscard]] bool Changed(uint16 index) const { return (changesMask[index >> 5] >> (index & 31)) & 1; }
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

    // Owner: the WorldObject, by value (step 16 table). Init at _OnValuesInitialized, freed with the object.
    // One allocation for both slots' values and masks; the aura and movement arrays are one allocation per
    // slot, so growing the write slot's never moves or frees what a reader of the published slot holds.
    void Init(uint16 valuesCount, bool isUnit);
    [[nodiscard]] bool IsInitialized() const { return _storage != nullptr; }

    ObjectSlot& WriteSlot() { return _slot[(_publishedBeat.load(std::memory_order_relaxed) + 1) & 1]; }
    [[nodiscard]] ObjectSlot const& ReadSlot() const
    {
        return _slot[_publishedBeat.load(std::memory_order_acquire) & 1];
    }
    void Commit(uint32 beat) { _publishedBeat.store(beat, std::memory_order_release); }
    [[nodiscard]] uint32 PublishedBeat() const { return _publishedBeat.load(std::memory_order_relaxed); }

    // Grows the write slot's aura array when count exceeds its capacity. Never shrinks; the published slot's
    // array is not touched (a reader on another core may be walking it). Called from FillSlot only.
    void EnsureAuraCapacity(uint32 count);
    [[nodiscard]] uint32 AuraCapacity() const { return _auraCapacity[WriteIndex()]; }

    // Grows the write slot's movement block storage. Never shrinks. Called from PublishSlot only.
    void EnsureMovementCapacity(uint32 bytes);
    [[nodiscard]] uint32 MovementCapacity() const { return _movementCapacity[WriteIndex()]; }

    // Step 16: regrowths since Init, for `.debug slot` and `.debug memory`.
    [[nodiscard]] uint32 Regrowths() const { return _regrowths; }
    // Heap bytes held by this pair (values, masks, both slots' aura and movement arrays).
    [[nodiscard]] std::size_t HeapBytes() const;

    // Dense per-process index for the record log's head arrays (step 06). NO_SLOT_INDEX until AddToWorld.
    [[nodiscard]] uint32 SlotIndex() const { return _slotIndex; }
    void SetSlotIndex(uint32 index) { _slotIndex = index; }

private:
    [[nodiscard]] uint32 WriteIndex() const { return (_publishedBeat.load(std::memory_order_relaxed) + 1) & 1; }

    ObjectSlot _slot[2];
    std::atomic<uint32> _publishedBeat{0};
    uint32 _slotIndex = NO_SLOT_INDEX;
    uint32 _valuesWords = 0;                      // words per slot in _storage (values + mask)
    std::unique_ptr<uint32[]> _storage;           // values and masks for both slots
    std::unique_ptr<SlotAura[]> _auraStorage[2];  // one aura array per slot
    uint32 _auraCapacity[2] = { 0, 0 };
    std::unique_ptr<uint8[]> _movementStorage[2]; // one movement block per slot
    uint32 _movementCapacity[2] = { 0, 0 };
    uint32 _regrowths = 0;
};

// Hands out dense slot indices. Taken at add/remove only, which happen between beats or at the seal.
// Step 16: an index released in beat k addresses record heads written in beat k that are still readable in
// beat k + 1, so it is not handed out again before beat k + 1 has begun (the free list is beat-tagged).
class SlotIndexAllocator
{
public:
    static SlotIndexAllocator* instance();

    uint32 Allocate();
    void Release(uint32 index);
    [[nodiscard]] uint32 Capacity() const;
    // Indices on the free list (Capacity() − FreeCount() are in use).
    [[nodiscard]] uint32 FreeCount() const;

private:
    struct Freed
    {
        uint32 index;
        uint32 beat;    // BeatContext::beat at Release
    };

    std::deque<Freed> _free;   // oldest release first
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
    [[nodiscard]] bool HasAuraEffect(uint32 spellId, uint8 effIndex) const;
    [[nodiscard]] SlotAura const* FindAura(uint32 spellId, ObjectGuid caster = ObjectGuid::Empty) const;
    // Sum of amount[i] over slot auras whose effect i applies the given aura type.
    [[nodiscard]] int32 TotalAuraModifier(AuraType type) const;
    // Unit::HasAuraType / HasAuraTypeWithCaster on the slot.
    [[nodiscard]] bool HasAuraType(AuraType type) const;
    [[nodiscard]] bool HasAuraTypeWithCaster(AuraType type, ObjectGuid caster) const;
    // Unit::BuildAuraStateUpdateForTarget on the slot: UNIT_FIELD_AURASTATE with the per-caster states
    // recomputed for the given viewer (step 13: a viewer's update build must not read the owner's aura maps).
    [[nodiscard]] uint32 AuraStateForTarget(ObjectGuid target) const;

    [[nodiscard]] uint32 Health() const;
    [[nodiscard]] uint32 MaxHealth() const;
    [[nodiscard]] uint8 Level() const;
    [[nodiscard]] uint32 Faction() const;
    [[nodiscard]] bool IsAlive() const;
};

#endif
