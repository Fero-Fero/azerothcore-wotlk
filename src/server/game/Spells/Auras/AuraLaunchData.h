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

#ifndef ACORE_AURA_LAUNCH_DATA_H
#define ACORE_AURA_LAUNCH_DATA_H

#include "Define.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"
#include <type_traits>

struct SpellDestination;

// SpellDestination as plain values (Position has a user-provided copy constructor, so SpellDestination is not
// trivially copyable and cannot sit in a record).
struct SpellDestRecord
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float o = 0.0f;
    uint32 mapId = MAPID_INVALID;
    ObjectGuid transportGuid;
    float tx = 0.0f;
    float ty = 0.0f;
    float tz = 0.0f;
    float to = 0.0f;

    void Set(SpellDestination const& dest);
    [[nodiscard]] SpellDestination Get() const;
};

// Everything the caster computes for the unit-owned aura a hit creates on its target (plan 07 §5.3).
// Computed in Spell::DoAllEffectOnLaunchTarget by Aura::ComputeLaunchData, carried in TargetInfo and
// SpellHitRecord, consumed by Aura::TryRefreshStackOrCreateFromHit on the target. Caster auras, spell mods,
// spell power, haste and item suffix data are inside these numbers; the land step does not read the caster
// to build the aura.
struct AuraLaunchData
{
    uint32 spellId = 0;                             // aurSpellInfo->Id after GetAuraRankForLevel; 0 = nothing computed
    uint8 effMask = 0;                              // effects the launch computed (BuildEffectMaskForOwner result)
    uint8 canBeRecalculatedMask = 0;                // AuraEffect::m_canBeRecalculated
    uint8 periodicMask = 0;                         // AuraEffect::m_isPeriodic
    uint8 charges = 0;                              // Aura::CalcMaxCharges(caster)
    uint8 casterLevel = 0;                          // Aura::m_casterLevel / AuraEffect::m_casterLevel
    uint8 applyResilience = 0;                      // AuraEffect::m_applyResilience
    uint8 casterPresent = 0;                        // a unit caster existed at launch (CalculatePeriodicData ran)
    uint8 casterIsTotem = 0;                        // caster->IsTotem() (SPELL_ATTR5_EXTRA_INITIAL_PERIOD timer)
    uint8 hasChannelData = 0;                       // AuraEffect::m_channelData present
    int32 maxDuration = 0;                          // Aura::CalcMaxDuration(caster): SPELLMOD_DURATION applied
    int32 duration = 0;                             // Aura::GetDuration() after effect init (totem initial period)
    float hasteMultiplier = 1.0f;                   // UNIT_MOD_CAST_SPEED when SPELL_AURA_PERIODIC_HASTE / ATTR5 applies
    int32 baseAmount[MAX_SPELL_EFFECTS] = {};       // AuraEffect::m_baseAmount
    int32 amount[MAX_SPELL_EFFECTS] = {};           // AuraEffect::m_amount (stack 1)
    int32 oldAmount[MAX_SPELL_EFFECTS] = {};        // AuraEffect::m_oldAmount
    int32 amplitude[MAX_SPELL_EFFECTS] = {};        // AuraEffect::m_amplitude (SPELLMOD_ACTIVATION_TIME, haste applied)
    int32 periodicTimer[MAX_SPELL_EFFECTS] = {};    // AuraEffect::m_periodicTimer after CalculatePeriodic(create)
    float pctMods[MAX_SPELL_EFFECTS] = {};          // AuraEffect::m_pctMods
    float critChance[MAX_SPELL_EFFECTS] = {};       // AuraEffect::m_critChance
    ObjectGuid channelObject;                       // ChannelTargetData::channelGUID
    SpellDestRecord channelDst;                     // ChannelTargetData::spellDst

    [[nodiscard]] bool IsSet() const { return spellId != 0 && effMask != 0; }
};

static_assert(std::is_trivially_copyable_v<SpellDestRecord>);
static_assert(std::is_trivially_copyable_v<AuraLaunchData>);

#endif
