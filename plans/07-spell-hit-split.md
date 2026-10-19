# 07 — Spell hit split: launch on the caster, land on the target, feedback to the caster

This is the implementation guide for the spell problem. It turns the spell pipeline's existing two-phase shape into two records, so a cast by A that hits B runs its caster half on A's task and its target half on B's task, with no lock and without rewriting the 124 effect handlers one at a time.

All of this lands on today's single thread, inside `Map::Update`, with the step 06 log. Behaviour is identical in slice 1 and changes only where this file says so.

## 1. What the code does today

`Spell` already splits into launch and hit with a per-target record between them.

**Launch** (caster): `Spell::cast` → `_cast` (3912) → `HandleLaunchPhase` (8584) → `DoAllEffectOnLaunchTarget` (8662), per `TargetInfo` in `m_UniqueTargetInfo`:

- calls every effect handler in `SPELL_EFFECT_HANDLE_LAUNCH_TARGET` mode (8684),
- AoE reduction and chain multipliers (8687–8706),
- sums `targetInfo.damage`, `targetInfo.damageBeforeTakenMods` (8707–8708),
- rolls crit (8723–8726).

Hit, miss, reflect, resist, immune are rolled earlier in `AddUnitTarget` (2395–2460). `EffectSchoolDMG` (`SpellEffects.cpp:330`) computes the number only in launch mode. Power, reagents, combo points, cooldowns, spell mods, `SMSG_SPELL_GO` are launch-side. `SetInCombatWith(targetUnit, true)` for hostile hits is already done at launch (8649–8654).

`TargetInfo` (`Spell.h:266`): `targetGUID, timeDelay, missCondition, reflectResult, effectMask, processed, alive, crit, scaleAura, damage, damageBeforeTakenMods`.

**Hit** (today on the caster's thread, later): `handle_immediate` (4219) loops `DoAllEffectOnTarget(&*ihit)` (4268); `handle_delayed` (4290) does the same for targets whose `timeDelay <= t_offset` (4324) from the caster's event queue. `DoAllEffectOnTarget(TargetInfo*)` (2623) → `DoSpellHitOnUnit` (3084) → `HandleEffects(..., HIT_TARGET)` (3342), then damage/heal, procs, combat, AI hooks, scripts (2817–3081).

## 2. Rule

| Case | Where the hit runs |
| --- | --- |
| `target->targetGUID == m_caster->GetGUID()` | Inline at launch, unchanged. Covers the 15 caster-only handlers (summon, create item, teleport self, learn, open lock, bind, taxi) |
| Reflected and `reflectResult == SPELL_MISS_NONE` | Inline at launch as a self-hit with `reflectionSourceGuid = targetGUID`. The caster is the one being hit |
| Any other unit target | `SpellHitRecord` → target's next task (land) → `SpellLandedRecord` → caster's task the beat after |
| Target not on this map (`farMask`, spell 45927), or spell id in the `Deferred` table | `DeferredSpellHit` → coordinator at the seal with both units live |
| `GOTargetInfo`, `ItemTargetInfo` | Unchanged. Gameobjects and items are not unit tasks. A GO hit runs at launch; a GO is written only at the seal if it mutates the map (door open = dynamic-tree defer) |

## 3. Files

- New `src/server/game/Spells/SpellHitRecord.h` — `SpellHitRecord`, `SpellLandedRecord`, `DeferredSpellHitRecord`, `FollowUpCast`.
- New `src/server/game/Spells/SpellLand.cpp` — `Spell::Spell(SpellHitRecord const&, Unit* target)`, `Spell::EmitSpellHit`, `Spell::Land`, `Unit::ApplySpellLanded`.
- New `src/server/game/Entities/Unit/UnitView.h` — `UnitView`, `CasterView`.
- `Spell.h`, `Spell.cpp` — the edits listed in §5–§8.
- `SpellEffects.cpp` — three `SpellDamageBonusTaken` moves; the 22 "both" handlers.
- `SpellAuras.h/.cpp`, `SpellAuraEffects.h/.cpp` — `Aura::CreateFromHit`, launch-side amount/amplitude/duration.
- `Unit.h`, `Unit.cpp` — `DealDamage`, `Kill`, `DealHeal`, `ProcSkillsAndAuras` split.
- `SpellScript.h/.cpp` — `GetCaster()` return type on hit hooks.
- `ScriptMgr` / `CreatureAI.h` — `SpellHit(Unit* caster, ...)` becomes `SpellHit(UnitView caster, ...)`; `SpellHitTarget` stays on the caster (feedback).
- `SpellMgr.cpp` — the `Deferred` table (`sSpellMgr->IsDeferredHit(spellId)`), loaded from a static list in code, not SQL.

## 4. Records

All plain values. `SpellValue` (`Spell.h`) is already plain and is embedded by value.

```cpp
struct FollowUpCast
{
    uint32 spellId; ObjectGuid target; uint32 triggerFlags;    // TriggerCastFlags
    int32 bp[MAX_SPELL_EFFECTS]; uint8 bpMask;                 // CastCustomSpell base points
    ObjectGuid originalCaster;
};

struct SpellHitRecord
{
    // identity
    uint32 spellId; uint32 castSerial;                 // castSerial: per-process atomic, set in Spell::Spell
    ObjectGuid caster, originalCaster, castItemGuid; uint32 castItemEntry;
    uint8  casterTypeId;                               // TYPEID_PLAYER / UNIT / GAMEOBJECT
    uint32 triggerFlags;                               // _triggeredCastFlags
    uint32 castFlags;                                  // m_castFlags
    // rolled at launch (TargetInfo)
    uint8  effectMask;
    SpellMissInfo missCondition, reflectResult;
    bool   crit, aliveAtLaunch, scaleAura;
    int32  damage;                 // after done-side bonus, AoE reduction, multipliers, AND crit bonus (§5.1)
    int32  healing;                // same for heals (TargetInfo::damage < 0 today)
    int32  damageBeforeTakenMods;  // Beacon of Light; see §5.2
    uint64 timeDelay; uint32 launchTimeMs;             // Sanctuary windows (2667–2675)
    // cast data
    uint32 schoolMask; uint8 attackType;               // m_spellSchoolMask, m_attackType
    uint32 procAttacker, procVictim, procEx;           // after prepareDataForTriggerSystem
    SpellValue value;                                  // *m_spellValue
    uint32 triggeredByAuraSpellId; int8 triggeredByAuraEffIndex; uint32 triggeredByAuraTick;
    uint32 appliedModAuraIds[8]; uint8 appliedModCount;// m_appliedMods → aura spell ids
    uint32 preCastSpell;                               // m_preCastSpell
    struct { uint32 triggeredSpellId; int32 chance; uint32 triggeredByAuraId; } hitTriggers[6]; uint8 hitTriggerCount;  // m_hitTriggerSpells
    bool hasDst; SpellDestination dst;                 // m_targets.GetDst()
    ObjectGuid primaryUnitTarget;                      // m_targets.GetUnitTargetGUID(), for the combo-point rule (2759)
    bool isChannel; int32 channelDuration;
    // aura data computed on the caster (§5.3)
    uint8  auraEffMask;                 // effects that are unit-owned auras
    uint32 auraSpellIdForLevel;         // aurSpellInfo->Id after GetAuraRankForLevel, or spellId
    int32  auraAmount[MAX_SPELL_EFFECTS];
    int32  auraAmplitude[MAX_SPELL_EFFECTS];
    int32  auraMaxDurationCasterSide;   // after SPELLMOD_DURATION and periodic haste; -1 for infinite
    uint8  auraCanBeRecalculatedMask;   // effects whose amount may be recomputed later (from a slot)
    // caster snapshot the land step needs and the slot does not carry
    uint8  casterLevelForTarget;        // getLevelForTarget(target) — level and expansion rules
    bool   casterIsWorldBoss, casterIsTotem, casterIsPlayerControlled;
    ObjectGuid casterOwnerGuid;         // GetOwnerGUID()
};

struct SpellLandedRecord
{
    uint32 spellId; uint32 castSerial; ObjectGuid target; ObjectGuid caster;
    uint8  effectMask; SpellMissInfo missAfterLand;    // missInfo after DoSpellHitOnUnit (2740–2755)
    bool   periodic;                                   // step 08
    // numbers
    uint32 hitMask;                                    // PROC_HIT_* after mitigation
    uint32 damage, absorb, resist, blocked, overkill;  // SpellNonMeleeDamage fields after DealDamageVictimSide
    uint32 heal, overheal;
    int32  healthLeechGain;                            // §7 health leech
    int32  powerGain; uint8 powerType;                 // drain / energize-from-target
    bool   killed; ObjectGuid victimGuid; uint32 victimEntry; uint8 victimLevel; bool victimWasPlayer;
    bool   victimWasEngaged;                           // for AtTargetAttacked canInitialAggro and SetInCombatWith
    bool   targetIsPvP, targetHostile, targetFriendly; // the three IsFriendlyTo/IsPvP reads at 3035–3078
    bool   auraApplied, auraRefreshed;                 // channel tracking, PROC_EX_NO_AURA_REFRESH
    uint32 procAttacker, procVictim, procEx;           // final values the land step computed (2769–2816)
    uint32 attackType, schoolMask; uint32 triggeredByAuraSpellId; int8 triggeredByAuraEffIndex;
    uint32 appliedModAuraIds[8]; uint8 appliedModCount;
    bool   comboPointsRefund;                          // land-side immune/evade on the primary target
    // actions
    FollowUpCast follow[4]; uint8 followCount;
    bool charge; Position chargeDest;
    uint32 cooldownSpellId, cooldownItemEntry, cooldownMs;   // relic cooldown (3320–3322)
    bool   setContestedPvP, updatePvP;                 // 3165–3168, 3077
};

struct DeferredSpellHitRecord { SpellHitRecord hit; };   // applied by the coordinator with both units live
```

Size: `SpellHitRecord` ≈ 300 B, `SpellLandedRecord` ≈ 260 B. A Blizzard on 40 units is 12 KB in the log.

## 5. Launch changes (caster side, `Spell.cpp` / `SpellEffects.cpp` / `SpellAuraEffects.cpp`)

### 5.1 Crit bonus at launch

Today `CalculateSpellDamageTaken` (`Unit.cpp:1505`) applies `SpellCriticalDamageBonus(caster, ...)` at hit, and `SpellCriticalHealingBonus(caster, ...)` is applied at 2830. Both read caster auras.

Edit `DoAllEffectOnLaunchTarget` after the crit roll (8726):

```cpp
if (targetInfo.crit)
{
    if (targetInfo.damage > 0)
        targetInfo.damage = Unit::SpellCriticalDamageBonus(caster, m_spellInfo, targetInfo.damage, nullptr /*victim: slot read inside*/);
    else if (targetInfo.damage < 0)
        targetInfo.damage = -int32(Unit::SpellCriticalHealingBonus(caster, m_spellInfo, uint32(-targetInfo.damage), nullptr));
}
```

`SpellCriticalDamageBonus(caster, spellProto, damage, victim)` (`Unit.cpp:9161`) reads `victim` for `SPELL_AURA_MOD_CRIT_DAMAGE_BONUS`-style taken mods; change the signature to take `UnitView const* victim` and read from the slot. `CalculateSpellDamageTaken` loses its crit branch and keeps only taken-side work (armor, taken bonus, absorb/resist/block).

### 5.2 Taken-side bonus moves to land

`EffectSchoolDMG` calls `unitTarget->SpellDamageBonusTaken(...)` in launch mode at `SpellEffects.cpp:683, 1438, 1717`. That is a target read on the caster's task. Remove those three calls from the launch path; `m_damage` at launch becomes the done-side number. At land, inside the taken-side block of `DoAllEffectOnTarget` (2902), call `unitTarget->SpellDamageBonusTaken(casterView, m_spellInfo, damage, SPELL_DIRECT_DAMAGE, stack)` before `CalculateSpellDamageTaken`. `SpellDamageBonusTaken(Unit* caster, ...)` (`Unit.cpp:8681`) reads the caster only for `SPELL_AURA_MOD_DAMAGE_FROM_CASTER` (caster guid match) → `UnitView`. `m_damageBeforeTakenMods` is then equal to `damage` at launch for ordinary spells; Beacon of Light keeps the field.

### 5.3 Aura amounts, amplitude, and caster-side duration at launch

Today `Aura::TryRefreshStackOrCreate(..., m_originalCaster, basePoints, m_CastItem, casterGuid, ...)` (3245) constructs `AuraEffect`s whose constructor calls `CalculateAmount(caster)`, `CalculatePeriodic(caster)`, `CalculateSpellMod()`; then `ModSpellDuration` (3298) and periodic haste (3301–3302) read the caster.

Add to `DoAllEffectOnLaunchTarget`, after damage:

```cpp
targetInfo.auraEffMask = 0;
for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    if ((targetInfo.effectMask & (1 << i)) && m_spellInfo->Effects[i].IsUnitOwnedAuraEffect())
        targetInfo.auraEffMask |= 1 << i;
if (targetInfo.auraEffMask)
{
    SpellInfo const* aurInfo = targetInfo.scaleAura ? m_spellInfo->GetAuraRankForLevel(targetSlot.Level()) : m_spellInfo;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i) if (targetInfo.auraEffMask & (1 << i))
    {
        AuraEffectLaunchCalc c = AuraEffect::CalculateAtLaunch(aurInfo, i, caster, targetView, basePoints[i], m_CastItem, this);
        targetInfo.auraAmount[i] = c.amount; targetInfo.auraAmplitude[i] = c.amplitude;
        if (c.canBeRecalculated) targetInfo.auraCanBeRecalculatedMask |= 1 << i;
    }
    int32 dur = aurInfo->GetMaxDuration();
    if (dur > 0) { if (Player* mo = m_caster->GetSpellModOwner()) mo->ApplySpellMod(aurInfo->Id, SPELLMOD_DURATION, dur);
                   if (hasPeriodicHaste) dur = int32(dur * unitCaster->GetFloatValue(UNIT_MOD_CAST_SPEED)); }
    targetInfo.auraMaxDurationCasterSide = dur;
}
```

`AuraEffect::CalculateAtLaunch` is `CalculateAmount` + `CalculatePeriodic` with the target parameter typed `UnitView const&`. `CalculateAmount`'s per-spell switch reads the target for level, class, health, specific auras: each read becomes a slot read. The build enumerates them (the target parameter changes type). `ModSpellDuration`'s target-side part (`SPELL_AURA_MECHANIC_DURATION_MOD`, `SPELL_AURA_MECHANIC_DURATION_MOD_NOT_STACK`) runs at land on the live target; split `WorldObject::ModSpellDuration` into `ModSpellDurationCasterSide` and `Unit::ModSpellDurationTakenSide`.

`TargetInfo` gains the aura fields so `EmitSpellHit` can copy them.

### 5.4 Target reads at launch go through the slot

`AddUnitTarget` (2395–2460) reads the target for `SpellHitResult`, immunity, level, reflect auras. `EffectSchoolDMG`'s per-spell hacks read target auras and health. `DoAllEffectOnLaunchTarget` reads `unit->CalculateAOEDamageReduction` and `unit->SpellTakenCritChance(...)`. All of these are reads of another unit on the caster's task. They take `UnitView const&` from `target->ReadSlot()`. `SpellHitResult`, `MagicSpellHitResult`, `MeleeSpellHitResult`, `SpellTakenCritChance`, `CalculateAOEDamageReduction`, `IsImmunedToSpell`, `IsImmunedToSpellEffect`, `IsImmunedToDamage` get `UnitView` overloads that read resistance, level, immunity masks (`m_spellImmune` → carried in the slot as a compact list: add `SlotImmunity { uint8 op; uint32 type; uint32 spellId; }[]` to `ObjectSlot`), and aura modifiers via `TotalAuraModifier`.

`m_originalCaster` reads at launch (totem owner, Dancing Rune Weapon owner, pet owner for `SpellDamageBonusDone`) are foreign reads too: `GetOwner()` returns a handle; its stats come from `ReadSlot()`. Since the owner is a different task, those bonuses are one beat stale. Accepted.

## 6. Emission

Replace the two hit loops:

```cpp
// handle_immediate, 4268
for (TargetInfo& t : m_UniqueTargetInfo)
    LandOrEmit(t);
// handle_delayed, 4320–4329
if (!t.processed && (single_missile || t.timeDelay <= t_offset)) { t.timeDelay = t_offset; LandOrEmit(t); }

void Spell::LandOrEmit(TargetInfo& t)
{
    if (t.targetGUID == m_caster->GetGUID() || (t.missCondition == SPELL_MISS_REFLECT && t.reflectResult == SPELL_MISS_NONE))
    { DoAllEffectOnTarget(&t); return; }                       // inline, as today (self or reflected onto self)
    t.processed = true;
    Unit* target = ObjectAccessor::GetUnit(*m_caster, t.targetGUID);   // handle lookup, allowed
    if (!target || sSpellMgr->IsDeferredHit(m_spellInfo->Id) || NeedsFarHit(t))
    { sDefers->Append(DeferredSpellHitRecord{ BuildHitRecord(t) }); return; }
    sRecordLogs->Current().Append(m_caster->GetGUID(), target, RecordKind::SpellHit, BuildHitRecord(t));
}
```

`BuildHitRecord` copies the fields in §4 from `this` and `t`. `m_appliedMods` (a set of `Aura*`) is copied as spell ids. `m_hitTriggerSpells` as ids and chances.

Everything after the loops (`FinishTargetProcessing`, `_handle_finish_phase`, `TakeCastItem`, `TakeAmmo`, `finish`) runs as today. `finish()` already assumes hits are done; the only hit-dependent inputs are:

- `m_needComboPoints` cleared on a miss of the primary target (2759–2766). Launch knows `missCondition`; it does not know a land-side `missInfo2` (immune/evade recheck). Launch applies the rule from `missCondition`; `SpellLandedRecord::comboPointsRefund` makes the caster call `AddComboPoints(primaryTarget, points)` back when land said immune or evade.
- `RestoreSpellMods(this, 14177)` on miss/dodge/parry (2765): same, from `missCondition` at launch. Land-side changes do not re-trigger it.
- `m_spellAura` for channels (4261–4266 and `UpdateChanneledTargetList`). See §10.

`Spell::m_castSerial` is assigned in the constructor from `static std::atomic<uint32>`.

## 7. Land (`Spell::Land`, target's task)

`Unit::ApplyRecords` (step 06) dispatches `SpellHit` to:

```cpp
void Unit::ApplySpellHit(SpellHitRecord const& r)
{
    Spell* s = SpellLandPool::Acquire();          // thread_local pool of land-side Spell objects, reused
    s->InitFromHit(r, this);                      // §7.1
    s->Land();                                    // §7.2
    SpellLandPool::Release(s);
}
```

### 7.1 `Spell::InitFromHit(SpellHitRecord const&, Unit* target)`

Fills the `Spell` fields that `DoAllEffectOnTarget` / `DoSpellHitOnUnit` / the `HIT_TARGET` handlers read, and nothing else:

- `m_spellInfo = sSpellMgr->GetSpellInfo(r.spellId)`, `m_spellValue = &_ownedValue` (copied from `r.value`), `m_spellSchoolMask`, `m_attackType`, `_triggeredCastFlags`, `m_castFlags`, `m_procAttacker/Victim/Ex`, `m_triggeredByAuraSpell` (from id + index), `m_preCastSpell`, `m_hitTriggerSpells` (rebuilt), `m_targets` (dst and primary unit guid only), `m_CastItem = nullptr` with `m_castItemGUID`/`m_castItemEntry` kept for the two handlers that need the entry.
- `m_caster = ObjectAccessor::GetWorldObject(*target, r.caster)` as a **handle** (may be `nullptr` if the caster left; land still runs). `m_originalCaster` likewise. `_casterView = UnitView(casterHandle, r)`: a slot view with the record's snapshot fields overlaid (`casterLevelForTarget`, `casterIsWorldBoss`, …).
- `m_UniqueTargetInfo` gets exactly one `TargetInfo` rebuilt from the record (guid, delay, miss, reflect, mask, alive, crit, scaleAura, damage, damageBeforeTakenMods, aura fields).
- `_landFeedback = SpellLandedRecord{}` with identity fields pre-filled.
- `m_spellState = SPELL_STATE_DELAYED if r.timeDelay else SPELL_STATE_FINISHED` so the Sanctuary checks (2667, 2672) behave as today.

### 7.2 `Spell::Land()`

Calls `DoAllEffectOnTarget(&m_UniqueTargetInfo.front())`, then appends `_landFeedback` to the caster's log entry (`Append(target guid, casterHandle, RecordKind::SpellLanded, _landFeedback)`) if `casterHandle` is non-null and not the target.

`DoAllEffectOnTarget` and `DoSpellHitOnUnit` are edited in place so that the inline (self-hit) path and the land path are the same code. The caster is reached through two members only:

- `WorldObject* m_caster` — identity, guid, `ToUnit()` for type checks, and pointer equality. Rename the field to `m_casterHandle` and add `WorldObject* GetCasterHandle() const`.
- `UnitView const& CasterView() const` — every read of a caster field.
- `SpellLandedRecord& Feedback()` — every write to the caster.

The substitutions, by line in today's `Spell.cpp`:

| Line | Today | Land |
| --- | --- | --- |
| 2634 | `ObjectAccessor::GetUnit(*m_caster, target->targetGUID)` | `effectUnit = this unit` (the record's target is the task owner) |
| 2639–2661 | far-target path | not reachable; those spells are `Deferred` |
| 2679 | `Unit* caster = m_originalCaster ? ... : m_caster->ToUnit()` | `UnitView const& caster = CasterView()`; `casterHandle` for identity |
| 2693 | `effectUnit->SetInCombatWith(m_originalCaster)` | `SetInCombatWith(casterGuid)` own half + `CombatRef` record (step 06) |
| 2697, 2817, 3080 | `CallScript*HitHandlers` | unchanged; scripts see `CasterView` (§9) |
| 2740 | `DoSpellHitOnUnit(spellHitTarget, mask, scaleAura)` | unchanged call |
| 2748 | `m_caster->SendSpellMiss(spellHitTarget, id, missInfo2)` | `Feedback().missAfterLand = missInfo2`; the caster sends from feedback (the packet is from the caster's guid, so the caster builds it) |
| 2759–2766 | combo points / `RestoreSpellMods` | `Feedback().comboPointsRefund = (missInfo2 immune/evade && primary target)` |
| 2830, 2839 | `SpellCriticalHealingBonus(caster, ...)` | gone; `r.healing` already includes it (§5.1) |
| 2858 | `caster->HealBySpell(healInfo, crit)` | `Unit::HealBySpellVictimSide(casterView, this, healInfo, crit)` → `DealHealVictimSide`; `Feedback().heal/overheal` |
| 2862 | `unitTarget->GetThreatMgr().ForwardThreatForAssistingMe(caster, threat, ...)` | own threat manager, caster guid → records to each threatened unit (`Threat`) |
| 2873, 2962, 2983 | `Unit::ProcSkillsAndAuras(caster, unitTarget, ...)` | `Unit::ProcVictimSide(casterView, this, ...)`; `Feedback().procAttacker/procVictim/procEx/hitMask` carry the inputs for the actor side (§8) |
| 2880 | `caster->SetLastDamagedTargetGuid(...)` | feedback (implicit: caster sets it when applying a damage feedback) |
| 2886 | `unitTarget->IsImmunedToDamage(caster, ...)` | `casterView` overload |
| 2896–2902 | Dancing Rune Weapon owner / `caster->CalculateSpellDamageTaken(...)` | `Unit::CalculateSpellDamageTakenVictimSide(casterView, this, &damageInfo, damage, ...)`; DRW owner is `casterView` already resolved to the owner at launch (`casterOwnerGuid`) |
| 2919 | `Unit::DealDamageMods(damageInfo.target, ...)` | unchanged (target side) |
| 2921–2942 | health leech: `unitCaster->HealBySpell(self)` | `Feedback().healthLeechGain = healthGain` (done-side bonus computed at launch into `r.value` custom field; taken-side on the caster at feedback) |
| 2945 | `caster->SendSpellNonMeleeDamageLog(&damageInfo)` | `SendSpellNonMeleeDamageLogFrom(casterGuid, &damageInfo)`: build the packet with the caster guid and broadcast from the **target's** position (`SendMessageToSet` on this unit). Same recipients in practice; the caster sees it if it sees the target |
| 2948 | `effectUnit->SendSpellNonMeleeReflectLog` | unchanged (target) |
| 2952 | `caster->DealSpellDamage(&damageInfo, true, this)` | `Unit::DealSpellDamageVictimSide(casterView, &damageInfo, this)` → `DealDamageVictimSide` (§7.3) |
| 2965–2968, 2988–2990 | `caster->ToPlayer()->CastItemCombatSpell(...)` | feedback: `procVictim`, `hitMask` carried; the caster runs `CastItemCombatSpell(targetGuid, ...)` at feedback |
| 2994–2998 | pickpocket reveal: `unitCaster->RemoveAurasWithInterruptFlags(TALK)`, `EngageWithTarget(unitCaster)` | own `EngageWithTarget(casterGuid)`; feedback flag `revealCaster` → caster removes its own aura |
| 3036–3039 | `unitCaster->IsFriendlyTo(effectUnit)`, `AtTargetAttacked` | `casterView.IsFriendlyTo(this)` → `Feedback().targetHostile`; `AtTargetAttacked` runs at feedback |
| 3042 | `effectUnit->SetStandState(STAND)` | unchanged |
| 3047–3050 | `unitCaster->SetInCombatWith(effectUnit)` (assist case) | `Feedback().victimWasEngaged`, `targetFriendly`; caster's half at feedback plus `CombatRef` to target... which is this unit, so this unit inserts its half now |
| 3054–3055 | `caster->CastSpell(effectUnit, SPELL_INTERRUPT_NONPLAYER, true)` | `Feedback().follow[]` |
| 3060–3063 | `spellHitTarget->ToCreature()->AI()->SpellHit(unitCaster, m_spellInfo)` | `AI()->SpellHit(casterView, m_spellInfo)` — signature change on `CreatureAI`, `UnitAI`, SmartAI, `ScriptMgr` hooks. SmartAI stores caster guid for `SMART_EVENT_SPELLHIT` already |
| 3066–3067 | `unitCaster->ToCreature()->AI()->SpellHitTarget(...)` | feedback: the caster's AI hook runs at feedback with `targetGuid` |
| 3070 | `DoTriggersOnSpellHit(spellHitTarget, mask)` | §7.4 |
| 3074–3078 | `unitCaster->ToPlayer()->UpdatePvP(true)` | `Feedback().updatePvP` |
| 3127–3128 | target achievements with `unitCaster` | `UpdateAchievementCriteria(..., casterHandle)` — achievement criteria that read the caster's fields go through the view; most read entry/type only |
| 3131–3134 | caster achievements | feedback |
| 3146 | `unit->GetCharmerOrOwnerGUID() != m_caster->GetGUID()` | guid compare, unchanged |
| 3150, 3154 | `m_caster->IsValidAttackTarget(unit, ...)`, `m_caster->IsFriendlyTo(unit)` | `casterView.IsValidAttackTarget(this)`, `casterView.IsFriendlyTo(this)` |
| 3165–3168 | `unitCaster->SetContestedPvP()`, `UpdatePvP` | feedback flags |
| 3173–3177 | `m_originalCaster->GetCombatManager().InheritCombatStatesFrom(unit)`; `ForwardThreatForAssistingMe(m_originalCaster, ...)` | feedback `victimWasEngaged`; own threat forward by guid |
| 3188–3189, 3196, 3208–3209 | `GetOriginalCaster()`, `unitCaster->IsCreature()->isWorldBoss()`, `IsCharmedOwnedByPlayerOrPlayer()` | record snapshot fields |
| 3216 | `m_caster->IsHostileTo(unit)` | view |
| 3241–3246 | `Aura::TryRefreshStackOrCreate(aurSpellInfo, effectMask, unit, m_originalCaster, basePoints, m_CastItem, casterGuid, &refresh, periodicReset)` | `Aura::TryRefreshStackOrCreateFromHit(aurSpellInfo, effectMask, unit, r.originalCaster, r.auraAmount, r.auraAmplitude, r.auraCanBeRecalculatedMask, r.castItemGuid, &refresh, periodicReset)` (§7.5) |
| 3271–3274 | `m_targets.GetUnitTarget()->ApplyDiminishingToDuration(...)` (self-hit only) | not reachable on land (self-hits are inline) |
| 3298 | `caster->ModSpellDuration(aurSpellInfo, unit, duration, positive, effectMask)` | `duration = r.auraMaxDurationCasterSide; duration = unit->ModSpellDurationTakenSide(aurSpellInfo, duration, positive, effectMask)` |
| 3301–3302 | periodic haste | gone (in `auraMaxDurationCasterSide`) |
| 3320–3322 | `unitCaster->AddSpellCooldown(SPELL_RELIC_COOLDOWN, m_CastItem->GetEntry(), duration)` | `Feedback().cooldownSpellId/ItemEntry/Ms` |
| 3342, 3346 | `HandleEffects(unit, nullptr, nullptr, i, HIT_TARGET)` | unchanged call; handler bodies per §7.6 |

### 7.3 `DealDamage`, `Kill`, `DealHeal` split (`Unit.cpp`)

`static uint32 Unit::DealDamage(Unit* attacker, Unit* victim, ...)` (990) becomes:

```cpp
struct DamageResult { uint32 dealt; uint32 overkill; bool killed; };
static DamageResult DealDamageVictimSide(UnitView const* attacker, Unit* victim, uint32 damage, CleanDamage const*, DamageEffectType, SpellSchoolMask, SpellInfo const*, bool durabilityLoss, Spell const* landSpell);
void ApplyDamageDealt(SpellLandedRecord const& fb);      // attacker side, from feedback
```

Victim side keeps: `sScriptMgr->OnUnitDealDamage`-style hooks that take the victim, `DealDamageMods`, health write, `SetInCombatWith` own half, `RemoveAurasWithInterruptFlags(TAKE_DAMAGE)`, interrupt casts on damage, `OnDamageTaken` AI hook, rage from damage taken (`RewardRage(damage, 0, false)` is the **victim's** rage — stays), duel/battleground damage counters on the victim, `Kill` victim half. Attacker side moves to `ApplyDamageDealt`: `GetAI()->DamageDealt(victimHandle, damage, type)`, `AtTargetAttacked`, `RewardRage(damage, weaponSpeedHitFactor, true)`, `UpdateAchievementCriteria(DAMAGE_DONE, ...)`, attacker durability loss, `SetLastDamagedTargetGuid`.

`Unit::Kill(killer, victim, ...)` (13596) becomes `KillVictimSide(UnitView const* killer, Unit* victim, ...)`: death state, `RemoveAllAurasOnDeath`, `CombatStop` (own half + `CombatEnd` records), lootable flag, `SetLootRecipient(killerGuid)` (already guid-based), duel end (defer), self-res check, battleground `HandleKillPlayer/HandleKillUnit` → defer, group looter broadcast → defer. Killer side `ApplyKill(fb)`: `RewardPlayerAndGroupAtKill` (§7.3a), killing-blow achievement, pet AI `KilledUnit`.

**7.3a Kill reward.** `Player::RewardPlayerAndGroupAtKill(victim, isBattleground)` uses `KillRewarder`, which reads the victim (level, elite, entry, template, XP) and walks the group rewarding each member. From feedback the victim is a `UnitView` built from `victimGuid` (handle → `ReadSlot()`, still alive in the store until the seal) plus `victimEntry/Level/WasPlayer`. The killer's task computes each member's share from member slots (level, position, alive) and appends `KillCredit { victimGuid, victimEntry, victimLevel, xp, honor, repFaction, repAmount, isPvP, groupRate }` to each other member. `KillCredit` applies `GiveXP`, `KilledMonsterCredit(entry, guid)`, `RewardReputation`, `RewardHonor` on the member's own task. Add `KillCredit` to `RecordKind`.

`Unit::DealHeal(healer, victim, addhealth)` (7844) → `DealHealVictimSide(UnitView const* healer, Unit* victim, uint32)` returns gain; healer-side `UpdateAchievementCriteria(HEALING_DONE)`, battleground score, `HealDone` AI from feedback.

### 7.4 `DoTriggersOnSpellHit` (3351)

| Line | Today | Land |
| --- | --- | --- |
| 3360, 3364 | `m_caster->CastSpell(unit, id, true)` | `Feedback().follow[]` |
| 3368 | `unitCaster->AddAura(m_preCastSpell, unit)` | `Feedback().follow[]` with `TRIGGERED_FULL_MASK` (one beat later than today) |
| 3382 | `m_caster->CastSpell(unit, triggeredSpell->Id, true)` for `SPELL_AURA_ADD_TARGET_TRIGGER` | `Feedback().follow[]`; roll stays at land |
| 3389–3398 | `unit->GetAura(...)`, `SetDuration` | unchanged (target's own auras) |
| 3411, 3415 | `unit->RemoveAurasDueToSpell`, `unit->CastSpell(unit, *i, true, 0, 0, m_caster->GetGUID())` | unchanged: the target casts on itself with `originalCasterGUID = caster`. That new `Spell`'s constructor resolves `m_originalCaster` as a handle; its launch reads the original caster through the view |

### 7.5 `Aura::TryRefreshStackOrCreateFromHit`

`SpellAuras.cpp`: a sibling of `TryRefreshStackOrCreate` (`SpellAuras.h:94`) that:

- looks up `owner->GetOwnedAura(spellId, casterGuid, castItemGuid)` for refresh exactly as today,
- on create calls `Aura::Create(spellproto, effMask, owner, /*caster*/ nullptr, /*baseAmount*/ nullptr, /*castItem*/ nullptr, casterGuid, castItemGuid)`,
- then for each effect `GetEffect(i)->SetAmountFromLaunch(amount[i], amplitude[i], canRecalc & (1<<i))`, which sets `m_amount`, `m_amplitude`, `m_canBeRecalculated`, and recomputes `m_spellmod` from the amount,
- on refresh with `periodicReset` behaves as today.

`AuraEffect::CalculateAmount(Unit* caster)` keeps working with `caster == nullptr` as it already does for ownerless casts; it is only called on the land path when `m_canBeRecalculated` and a later `RecalculateAmount` fires (stack change, `RecalculateAmountOfEffects`). `AuraEffect::RecalculateAmount()` (`SpellAuraEffects.h:76`) calls `GetCaster()` → `Aura::GetCaster()` → `ObjectAccessor::GetUnit`, a handle. Change `RecalculateAmount` to pass `UnitView` of that handle's slot. Effects whose recalculation needs the live caster keep the launch value (`auraCanBeRecalculatedMask` bit clear).

### 7.6 Effect handlers in `HIT_TARGET` mode

The handlers are `Spell` member functions and run unchanged in structure. Their caster access is through `m_caster` / `unitCaster` / `m_originalCaster`. After the rename in §7.2, every handler line that touched the caster is a compile error. The fix per class:

| Class (count) | Fix |
| --- | --- |
| Writes nothing on the caster (87) | replace `m_caster->X()` reads with `CasterView().X()`; identity uses `GetCasterHandle()` |
| Caster-only, self-targeted (15) | these never land (self-hits are inline), but still compile: same substitution; they keep working on the inline path because `CasterView()` on an inline hit wraps the live caster |
| Both (22): `unitCaster->CastSpell(...)` / `CastCustomSpell(...)` in `EffectSchoolDMG`, `EffectHeal`, `EffectEnergize`, `EffectDispel`, `EffectTaunt`, `EffectWeaponDmg`, `EffectTriggerSpell`, `EffectForceCast`, `EffectScriptEffect`, `EffectDummy` | `Feedback().AddFollowUp(spellId, targetGuid, flags, bp...)` |
| Both: `unitCaster->EnergizeBySpell(unitCaster, ...)` / `ModifyPower` in `EffectPowerDrain`, `EffectEnergizePct`, `EffectSchoolDMG` (Ferocious Bite) | `Feedback().powerGain += n; powerType = p` |
| Both: `EffectCharge` → `unitCaster->GetMotionMaster()->MoveCharge(...)` | `Feedback().charge = true; chargeDest = unitTarget position` |
| Both: `EffectTeleportUnits`, `EffectSummonPlayer`, `EffectSummonRaFFriend`, `EffectBind`, `EffectSendTaxi`, `EffectDuel`, `EffectInebriate`, `EffectCreateItem2` | spell ids with these effects on a foreign target go in the `Deferred` table |

`CasterView()` on the inline path (self-hit at launch) is a view over the **live** caster, not the slot, so inline behaviour is unchanged. `Feedback()` on the inline path applies immediately at the end of `DoAllEffectOnTarget` (`ApplySpellLanded(_landFeedback)` on `this` caster) instead of being appended. One code path, two delivery modes.

## 8. Feedback apply (`Unit::ApplySpellLanded`, caster's task)

```cpp
void Unit::ApplySpellLanded(SpellLandedRecord const& f)
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(f.spellId);
    Unit* targetHandle = ObjectAccessor::GetUnit(*this, f.target);     // may be null; view from slot if present
    UnitView targetView(targetHandle);
    if (f.missAfterLand != SPELL_MISS_NONE && f.missAfterLand != SPELL_MISS_MISS) SendSpellMiss(f.target, f.spellId, f.missAfterLand);
    if (f.comboPointsRefund && IsPlayer()) ToPlayer()->AddComboPoints(f.target, info->... /*points taken at launch, stored in f*/);
    if (f.damage || f.hitMask & PROC_HIT_ABSORB /* etc */) { SetLastDamagedTargetGuid(f.target); ApplyDamageDealt(f); }
    if (f.heal) ApplyHealDone(f);
    if (f.healthLeechGain) { HealInfo hi(this, this, SpellHealingBonusTaken(this, info, f.healthLeechGain, HEAL), info, info->GetSchoolMask()); HealBySpell(hi); }
    if (f.powerGain) EnergizeBySpell(this, f.spellId, f.powerGain, Powers(f.powerType));
    if (f.killed) ApplyKill(f);
    // procs, actor half (§8.1)
    ProcActorSide(targetView, f.procAttacker, f.procVictim, f.procEx | f.hitMask, f.damage, WeaponAttackType(f.attackType), info, f.triggeredByAuraSpellId, f.triggeredByAuraEffIndex, f.appliedModAuraIds, f.appliedModCount, f);
    if (IsPlayer() && !info->HasAttribute(SPELL_ATTR0_CANCELS_AUTO_ATTACK_COMBAT) && !info->HasAttribute(SPELL_ATTR4_SUPPRESS_WEAPON_PROCS) && (info->DmgClass == MELEE || RANGED))
        ToPlayer()->CastItemCombatSpell(f.target, WeaponAttackType(f.attackType), f.procVictim, f.hitMask);
    if (f.targetHostile && f.missAfterLand != SPELL_MISS_EVADE) AtTargetAttacked(f.target, canInitialAggro /* from info attrs */);  // own combat half + CombatRef record
    if (f.targetFriendly && f.victimWasEngaged && info->IsPositive() && !NO_THREAT) SetInCombatWith(f.target);                   // own half + CombatRef
    if (f.setContestedPvP) SetContestedPvP(); if (f.updatePvP && IsPlayer()) ToPlayer()->UpdatePvP(true);
    if (f.cooldownMs) AddSpellCooldown(f.cooldownSpellId, f.cooldownItemEntry, f.cooldownMs);
    if (IsCreature() && IsAIEnabled && f.missAfterLand == SPELL_MISS_NONE) ToCreature()->AI()->SpellHitTarget(targetHandle, info);  // handle; AI reads via slot
    if (IsPlayer()) { ToPlayer()->StartTimedAchievement(ACHIEVEMENT_TIMED_TYPE_SPELL_CASTER, f.spellId); ToPlayer()->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_CAST_SPELL2, f.spellId, 0, targetHandle); }
    for (uint8 i = 0; i < f.followCount; ++i) CastFollowUp(f.follow[i]);   // CastSpell / CastCustomSpell on this caster → new Spell → new SpellHit next beat
    if (f.charge) GetMotionMaster()->MoveCharge(f.chargeDest...);
    if (f.auraApplied || f.auraRefreshed) NoteChannelTarget(f.castSerial, f.target, f.auraApplied);   // §10
}
```

Proc latency: cast in beat *k*, land *k+1*, feedback and actor-side proc *k+2*; a proc that casts lands *k+3*. 30 ms at the default interval.

### 8.1 Procs

`Unit::ProcSkillsAndAuras(actor, victim, procAttacker, procVictim, procExtra, amount, attType, procSpellInfo, procAura, procAuraEffectIndex, procSpell, damageInfo, healInfo, procPhase)` (6564) is already two halves inside:

- `actor->ProcSkillsAndReactives(false, victim, procAttacker, ...)` and `victim->ProcSkillsAndReactives(true, actor, procVictim, ...)` (12389),
- `TriggerAurasProcOnEvent(nullptr, nullptr, victim, procAttacker, procVictim, spellTypeMask, phaseMask, hitMask, procSpell, damageInfo, healInfo)` (12521), which builds one `ProcEventInfo` for the actor's auras and one for the target's.

Split the entry point:

```cpp
static void ProcVictimSide(UnitView const& actor, Unit* victim, uint32 procVictim, uint32 hitMask, uint32 amount, WeaponAttackType, SpellInfo const* procSpellInfo, SpellInfo const* procAura, int8 procAuraIdx, Spell const* landSpell, DamageInfo*, HealInfo*);
void ProcActorSide(UnitView const& target, uint32 procAttacker, uint32 procVictim, uint32 hitMask, uint32 amount, WeaponAttackType, SpellInfo const*, uint32 procAuraId, int8 procAuraIdx, uint32 const* appliedModIds, uint8 appliedModCount, SpellLandedRecord const&);
```

`ProcEventInfo` gets `UnitView _actorView` / `_targetView` alongside the handles. `GetActor()` / `GetActionTarget()` / `GetProcTarget()` keep returning `Unit*` (handles) so the hundreds of aura proc handlers compile; the step 13 `ASSERT` turns every foreign *field* access in a proc handler into a row. The fix for a victim-side proc that writes the actor is a `FollowUpCast` on the victim's own feedback (the victim casts at the actor — Thorns, damage shields, and most "on being hit" procs already are casts by the victim) or a `Feedback().follow[]` entry when the victim's aura wants the *actor* to cast.

`procSpell->m_appliedMods` (read in `TriggerAurasProcOnEvent` for mod-owner auras) is `appliedModAuraIds` on the actor side, where those auras are live.

## 9. `UnitView` and `CasterView`

`src/server/game/Entities/Unit/UnitView.h`:

```cpp
class UnitView : public SlotReader
{
public:
    UnitView(Unit const* handle);                      // slot = handle->ReadSlot(); live if handle is the current owner
    UnitView(Unit const* handle, SpellHitRecord const& snapshot);  // overlays casterLevelForTarget etc.
    ObjectGuid GetGUID() const; Unit const* Handle() const;        // handle: identity only
    bool IsPlayer() const; bool IsCreature() const; bool IsPet() const; bool IsTotem() const;
    uint8 GetLevel() const; uint8 GetLevelForTarget(Unit const*) const;
    uint32 GetHealth() const; uint32 GetMaxHealth() const; uint32 GetPower(Powers) const; uint32 GetMaxPower(Powers) const;
    uint32 GetFaction() const; FactionTemplateEntry const* GetFactionTemplateEntry() const;
    ReputationRank GetReactionTo(Unit const* target) const;         // today's Unit::GetReactionTo, reading this view and target live (target is the owner)
    bool IsFriendlyTo(Unit const*) const; bool IsHostileTo(Unit const*) const; bool IsValidAttackTarget(Unit const*, SpellInfo const*) const;
    bool HasUnitFlag(uint32) const; bool HasUnitState(uint32) const; bool IsAlive() const; bool IsInCombat() const; bool IsPvP() const; bool IsFFAPvP() const;
    ObjectGuid GetOwnerGUID() const; ObjectGuid GetCharmerOrOwnerGUID() const; UnitView Owner() const;   // resolves via ObjectAccessor::GetUnit → ReadSlot
    Position const& GetPosition() const; float GetDistance(WorldObject const*) const; bool IsWithinMeleeRange(Unit const*) const;
    bool HasAura(uint32, ObjectGuid = Empty) const; SlotAura const* GetAura(uint32, ObjectGuid = Empty) const;
    int32 GetTotalAuraModifier(AuraType) const; float GetTotalAuraMultiplier(AuraType) const;
    int32 GetMaxPositiveAuraModifier(AuraType) const; int32 GetTotalAuraModifierByMiscMask(AuraType, uint32) const;
    bool HasAuraType(AuraType) const; bool HasAuraTypeWithAffectMask(AuraType, SpellInfo const*) const;
    uint32 GetCreatureType() const; uint32 GetCreatureTypeMask() const; CreatureTemplate const* GetCreatureTemplate() const;  // template is static data
    bool isWorldBoss() const; uint32 GetEntry() const; uint8 GetClass() const; uint8 GetRace() const;
    float GetFloatValue(uint16) const; uint32 GetUInt32Value(uint16) const;   // raw
};
using CasterView = UnitView;
```

It has no mutator. There is no `ToPlayer()`, `GetMotionMaster()`, `AddAura()`, `CastSpell()`, `SetHealth()`. A call site that needs one fails to compile; that is the audit.

Spell scripts: `SpellScript::GetCaster()` (`SpellScript.h:885`) returns `Unit*`. Add `SpellScript::GetCasterView()` and make `GetCaster()` return `Unit*` only in `OnCast`/`BeforeCast`/`AfterCast`/`OnCheckCast`/launch hooks (where the caster is the owner) and **`UnitView`** in `BeforeHit`/`OnHit`/`AfterHit`/`OnEffectHitTarget`/`OnEffectHit`/`OnEffectLaunchTarget`-with-target hooks. Implement by giving `SpellScript` a `_hitPhase` flag set by `Spell::CallScript*HitHandlers` and making `GetCaster()` return a proxy type that converts to `Unit*` only when `!_hitPhase`... a runtime check cannot drive the compiler. Instead: two names. `GetCaster()` becomes **`UnitView`** everywhere in `SpellScript`; `GetCasterUnit()` returns `Unit*` and `ASSERT`s `!_hitPhase`. Every script's hit hook that calls a mutator on `GetCaster()` is a compile error; the fix is `GetCasterUnit()` (launch hooks), `Feedback().AddFollowUp` (hit hooks that cast from the caster), or the `Deferred` table. `AuraScript::GetCaster()` likewise becomes a view on proc/tick hooks (step 08).

## 10. Channels, reflect, dynobj

- **Channel.** `handle_immediate` 4261–4266 and `UpdateChanneledTargetList` use `m_spellAura` (an `Aura*` on the target). Replace `m_spellAura` for foreign targets with `m_channelTargets: small_vector<{ guid, auraApplied, lastSeenBeat }>`. `SpellLandedRecord::auraApplied` sets `auraApplied`. `UpdateChanneledTargetList` checks `targetHandle->ReadSlot()` for the aura (`FindAura(spellId, casterGuid)`), range, and LOS; a target whose slot no longer has the aura is dropped as today. The `DYNOBJ_AURA_TYPE` branch (4262) is caster-owned and unchanged.
- **Reflect.** Known at launch (`missCondition == REFLECT`, `reflectResult`). Inline self-hit with `m_reflectionTargetGuid` and `m_reflectionTargetPosition` set from the target's slot (2729–2731 already store guid and position; drop the `m_reflectionTarget` pointer). `LowerPlayerDamageReq` (2720) runs on the caster if the caster is a creature — self.
- **Dynamic objects / persistent area auras.** `EffectPersistentAA` creates a caster-owned `DynamicObject` (a separate task). Its area aura targets are step 08.
- **Far targets** (`farMask`, 45927): `Deferred`.

## 11. `Deferred` table

`SpellMgr`: `std::unordered_set<uint32> _deferredHitSpells` built from a static list in `SpellMgr.cpp` plus every spell that has one of these effects on a non-self target: `SPELL_EFFECT_DUEL`, `SPELL_EFFECT_SUMMON_PLAYER`, `SPELL_EFFECT_SUMMON_RAF_FRIEND`, `SPELL_EFFECT_TELEPORT_UNITS` (when the target is not the caster), `SPELL_EFFECT_SEND_TAXI`, `SPELL_EFFECT_BIND`. `IsDeferredHit(spellId)` is a hash lookup at emission. `DeferredSpellHitRecord` is applied by the coordinator at the seal (step 09's defer buffer; on this step, at the end of `Map::Update`) by constructing the land-side `Spell` with `BeatContext::owner == nullptr`, so `CasterView()` wraps the live caster and `Feedback()` applies inline. It is today's code path with both units live and no task running. The list grows only from compiler output in slice 4.

## 12. Slices

Each slice boots and plays on one thread.

1. **Records built, consumed immediately.** `LandOrEmit` builds the record and calls `ApplySpellHit` on the target right away, same tick, same thread. `Feedback()` is appended to the step 06 log and applied next tick. `CasterView()` wraps the live caster. No behaviour change except feedback timing. Proves the record carries everything the land step reads.
2. **Launch changes §5.1–5.4.** Crit at launch, taken-side move, aura amounts/amplitude/duration at launch, slot reads for the target at launch. Behaviour changes: DoT snapshot, one-beat-stale owner bonuses.
3. **Next-tick land.** `LandOrEmit` appends to the log; land runs at the start of the target's next tick; feedback the tick after. Self-hits inline. `Deferred` table active. The wake rule (step 06) makes an idle creature open its inbox.
4. **`CasterView` audit.** Rename `m_caster`, change `SpellScript::GetCaster()`, `CreatureAI::SpellHit`, `AuraEffect::CalculateAmount` target type. Build. Record the error count. Fix each site per §7.6/§9. Record the count after. Every `Deferred` addition is a row in the exit note with the reason.
5. **`DealDamage`/`Kill`/`DealHeal`/proc split §7.3, §8.1, `KillCredit`.** Until this slice those functions run with the live caster handle and the step 06 foreign-read counter climbs; after it the counter on a spell-only fight is zero.

## 13. Checks

Record each before slice 1, then after each slice: damage number, log packet contents, aura duration and tick amounts, proc occurrence.

| Spell | Exercises |
| --- | --- |
| Fireball | projectile, crit bonus, Ignite proc (actor side), Impact |
| Frostbolt | projectile, slow aura with caster-side duration |
| Chain Lightning | chain multipliers at launch |
| Blizzard | caster-owned dynobj, AoE reduction, 40 records per tick |
| Drain Life, Mana Burn, Viper Sting | health leech, power drain feedback |
| Charge | `chargeDest` feedback |
| Polymorph, Fear | DR, `ModSpellDurationTakenSide` |
| Dispel Magic with Glyph | dispel on target, follow-up heal cast from caster |
| Judgement of Light | procs both ways; victim-side proc casting at actor |
| Thorns on a target being meleed | victim-side proc cast |
| Mind Control | `SpellHit` applies the aura on the target; the aura's charm effect appends `TaskGroupJoin` (step 09); from the next seal caster and target are one task group (step 12) and the charm body runs with both live |
| Killing blow in a 5-man group | `KillVictimSide`, `ApplyKill`, `KillCredit` to each member, loot recipient |
| Spell Reflection | inline self-hit |
| Summon Player, Duel | `Deferred` |

Slice 3: every hit lands next tick, feedback the tick after; `.debug slot` on the target shows health moving one tick after the cast. Slice 4: error count before/after in the exit note. Slice 5: `.debug foreignreads` is 0 for a spell-only fight.

## 14. Behaviour changes

- Hit lands one beat after launch; caster-side effects one beat after that.
- DoT/HoT amounts and amplitude fixed at application. Spell power changes mid-duration do not retick differently. Haste changes do not change amplitude mid-aura.
- Owner-derived bonuses (totem, pet, DRW) are one beat stale.
- Pre-cast aura (`m_preCastSpell`) and `ADD_TARGET_TRIGGER` casts arrive one beat later.
- Combo points are taken at launch; a land-side immune/evade refunds via feedback one beat later.
- Damage log packets are broadcast from the target's position with the caster's guid.

## 15. Open

1. **`AuraEffect::CalculateAmount` target reads.** Enumerated by the slice 4 build when the target parameter becomes `UnitView const&`. Fields the slot does not carry yet are added to `ObjectSlot` (immunity list is already added in §5.4).
2. **Scripts.** Count unknown until slice 4 builds. Each is one line or one `Deferred` row.
3. **Proc handlers writing the actor.** Found at runtime by the step 13 `ASSERT`, not by the compiler, because `GetActor()` keeps its type. If the count is large, change `ProcEventInfo::GetActor()` to `UnitView` and take the compile errors.
4. **`SpellLandPool`.** Land-side `Spell` reuse per thread; `Spell` has `std::list` members (`m_UniqueTargetInfo`, `m_hitTriggerSpells`) whose nodes would be freed and reallocated per land. Replace with `small_vector` in this slice or accept the allocation until the `inbox` clock field says otherwise.

## 16. Status

### Slice 1 — done (not yet built or played)

- `Spells/SpellHitRecord.h`: `SpellDestRecord` (plain `SpellDestination`: `Position` has a user-provided copy constructor and is not trivially copyable), `FollowUpCast` (+ `mode` cast/add-aura, `by` caster/effect-caster, `auraDuration`), `SpellHitTrigger`, `SpellHitRecord`, `SpellLandedRecord`, `DeferredSpellHitRecord`; `static_assert`s on trivial copyability. `SpellValue` gained a default constructor so it can be embedded.
- `Spells/SpellLand.cpp`: land-side `Spell::Spell(SpellHitRecord const&, Unit* target, WorldObject* casterHandle, SpellLandedRecord*)`, `Spell::Land`, `LandOrEmit`, `BuildHitRecord`, `HandleComboPointsOnMiss`, `Feedback`, `DeliverFeedback`, `DoFollowUpCast`, `CasterView`, `Unit::ApplySpellHit`, `Unit::ApplySpellLanded`, `Unit::CastFollowUp`.
- `Spell.cpp`: `handle_immediate` / `handle_delayed` call `LandOrEmit`; `DoAllEffectOnTarget` / `DoSpellHitOnUnit` / `DoTriggersOnSpellHit` route these caster writes through `Feedback()`: `SendSpellMiss` (unit casters), combo refund, `SetLastDamagedTargetGuid`, health-leech self-heal (caster bonus applied at feedback), `CastItemCombatSpell`, pickpocket reveal, `AtTargetAttacked`, assist `SetInCombatWith`, `InheritCombatStatesFrom`, `SPELL_INTERRUPT_NONPLAYER`, `SpellHitTarget` AI hook, `UpdatePvP` / `SetContestedPvP`, caster achievements, relic cooldown, pre-cast aura and `ADD_TARGET_TRIGGER` casts (duration sync via `SPELLVALUE_AURA_DURATION`), handler combo gain. Everything else in the hit path still writes the live caster (slices 4–5).
- Delivery: land inside a task → `SpellLanded` record to `m_caster` and, when different, to `m_originalCaster` (each applies its own pieces by guid match). Inline hit or no task → applied immediately.
- `Unit::ApplyRecord` dispatches `SpellHit`, `DeferredSpellHit` (lands like a hit; no coordinator yet), `SpellLanded`.
- `SpellMgr::LoadDeferredHitSpells` / `IsDeferredHit` (static list: 45927; effect scan: duel, summon player, RAF summon, send taxi, bind, teleport-units on a non-caster target). Called from `World::SetInitialWorldSettings` after the spell corrections. In slice 1 deferred and far hits run `DoAllEffectOnTarget` inline with both units live.
- `Entities/Unit/UnitView.h/.cpp`: read-only view, live when the running task may access the handle, slot otherwise. `Spell::CasterView()` wraps the live caster; no call site uses it yet.
- Slice-1 bridge (removed in slice 3): the land spell borrows the launch spell's scripts and `m_appliedMods` (`Spell::BorrowScripts` / `ReturnScripts`, `SpellScript::_Rebind`) and `GetUniqueTargetInfo()` returns the launch spell's list, so cast-hook state, chain-target state (`spell_sha_chain_heal`), `PROC_ATTR_REQ_SPELLMOD` charge drops and `spell_pet_gen_valkyr_guardian_smite` / `instance_ruby_sanctum` keep today's behaviour.

Behaviour changes in slice 1, all one beat late on the caster: the feedback list above (including `AtTargetAttacked`, follow-up casts, `CastItemCombatSpell`, `SpellHitTarget`). Combo points on a land-side evade are cleared at launch and re-added at feedback. A GameObject caster without an owner loses `SMSG_SPELLLOGMISS` for a land-side immune result.

Two-pass health landed in slice 5 as a health batch (see there).

### Slice 2 — done (not yet built or played)

- §5.1 crit at launch: `DoAllEffectOnLaunchTarget` applies `SpellCriticalDamageBonus` / `SpellCriticalHealingBonus` after the crit roll, victim read through a `UnitView` overload (`SpellCriticalDamageBonus(Unit const*, SpellInfo const*, uint32, UnitView const*, WeaponAttackType)`; `UnitView::GetCreatureType/Mask` added). `Unit::CalculateSpellDamageTaken` keeps `crit` only for `SPELL_HIT_TYPE_CRIT` and resilience. **Order change:** crit bonus is applied before the taken-side bonus (today: taken then crit); the two are multiplicative for most spells so the number is the same, flat taken bonuses (`TakenTotal`) are no longer doubled by a crit.
- §5.2 taken-side at land: `EffectSchoolDMG`, `EffectHeal`, `EffectHealPct`, `EffectHealMechanical`, `EffectWeaponDmg` no longer call `*BonusTaken` in launch mode; they set `m_pendingTakenKind`, recorded per effect in `TargetInfo::effDamage[] / takenKind[]` (and `SpellHitRecord`). `Spell::ApplyTakenBonuses` runs at the top of `DoAllEffectOnTarget` after reflect resolution, before hit handlers and scripts, so `GetHitDamage()` still sees the taken-adjusted number. **Change:** the taken bonus is applied once per effect at land instead of once per `EffectXxx` call; multi-effect damage spells with differing kinds get each part its own bonus.
- §5.3 aura numbers at launch: `AuraLaunchData` (`Spells/Auras/AuraLaunchData.h`) carries effMask, amounts, old amounts, base amounts, pct mods, crit chance, amplitude, periodic timers, max duration (caster-side mods applied), charges, caster level, haste multiplier, channel data. `Aura::ComputeLaunchData` builds a scratch `LaunchCalcAura` on the caster's task and exports it; `Spell::ComputeAuraLaunchData` fills `TargetInfo::aura` per unit target. At land `Aura::TryRefreshStackOrCreateFromHit` → `Aura::Create(..., launch)` → `UnitAura` → `AuraEffect(Aura*, uint8, AuraLaunchData const&)` (no `CalculateAmount` / `CalculatePeriodic`; `CalculateSpellMod` still runs); refresh goes through `Aura::ModStackAmountFromLaunch` → `RefreshPeriodicFromLaunch` / `SetStackAmountFromLaunch`. `WorldObject::ModSpellDuration` is split: the target half is `Unit::ModSpellDurationTakenSide` (mechanic / dispel duration mods), the self-cast glyph half stays in `ModSpellDuration`; `DoSpellHitOnUnit` calls the taken side only for foreign targets and multiplies by the launch haste multiplier. **Changes:** DoT/HoT amounts, amplitude and the caster-side duration are fixed at launch (plan §14); `SetStackAmountFromLaunch` runs `HandleAuraSpecificMods` only when the caster handle is present.
- §5.4 slot reads at launch: not done. `SpellHitResult`, `SpellTakenCritChance`, `CalculateAOEDamageReduction`, `IsImmunedTo*` still read the live target at launch (step 13's counter lists them). They need `UnitView` overloads whose slot fields the audit confirms; adding them blind was judged more likely to break the build than to help.

Remaining caster reads inside the land-side aura path (slice 4 rows): `Aura::Create` → `_AddAura` single-target registration on the caster, `HandleAuraSpecificMods(caster)`, `ApplyDiminishingToDuration(caster)`, later `RecalculateAmount()` through `GetCaster()`.

### Slice 3 — done (not yet built or played)

- `Spell::LandOrEmit`: inside a task (`BeatContext::InTask()` and the target in the world) the record is appended with `sRecordLogs->Append(casterGuid, unit, RecordKind::SpellHit, hit)` and lands at the start of the target's next update (`Unit::ApplyRecord` → `ApplySpellHit`); `TargetInfo::landPending / landEmittedMs` mark it. Outside a task (unit tests, casts between beats) the record is still consumed on the spot with the launch spell's scripts borrowed (`BorrowScripts` / `ReturnScripts` stay for that path only). Self-hits and reflects stay inline; far targets (`!unit`, 45927) stay on today's inline far path until step 09.
- `Deferred` coordinator: spells in `SpellMgr::IsDeferredHit` build the record and call `Map::DeferSpellHit`; `Map::ApplyDeferredSpellHits` runs after `sScriptMgr->OnMapUpdate` with no task running (`ASSERT(!BeatContext::InTask())`), resolves the target by guid type (`ObjectAccessor::GetPlayer(map, guid)`, `Map::GetPet`, `Map::GetCreature`) and lands it with both units live. `_deferredSpellHits` is `std::vector<std::unique_ptr<DeferredSpellHitRecord>>` so `Map.h` only needs a forward declaration.
- Record fields replacing the slice-1 bridge: `SpellHitRecord::target`, `appliedModAuraIds[8] / appliedModCount` (rebuilt into `m_appliedMods` through the caster handle in the land constructor; the proc path reads them), `unitTargetCount[3] / unitTargetIndex[3]` (see slice 4), `castTime`, `powerCost`, `originalTarget`.
- Channels: `UpdateChanneledTargetList` keeps a target whose record has not landed yet for up to `SPELL_LAND_PENDING_GRACE_MS` (1 s) when its aura is not on the target; a channel whose first hit is resisted ends by the "lack of targets" rule up to one second later than today.
- Land without a caster handle: **not done**. `Unit::ApplySpellHit` still drops a hit whose caster left the map between launch and land (debug log). Making `m_caster` nullable through the hit path is the slice 4 audit.

### Slice 4 — partial (the compile-driven part is open)

Done without a build:

- `SpellScript::GetCasterView()` (`UnitView` over `Spell::CasterView()`), for hit hooks that read the caster.
- `Spell::GetUnitTargetCountForEffect(SpellEffIndex)` / `GetUnitTargetIndexForEffect(SpellEffIndex)` and the `SpellScript` wrappers (same names as TrinityCore's `Spell::GetUnitTargetCountForEffect`; the index is ours). At launch they count `m_UniqueTargetInfo`; at land they read the values carried in `SpellHitRecord`. A hit landing next beat has a fresh `SpellScript` instance, so state set in `OnObjectAreaTargetSelect` / cast hooks is not visible in hit hooks; the scripts that relied on it now read the launch count instead: `spell_marrowgar_bone_slice`, `spell_sindragosa_s_fury`, `spell_ahn_kahet_swarmer_aura`, `spell_skadi_reset_check`, `spell_pal_divine_storm_dummy`, `spell_gahzranka_slam`, `spell_morogrim_tidewalker_watery_grave` (index for the per-target grave spell), `spell_illidari_council_empyreal_balance` / `_equivalency` (`AfterCast` counts hitting targets from the launch list instead of counting hits).

Open, needs the build:

- The rename of `m_caster` on the land path and the per-handler fix-up (§7.6 table), `AuraEffect::CalculateAmount` target type (§15.1), `AuraEffect::RecalculateAmount` through a view (§7.5). Every handler and script still reaches the caster through the live handle; at runtime that is a foreign write counted by step 13's `ASSERT_OWNED` / counter, not a crash, because `BeatContext::inBeat` is false until step 12.
- Known script regressions from asynchronous landing that the count/index API does not cover: `spell_pet_gen_valkyr_guardian_smite` (edits the caster's own `TargetInfo` from other targets' hits; the caster entry is processed inline at launch, so the self-heal share is lost → `Feedback().AddFollowUp` with a heal spell), `spell_dk_raise_dead` (`GetCaster()->CastSpell` from the corpse target's hit; `Deferred` row 46584 or a follow-up), `spell_pal_lay_on_hands` (`_manaAmount` from `CheckCast` used in `OnHit` on a foreign target; Glyph of Divinity energize lost → carry it in the record or a `Deferred` row per rank).
- `Unit::ApplySpellHit` without a caster handle (above).

### Slice 5 — done (not yet built or played)

`Entities/Unit/CombatRecords.h`; `RecordKind::{DamageDealt, HealDone, KillingBlow, KillCredit, ProcActor}`; dispatched in `Unit::ApplyRecord`. Each split is a runtime route on `BeatContext::IsForeign(otherUnit)`: inline when the other unit is the owner or in its task group (and always outside a task), a record to its inbox otherwise. The victim-side code is unchanged.

- `Unit::DealDamage` → `DamageDealtRecord` (appended on every exit path by a scope guard): `GetAI()->DamageDealt`, `DealDamageShieldDamage(victim)`, rage from damage done / from a fully absorbed hit, battleground `SCORE_DAMAGE_DONE` + `ACHIEVEMENT_CRITERIA_TYPE_DAMAGE_DONE`, player durability roll (HIT DONE). Applied by `Unit::ApplyDamageDealt`. Left inline: `sScriptMgr->OnDamage` (global hook), `AtTargetAttacked` inside the `SPELL_AURA_SHARE_DAMAGE_PCT` loop (rare; a `CombatRef`-style record later), the duel end block (`Deferred` in the plan).
- `Unit::DealHeal` → `HealDoneRecord`: `GetAI()->HealDone` to the healer, battleground `SCORE_HEALING_DONE` + `HEALING_DONE` criteria to the credited player (healer or totem owner). `Unit::ApplyHealDone`.
- `Unit::Kill` → `KillingBlowRecord`: `GET_KILLING_BLOWS` criteria to the killer player, `AI()->KilledUnit(victim)` to the killer creature and to the killer's pet (each record goes to the unit that writes). `Unit::ApplyKillingBlow`. Left inline: loot generation and looter broadcast (victim and group), `OutdoorPvP` / `Battlefield` / `Battleground::HandleKill*` (map-level managers), script hooks.
- `KillRewarder` → `KillCreditRecord` per member (§7.3a): `_RewardGroup` / `Reward` call `_RewardOrRecord`, which appends the rewarder's state (`xp`, `groupRate`, `aliveSumLevel`, `count`, `maxLevel`, `maxNotGrayMemberLevel`, `isFullXP`, `isDungeon`, `isBattleGround`, `isPvP`, `inGroup`, victim entry/level) for a foreign member; `KillRewarder::RewardFromCredit` rebuilds a rewarder from the record on the member's task and runs `_RewardPlayer` with the victim as a handle. If the victim already left the world (a summon despawning on death) XP and kill credit are given by entry, reputation and the honor roll are skipped.
- `Unit::ProcSkillsAndAuras` → `ProcActorRecord` (§8.1): when the actor is foreign and the victim is not, the victim half runs inline (`ProcSkillsAndReactives(true, ...)` and `TriggerAurasProcOnEvent` with `typeMaskActor = 0`), the actor half is a record (proc flags, hit mask, amount, phase, spell/aura ids, `DamageInfo` and `HealInfo` as numbers, `m_appliedMods` as ids, `IsProcDisabled`). `Unit::ApplyProcActor` rebuilds `DamageInfo` / `HealInfo` (`DamageInfo::SetAttackType` added) and runs the actor half with `procSpell == nullptr`; `TriggerAurasProcOnEvent` takes `appliedMods` as a parameter for the mod owner's procs when there is no `Spell`. A `ProcActor` whose target left the world is dropped (handlers dereference the action target). **Changes:** actor-side procs land one beat after the hit (plan §8); `ProcSkillsAndReactives` on the actor has no `m_weaponItem` for `UpdateCombatSkills`; `Aura::GetProcEffectMask` checks that read the proc `Spell` (triggered-by-aura, cast item) do not run on the actor side.
- Two-pass health (step 06 rule): **done, as a health batch rather than a sum-then-apply pass.** `Unit::ApplyRecords` opens a batch (`BeginHealthBatch` / `EndHealthBatch`, RAII); inside it `ModifyHealth` accumulates into `_pendingHealthDelta` instead of writing the field, `GetHealth()` returns field + delta so every per-record check (`health <= damage`, duel clamp, sparring, overheal, `GetHealthGain` for leech, `HealthBelowPct`) sees the health the earlier records produced, and `UNIT_FIELD_HEALTH` is written once at the end. `SetHealth` is authoritative and clears the delta, so a kill (`setDeathState` → `SetHealth(0)`) or a duel end is one write too. Records still apply in order and death is decided at the record whose damage reaches zero (that record's source is the killer; later records land on a dead unit exactly as today), which is the step 06 property — one `SetHealth` per beat, one consistent death — without the plan 06 ordering (heals before damage, health before effects) that would have needed `DoAllEffectOnTarget` split across N live land spells and `DealDamage` taught to skip its own write and clamps. That literal form stays open until there is a build to drive it; it is only worth doing if the ordering itself turns out to matter.

`.debug foreignreads` for a spell-only fight is expected to be non-zero after this slice: the launch path still reads the target live (§5.4), the land path still reads the caster live (slice 4), and the victim halves above read the other unit through its handle.
