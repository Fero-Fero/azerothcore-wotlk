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

// Step 07: the spell hit split. Launch stays on the caster's task (Spell::cast and friends in Spell.cpp);
// every unit target other than the caster becomes a SpellHitRecord, landed by a Spell rebuilt on the target
// (Spell::Spell(SpellHitRecord const&, ...) + Spell::Land), whose caster-side writes travel back as a
// SpellLandedRecord (Unit::ApplySpellLanded).
//
// Inside a task (a unit's update in Map::Update) the hit record goes through the step 06 log and lands at the
// start of the target's next update; feedback goes back the same way and is applied at the start of the
// caster's update after that. Outside a task (unit tests, casts between beats) the record is consumed on the
// spot with the launch spell's scripts. Spells in the Deferred table land at the end of Map::Update with no
// task running.

#include "BeatContext.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellHitRecord.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "Unit.h"
#include "UnitRecordLog.h"
#include "UnitView.h"
#include <algorithm>
#include <atomic>
#include <cstring>

uint32 Spell::NextCastSerial()
{
    static std::atomic<uint32> serial{0};
    return ++serial;
}

// ---- SpellDestRecord ----

void SpellDestRecord::Set(SpellDestination const& dest)
{
    x = dest._position.GetPositionX();
    y = dest._position.GetPositionY();
    z = dest._position.GetPositionZ();
    o = dest._position.GetOrientation();
    mapId = dest._position.GetMapId();
    transportGuid = dest._transportGUID;
    tx = dest._transportOffset.GetPositionX();
    ty = dest._transportOffset.GetPositionY();
    tz = dest._transportOffset.GetPositionZ();
    to = dest._transportOffset.GetOrientation();
}

SpellDestination SpellDestRecord::Get() const
{
    SpellDestination dest(x, y, z, o, mapId);
    dest._transportGUID = transportGuid;
    dest._transportOffset.Relocate(tx, ty, tz, to);
    return dest;
}

// ---- land-side Spell ----

Spell::Spell(SpellHitRecord const& hit, Unit* target, WorldObject* casterHandle, SpellLandedRecord* feedback) :
    m_spellInfo(sSpellMgr->GetSpellInfo(hit.spellId)),
    m_caster(casterHandle),
    m_spellValue(new SpellValue(hit.value)),
    _spellEvent(nullptr)
{
    ASSERT(m_spellInfo, "land-side Spell for unknown spell {}", hit.spellId);
    ASSERT(target && casterHandle);

    m_castSerial = hit.castSerial;
    _landSide = true;
    _landTarget = target;
    _landFeedback = feedback;
    _launchComboPoints = hit.comboPoints;

    // Defaults the launch-side constructor sets and the hit path may read.
    m_customError = SPELL_CUSTOM_ERROR_NONE;
    m_skipCheck = true;
    m_selfContainer = nullptr;
    m_referencedFromCurrentSpell = false;
    m_executedCurrently = false;
    m_needComboPoints = hit.needComboPoints != 0;
    m_comboPointGain = 0;
    m_comboTarget = nullptr;
    m_delayStart = 0;
    m_delayAtDamageCount = 0;
    m_applyMultiplierMask = 0;
    m_auraScaleMask = 0;
    memset(m_damageMultipliers, 0, sizeof(m_damageMultipliers));

    m_attackType = WeaponAttackType(hit.attackType);
    m_spellSchoolMask = SpellSchoolMask(hit.schoolMask);

    m_originalCasterGUID = hit.originalCaster;
    if (m_originalCasterGUID == m_caster->GetGUID())
        m_originalCaster = m_caster->ToUnit();
    else
    {
        m_originalCaster = ObjectAccessor::GetUnit(*target, m_originalCasterGUID);
        if (m_originalCaster && !m_originalCaster->IsInWorld())
            m_originalCaster = nullptr;
    }

    // The Sanctuary windows in DoAllEffectOnTarget compare against SPELL_STATE_DELAYED only.
    m_spellState = hit.wasDelayed ? SPELL_STATE_DELAYED : SPELL_STATE_FINISHED;
    _triggeredCastFlags = TriggerCastFlags(hit.triggerFlags);

    // The cast item is resolved through the caster handle (live on this slice); slice 3 carries the entry.
    m_castItemGUID = hit.castItemGuid;
    m_CastItem = nullptr;
    if (m_castItemGUID)
        if (Player* casterPlayer = m_caster->ToPlayer())
            m_CastItem = casterPlayer->GetItemByGuid(m_castItemGUID);

    // Procs run at land until slice 5 and read procSpell->m_appliedMods for the caster's mod auras
    // (PROC_ATTR_REQ_SPELLMOD). Resolve the ids on the caster handle; slice 5 moves this to the actor side.
    if (Unit* casterUnit = m_caster->ToUnit())
        for (uint8 i = 0; i < hit.appliedModCount && i < MAX_SPELL_APPLIED_MODS; ++i)
            if (Aura* mod = casterUnit->GetAura(hit.appliedModAuraIds[i]))
                m_appliedMods.insert(mod);
    m_weaponItem = nullptr;

    unitTarget = nullptr;
    itemTarget = nullptr;
    gameObjTarget = nullptr;
    destTarget = nullptr;
    damage = 0;
    m_reflectionTarget = nullptr;
    m_reflectionTargetGuid.Clear();
    effectHandleMode = SPELL_EFFECT_HANDLE_HIT_TARGET;
    m_diminishLevel = DIMINISHING_LEVEL_1;
    m_diminishGroup = DIMINISHING_NONE;
    m_damage = 0;
    m_healing = 0;
    m_damageBeforeTakenMods = 0;
    m_pendingTakenKind = TAKEN_NONE;
    m_procAttacker = hit.procAttacker;
    m_procVictim = hit.procVictim;
    m_procEx = hit.procEx;
    focusObject = nullptr;
    m_cast_count = 0;
    m_glyphIndex = 0;
    m_preCastSpell = hit.preCastSpell;
    m_spellAura = nullptr;
    _scriptsLoaded = false;
    m_autoRepeat = false;
    m_runesState = 0;
    m_powerCost = hit.powerCost;
    m_launchScriptValue = hit.scriptValue;
    m_casttime = hit.castTime;
    m_originalTargetGUID = hit.originalTarget;
    m_timer = 0;
    m_channeledDuration = 0;
    m_immediateHandled = true;
    m_channelTargetEffectMask = 0;
    m_spellFlags = hit.spellFlags;
    m_canReflect = false;

    CleanupTargetList();
    memset(m_effectExecuteData, 0, MAX_SPELL_EFFECTS * sizeof(ByteBuffer*));
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        m_destTargets[i] = hit.effectDst[i].Get();
    _spellTargetsSelected = true;

    if (hit.triggeredByAuraSpellId)
    {
        m_triggeredByAuraSpell.spellInfo = sSpellMgr->GetSpellInfo(hit.triggeredByAuraSpellId);
        m_triggeredByAuraSpell.effectIndex = hit.triggeredByAuraEffIndex;
        m_triggeredByAuraSpell.tickNumber = hit.triggeredByAuraTick;
    }

    for (uint8 i = 0; i < hit.hitTriggerCount && i < MAX_SPELL_HIT_TRIGGERS; ++i)
    {
        SpellHitTrigger const& t = hit.hitTriggers[i];
        SpellInfo const* triggered = sSpellMgr->GetSpellInfo(t.triggeredSpellId);
        SpellInfo const* triggeredBy = sSpellMgr->GetSpellInfo(t.triggeredByAuraId);
        if (triggered && triggeredBy)
            m_hitTriggerSpells.push_back(HitTriggerSpell{ triggered, triggeredBy, t.triggeredByEffIdx, t.chance });
    }

    if (hit.hasDst)
        m_targets.SetDst(hit.dst.Get());
    if (hit.primaryUnitTarget == target->GetGUID())
        m_targets.SetUnitTarget(target);

    TargetInfo info = {};
    info.targetGUID = target->GetGUID();
    info.timeDelay = hit.timeDelay;
    info.missCondition = SpellMissInfo(hit.missCondition);
    info.reflectResult = SpellMissInfo(hit.reflectResult);
    info.effectMask = hit.effectMask;
    info.processed = false;
    info.alive = hit.aliveAtLaunch != 0;
    info.crit = hit.crit != 0;
    info.scaleAura = hit.scaleAura != 0;
    info.damage = hit.damage;
    info.damageBeforeTakenMods = hit.damageBeforeTakenMods;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        info.effDamage[i] = hit.effDamage[i];
        info.takenKind[i] = hit.takenKind[i];
        _landUnitTargetCount[i] = hit.unitTargetCount[i];
        _landUnitTargetIndex[i] = hit.unitTargetIndex[i];
    }
    info.aura = hit.aura;
    m_UniqueTargetInfo.push_back(info);

    // Scripts: borrowed from the launch spell (BorrowScripts, inline landing) or loaded fresh in Land() (record
    // landing next beat). A fresh instance does not see state set in cast hooks or on other targets' hits; the
    // scripts that rely on it are listed in plan 07 §16 and move to the Deferred table or to Feedback() in slice 4.
}

void Spell::Land()
{
    ASSERT(_landSide && !m_UniqueTargetInfo.empty());

    LoadScripts();
    PrepareTargetProcessing();
    DoAllEffectOnTarget(&m_UniqueTargetInfo.front());
    FinishTargetProcessing();
}

void Spell::BorrowScripts(Spell& from)
{
    ASSERT(_landSide && m_loadedScripts.empty());
    from.LoadScripts();
    m_loadedScripts.swap(from.m_loadedScripts);
    for (SpellScript* script : m_loadedScripts)
        script->_Rebind(this);
    _scriptsLoaded = true;
    // Procs read procSpell->m_appliedMods (PROC_ATTR_REQ_SPELLMOD, mod-owner auras); slice 3 carries them as ids.
    m_appliedMods.swap(from.m_appliedMods);
    _launchSpell = &from;
}

void Spell::ReturnScripts(Spell& to)
{
    ASSERT(_launchSpell == &to);
    for (SpellScript* script : m_loadedScripts)
        script->_Rebind(&to);
    to.m_loadedScripts.swap(m_loadedScripts);
    to.m_appliedMods.swap(m_appliedMods);
    _scriptsLoaded = false;
    _launchSpell = nullptr;
}

void Spell::ApplyTakenBonuses(TargetInfo const* target, Unit* victim, Unit* caster)
{
    bool any = false;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (target->takenKind[i] != TAKEN_NONE)
            any = true;
    if (!any || !victim)
        return;

    int32 total = 0;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        int32 part = target->effDamage[i];
        switch (SpellTakenKind(target->takenKind[i]))
        {
            case TAKEN_SPELL_DAMAGE:
                if (part > 0)
                    part = int32(victim->SpellDamageBonusTaken(caster, m_spellInfo, uint32(part), SPELL_DIRECT_DAMAGE));
                break;
            case TAKEN_MELEE_DAMAGE:
                if (part > 0 && caster)
                    part = int32(victim->MeleeDamageBonusTaken(caster, uint32(part), m_attackType, m_spellInfo, m_spellSchoolMask));
                break;
            case TAKEN_HEAL:
                if (part < 0)
                    part = -int32(victim->SpellHealingBonusTaken(caster, m_spellInfo, uint32(-part), HEAL));
                break;
            default:
                break;
        }
        total += part;
    }

    m_damage = total;
    m_healing = -total;
}

UnitView Spell::CasterView() const
{
    return UnitView(m_originalCaster ? m_originalCaster : m_caster->ToUnit());
}

void Spell::SetTriggeredByAuraSpell(TriggeredByAuraSpellData const& data, bool interruptChannelOnFail)
{
    m_triggeredByAuraSpell = data;
    m_recordedTriggerInterruptsChannel = interruptChannelOnFail && data.spellInfo && !data.spellInfo->IsPassive();
}

uint32 Spell::GetUnitTargetCountForEffect(SpellEffIndex effIndex) const
{
    if (_landSide && !_launchSpell)
        return _landUnitTargetCount[effIndex];

    std::list<TargetInfo> const& targets = _launchSpell ? _launchSpell->m_UniqueTargetInfo : m_UniqueTargetInfo;
    return uint32(std::count_if(targets.begin(), targets.end(), [effIndex](TargetInfo const& info)
    {
        return (info.effectMask & (1 << effIndex)) != 0;
    }));
}

uint32 Spell::GetUnitTargetIndexForEffect(SpellEffIndex effIndex) const
{
    if (_landSide && !_launchSpell)
        return _landUnitTargetIndex[effIndex];

    ObjectGuid const self = _landSide ? m_UniqueTargetInfo.front().targetGUID : (unitTarget ? unitTarget->GetGUID() : ObjectGuid::Empty);
    std::list<TargetInfo> const& targets = _launchSpell ? _launchSpell->m_UniqueTargetInfo : m_UniqueTargetInfo;
    uint32 index = 0;
    for (TargetInfo const& info : targets)
    {
        if (!(info.effectMask & (1 << effIndex)))
            continue;
        if (info.targetGUID == self)
            return index;
        ++index;
    }
    return 0;
}

// ---- launch side: emission ----

void Spell::LandOrEmit(TargetInfo& target)
{
    if (target.processed)
        return;

    // Self-hit, or reflected onto the caster: the caster is the one being hit. Inline, as today.
    if (target.targetGUID == m_caster->GetGUID() || (target.missCondition == SPELL_MISS_REFLECT && target.reflectResult == SPELL_MISS_NONE))
    {
        DoAllEffectOnTarget(&target);
        return;
    }

    Unit* unit = ObjectAccessor::GetUnit(*m_caster, target.targetGUID);

    // Far target (cross-map player, spell 45927) or target not on this map: today's far-effect path, inline.
    // Step 09's defer buffer takes these.
    if (!unit || m_spellInfo->Id == 45927)
    {
        DoAllEffectOnTarget(&target);
        return;
    }

    bool const inTask = BeatContext::InTask() && unit->IsInWorld();

    // Spell in the Deferred table (plan §11): lands at the end of Map::Update with both units live and no task
    // running, so CasterView() is live and Feedback() applies inline. Outside a task there is nothing to wait for.
    if (sSpellMgr->IsDeferredHit(m_spellInfo->Id))
    {
        if (!inTask)
        {
            DoAllEffectOnTarget(&target);
            return;
        }

        target.processed = true;
        HandleComboPointsOnMiss(target.missCondition, target.targetGUID);
        DeferredSpellHitRecord deferred;
        BuildHitRecord(target, deferred.hit);
        unit->GetMap()->DeferSpellHit(deferred);
        return;
    }

    target.processed = true;

    // Launch knows missCondition; a land-side evade is refunded through SpellLandedRecord::comboPointsRefund.
    HandleComboPointsOnMiss(target.missCondition, target.targetGUID);

    SpellHitRecord hit;
    BuildHitRecord(target, hit);

    // Inside a task: the target's inbox, landed at the start of its next update; feedback the update after.
    // No task (unit tests, casts between beats): consumed now on the target with this spell's scripts.
    if (inTask)
    {
        target.landPending = true;
        target.landEmittedMs = GameTime::GetGameTimeMS().count();
        sRecordLogs->Append(m_caster->GetGUID(), unit, RecordKind::SpellHit, hit);
    }
    else
        unit->ApplySpellHit(hit, this);
}

void Spell::BuildHitRecord(TargetInfo const& target, SpellHitRecord& out) const
{
    Unit* unitCaster = m_caster->ToUnit();

    out.spellId = m_spellInfo->Id;
    out.castSerial = m_castSerial;
    out.target = target.targetGUID;
    out.caster = m_caster->GetGUID();
    out.originalCaster = m_originalCasterGUID;
    out.castItemGuid = m_castItemGUID;
    out.casterTypeId = uint8(m_caster->GetTypeId());
    out.triggerFlags = uint32(_triggeredCastFlags);
    out.spellFlags = m_spellFlags;

    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        uint8 count = 0;
        uint8 index = 0;
        for (TargetInfo const& other : m_UniqueTargetInfo)
        {
            if (!(other.effectMask & (1 << i)))
                continue;
            if (other.targetGUID == target.targetGUID)
                index = count;
            if (count < 0xFF)
                ++count;
        }
        out.unitTargetCount[i] = count;
        out.unitTargetIndex[i] = index;
    }

    out.appliedModCount = 0;
    for (Aura* mod : m_appliedMods)
    {
        if (out.appliedModCount >= MAX_SPELL_APPLIED_MODS)
        {
            LOG_DEBUG("spells", "Spell {}: more than {} applied spell mods, rest dropped from the hit record", m_spellInfo->Id, uint32(MAX_SPELL_APPLIED_MODS));
            break;
        }
        out.appliedModAuraIds[out.appliedModCount++] = mod->GetId();
    }

    out.effectMask = target.effectMask;
    out.missCondition = uint8(target.missCondition);
    out.reflectResult = uint8(target.reflectResult);
    out.crit = target.crit ? 1 : 0;
    out.aliveAtLaunch = target.alive ? 1 : 0;
    out.scaleAura = target.scaleAura ? 1 : 0;
    out.wasDelayed = m_spellState == SPELL_STATE_DELAYED ? 1 : 0;
    out.damage = target.damage;
    out.damageBeforeTakenMods = target.damageBeforeTakenMods;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        out.effDamage[i] = target.effDamage[i];
        out.takenKind[i] = target.takenKind[i];
    }
    out.aura = target.aura;
    out.timeDelay = target.timeDelay;

    out.schoolMask = uint32(m_spellSchoolMask);
    out.attackType = uint8(m_attackType);
    out.castTime = m_casttime;
    out.powerCost = m_powerCost;
    out.scriptValue = m_launchScriptValue;
    out.originalTarget = m_originalTargetGUID;
    out.procAttacker = m_procAttacker;
    out.procVictim = m_procVictim;
    out.procEx = m_procEx;
    out.value = *m_spellValue;
    out.triggeredByAuraSpellId = m_triggeredByAuraSpell.spellInfo ? m_triggeredByAuraSpell.spellInfo->Id : 0;
    out.triggeredByAuraEffIndex = m_triggeredByAuraSpell.effectIndex;
    out.triggeredByAuraTick = m_triggeredByAuraSpell.tickNumber;
    out.preCastSpell = m_preCastSpell;

    out.hitTriggerCount = 0;
    for (HitTriggerSpell const& trigger : m_hitTriggerSpells)
    {
        if (out.hitTriggerCount >= MAX_SPELL_HIT_TRIGGERS)
        {
            LOG_WARN("spells", "Spell {}: more than {} ADD_TARGET_TRIGGER auras, the rest are dropped from the hit record",
                m_spellInfo->Id, uint32(MAX_SPELL_HIT_TRIGGERS));
            break;
        }
        SpellHitTrigger& t = out.hitTriggers[out.hitTriggerCount++];
        t.triggeredSpellId = trigger.triggeredSpell->Id;
        t.triggeredByAuraId = trigger.triggeredByAura ? trigger.triggeredByAura->Id : 0;
        t.triggeredByEffIdx = trigger.triggeredByEffIdx;
        t.chance = trigger.chance;
    }

    out.hasDst = m_targets.HasDst() ? 1 : 0;
    if (out.hasDst)
        out.dst.Set(*m_targets.GetDst());
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        out.effectDst[i].Set(m_destTargets[i]);
    out.primaryUnitTarget = m_targets.GetUnitTargetGUID();
    out.needComboPoints = m_needComboPoints ? 1 : 0;
    out.comboPoints = unitCaster ? unitCaster->GetComboPoints() : 0;
}

void Spell::HandleComboPointsOnMiss(SpellMissInfo missInfo, ObjectGuid const& targetGuid)
{
    // Do not take combo points on dodge and miss
    if (missInfo == SPELL_MISS_NONE || !m_needComboPoints || m_targets.GetUnitTargetGUID() != targetGuid)
        return;

    m_needComboPoints = false;
    // Restore spell mods for a miss/dodge/parry Cold Blood
    /// @todo: check how broad this rule should be
    Unit* unitCaster = m_caster->ToUnit();
    if (unitCaster && unitCaster->IsPlayer() && (missInfo == SPELL_MISS_MISS || missInfo == SPELL_MISS_DODGE || missInfo == SPELL_MISS_PARRY))
        unitCaster->ToPlayer()->RestoreSpellMods(this, 14177);
}

// ---- feedback ----

SpellLandedRecord& Spell::Feedback()
{
    if (_landFeedback)
        return *_landFeedback;
    if (!_ownFeedback)
        _ownFeedback = std::make_unique<SpellLandedRecord>();
    return *_ownFeedback;
}

void Spell::DoFollowUpCast(uint32 spellId, Unit* target, bool addAura, int32 auraDuration, bool byEffectCaster)
{
    if (!target)
        return;

    uint8 const by = byEffectCaster ? FEEDBACK_BY_EFFECT_CASTER : FEEDBACK_BY_CASTER;
    if (FollowUpCast* follow = Feedback().AddFollowUp(spellId, target->GetGUID(), uint32(TRIGGERED_FULL_MASK), addAura ? FOLLOWUP_ADD_AURA : FOLLOWUP_CAST, by))
    {
        follow->auraDuration = auraDuration;
        return;
    }

    // List full: perform it now through the live caster rather than lose the cast.
    LOG_WARN("spells", "Spell {}: follow-up list full ({}), casting {} inline", m_spellInfo->Id, uint32(MAX_SPELL_FOLLOWUPS), spellId);
    Unit* unitCaster = m_caster->ToUnit();
    Unit* actor = (byEffectCaster && m_originalCaster) ? m_originalCaster : unitCaster;
    if (addAura)
    {
        if (actor)
            actor->AddAura(spellId, target);
    }
    else if (actor)
        actor->CastSpell(target, spellId, true);
    else
        m_caster->CastSpell(target, spellId, true);
}

void Spell::DeliverFeedback(Unit* effectUnit, Unit* hitUnit)
{
    SpellLandedRecord* fb = _landFeedback ? _landFeedback : _ownFeedback.get();
    if (!fb)
        return;

    // Combo points gained by HIT_TARGET handlers are added by _handle_finish_phase on the launch side; the
    // land side has no finish phase and hands them back.
    if (_landSide && m_comboTarget && m_comboPointGain)
        fb->comboPointGain = m_comboPointGain;

    if (!fb->IsEmpty())
    {
        fb->spellId = m_spellInfo->Id;
        fb->castSerial = m_castSerial;
        fb->target = effectUnit ? effectUnit->GetGUID() : ObjectGuid::Empty;
        fb->hitTarget = hitUnit ? hitUnit->GetGUID() : fb->target;
        fb->caster = m_caster->GetGUID();
        fb->originalCaster = m_originalCasterGUID;
        fb->effectCaster = m_originalCaster ? m_originalCaster->GetGUID() : m_caster->GetGUID();

        Unit* casterUnit = m_caster->ToUnit();
        Unit* recipients[2] = { casterUnit, (m_originalCaster && m_originalCaster != casterUnit) ? m_originalCaster : nullptr };
        for (Unit* recipient : recipients)
        {
            if (!recipient)
                continue;

            // Land side inside a task: the recipient's inbox, applied at the start of its next update. Inline hit
            // (self or reflected), or no task running (unit tests, deferred hits at the seal): applied now.
            if (_landSide && BeatContext::InTask() && recipient->IsInWorld())
                sRecordLogs->Append(fb->target, recipient, RecordKind::SpellLanded, *fb);
            else
                recipient->ApplySpellLanded(*fb);
        }
    }

    if (_landFeedback)
        *_landFeedback = SpellLandedRecord();
    else
        _ownFeedback.reset();
}

// ---- Unit appliers ----

void Unit::ApplySpellHit(SpellHitRecord const& hit, Spell* launchSpell)
{
    if (!IsInWorld())
        return;

    if (!sSpellMgr->GetSpellInfo(hit.spellId))
        return;

    WorldObject* casterHandle = hit.caster == GetGUID() ? this : ObjectAccessor::GetWorldObject(*this, hit.caster);
    if (!casterHandle)
    {
        // Landing without a caster handle needs m_caster to be nullable through the hit path and the handlers;
        // that is the slice 4 audit. Until then a hit whose caster left the map between launch and land is
        // dropped (today the caster's SpellEvent is cancelled with it).
        LOG_DEBUG("spells", "{} dropped SpellHit {} from {}: caster not in world", GetGUID().ToString(), hit.spellId, hit.caster.ToString());
        return;
    }

    SpellLandedRecord feedback;
    Spell land(hit, this, casterHandle, &feedback);
    // Inline landing (no task running) borrows the launch spell's scripts so cast-hook state is visible to hit
    // hooks, as before the split. A record landing next beat has no launch spell and loads its own scripts.
    if (launchSpell)
        land.BorrowScripts(*launchSpell);
    land.Land();
    if (launchSpell)
        land.ReturnScripts(*launchSpell);
}

void Unit::ApplySpellLanded(SpellLandedRecord const& landed)
{
    if (!IsInWorld())
        return;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(landed.spellId);
    if (!spellInfo)
        return;

    // One record can reach two units: m_caster and the original caster (a trap owner, an aura's caster).
    bool const asCaster = landed.caster == GetGUID();
    bool const asEffectCaster = landed.effectCaster == GetGUID();
    bool const asOriginalCaster = landed.originalCaster == GetGUID();

    Unit* target = landed.target == GetGUID() ? this : ObjectAccessor::GetUnit(*this, landed.target);
    Unit* hitTarget = landed.hitTarget == landed.target ? target
        : (landed.hitTarget == GetGUID() ? this : ObjectAccessor::GetUnit(*this, landed.hitTarget));

    if (asEffectCaster)
    {
        if (landed.lastDamagedTarget)
            SetLastDamagedTargetGuid(landed.hitTarget);

        if (landed.castItemCombatSpell && hitTarget && IsPlayer())
            ToPlayer()->CastItemCombatSpell(hitTarget, WeaponAttackType(landed.attackType), landed.procVictim, landed.hitMask);
    }

    if (asOriginalCaster && landed.inheritCombatStates && hitTarget)
        GetCombatManager().InheritCombatStatesFrom(hitTarget);

    if (asCaster)
    {
        if (landed.missAfterLand && landed.missAfterLand != SPELL_MISS_MISS && hitTarget)
            SendSpellMiss(hitTarget, landed.spellId, SpellMissInfo(landed.missAfterLand));

        if (landed.comboPointsRefund && hitTarget)
            AddComboPoints(hitTarget, int8(landed.comboPointsRefund));

        if (landed.comboPointGain && hitTarget)
        {
            // remove Premed-like effects unless they were caused by ourselves (see _handle_finish_phase)
            if (!spellInfo->HasAura(SPELL_AURA_RETAIN_COMBO_POINTS))
                RemoveAurasByType(SPELL_AURA_RETAIN_COMBO_POINTS);
            AddComboPoints(hitTarget, landed.comboPointGain);
        }

        if (landed.healthLeechGain > 0 && IsAlive())
        {
            uint8 effIndex = EFFECT_0;
            for (; effIndex < MAX_SPELL_EFFECTS; ++effIndex)
                if (spellInfo->Effects[effIndex].Effect == SPELL_EFFECT_HEALTH_LEECH)
                    break;
            if (effIndex == MAX_SPELL_EFFECTS)
                effIndex = EFFECT_0;

            uint32 healthGain = SpellHealingBonusDone(this, spellInfo, uint32(landed.healthLeechGain), HEAL, effIndex);
            healthGain = SpellHealingBonusTaken(this, spellInfo, healthGain, HEAL);

            HealInfo healInfo(this, this, healthGain, spellInfo, spellInfo->GetSchoolMask());
            HealBySpell(healInfo);
        }

        if (landed.directSelfHeal > 0 && IsAlive())
        {
            HealInfo healInfo(this, this, uint32(landed.directSelfHeal), spellInfo, spellInfo->GetSchoolMask());
            HealBySpell(healInfo);
        }

        if (landed.revealCaster)
            RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TALK);

        if (landed.atTargetAttacked && target)
            AtTargetAttacked(target, landed.canInitialAggro != 0);

        if (landed.assistCombat && target)
            SetInCombatWith(target);

        if (landed.setContestedPvP)
            SetContestedPvP();

        if (landed.updatePvP && IsPlayer())
            ToPlayer()->UpdatePvP(true);

        if (landed.cooldownMs)
            AddSpellCooldown(landed.cooldownSpellId, landed.cooldownItemEntry, uint32(landed.cooldownMs));

        if (landed.casterAchievements && IsPlayer())
        {
            ToPlayer()->StartTimedAchievement(ACHIEVEMENT_TIMED_TYPE_SPELL_CASTER, landed.spellId);
            ToPlayer()->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_CAST_SPELL2, landed.spellId, 0, hitTarget);
        }

        if (landed.aiSpellHitTarget && hitTarget && IsCreature() && ToCreature()->IsAIEnabled)
            ToCreature()->AI()->SpellHitTarget(hitTarget, spellInfo);
    }

    for (uint8 i = 0; i < landed.followCount && i < MAX_SPELL_FOLLOWUPS; ++i)
    {
        FollowUpCast const& follow = landed.follow[i];
        bool const mine = follow.by == FEEDBACK_BY_EFFECT_CASTER ? asEffectCaster : asCaster;
        if (mine)
            CastFollowUp(follow);
    }
}

void Unit::CastFollowUp(FollowUpCast const& follow)
{
    Unit* target = follow.target == GetGUID() ? this : ObjectAccessor::GetUnit(*this, follow.target);
    if (!target)
        return;

    if (follow.mode == FOLLOWUP_ADD_AURA)
    {
        AddAura(follow.spellId, target);
        return;
    }

    // Step 08 §2: a cast a periodic tick asked for keeps its "triggered by aura" link as ids
    if (follow.triggeredByAuraId)
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(follow.spellId);
        if (!spellInfo)
            return;

        SpellCastTargets targets;
        targets.SetUnitTarget(target);

        CustomSpellValues values;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (follow.bpMask & (1 << i))
                values.AddSpellMod(SpellValueMod(SPELLVALUE_BASE_POINT0 + i), follow.bp[i]);
        if (follow.auraDuration)
            values.AddSpellMod(SPELLVALUE_AURA_DURATION, follow.auraDuration);

        TriggeredByAuraSpellData byAura;
        byAura.spellInfo = sSpellMgr->GetSpellInfo(follow.triggeredByAuraId);
        byAura.effectIndex = follow.triggeredByAuraEffIndex;
        byAura.tickNumber = follow.triggeredByAuraTick;

        ObjectGuid originalCaster = follow.originalCaster ? follow.originalCaster : GetGUID();
        CastSpell(targets, spellInfo, values.empty() ? nullptr : &values, TriggerCastFlags(follow.triggerFlags), byAura, false, originalCaster);
        return;
    }

    if (!follow.bpMask && !follow.auraDuration)
    {
        CastSpell(target, follow.spellId, TriggerCastFlags(follow.triggerFlags), nullptr, nullptr, follow.originalCaster);
        return;
    }

    CustomSpellValues values;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (follow.bpMask & (1 << i))
            values.AddSpellMod(SpellValueMod(SPELLVALUE_BASE_POINT0 + i), follow.bp[i]);
    if (follow.auraDuration)
        values.AddSpellMod(SPELLVALUE_AURA_DURATION, follow.auraDuration);

    CastCustomSpell(follow.spellId, values, target, TriggerCastFlags(follow.triggerFlags), nullptr, nullptr, follow.originalCaster);
}
