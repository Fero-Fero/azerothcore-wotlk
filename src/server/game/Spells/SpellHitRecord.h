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

#ifndef ACORE_SPELL_HIT_RECORD_H
#define ACORE_SPELL_HIT_RECORD_H

#include "AuraLaunchData.h"
#include "Define.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"
#include "Spell.h"
#include <type_traits>

// The spell pipeline split in two records (step 07):
//
//   caster task:  launch  ──SpellHitRecord──▶  target task:  land  ──SpellLandedRecord──▶  caster task:  feedback
//
// SpellHitRecord carries everything the land step reads from the cast and from the launch-side TargetInfo.
// SpellLandedRecord carries every write the land step wants to do on the caster. Both are plain values;
// a pointer never crosses. SpellValue (Spell.h) is already plain and is embedded by value; SpellDestination
// travels as SpellDestRecord (AuraLaunchData.h).

enum FollowUpMode : uint8
{
    FOLLOWUP_CAST       = 0,   // Unit::CastCustomSpell / CastSpell on the caster, at the record's target
    FOLLOWUP_ADD_AURA   = 1    // Unit::AddAura(spellId, target) - no cast, mirrors the pre-cast aura path
};

// Which unit of the cast performs an action in a SpellLandedRecord. Today's DoAllEffectOnTarget writes three
// units: m_caster, the "effect caster" (m_originalCaster when set, else m_caster) and m_originalCaster itself.
enum FeedbackActor : uint8
{
    FEEDBACK_BY_CASTER          = 0,   // SpellLandedRecord::caster
    FEEDBACK_BY_EFFECT_CASTER   = 1    // SpellLandedRecord::effectCaster
};

// A cast the land step wants the caster to perform. Applied at feedback; the new Spell lands next beat.
struct FollowUpCast
{
    uint32 spellId = 0;
    ObjectGuid target;
    uint32 triggerFlags = 0;                // TriggerCastFlags
    int32 bp[MAX_SPELL_EFFECTS] = {};       // CastCustomSpell base points, valid where bpMask has the bit
    uint8 bpMask = 0;
    uint8 mode = FOLLOWUP_CAST;             // FollowUpMode
    uint8 by = FEEDBACK_BY_CASTER;          // FeedbackActor
    int32 auraDuration = 0;                 // SPELLVALUE_AURA_DURATION when != 0 (ADD_TARGET_TRIGGER duration sync)
    ObjectGuid originalCaster;
    // Step 08 §2: a cast a periodic tick asked for carries the ticking aura as ids (Spell::m_triggeredByAuraSpell)
    uint32 triggeredByAuraId = 0;
    int8 triggeredByAuraEffIndex = -1;
    uint32 triggeredByAuraTick = 0;
};

// One SPELL_AURA_ADD_TARGET_TRIGGER entry of Spell::m_hitTriggerSpells, as ids.
struct SpellHitTrigger
{
    uint32 triggeredSpellId = 0;
    uint32 triggeredByAuraId = 0;
    uint8 triggeredByEffIdx = 0;
    int32 chance = 0;
};

constexpr uint8 MAX_SPELL_HIT_TRIGGERS = 6;
constexpr uint8 MAX_SPELL_FOLLOWUPS = 8;     // pre-cast aura + interrupt + up to MAX_SPELL_HIT_TRIGGERS target triggers
constexpr uint8 MAX_SPELL_APPLIED_MODS = 8;  // Spell::m_appliedMods entries carried as ids

struct SpellHitRecord
{
    // identity
    uint32 spellId = 0;                     // difficulty-resolved id (m_spellInfo->Id)
    uint32 castSerial = 0;                  // Spell::m_castSerial, per-process atomic
    ObjectGuid target;                      // TargetInfo::targetGUID (the log keys by target; the Deferred list needs it)
    ObjectGuid caster;                      // m_caster
    ObjectGuid originalCaster;              // m_originalCasterGUID
    ObjectGuid castItemGuid;                // m_castItemGUID
    uint8 casterTypeId = 0;                 // TYPEID_PLAYER / UNIT / GAMEOBJECT
    uint32 triggerFlags = 0;                // _triggeredCastFlags
    uint8 spellFlags = 0;                   // m_spellFlags (SPELL_FLAG_REFLECTED, ...)
    uint32 appliedModAuraIds[MAX_SPELL_APPLIED_MODS] = {};   // m_appliedMods as aura spell ids (procs: PROC_ATTR_REQ_SPELLMOD)
    uint8 appliedModCount = 0;
    uint8 unitTargetCount[MAX_SPELL_EFFECTS] = {};   // launch unit targets per effect (Spell::GetUnitTargetCountForEffect)
    uint8 unitTargetIndex[MAX_SPELL_EFFECTS] = {};   // this target's position among them, launch order

    // rolled at launch (TargetInfo)
    uint8 effectMask = 0;
    uint8 missCondition = 0;                // SpellMissInfo
    uint8 reflectResult = 0;                // SpellMissInfo
    uint8 crit = 0;
    uint8 aliveAtLaunch = 0;
    uint8 scaleAura = 0;
    uint8 wasDelayed = 0;                   // getState() == SPELL_STATE_DELAYED at emission (Sanctuary windows)
    int32 damage = 0;                       // TargetInfo::damage: > 0 damage, < 0 heal; done-side + crit (§5.1)
    int32 damageBeforeTakenMods = 0;        // Beacon of Light
    int32 effDamage[MAX_SPELL_EFFECTS] = {};   // TargetInfo::effDamage: per-effect share of damage
    uint8 takenKind[MAX_SPELL_EFFECTS] = {};   // TargetInfo::takenKind: SpellTakenKind, taken-side bonus at land (§5.2)
    uint64 timeDelay = 0;                   // TargetInfo::timeDelay

    // cast data
    uint32 schoolMask = 0;                  // m_spellSchoolMask
    uint8 attackType = 0;                   // m_attackType
    int32 castTime = 0;                     // m_casttime (procs: SPELL_AURA_MOD_CASTING_SPEED_NOT_STACK)
    int32 powerCost = 0;                    // m_powerCost
    ObjectGuid originalTarget;              // m_originalTargetGUID (Spell::GetOriginalTarget in procs)
    uint32 procAttacker = 0;                // after prepareDataForTriggerSystem
    uint32 procVictim = 0;
    uint32 procEx = 0;
    SpellValue value;                       // *m_spellValue
    uint32 triggeredByAuraSpellId = 0;
    int8 triggeredByAuraEffIndex = -1;
    uint32 triggeredByAuraTick = 0;
    uint32 preCastSpell = 0;                // m_preCastSpell
    SpellHitTrigger hitTriggers[MAX_SPELL_HIT_TRIGGERS];
    uint8 hitTriggerCount = 0;
    uint8 hasDst = 0;                       // m_targets.HasDst()
    SpellDestRecord dst;                    // *m_targets.GetDst()
    SpellDestRecord effectDst[MAX_SPELL_EFFECTS];    // m_destTargets
    ObjectGuid primaryUnitTarget;           // m_targets.GetUnitTargetGUID(), for the combo-point rule
    uint8 needComboPoints = 0;              // m_needComboPoints at emission
    uint8 comboPoints = 0;                  // the caster's combo points at emission, refunded on a land-side evade

    // Aura amounts, amplitude, charges and caster-side duration computed at launch (§5.3).
    AuraLaunchData aura;

    SpellHitRecord() = default;
};

struct SpellLandedRecord
{
    // identity
    uint32 spellId = 0;
    uint32 castSerial = 0;
    ObjectGuid target;                      // the TargetInfo's unit (effectUnit)
    ObjectGuid hitTarget;                   // the unit the effects ran on: target, or the caster for a reflected hit
    ObjectGuid caster;                      // m_caster
    ObjectGuid originalCaster;              // m_originalCasterGUID
    ObjectGuid effectCaster;                // originalCaster when set, else caster (today's `caster` local)

    uint8 effectMask = 0;
    uint8 missAfterLand = 0;                // SpellMissInfo returned by DoSpellHitOnUnit; the caster sends SMSG_SPELLLOGMISS
    uint8 hitLanded = 0;                    // DoSpellHitOnUnit returned SPELL_MISS_NONE

    // numbers (applied by effectCaster)
    uint8 lastDamagedTarget = 0;            // SetLastDamagedTargetGuid(hitTarget)
    uint32 hitMask = 0;                     // PROC_HIT_* after mitigation
    uint32 procVictim = 0;                  // for CastItemCombatSpell
    uint8 attackType = 0;
    uint8 castItemCombatSpell = 0;          // Player::CastItemCombatSpell(hitTarget, attackType, procVictim, hitMask)

    // numbers (applied by caster)
    int32 healthLeechGain = 0;              // SPELL_EFFECT_HEALTH_LEECH, before the caster's done/taken bonus
    uint8 comboPointsRefund = 0;            // land-side evade on the primary target: give these back
    int8 comboPointGain = 0;                // AddComboPointGain from HIT_TARGET handlers

    // actions (applied by caster unless noted)
    uint8 atTargetAttacked = 0;             // Unit::AtTargetAttacked(target, canInitialAggro)
    uint8 canInitialAggro = 0;
    uint8 assistCombat = 0;                 // SetInCombatWith(target), friendly target in combat
    uint8 inheritCombatStates = 0;          // originalCaster: GetCombatManager().InheritCombatStatesFrom(hitTarget)
    uint8 revealCaster = 0;                 // failed pickpocket: RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TALK)
    uint8 setContestedPvP = 0;
    uint8 updatePvP = 0;
    uint8 casterAchievements = 0;           // ACHIEVEMENT_TIMED_TYPE_SPELL_CASTER, ACHIEVEMENT_CRITERIA_TYPE_CAST_SPELL2
    uint8 aiSpellHitTarget = 0;             // CreatureAI::SpellHitTarget(hitTarget, spellInfo)
    uint32 cooldownSpellId = 0;             // relic cooldown (SPELL_RELIC_COOLDOWN, item entry, ms)
    uint32 cooldownItemEntry = 0;
    int32 cooldownMs = 0;
    FollowUpCast follow[MAX_SPELL_FOLLOWUPS];
    uint8 followCount = 0;

    SpellLandedRecord() = default;

    // True when nothing in the record asks the caster to do anything.
    [[nodiscard]] bool IsEmpty() const
    {
        return !missAfterLand && !lastDamagedTarget && !castItemCombatSpell && !healthLeechGain && !comboPointsRefund
            && !comboPointGain && !atTargetAttacked && !assistCombat && !inheritCombatStates && !revealCaster
            && !setContestedPvP && !updatePvP && !casterAchievements && !aiSpellHitTarget && !cooldownMs && !followCount;
    }

    // Returns nullptr when the follow-up list is full; the caller then performs the cast inline.
    FollowUpCast* AddFollowUp(uint32 spellId, ObjectGuid const& target, uint32 triggerFlags, uint8 mode = FOLLOWUP_CAST, uint8 by = FEEDBACK_BY_CASTER)
    {
        if (followCount >= MAX_SPELL_FOLLOWUPS)
            return nullptr;
        FollowUpCast& f = follow[followCount++];
        f = FollowUpCast();
        f.spellId = spellId;
        f.target = target;
        f.triggerFlags = triggerFlags;
        f.mode = mode;
        f.by = by;
        return &f;
    }
};

// Applied by the coordinator with both units live (plan §11). On this slice it is landed inline at emission.
struct DeferredSpellHitRecord
{
    SpellHitRecord hit;
};

// Step 08 §2: the caster half of a periodic tick. The aura is owned by the target, which ticks it on its own
// task; everything the tick handlers wrote on the caster (self heal of a leech, health funnel cost, drained
// power, assist threat, trigger casts) travels back in one of these per tick and is applied by
// Unit::ApplyPeriodicLanded. Victim-side writes (damage, heal, logs, victim procs) stay inline on the target;
// DealDamage / DealHeal / ProcSkillsAndAuras already route their own caster halves (step 07 slice 5).
constexpr uint8 MAX_PERIODIC_FOLLOWUPS = 2;

struct PeriodicLandedRecord
{
    ObjectGuid target;                      // tick target (the aura owner)
    ObjectGuid caster;                      // the aura's caster, the record's recipient
    uint32 spellId = 0;
    uint8 effIndex = 0;
    uint8 auraType = 0;                     // AuraType
    uint8 stackAmount = 1;
    uint8 faceChannelTarget = 0;            // SPELL_ATTR1_TRACK_TARGET_IN_CHANNEL creature caster: SetInFront(channel object)
    uint32 tickNumber = 0;

    // SPELL_AURA_PERIODIC_LEECH: the caster heals itself for leechBase * CalcValueMultiplier with its own done
    // and taken bonus, then forwards half the gain as assist threat
    uint32 leechBase = 0;
    // SPELL_AURA_PERIODIC_HEALTH_FUNNEL: the caster pays this much health (ModifyHealth)
    uint32 funnelHealthCost = 0;
    // SPELL_AURA_PERIODIC_HEAL with SPELL_ATTR2_NO_TARGET_PER_SECOND_COST: DealDamage(self, SELF_DAMAGE)
    uint32 selfDamage = 0;
    // SPELL_AURA_PERIODIC_MANA_LEECH gain, Aura::Update ManaPerSecond cost (negative)
    int32 powerGain = 0;
    uint8 powerType = 0;                    // Powers
    uint8 manaFeedPct = 0;                  // Drain Mana + Mana Feed: cast 32554 with this percent of the gained power
    uint8 restoration = 0;                  // 24379 / 23493: heal self 10% max health, energize 10% max mana
    uint8 triggerHasBp0 = 0;
    // Aura::Update ManaPerSecond with POWER_HEALTH: ModifyHealth(healthDelta)
    int32 healthDelta = 0;
    // ForwardThreatForAssistingMe on the caster's own threat manager (leech heal)
    float assistThreat = 0.0f;

    // The periodic trigger cast when NeedsToBeTriggeredByCaster: performed by the caster at feedback with
    // m_triggeredByAuraSpell rebuilt from spellId / effIndex / tickNumber.
    uint32 triggerSpellId = 0;
    ObjectGuid triggerTarget;
    uint32 triggerFlags = 0;                // TriggerCastFlags
    int32 triggerBp0 = 0;
    uint8 hasChannelData = 0;
    uint8 triggerAuraPeriodic = 0;          // Spell::prepare interrupts the channel of a player caster on a failed cast
    ObjectGuid channelObject;
    SpellDestRecord channelDst;

    FollowUpCast follow[MAX_PERIODIC_FOLLOWUPS];
    uint8 followCount = 0;

    [[nodiscard]] bool IsEmpty() const
    {
        return !faceChannelTarget && !leechBase && !funnelHealthCost && !selfDamage && !powerGain && !restoration
            && !healthDelta && assistThreat == 0.0f && !triggerSpellId && !followCount;
    }

    // byAura: the inline code passed `this` (the ticking AuraEffect) as triggeredByAura
    FollowUpCast* AddFollowUp(uint32 followSpellId, ObjectGuid const& followTarget, uint32 followTriggerFlags, bool byAura = true)
    {
        if (followCount >= MAX_PERIODIC_FOLLOWUPS)
            return nullptr;
        FollowUpCast& f = follow[followCount++];
        f = FollowUpCast();
        f.spellId = followSpellId;
        f.target = followTarget;
        f.triggerFlags = followTriggerFlags;
        if (byAura)
        {
            f.triggeredByAuraId = spellId;
            f.triggeredByAuraEffIndex = int8(effIndex);
            f.triggeredByAuraTick = tickNumber;
        }
        return &f;
    }
};

static_assert(std::is_trivially_copyable_v<SpellDestRecord>, "record payloads are plain values");
static_assert(std::is_trivially_copyable_v<SpellValue>, "SpellValue is embedded by value in SpellHitRecord");
static_assert(std::is_trivially_copyable_v<FollowUpCast>, "record payloads are plain values");
static_assert(std::is_trivially_copyable_v<SpellHitRecord>, "record payloads are plain values");
static_assert(std::is_trivially_copyable_v<SpellLandedRecord>, "record payloads are plain values");
static_assert(std::is_trivially_copyable_v<DeferredSpellHitRecord>, "record payloads are plain values");
static_assert(std::is_trivially_copyable_v<PeriodicLandedRecord>, "record payloads are plain values");

#endif
