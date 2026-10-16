# 08 — Melee, periodic ticks, area auras on records

Step 07 covers direct spell hits. This step puts the three remaining cross-unit combat paths on the same records: auto-attack, periodic aura ticks, and area auras. After this step a fight between two units on two tasks has no inline write from one to the other.

## 1. Melee

### Today

`Unit::AttackerStateUpdate(victim, attType, extra, ignoreCasting)` (`Unit.cpp:2760`):

1. attacker-side checks (casting, `HasUnitState(UNIT_STATE_CANNOT_AUTOATTACK)`, `IsNonMeleeSpellCast`),
2. `CalculateMeleeDamage(victim, &damageInfo, attType, sittingVictim)` (1678) — attacker stats plus victim armor, defense skill, dodge/parry/block rolls via `RollMeleeOutcomeAgainst(victim, ...)`, victim absorb/resist via `CalcAbsorbResist`, all reading the victim,
3. `SendAttackStateUpdate(&damageInfo)` (6674) — broadcast from the attacker,
4. `DealMeleeDamage(&damageInfo, true)` (2020) — victim health, durability, `DealDamage`, victim procs, attacker procs, rage both sides, `CastItemCombatSpell`,
5. `ProcSkillsAndAuras` and extra attacks.

### Split

```cpp
struct MeleeHitRecord
{
    ObjectGuid attacker; uint8 attackType; bool extra;
    // CalcDamageInfo fields computed on the attacker from its own stats and the victim's slot
    uint32 damageSchoolMask; uint32 damage; uint32 absorb; uint32 resist; uint32 blockedAmount;
    uint32 hitInfo; uint8 targetState; uint8 hitOutCome;      // MeleeHitOutcome
    uint32 procAttacker, procVictim, procEx;
    uint32 cleanDamage; uint8 cleanDamageAttackType; uint8 cleanDamageHitOutCome;
    uint32 weaponSpeedHitFactor;      // for the attacker's RewardRage, carried back in MeleeLanded
    uint8 attackerLevelForTarget;
};

struct MeleeLandedRecord
{
    ObjectGuid victim; uint8 attackType; uint32 hitInfo; uint8 hitOutCome;
    uint32 damage, absorb, resist, blocked, overkill; bool killed; ObjectGuid victimGuid; uint32 victimEntry; uint8 victimLevel; bool victimWasPlayer;
    uint32 procAttacker, procVictim, procEx; uint32 weaponSpeedHitFactor;
    bool victimWasEngaged; bool targetHostile;
    FollowUpCast follow[2]; uint8 followCount;   // victim-side procs that want the attacker to cast (rare)
    uint8 extraAttacks;                           // Sword Specialization etc. rolled on the attacker side at feedback, not here; reserved
};
```

- `AttackerStateUpdate` runs steps 1–3 on the attacker. Step 2's victim reads take `UnitView const& victim` (`RollMeleeOutcomeAgainst`, `GetUnitDodgeChance`, `GetUnitParryChance`, `GetUnitBlockChance`, `GetArmor`, `GetDefenseSkillValue`, `CalcArmorReducedDamage`, `CalcAbsorbResist` — absorb needs the victim's absorb auras and their remaining amounts: `SlotAura::amount[]` carries the current absorb amount, so the attacker can **predict** absorb but must not consume it). Therefore: the attacker computes everything except absorb/resist/block consumption; those run on the victim in the land step from the live absorb auras. The record carries the pre-mitigation `damage`, `hitOutCome`, `hitInfo`, and `blockedAmount = 0`; the land step fills absorb, resist, blocked.
- `SendAttackStateUpdate` moves to land: the victim broadcasts the packet with the attacker's guid from its own position after mitigation (so the numbers are final). Same recipients in practice.
- Land (`Unit::ApplyMeleeHit`): `CalcAbsorbResist` on self, block, `DealDamageVictimSide`, durability on self, `RewardRage(damage, 0, false)` (victim rage), `ProcVictimSide`, `AI()->AttackedBy`/`DamageTaken`, interrupt on damage, `SetInCombatWith` own half + `CombatRef`. Then `MeleeLandedRecord` to the attacker.
- Feedback (`Unit::ApplyMeleeLanded`): `ApplyDamageDealt`, `RewardRage(damage, weaponSpeedHitFactor, true)`, `ProcActorSide`, `CastItemCombatSpell(victimGuid, ...)`, `AtTargetAttacked`, extra attacks roll (`HandleProcExtraAttackFor` → more `MeleeHitRecord`s next beat), `ApplyKill` if killed.

The two-pass health rule from step 06 applies: ten melee hits and three spell hits on one target in one beat are one `ModifyHealth`.

`Unit::Update` 569–587 (extra attacks via `ObjectAccessor::GetUnit`) becomes: the attacker emits a `MeleeHitRecord` with `extra = true` per extra attack; `IsWithinMeleeRange` reads the victim's slot.

## 2. Periodic ticks

### Today

The aura is owned by the target (`m_ownedAuras`). `Unit::_UpdateSpells` → `Aura::UpdateOwner(diff, owner)` → `Aura::Update(diff, caster)` with `caster = GetCaster()` (an `ObjectAccessor` lookup) → `AuraEffect::Update` → `PeriodicTick(aurApp, caster)` (`SpellAuraEffects.h:98`) → e.g. `HandlePeriodicDamageAurasTick(target, caster)` (6277), which:

- reads `caster->SpellDamageBonusDone(...)`, `caster->SpellCriticalDamageBonus`, `caster->GetSpellModOwner()`, `caster->SpellHitResult(target, ...)` for Consecrate-style ticks,
- writes `caster->SendSpellNonMeleeDamageLog`, `caster->DealDamage(...)` (attacker-side half), `caster->ProcSkillsAndAuras` actor side, `caster->EnergizeBySpell` for drains, `caster->HealBySpell` for leech ticks.

### Change

With step 07's launch snapshot, `m_amount` is the ticked number (done-side bonus and spell power already inside). `PeriodicTick` keeps `UnitView caster` instead of `Unit* caster`:

- `Aura::Update(diff, UnitView caster)`: `caster = UnitView(ObjectAccessor::GetUnit(*owner, m_casterGuid))`, a view that is empty if the caster is gone (today's `caster == nullptr` branches stay).
- `HandlePeriodicDamageAurasTick(target, UnitView caster)`: the `SpellDamageBonusDone` call is removed (amount is final; `m_canBeRecalculated` effects were recomputed from the slot in `RecalculateAmount`); crit uses `m_critChance` already stored on the effect? — AC rolls `caster->IsSpellCrit(...)` per tick for some auras: keep the roll, with the chance computed from the slot (`SpellDoneCritChance` view overload) or fixed at launch (`auraCritChance[i]` added to `SpellHitRecord`). Choose launch-fixed; it matches the amount snapshot.
- `SpellHitResult` for Consecrate-style ticks: view overload (resistance from the target live, hit rating from the caster slot).
- Target-side writes stay inline (the target is the owner): `CalcAbsorbResist`, `DealDamageVictimSide(casterView, target, ...)`, `SendSpellNonMeleeDamageLogFrom(casterGuid, ...)` from the target, `SendPeriodicAuraLog`, `ProcVictimSide`.
- Caster-side writes become one `SpellLandedRecord` with `periodic = true` per tick: damage/absorb/resist/overkill, `killed`, `healthLeechGain`, `powerGain`, proc inputs. `Unit::ApplySpellLanded` already handles it (step 07 §8); the `periodic` flag selects `SPELL_PERIODIC_DAMAGE` proc phase and skips the combat-entry parts that only apply to the first hit.
- Heal ticks: `HandlePeriodicHealAurasTick(target, casterView)` likewise; `DealHealVictimSide`.
- `HandlePeriodicManaLeechAuraTick`, `HandlePeriodicHealthFunnelAuraTick`, `HandleObsModPowerAuraTick`, `HandlePeriodicEnergizeAuraTick`, `HandlePeriodicPowerBurnAuraTick`: target half inline, caster half (`EnergizeBySpell(caster)`, `ModifyHealth(caster)`, the funnel's `caster->ModifyHealth(-...)`) as feedback fields `powerGain`, `healthLeechGain` (negative for health funnel, which costs the caster health).
- `HandlePeriodicTriggerSpellAuraEffect`: `caster->CastSpell(target, triggerSpellId, true, nullptr, this)` → `FollowUpCast` in the feedback (one beat later than today); `target->CastSpell(target, ...)` variants (`triggerCaster == target`) stay inline.

`AuraEffect::PeriodicTick`'s `aurApp->GetTarget()` is the owner; nothing changes there.

## 3. Area auras

### Today

`UnitAura::FillTargetMap(targets, caster)` picks units around the owner for `SPELL_EFFECT_APPLY_AREA_AURA_*`; `Aura::UpdateTargetMap(caster, apply)` calls `_ApplyForTarget` / `_UnapplyForTarget` which insert an `AuraApplication` into **each target's** `m_appliedAuras`. One `Aura` object (owned by the caster, in the caster's `m_ownedAuras`) has a `m_applications` map of `guid → AuraApplication*`, each holding `Unit* _target`. `Aura::Remove` walks them all.

### Change: per-target clones

```cpp
struct AreaAuraApplyRecord { uint32 spellId; ObjectGuid caster; uint8 effMask; int32 amount[3]; int32 amplitude[3]; int32 maxDuration; uint8 canRecalcMask; uint32 castItemEntry; };
struct AreaAuraRemoveRecord { uint32 spellId; ObjectGuid caster; uint8 removeMode; };
```

- The owner's `Aura` keeps `m_applications` but as a **guid set** (`std::unordered_set<ObjectGuid>` of current members). `UpdateTargetMap` runs on the owner's task, computes membership from slots (`Cell::VisitObjects` over live grid lists, reading `ReadSlot()` for range, faction, group via `Group` lookups which are read-only during a beat), and appends `AreaAuraApply` to new members and `AreaAuraRemove` to departed ones. The owner's own application stays inline.
- On a target, `AreaAuraApply` runs `Aura::TryRefreshStackOrCreateFromHit(...)` (step 07 §7.5) creating a **target-owned** `UnitAura` with `m_casterGuid = caster` and a new flag `m_isAreaClone = true`. The clone ticks on the target (§2) and expires on its own `maxDuration` if no refresh record arrives (`UpdateTargetMap` refreshes members every `UPDATE_TARGET_MAP_INTERVAL` as today; the clone's duration is set to that interval plus one beat so a lost owner removes it naturally, except for permanent auras where the owner's removal is the `AreaAuraRemove` record and a `CleanupsBeforeDelete` sweep at the seal handles a vanished owner).
- `Aura::Remove` on the owner appends `AreaAuraRemove` to every guid in the set.
- `Aura::GetApplicationOfTarget(guid)`, `GetApplicationMap()`, `GetUnitOwner()->...` callers that walked applications to touch targets become records or slot reads. The step 13 `ASSERT` finds the remainder.
- Charges and stacks on area auras are per clone. Nothing in 3.3.5 relies on shared charges across area-aura targets; note it as a behaviour change.

Dynamic-object persistent area auras (`DynObjAura`): the dynobj is a task; `FillTargetMap` runs on it the same way.

## 4. `AuraScript` and AI hooks

- `AuraScript::GetCaster()` returns `UnitView` in tick, proc, and apply/remove hooks (the aura's owner is the task; the caster is foreign). `GetCasterUnit()` with an `ASSERT` for the caster-owned paths (area-aura owner side).
- `CreatureAI::SpellHit(UnitView caster, SpellInfo const*)` (changed in step 07), `AttackedBy(UnitView)`, `DamageTaken(UnitView attacker, uint32&, DamageEffectType, SpellSchoolMask)` — the `uint32& damage` out-parameter still works (victim side). `DamageDealt(Unit* victim, ...)` runs at feedback on the attacker with the victim handle. SmartAI event bodies that call `caster->CastSpell(...)` or `attacker->Kill(...)` on the foreign unit compile-fail and become follow-up casts on the owner (`me->CastSpell(caster, ...)`) or `Deferred`.

## 5. Slices

1. `MeleeHitRecord` / `MeleeLandedRecord`, melee lands next tick. Check: a duel, each swing lands next tick, rage on both sides, Sword Specialization extra attacks appear the tick after the proc.
2. Periodic ticks with `UnitView caster` and periodic feedback. Check: Corruption, Rejuvenation, Drain Mana, Health Funnel, Consecration; the tick numbers equal the step 07 slice 2 recording; caster-side procs (Nightfall) fire one tick after the tick.
3. Area aura clones. Check: Devotion Aura in a party of 3 as members walk in and out of range; Tranquility; Consecration under three enemies; a dead member loses the aura within one interval; the owner dying removes it next tick.
4. `AuraScript::GetCaster()` and AI hook signature changes; fix the compile errors; exit note gets the counts.

## 6. Behaviour changes

- Melee lands one beat after the swing; `SMSG_ATTACKERSTATEUPDATE` is sent by the victim's task.
- Periodic crit chance is fixed at application.
- Area aura membership changes land one beat later; charges and stacks are per target.
- `PeriodicTriggerSpell` with the caster as trigger caster fires one beat later.

## Checklist

- [ ] `AttackerStateUpdate` emits `MeleeHitRecord`; land does absorb/resist/block, health, victim procs, `SendAttackStateUpdate`; feedback does rage, actor procs, extra attacks, `AtTargetAttacked`, kill.
- [ ] `PeriodicTick` takes `UnitView`; no `caster->Deal*`/`Send*`/`Energize*`/`Heal*` inside a tick handler.
- [ ] Area auras: owner holds a guid set; targets hold clones; apply/remove are records.
- [ ] `AuraScript::GetCaster()` is a view on foreign-caster hooks.
- [ ] Foreign-read counter is 0 for a melee-plus-DoT fight.
