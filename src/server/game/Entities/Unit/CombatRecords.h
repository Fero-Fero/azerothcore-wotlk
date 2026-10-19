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

#ifndef ACORE_COMBAT_RECORDS_H
#define ACORE_COMBAT_RECORDS_H

#include "Define.h"
#include "ObjectGuid.h"
#include <type_traits>

// Step 07 slice 5 (plan §7.3, §8.1). Unit::DealDamage, Unit::DealHeal, Unit::Kill and Unit::ProcSkillsAndAuras
// run on the victim's task. The half of each that writes the other unit (attacker, healer, killer, proc actor)
// becomes one of these records when that unit is foreign to the running task, and is applied on its own task
// at the start of its next update. Outside a task, or when the other unit is in the task group, the writes
// stay inline as before.

constexpr uint8 MAX_PROC_APPLIED_MODS = 8;

// Attacker half of Unit::DealDamage.
struct DamageDealtRecord
{
    ObjectGuid victim;
    uint32 damage = 0;              // after mitigation, what DealDamage returned
    uint32 rageDamage = 0;          // damage + absorbed, Xinef's rage base
    uint32 absorbed = 0;            // CleanDamage::absorbed_damage
    uint32 spellId = 0;
    uint32 schoolMask = 0;
    uint8 damageType = 0;           // DamageEffectType
    uint8 attackType = 0;           // CleanDamage::attackType
    uint8 hitOutcome = 0;           // CleanDamage::hitOutCome
    uint8 aiHook = 0;               // GetAI()->DamageDealt
    uint8 damageShield = 0;         // DealDamageShieldDamage(victim)
    uint8 rageFromDamage = 0;       // RewardRage(rageDamage, weaponSpeedHitFactor, true)
    uint8 rageFromAbsorb = 0;       // RewardRage(absorbed, 0, true) on a fully absorbed hit
    uint8 bgScore = 0;              // Battleground SCORE_DAMAGE_DONE + ACHIEVEMENT_CRITERIA_TYPE_DAMAGE_DONE
    uint8 durabilityRoll = 0;       // player attacker: random durability point loss (HIT DONE)

    [[nodiscard]] bool Any() const { return aiHook || damageShield || rageFromDamage || rageFromAbsorb || bgScore || durabilityRoll; }
};

// Healer half of Unit::DealHeal. One record to the healer (AI hook) and one to the player credited with the
// heal (the healer or a totem's owner) for the battleground score.
struct HealDoneRecord
{
    ObjectGuid target;
    uint32 heal = 0;                // addhealth
    uint32 gain = 0;                // effective
    uint8 aiHook = 0;
    uint8 bgScore = 0;
};

// Killer half of Unit::Kill that is not a reward: achievement, AI hooks. One record per unit that writes.
struct KillingBlowRecord
{
    ObjectGuid victim;
    uint32 victimEntry = 0;
    uint8 victimIsPlayer = 0;
    uint8 killingBlowAchievement = 0;   // player: ACHIEVEMENT_CRITERIA_TYPE_GET_KILLING_BLOWS
    uint8 aiKilledUnit = 0;             // creature (killer or the killer's pet): AI()->KilledUnit(victim)
};

// One group member's share of a kill (plan §7.3a). Everything KillRewarder::_RewardPlayer reads from the
// rewarder, so the member runs it on its own task with the victim as a handle.
struct KillCreditRecord
{
    ObjectGuid victim;
    ObjectGuid killer;
    uint32 victimEntry = 0;
    uint32 xp = 0;                  // KillRewarder::_xp
    uint32 aliveSumLevel = 0;
    uint32 count = 0;
    float groupRate = 1.0f;
    uint8 victimLevel = 0;
    uint8 maxLevel = 0;
    uint8 maxNotGrayMemberLevel = 0;
    uint8 hasMaxNotGrayMember = 0;
    uint8 isFullXP = 0;
    uint8 isDungeon = 0;
    uint8 isBattleGround = 0;
    uint8 isPvP = 0;
    uint8 inGroup = 0;
};

// Actor half of Unit::ProcSkillsAndAuras: skills/reactives and aura procs on the attacker or healer.
struct ProcActorRecord
{
    ObjectGuid target;              // the victim / heal target
    uint32 procAttacker = 0;
    uint32 procExtra = 0;
    uint32 amount = 0;
    uint32 spellTypeMask = 0;
    uint32 procPhase = 0;
    uint32 procSpellId = 0;         // procSpellInfo
    uint32 procAuraId = 0;
    int8 procAuraIdx = -1;
    uint8 attType = 0;
    uint8 procDisabled = 0;         // procSpell->IsProcDisabled()
    uint8 hasDamage = 0;
    uint8 hasHeal = 0;
    // DamageInfo
    uint32 dmgDamage = 0;
    uint32 dmgAbsorb = 0;
    uint32 dmgResist = 0;
    uint32 dmgBlock = 0;
    uint32 dmgClean = 0;
    uint32 dmgSchoolMask = 0;
    uint32 dmgSpellId = 0;
    uint32 dmgHitMask = 0;
    uint8 dmgType = 0;
    uint8 dmgAttackType = 0;
    // HealInfo
    uint32 healHeal = 0;
    uint32 healBeforeTakenMods = 0;
    uint32 healEffective = 0;
    uint32 healAbsorb = 0;
    uint32 healSpellId = 0;
    uint32 healSchoolMask = 0;
    uint32 healHitMask = 0;
    // procSpell->m_appliedMods as aura ids, for the mod owner's procs (Cobra Strikes)
    uint32 appliedModAuraIds[MAX_PROC_APPLIED_MODS] = {};
    uint8 appliedModCount = 0;
};

static_assert(std::is_trivially_copyable_v<DamageDealtRecord>, "record payloads are plain values");
static_assert(std::is_trivially_copyable_v<HealDoneRecord>, "record payloads are plain values");
static_assert(std::is_trivially_copyable_v<KillingBlowRecord>, "record payloads are plain values");
static_assert(std::is_trivially_copyable_v<KillCreditRecord>, "record payloads are plain values");
static_assert(std::is_trivially_copyable_v<ProcActorRecord>, "record payloads are plain values");

#endif
