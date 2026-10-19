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

- [x] All listed getters and setters carry `ASSERT_OWNED` in debug.
- [x] Every singleton mutator in the step 03 table carries `ASSERT_NOT_IN_TASK` or `ASSERT(!inBeat)`; `.debug singletons` prints mutex counts.
- [x] `Position` getters excluded; `SendPacket` excluded and documented; position-math division sites clamped or moved to slot reads.
- [ ] Procedure steps 1–7 run to zero asserts. **Not run**: the audit was done statically; building and running the
      debug server was outside what this session was allowed to do. The rows below marked `static` are the sites
      that would have asserted on the first beat and were fixed ahead of the play-test; rows marked `open` are known
      remaining foreign reads whose fix belongs to another step and will show up as the first asserts of step 1–4.
- [x] Exit note table complete (for the static pass).

## Exit note

### What was turned on

- `Object.cpp`: `AUDIT_FIELD_READ(index)` on every `Get*Value`, `HasFlag`, `HasByteFlag` (`HasFlag64` goes through
  `GetUInt64Value`), skipping indices below `OBJECT_FIELD_SCALE_X` (GUID/TYPE/ENTRY); `AUDIT_FIELD_WRITE()` on
  every setter, flag mutator and `ForceValuesUpdateAtIndex`. Both are `ASSERT_OWNED(this)` under `ACORE_DEBUG`.
- `Unit.h/.cpp`: aura containers, `GetAuraApplication`, `GetOwnedAura`, `GetDiminishing`, `GetThreatMgr`,
  `GetCombatManager`, `getAttackers`, `GetCurrentSpell`, `GetMotionMaster`, `GetSharedVisionList`, movement
  flags, unit state, `getDeathState`, `IsAlive`. `GetVictim`/`GetCharmer`/`GetOwner` stay handles.
- `Player`: `GetQuestStatus`, `GetItemCount`, `GetItemByGuid`, `GetItemByPos`, `HasItemCount`, `GetReputationMgr`,
  `GetAchievementMgr`, `UpdateAchievementCriteria`. `ObjectVisibilityContainer` maps. `GameObject::getLootState`.
  `Vehicle::GetPassenger` asserts on the vehicle owner (`_me`), which the task group makes true for passengers.
- `BeatContext::MayAccess` now resolves non-world objects: an `Item` is owned by its owner's task (owner guid via
  `FindConnectedPlayer`, recursion guard on the asserted getter), other plain `Object`s and not-in-world
  `WorldObject`s are free. Previously every bag/item read from the player's own task would have asserted.
- New `ASSERT_NOT_IN_BEAT()` for the *defer* row (legal in S2 and between beats only), `BeatContext::InBeat()`,
  `BeatContext::DeferMap()` (the map to append a defer to from a unit task **or** a map seal task),
  `BeatContext::singletonLocks[]` / `singletonLocksInBeat[]` + `.debug singletons` (`cs_debug.cpp`).
- `ASSERT_NOT_IN_TASK()`: `ObjectMgr::Add/RemoveCreatureToGrid`, `Add/RemoveGameobjectFromGrid`,
  `DeleteCreatureData`, `DeleteGOData`, `AddCreData`, `AddGOData`, `SetCreatureLinkedRespawn`;
  `FormationMgr::LoadCreatureFormations`; `GroupMgr::AddGroup/RemoveGroup`; `WorldSessionMgr::AddSession_`;
  `MapMgr::CreateMap/UnloadAll`; `AuctionHouseMgr::AddAItem/RemoveAItem`; `TicketMgr::AddTicket/CloseTicket/
  RemoveTicket`; `Guild::AddMember/DeleteMember`; `CharacterCache::Add/DeleteCharacterCacheEntry`.
- `ASSERT_NOT_IN_BEAT()`: `PoolMgr::SpawnPool/DespawnPool/UpdatePool`; `InstanceSaveMgr::AddInstanceSave/
  DeleteInstanceSavedData/PlayerBindToInstance/UnbindAllFor`; `Battleground::UpdatePlayerScore/HandleKillPlayer`;
  `OutdoorPvP::HandleKill`; `Battlefield::HandlePlayerEnterZone/LeaveZone`; `WorldState::setWorldState`;
  `GameEventMgr::StartEvent/StopEvent` (after the in-beat path has appended its defer).
- Mutex rows: `CharacterCache` (`std::shared_mutex`, shared on the 12 getters, unique + counter on the 9
  mutators); `AchievementGlobalMgr::SetRealmCompleted` / `IsRealmCompleted` (shared mutex + counter, the realm
  value is copied out before the script hook); `Guild::UpdateMemberData` counts `LOCK_GUILD_MEMBER` but takes no
  lock: it writes one member's own row from that player's task while the member map changes only between beats
  (`AddMember`/`DeleteMember` assert that).
- `WorldSession::SendPacket` documented as the allowed foreign exception (enqueues on the socket's own MPSC
  `_bufferQueue`, reads no session field); its debug statistic made `thread_local`; `SendPacketFromAnyThread`
  added as the intent-naming alias for cross-task broadcast sites.

### Table

| # | File:line | Caller | Shape | Fix | Step |
| --- | --- | --- | --- | --- | --- |
| 1 | `Unit.cpp:17571` `BuildValuesUpdateFrom` | U2 view task for every viewer | `plr->IsInSameRaidWith`, empathy `HasAuraTypeWithCaster` on the foreign viewed unit | `UpdateValuesSource` gained `slot`/`notifyFlags`; empathy read via `SlotReader(*src.slot).HasAuraTypeWithCaster`; owner/charmer guids via new `GetCharmerOrOwnerPlayerOrPlayerItself(src)` (slot `charmerOrOwnerGuid`). `IsInSameRaidWith` is on the viewer's own handle. `static` | 10 |
| 2 | `Unit.cpp:17721` `PatchValuesUpdate` | same | `GetNpcFlags`, `GetAuraState`, `GetFaction`, `HasAuraTypeWithCaster`, `GetGoState`-style live reads of the viewed unit | npc flags, faction template, aura state (`SlotReader::AuraStateForTarget`) and aura-type checks read from `src.values` / slot; `CanSeeSpellClickOn/CanSeeVendor/CanSeeTrainer(Creature const*, NPCFlags)` overloads take the flags instead of re-reading them. `static` | 10 |
| 3 | `Unit.cpp:17743–17752` `PatchValuesUpdate` → `Player::CanSeeSpellClickOn/CanSeeVendor/CanSeeTrainer` | same | conditions evaluation (`sConditionMgr->IsObjectMeetToConditions`) walks the foreign creature | `open`: conditions read the creature's auras/quest state live; needs a `ConditionSource` over `UnitView` (step 10 follow-up). Expected first assert of procedure step 2 (talk to an NPC) | 10 |
| 4 | `Unit.cpp:17787`, `18157` `getTransForm()` | same | plain `m_transform` member of the foreign unit (torn read, no assert) | `open`: publish `transform` into `ObjectSlot` (step 05) | 05 |
| 5 | `Unit.cpp:17816–17824` `hasLootRecipient`, `isTappedBy`, `isAllowedToLoot` | same | loot recipient guid/group of the foreign creature | `isAllowedToLoot` gated behind `UNIT_DYNFLAG_LOOTABLE` from `src.values` so it runs only on corpses (dead units stop being tasks, step 11); `hasLootRecipient`/`isTappedBy` read plain guids (torn but not asserted). `open`: move recipient guid + group into the slot | 05/11 |
| 6 | `Unit.cpp:17839–17870` `IsControlledByPlayer()` | same | reads `m_ControlledByPlayer` (plain bool) of the foreign unit | tolerated torn read; `open` for slot publish | 05 |
| 7 | `Unit.cpp:17876` `sScriptMgr->OnPatchValuesUpdate(this, …)` | same | script hook receives the live foreign unit | `open`: hook signature should carry `UpdateValuesSource const&`; no in-tree script implements it today | 10 |
| 8 | `Object.cpp:644` `GetUpdateFieldData` | U2 view task | `GetOwnerGUID`/`GetCreatorGUID`/`GetCharmerOrOwnerPlayerOrPlayerItself` on the viewed object | guids read from `src.values` (`guidAt` lambda); empathy via slot. `static` | 10 |
| 9 | `GameObject.cpp:2854` `BuildValuesUpdateFrom` | U2 view task | `GetGoState()` on the viewed GO | `GOState(src.values[GAMEOBJECT_BYTES_1] & 0xFF)`; notify flags OR'd from `src`. `static` | 10 |
| 10 | `Corpse.cpp:230` `BuildValuesUpdateFrom` | U2 view task | `GetOwnerGUID()` on the viewed corpse | owner guid from `src.values[CORPSE_FIELD_OWNER]`. `static` | 10 |
| 11 | `PlayerUpdates.cpp:1928` `UpdateTriggerVisibility`, `:1975` `UpdateForQuestWorldObjects` | player task after quest/phase change | `SetFieldNotifyFlag` + live `BuildValuesUpdateBlockForPlayer` on every visible creature/GO | read `ReadSlot()` and build via `BuildValuesUpdateFromSlot(s, &udata, this, UF_FLAG_PUBLIC)`; the write to the foreign object's notify flags is gone. `static` | 10 |
| 12 | `ThreatManager.cpp:476` `AddThreat` | attacker's task, every hit | `target->GetThreatMgr()._redirectInfo` iteration on the foreign victim | `ObjectSlot::threatRedirects[]` published in `Unit::FillSlot`; `AddThreat` copies redirects from `target->ReadSlot()` when `target->IsForeignToTask()`. `static` | 06 |
| 13 | `Unit.cpp:8062` `EngageWithTarget` | attacker's task | `who->GetThreatMgr().AddThreat(this, 0)` on a foreign creature | appends a zero `ThreatRecord` (`ignoreModifiers`, `ignoreRedirects`) via `sRecordLogs->Append` when `IsForeignToTask()`. `open`: a foreign **player** target still has no record consumer, `SetInCombatWith` path (step 08) | 06/08 |
| 14 | `Map.cpp:2806` `InstanceMap::PermBindAllPlayers`, `:3326` `Map::UpdatePoolDeferred` | boss kill / pool despawn from a map seal task | gate was `InTask()`, so the seal task ran the singleton mutator directly | gate on `BeatContext::InBeat()` so S1b also defers. `static` | 09 |
| 15 | `GameEventMgr.cpp:142`, `:210` `StartEvent/StopEvent` | `.event start` or script in a task | `AppendDefer` only found the map through `BeatContext::root` | `BeatContext::DeferMap()` (root's map, else `sealMap`), then `ASSERT_NOT_IN_BEAT()`. `static` | 09 |
| 16 | `CreatureGroups.cpp:207`, `:247`, `:294` | member AI | `CreatureGroupMap[spawnId]` default-inserts into the singleton from a task | `FormationMgr::GetGroupAI(spawnId)` via `find`. `static` | 03 |
| 17 | `CreatureGroups.cpp:228`, `:273`, `:442` | member AI / leader task | `pMember->IsAlive()` on sibling members | `UnitView(pMember).IsAlive()`; `IsEngaged`/`IsInEvadeMode` are plain AI flags. `static` | 13 |
| 18 | `CreatureGroups.cpp:413` `LeaderStartedMoving`, `:402`, `:458` | leader task | `member->GetMotionMaster()->MoveFormation/MoveIdle/MovementExpiredOnSlot` on every member | `open`: asserts on `GetMotionMaster` from the leader's task; needs a `FormationMove` record consumed on the member's task (step 03 "follow" row) | 03 |
| 19 | `FormationMovementGenerator.cpp:141` `LaunchMovement` | member task | position math: `distance / travelDist` with `travelDist` from the foreign leader's `movespline->Velocity()` | `std::max(velocity * 1.65f, 0.01f)` | 13 |
| 20 | `Position.cpp:112` `GetSinCos`, `GameObject.cpp:2198/2224` `IsInRange/IsInRange3d`, `Unit.cpp:3180` `GetMeleeAttackPoint`, `Spell.cpp:1979` `SelectImplicitTrajTargets` | any | position math: divide by a distance built from two positions | already guarded (`fabs < 0.001` / `fuzzyEq(dist, 0)` / `meleeReach <= 0` / `!dist2d`) against the torn-read zero; no change | — |
| 21 | `Unit.cpp:924` `GetRandomContactPoint` | attacker task | position math: `/ combat_reach` | divisor is `this->GetCombatReach()` (own field) and clamped `< 0.1f → DEFAULT_COMBAT_REACH`; no change | — |
| 22 | `Vehicle.cpp` `GetPassenger` | vehicle update / passenger AI | `ASSERT_OWNED(_me)` | passengers are in the vehicle's task group (step 12) so `GetCharmer()->…` and `GetVehicleKit()->GetPassenger()` pass; no rewrite | 12 |
| 23 | `Unit::m_Controlled` iteration | owner task | listed in the check table | not assertable: the set is a plain member and iteration yields handles; writes through them assert on the controlled unit. Summons are in the owner's task group so pets pass | 12 |
| 24 | `WorldSession::SendPacket` | group/guild broadcasts from a foreign task | packet write to another player's session | allowed exception, documented; statistic made `thread_local`; `SendPacketFromAnyThread` added | 13 |
| 25 | `Guild::UpdateMemberData` | player task on level-up / zone change | *mutex* row | counter only (see above); membership mutators assert `NOT_IN_TASK` | 03 |
| 26 | `CharacterCache` getters/mutators, `AchievementGlobalMgr::SetRealmCompleted/IsRealmCompleted` | player task (level-up, rename, realm-first) | *mutex* row | `std::shared_mutex` + `CountSingletonLock`; read by `.debug singletons` | 03 |
| 27 | `ObjectMgr::*`, `GroupMgr`, `WorldSessionMgr`, `MapMgr`, `AuctionHouseMgr`, `TicketMgr`, `Guild::Add/DeleteMember`, `FormationMgr::Load*` | between beats (`UpdateSessions`, `World::Update`, reload commands) | *between beats* row | `ASSERT_NOT_IN_TASK()`; cannot be reached from a task today: every caller is a THREADUNSAFE opcode handler or `World::Update` | 03 |

### Deviations from the step text

- `ASSERT(!BeatContext::inBeat)` is spelled `ASSERT_NOT_IN_BEAT()` and prints the owner/seal map for the stack.
- The *defer* gates that previously tested `InTask()` now test `InBeat()`: a map seal task is inside a beat and
  must defer too, which the step's wording ("not even in a map seal task") requires.
- `ThreatManager` redirects are published into the slot rather than read live; this is the hottest foreign read
  (every `AddThreat`) and the slot was already the source of the other threat inputs.
- `EngageWithTarget` from a foreign task routes through a zero `ThreatRecord` instead of asserting; the player-
  target case stays open (row 13).
- The Guild member map got a counter but no mutex: membership changes are between-beats and assert so.
- `.debug singletons` resets the counters on read (same contract as `.debug foreignreads`).
