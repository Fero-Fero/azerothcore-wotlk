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

#ifndef ACORE_UNIT_RECORDS_H
#define ACORE_UNIT_RECORDS_H

#include "Define.h"
#include "ObjectGuid.h"
#include <cstddef>
#include <type_traits>

// A record is a plain-value message from one unit to another, appended to the writer's log and applied
// by the target at the start of its next task. Payloads hold guids, ids and numbers. Never a pointer.

enum class RecordKind : uint8
{
    SpellHit, SpellLanded, DeferredSpellHit,          // 07
    MeleeHit, MeleeLanded,                            // 08
    PeriodicLanded, AreaAuraApply, AreaAuraRemove,    // 08
    ThreatBackRef, CombatRef, CombatEnd,              // 06
    AuraRemove, Knockback,                            // 06
    ThreatRemove, MinionUnlink, GameObjectUnlink,     // 06: removal prepared on the dying unit's own task
    VisibilityLink, VisibilityUnlink, VisibilityPoke, // 10
    Wake,                                             // 06: empty record, only forces a task
    Threat,                                           // 06: AddThreat against a foreign unit
    DamageDealt, HealDone, KillingBlow, KillCredit,   // 07 slice 5: attacker/healer/killer half of DealDamage/DealHeal/Kill
    ProcActor,                                        // 07 slice 5: actor half of ProcSkillsAndAuras

    Max
};

char const* RecordKindName(RecordKind kind);

struct RecordHeader
{
    ObjectGuid target;
    ObjectGuid source;
    uint32 nextInTarget;   // byte offset of the previous record for the same target in this log, or NO_RECORD
    uint32 sequence;       // per-log monotonic, for a stable sort after source guid
    uint16 size;           // payload bytes
    RecordKind kind;
    uint8 depth;           // step 15: chain depth, 1 + the depth of the record being applied when this was written
                           // (1 for a task's own action: a hit is 1, the proc cast it triggers landing is 2, ...)

    static constexpr uint32 NO_RECORD = ~0u;
};

// The payload follows the header 8-byte aligned.
constexpr std::size_t RECORD_HEADER_SIZE = (sizeof(RecordHeader) + 7) & ~std::size_t(7);
constexpr std::size_t RecordAlignUp8(std::size_t n) { return (n + 7) & ~std::size_t(7); }

template <typename T>
T const& RecordPayload(uint8 const* payload)
{
    static_assert(std::is_trivially_copyable_v<T>, "record payloads are plain values");
    return *reinterpret_cast<T const*>(payload);
}

// ---- Kinds of this step ----

// Victim side of a new threat list entry: "I (victimGuid) am now on source's threat list".
struct ThreatBackRefRecord
{
    ObjectGuid victimGuid;
};

// Other half of a combat reference. Upsert: a half that already exists is refreshed instead.
struct CombatRefRecord
{
    ObjectGuid otherGuid;
    uint8 isPvP;
    uint8 suppressed;      // target's own side starts suppressed (vanish, feign death, missiles in flight)
};

struct CombatEndRecord
{
    ObjectGuid otherGuid;
    uint8 isPvP;
};

// AddThreat called against a foreign unit. The target adds the threat to its own list.
struct ThreatRecord
{
    ObjectGuid attackerGuid;
    float amount;
    uint32 spellId;
    uint8 ignoreModifiers;
    uint8 ignoreRedirects;
};

struct AuraRemoveRecord
{
    ObjectGuid casterGuid;
    uint32 spellId;
    uint8 reqEffMask;
    uint8 removeMode;      // AuraRemoveMode
};

struct KnockbackRecord
{
    float x;
    float y;
    float speedXY;
    float speedZ;
};

// "Drop deadGuid from your threat list and from your threatened-by-me set." With backRefOnly the sender only
// stopped threatening you (ClearThreat, evade): drop the threatened-by-me entry and keep your own threat on it.
struct ThreatRemoveRecord
{
    ObjectGuid deadGuid;
    uint8 backRefOnly = 0;
};

enum MinionUnlinkFlags : uint8
{
    MINION_UNLINK_GUARDIAN_PET = 0x01,
    MINION_UNLINK_MINIPET      = 0x02,
    MINION_UNLINK_TOTEM        = 0x04
};

// The owner's half of SetMinion(minion, false). Carries what the owner would have read from the minion.
struct MinionUnlinkRecord
{
    ObjectGuid minionGuid;
    uint32 createdBySpell;
    uint32 totemSpell;
    uint8 flags;           // MinionUnlinkFlags
};

// The owner's half of RemoveGameObject(go, false).
struct GameObjectUnlinkRecord
{
    ObjectGuid goGuid;
    uint32 spellId;
};

// Step 10: a viewer entered / left this object's visible-players map (the target-side half of the link).
struct VisibilityLinkRecord
{
    ObjectGuid viewer;
};

struct VisibilityUnlinkRecord
{
    ObjectGuid viewer;
};

// Step 10: an object asks a player to re-evaluate its visibility at the next view phase.
// An empty guid asks for a full scan (shared-vision mover, forced update).
struct VisibilityPokeRecord
{
    ObjectGuid object;
};

struct WakeRecord
{
    uint8 unused;
};

#endif
