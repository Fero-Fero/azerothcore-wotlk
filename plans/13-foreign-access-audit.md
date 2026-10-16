# 13 — Foreign-access audit at key 1

Turn on the `ASSERT`s that make the one-writer rule enforceable, then play at key 1 until they stop firing. Every hit is a site that reads or writes a live field of a unit it does not own. The fix is always one of: read the slot, append a record, append a defer, or add to the `Deferred` table. This step is done before any worker starts because a violation at key 1 is an `ASSERT` with a stack and at key 2 is a race.

Live pointers are not the problem. During a beat every container is read-only, so `ObjectAccessor::FindPlayer`, `GetUnit`, `Group::GetFirstMember`, `HashMapHolder::Find`, `Map::GetCreature`, `Unit::GetVictim`, `Aura::GetCaster` all return valid **handles**. The audit is on what the caller does with the handle.

## The checks

In `Object.h/.cpp`, debug builds only (`ACORE_DEBUG`), using `ASSERT_OWNED` from step 04:

| Method | Check |
| --- | --- |
| `Object::GetInt32Value`, `GetUInt32Value`, `GetUInt64Value`, `GetFloatValue`, `GetByteValue`, `GetUInt16Value`, `GetGuidValue`, `HasFlag`, `HasByteFlag`, `HasFlag64` | `ASSERT_OWNED(this)` — except index `OBJECT_FIELD_GUID`, `OBJECT_FIELD_ENTRY`, `OBJECT_FIELD_TYPE` which are immutable after create and read everywhere for identity |
| Every `Object::Set*`, `Apply*`, `SetFlag`, `RemoveFlag`, `ToggleFlag`, `AddGuidValue`, `RemoveGuidValue`, `ForceValuesUpdateAtIndex` | `ASSERT_OWNED(this)` |
| `Unit::GetAppliedAuras`, `GetOwnedAuras`, `GetAuraEffectsByType`, `GetAuraApplication*`, `GetAura*`, `HasAura*`, `GetTotalAuraModifier*`, `GetMaxPositiveAuraModifier*`, `GetDiminishing`, `GetThreatMgr`, `GetCombatManager`, `getAttackers`, `GetVictim` (returns a handle: allowed), `GetCurrentSpell`, `GetMotionMaster`, `movespline` access, `m_Controlled` iteration, `GetSharedVisionList`, `GetVehicleKit()->GetPassenger` | `ASSERT_OWNED(this)` |
| `Unit::m_movementInfo` getters (`GetUnitMovementFlags`, `HasUnitMovementFlag`), `GetUnitState`/`HasUnitState`, `getDeathState`, `IsInCombat`, `IsAlive` | `ASSERT_OWNED(this)`; slot equivalents exist on `UnitView` |
| `Player::GetGroup` (handle: allowed), `Player::GetSession` (allowed; the session object is owned by the player's task when the player is), `Player::GetQuestStatus`, item/inventory getters, `GetReputation*`, achievement criteria | `ASSERT_OWNED(this)` |
| `ObjectVisibilityContainer::GetVisiblePlayersMap`, `GetVisibleWorldObjectsMap` | `ASSERT_OWNED(_selfObject)` |
| `GameObject::GetGoState`, `getLootState`, `GetGOInfo` (static: allowed) | `ASSERT_OWNED(this)` on the mutable ones |
| `Position` getters | **not checked** (overview invariant 9) |
| Every mutator in the step 03 **Global singletons** table marked *between beats* or *read-only* (`ObjectMgr::AddCreatureToGrid`, `*::Reload*`, `AuctionHouseMgr`, `MailMgr`, `TicketMgr`, `GroupMgr::AddGroup/RemoveGroup`, `WorldSessionMgr` add/remove, `MapMgr::CreateMap/DestroyInstance`, …) | `ASSERT_NOT_IN_TASK()` (step 04 macro) |
| Every mutator marked *defer* (`PoolMgr::UpdatePool/SpawnPool`, `InstanceSaveMgr::PlayerBindToInstance`, `GameEventMgr::StartEvent/StopEvent`, `Battleground::UpdatePlayerScore`, `WorldState::setWorldState`, …) | `ASSERT(!BeatContext::inBeat)` — legal only in S2 or between beats, not even in a map seal task |
| Every mutator marked *mutex* (`CharacterCache`, `AchievementGlobalMgr::SetRealmCompleted`, `Guild` member map) | the mutex is taken inside; no assert. A debug counter of lock acquisitions per beat is printed by `.debug singletons`; a count that scales with population means a kind was mis-marked and should be a defer |

`MayAccess` returns `true` when `inBeat` is false, so load, seal phase S2, `UpdateSessions`, and scripts run between beats are unaffected. Inside a task it also returns `true` for every member of the owner's task group (a vehicle and its passengers, a charmer and its charmed unit — step 12), which is why `GetVehicleKit()->GetPassenger(...)->X()` from the vehicle's update and `GetCharmer()->X()` from the charmed unit's AI do not need rewriting. In a map seal task (S1b) it returns `true` for anything on that map; in a grid task (S1a) only for objects routed to that grid this beat (`_pendingInGrid`), which is how a `CleanupsBeforeDelete(prepared = true)` that still writes another unit is caught: that other unit is not in the grid's pending list.

## Procedure

1. Build debug. Boot at key 1. Idle on an empty continent for one minute: zero asserts expected (nothing interacts).
2. One character: walk, talk to an NPC, accept and complete a quest, loot, vendor, mount, fly a taxi, use the bank and mail (mail is THREADUNSAFE: between beats). Each assert is a row.
3. Two characters: duel with every class ability available, party up, trade (THREADUNSAFE), inspect, follow each other across a cell boundary, one dies and the other resurrects, both ride a ship.
4. Creatures: a pack pull with a pet, a caster mob, a patrol, a boss with scripted phases (Deadmines fully), an escort quest, a vehicle quest in Icecrown or Wintergrasp, a totem, a mind-controlled mob. Kill everything: every death runs `PrepareRemoval` on the dying unit's own task (so any cleanup that still writes another unit — a `ThreatMgr` or `CombatManager` method, a summon owner list, a `GameObject` owner, a visibility map, an aura application on a caster — asserts there with a stack under `ASSERT_OWNED`), and the grid item under `GridTaskScope` asserts `_removing` and does nothing but `RemoveFromGrid`.
5. Battleground: Warsong Gulch with two characters (flag capture, score defers).
6. Singletons: pooled spawn despawn and respawn, a game event start from a script (`.event start`), an instance boss kill that binds the group, a level-up (character cache), a realm-first achievement, a guild member levelling. `ASSERT_NOT_IN_TASK` / `!inBeat` quiet; `.debug singletons` lock counts not scaling with population.
7. **Position-derived math on foreign units** (invariant 9 allows torn position reads; it does not make every use of them safe): grep for `GetPosition*`, `GetExactDist*`, `GetAngle(`, `GetRelativeAngle(`, `GetNearPoint*`, `MovePositionToFirstCollision`, `GetContactPoint` where the argument is not `this`/`me`, and for any normalisation of a vector built from two units' positions (`/ dist`, `normalize`, `1.0f / `). A torn read that produces `dist == 0` divides by zero; a torn read fed to `atan2` produces a wrong facing for one beat, which is tolerable. Fix the division sites by clamping (`std::max(dist, 0.01f)`), or by reading `ReadSlot().pos` (a consistent snapshot) where the caller already has a slot. List each site in the output table under shape "position math".
8. Record each assert in the table below and fix it in the owning step's files. Repeat 2–7 until a full pass has no assert.

## Fix patterns

| Site shape | Fix |
| --- | --- |
| `target->GetHealth()`, `->GetLevel()`, `->HasAura(x)`, `->GetPower()`, `->IsAlive()`, `->IsInCombat()`, `->GetFaction()`, `->HasUnitState()` on a foreign unit | `UnitView(target).X()` |
| `target->GetAuraEffectsByType(T)` iteration for a sum | `UnitView(target).GetTotalAuraModifier(T)` |
| `target->GetAura(id)->GetDuration()` | `UnitView(target).GetAura(id)->duration` |
| Movement generator reading `GetVictim()->GetPosition()` / `IsWithinMeleeRange(victim)` | position is allowed; `IsWithinMeleeRange` reads `GetCombatReach` (`UNIT_FIELD_COMBATREACH`) → `UnitView` overload |
| `target->CastSpell(...)`, `->AddAura(...)`, `->RemoveAura*`, `->SetHealth`, `->ModifyPower`, `->GetMotionMaster()->Move*`, `->KnockbackFrom`, `->SetInCombatWith`, `->AddThreat` | record kind from steps 06–08; for spell effects on the target the handler is already on the target (step 07) |
| SmartAI / `CreatureAI` body doing `caster->X()` in `SpellHit`, `AttackedBy`, `DamageTaken`, `JustDied(killer)`, `KilledUnit(victim)`, `MoveInLineOfSight(who)` | `who` is `UnitView` (steps 07/08 changed the signatures); writes become `me->CastSpell(who.Handle(), …)` (a cast by the owner, which lands next beat) or `Deferred` |
| `Group`/`Guild` iteration writing members (`SendUpdate`, `BroadcastPacket`) | packets are writes to the member's session: `member->GetSession()->SendPacket` from a foreign task is an **allowed exception** — `WorldSession::SendPacket` enqueues on the socket's own locked queue (`_sendQueue`), so it is thread-safe today. Add `ASSERT`-free `WorldSession::SendPacketFromAnyThread` and make `SendPacket` assert-free; document it |
| GM commands run from a task (`CMSG_MESSAGECHAT` is THREADUNSAFE, so normally not) | none expected |
| `ObjectAccessor::FindPlayer(guid)->Set*` from a script (e.g. quest credit to another player) | `KillCredit`-style record or a defer |
| `Transport::AddPassenger` / `UpdatePassengerPositions` writing passenger fields | Already seal-only (`DelayedUpdate` in the map seal task). An assert here means a transport method is being called from a unit task (e.g. a player boarding) — the boarding call appends a `Relink`-style defer and the seal does the add |
| `Vehicle::AddPassenger` / `Unit::SetCharmedBy` first join from a unit task | `TaskGroupJoin` defer (step 09); the body runs at the seal |
| `sPoolMgr`, `sInstanceSaveMgr`, `sGameEventMgr`, `sCharacterCache`, … mutators | per the step 03 singletons table: defer, mutex, or move to between beats |

## Output

A table in this file's exit note:

| # | File:line | Caller | Shape | Fix | Step |
| --- | --- | --- | --- | --- | --- |

A row with "cannot be reached from a task" needs the reason (e.g. only called from `UpdateSessions`).

## Exit

A full pass of the procedure with zero asserts in a debug build. The `.debug foreignreads` counter from step 06 reads 0 for the same pass in a release build.

## Checklist

- [ ] All listed getters and setters carry `ASSERT_OWNED` in debug.
- [ ] Every singleton mutator in the step 03 table carries `ASSERT_NOT_IN_TASK` or `ASSERT(!inBeat)`; `.debug singletons` prints mutex counts.
- [ ] `Position` getters excluded; `SendPacket` excluded and documented; position-math division sites clamped or moved to slot reads.
- [ ] Procedure steps 1–7 run to zero asserts.
- [ ] Exit note table complete.
