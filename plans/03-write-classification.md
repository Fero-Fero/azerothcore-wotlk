# 03 — Write classification (reference)

Every write inside one `Map::Update` and its fan-out, marked by where it must execute under the beat loop. This file adds no code. Steps 05–12 implement the marks; step 13 audits what they missed.

## The three marks

| Mark | Executes | Mechanism | Step |
| --- | --- | --- | --- |
| **Own** | The task that claimed the object | Direct write to live fields, then `PublishSlot` | 05 |
| **Record** | The target's next task | Append to the per-core record log, applied at task start in source-guid order | 06, 07, 08, 10 |
| **Defer** | The coordinator at the seal | Append to the per-core defer buffer | 09, 12 |

A call that writes in two marks is listed under the wider one (Record wider than Own, Defer wider than Record). A read of another object's live field is not a write but is still foreign access and goes through the slot (step 05, audited in step 13).

## `Map::Update` body (`Map.cpp:437–558`) and where each line goes under the beat loop

Lines are the tree after step 01. The phase clock is measurement, not a gameplay write.

| Line | Call | Today | Under the beat loop |
| --- | --- | --- | --- |
| 439–442 | `MapPhaseClock` around the body | per map | Measurement on the coordinator. No gameplay write |
| 447 | `GetDynamicTree().update(t_diff)` | per map | Map epilogue at the seal (step 12) |
| 452–466 | session loop, `session->Update(s_diff, MapSessionFilter)`, zero-diff `player->Update` | per world tick | Inside the player's task, once per beat |
| 469 | `Events.Update` | per map | Map epilogue |
| 471–477 | zero-diff early return with `HandleDelayedVisibility` | per world tick | Gone. There is no zero-diff tick |
| 480–489 | `ProcessRespawns` on `_respawnCheckTimer` | per map | Map epilogue (S1b) pops due entries into `Spawn` defers; construction happens in the next beat's grid items (S1a) |
| 491 | `_updatableObjectListRecheckTimer.Update` | per map | Map epilogue |
| 492 | `resetMarkedCells` | per map | Seal, before cell-mark defers apply (step 09) |
| 495–518 | `player->Update`, `MarkNearbyCellsOf` on the player and on a far-sight viewpoint | per map | Player task; `MarkNearbyCellsOf` appends `CellMark` defers |
| 521 | `UpdateNonPlayerObjects` | per map | Each object is its own task; list edits are defers |
| 524 | `SendObjectUpdates` | per map | Gone. Each player task builds its own packet from slots (step 10) |
| 527–533 | `ScriptsProcess` under `i_scriptLock` | per map | Map epilogue |
| 536–538 | `MoveAll*InMoveList` | per map | Seal: relink defers (step 09) |
| 541 | `HandleDelayedVisibility` | per map | Gone. `Unit::Update` runs `ExecuteDelayedUnitRelocationEvent` on the unit's own task (step 10) |
| 544–549 | redirect kick, weather, corpses, `OnMapUpdate` | per map | Map epilogue |
| 551–557 | `METRIC_VALUE` | per map | Map epilogue |
| `DelayedUpdate` (1827) | transports, `RemoveAllObjectsInRemoveList` | world thread after join | Seal |

## Map containers (`Map.h`) — all Defer or seal-only

| Field | Line | Writer today | Under the beat loop |
| --- | --- | --- | --- |
| `m_mapRefMgr`, `m_mapRefIter` | 619–620 | both player loops; `Player` unlink advances the iterator | Read-only during a beat; add/remove player at the seal |
| `_transports`, `_transportsUpdateIter` | 622–623 | `DelayedUpdate`, transport `RemoveFromMap` | Seal |
| `marked_cells` | 639 | `resetMarkedCells`, `MarkNearbyCellsOf` | Seal applies `CellMark` defers |
| `i_scriptLock`, `m_scriptSchedule` | 641, 645 | script start inserts; `ScriptsProcess` erases | Insert is a defer; process in the epilogue |
| `i_objectsToRemove` | 642 | `AddObjectToRemoveList` (calls `CleanupsBeforeDelete` inline!) | `PrepareRemoval` on the unit's task does the own half inline and the foreign half (other units' threat, combat, owner lists, visibility) as records; the `RemoveFromMap` defer leaves only the grid unlink (S1a) and the store erase (S1b) |
| `Events` | 485 | `Events.Update`; `ScheduleCreatureRespawn` from `Creature` death (`Creature.cpp` despawn) | Epilogue drains the queue. The insert is the respawn-schedule defer |
| `CreatureGroupHolder` | 343 | `FormationMgr::AddCreatureToGroup` / `RemoveCreatureFromGroup` on spawn and despawn | Seal, in the same `AddToMap` / `RemoveFromMap` as the creature |
| `_respawnCheckTimer`, respawn queue and maps (`_creatureRespawnTimes`, `_goRespawnTimes`, `_forcedCreatureRespawns`, `_respawnQueue`) | 655–680 | `ProcessRespawns`, `SaveCreatureRespawnTime`, `ScheduleCreatureRespawn` | Epilogue; schedule is a defer |
| `_poolData` | 677 | `PoolMgr::UpdatePool` / `SpawnPool` / `DespawnPool` from despawn and `ProcessRespawns` | S2, inside `PoolEvent`. The map's `SpawnedPoolData` changes only there |
| `_toggledSpawnGroupIds` | 679 | `SpawnGroupSpawn` / `SpawnGroupDespawn` from SmartAI (`SmartScript.cpp`) and GM commands | Defer. S1b flips the set and emits `Spawn` / `RemoveFromMap` for the members. GM commands stay between beats |
| `_zonePlayerCountMap` | 682 | `UpdatePlayerZoneStats` | Defer |
| `_zoneDynamicInfo`, `_weatherUpdateTimer` | 684–685 | `SetZoneMusic` / `SetZoneWeather` / `SetZoneOverrideLight` from boss AI (Icecrown and others); `UpdateWeather` | `ZoneDynamic` defer, applied in S1b. `UpdateWeather` stays in the epilogue |
| `_corpseUpdateTimer` | 688 | `UpdateExpiredCorpses` | Epilogue only |
| `_objectsStore` | 701 | summon, respawn, `DeleteFromWorld` | Insert and erase at the seal |
| `_creatureBySpawnIdStore`, `_gameobjectBySpawnIdStore` | 702–703 | same add/remove path as `_objectsStore` | Seal, with the store |
| `_corpsesByGrid`, `_corpsesByPlayer`, `_corpseBones` | 704–706 | corpse create/remove, `ConvertCorpseToBones` | Seal |
| `_updateObjects` | 708 | `AddUpdateObject` from every setter | Gone (step 10) |
| `_updatableObjectList`, `_pendingAddUpdatableObjectList`, recheck timer | 710–712 | `UpdateNonPlayerObjects`, `AddObjectToPendingUpdateList` | Replaced by deque republish (step 12); list edits at the seal |
| `_zoneWideVisibleWorldObjectsMap` | 713 | `Add`/`RemoveWorldObjectToZoneWideVisibleMap` from `WorldObject` zone and visibility updates | Defer, applied with visibility at the seal (step 10). Far-visible entries live on the grid cell and move with the grid relink |
| `_redirectKickTimer`, `_lastAnnounceRedirectKickTimer` | 715–716 | `UpdatePlayersRedirectKickEvent` | Epilogue only |
| `_phaseTiming` | 718 | phase clock | Measurement. Not a gameplay container |
| `m_unloadTimer` | 615 | `CanUnload` on the map-mgr scan; player-leave paths | `CanUnload` is between beats. The leave-path write rides the player-remove defer |
| `Map::Lock` | 608 | `MapInstanced` instance lookup | Create/destroy only (S2 or between beats). Not taken on the per-beat path |
| `_creaturesToMove` and the GO/dynobj lists | 590–592 | `*Relocation` | Replaced by `Relink` defers |
| `i_objectsForDelayedVisibility` | 240 | `Unit::Update` inserts | Gone (step 10) |
| `_guidGenerators` (`Map::GenerateLowGuid`) | 700 | `_nextGuid++` | Atomic (step 04) |
| `_mapGridManager` grids | 610 | `EnsureGridLoaded` from relocation and visitors | Grid load is a defer; applied at the seal |
| `_mapCollisionData` dynamic tree | 612 | `update` at tick start, `insert`/`remove` from GO and spell code | Epilogue `update`; `insert`/`remove` are defers |
| Per-map `dtNavMeshQuery` | `MapCollisionData.h:73` (`_navMeshQuery`) | `PathGenerator` | Per-worker query (step 14) |

## Other-unit mechanisms — all Record

| Mechanism | Entry | What it writes on the other unit | Record kind |
| --- | --- | --- | --- |
| Spell hit | `Spell::DoAllEffectOnTarget` (2623) | everything in `DoSpellHitOnUnit` plus damage, heal, procs, combat | `SpellHit` → target, `SpellLanded` → caster (07) |
| Melee | `Unit::AttackerStateUpdate` (2760) → `DealMeleeDamage` (2020) | victim health, procs, threat | `MeleeHit` → victim, `MeleeLanded` → attacker (08) |
| Periodic tick | `AuraEffect::PeriodicTick` | target damage/heal already on target's task; caster-side procs and rage | `SpellLanded` with periodic flag → caster (08) |
| Threat | `ThreatMgr::AddThreat` → `PutThreatenedByMeRef` on the attacker | attacker's `m_threatenedByMe` | `ThreatBackRef` → attacker (06) |
| Combat refs | `CombatManager::SetInCombatWith` | both combat managers | `CombatRef` → other side (06) |
| Aura remove on another unit | `RemoveAurasDueToSpell(id, casterGuid)` etc. from a script or dispel | target aura map | `AuraRemove` (06); dispel inside a hit is part of `SpellHit` |
| Knockback | `Unit::KnockbackFrom` (15011) on a target | target motion | `Knockback` (06) |
| Charm | `SetCharmedBy` / `RemoveCharmedBy` | both units | **Task group** (12): join/leave is a `TaskGroupJoin`/`TaskGroupLeave` defer (09) applied at the seal; from then on charmer and charmed run in one task and write each other as Own |
| Vehicle passenger | `Vehicle::AddPassenger` / `RemovePassenger`, `RelocatePassengers` | passenger position, seat map | **Task group** (12): same mechanism, root = vehicle base; `RelocatePassengers` stays as today inside the group's task |
| Transport passenger | `Transport::AddPassenger` / `RemovePassenger`, `UpdatePassengerPositions` | passenger position, passenger set | Seal: transports update in `DelayedUpdate` inside the map seal task, where no unit task runs |
| Assistance | `CallAssistance`, `DoZoneInCombat` | nearby units' combat | `CombatRef` per unit |
| Visibility back-link | `LinkWorldObjectVisibility` into target `_visiblePlayersMap` | target container | `VisibilityLink` (10) |
| Kill credit | `RewardPlayerAndGroupAtKill` | killer and group | inside `SpellLanded` / `MeleeLanded`; group walk is a defer |
| Extra attacks | `Unit::Update` 569–587 via `ObjectAccessor::GetUnit` | victim | the swing is a `MeleeHit` record |
| Owned auras | `Aura::UpdateTargetMap` → `FillTargetMap` | every unit in range (area auras) | `AreaAuraApply` / `AreaAuraRemove` records; the aura owner's task decides membership from slots (08) |
| GO ritual, dynobj view | `GetPlayer` + `CastSpell`, `UpdateVisibilityForPlayer` | another player | follow-up cast record / visibility is the viewer's own task |
| Formation | `CreatureGroup::MemberEngagingTarget`, `LeaderStartedMoving` | member threat and combat; member motion | Assist appends `ThreatBackRef` / `CombatRef` per idle member (06). Follow is the member's own task: it reads the leader slot and writes its own motion. Group membership changes at the seal with the creature |
| Foreign event queue | `Unit::m_Events.AddEvent` on a unit that is not the owner: spell magnet (`GetMagicHitRedirectTarget`, 7927), reflect delay, aura munch, `VehicleDespawnEvent` | that unit's `EventProcessor` | Record. The apply on that unit queues the event. A hit target uses its `SpellHit` / `SpellLanded`; a third unit (grounding totem) gets its own record. Payload is guids and ids. `m_Events` on the owner stays Own |

## Globals

| Site | Today | Under the beat loop |
| --- | --- | --- |
| `ObjectAccessor::FindPlayer`, `GetUnit`, `GetPlayer`, `HashMapHolder<Player>::Find` | returns live `Player*`, lock covers the container only | Still returns the live pointer as a **handle**. The container changes only between beats (login/logout on the world thread). Reading a field on the result is foreign access → `ReadSlot()`; writing is a record (step 13) |
| Item GUIDs (`ObjectGuidGenerator::Generate`, `ObjectGuid.h:315`) | `_nextGuid++` | Atomic (step 04) |
| `Group` member edits, `EndRoll`, `EndBattleground` | map thread, racy today | Defer |
| `LFGMgr` | world thread / `MapUpdater` | world thread between beats |
| `WorldState`, outdoor PvP, battleground scores | map thread | Defer |
| Database | `SaveToDB` enqueues; `recoveryItem` blocks | step 11 |

## Global singletons

`ASSERT_OWNED` (step 13) only covers `Object` fields. A singleton's mutable state is reached from unit updates through a different door, and most of them are racy today across `MapUpdater` threads. The rule:

- **Read-only after load** → no change; listed so the audit can skip them.
- **Mutated rarely from a task** (a few times a minute server-wide) → one `std::mutex` inside the singleton, taken by its mutators and by the readers that iterate; listed as *mutex*. The cost is an uncontended lock on a rare path, never on the per-beat path.
- **Mutated per beat or per kill from a task**, or mutated in a way readers cannot tolerate mid-iteration → a global defer kind (step 09) applied in seal phase S2; listed as *defer*.
- **Mutated only between beats** (world thread, `PROCESS_THREADUNSAFE` handlers, GM commands outside a task) → `ASSERT_NOT_IN_TASK()` on the mutator (step 04 macro); listed as *between beats*.

A map seal task (S1, step 12) counts as "in task" for singletons: two maps' seal tasks run concurrently. Singleton mutations reached from `ProcessRespawns`, `ScriptsProcess`, or `OnMapUpdate` are therefore *defer* or *mutex*, never direct.

| Singleton | Mutators reachable from a unit update or a map seal task | Mark |
| --- | --- | --- |
| `sObjectMgr` templates, quest, gossip, `PlayerInfo`, `CreatureData`/`GameObjectData` stores | none from a task (`SaveToDB`/`DeleteFromDB` on spawns are GM commands) | read-only; `ASSERT_NOT_IN_TASK` on `AddCreatureToGrid`, `RemoveCreatureFromGrid`, `AddGameobjectToGrid`, `DeleteCreatureData`, `DeleteGameObjectData`, `NewOrExistCreatureData`, `SetCreatureLinkedRespawn` |
| `sObjectMgr` GUID generators (`GetGenerator<HighGuid::Item>` etc.) | every item create, every summon | atomic (step 04) |
| `sPoolMgr` | `UpdatePool` on despawn (`Creature::Update` corpse branch, `GameObject::Update`), `SpawnPool` from `ProcessRespawns` | **defer** `PoolEvent{poolId, guid, type, action}` → S2 applies `UpdatePool`/`SpawnPool`; spawns it creates become newcomers of the next beat |
| `sGameEventMgr` | `StartEvent`/`StopEvent` from scripts (`GameEventMgr::StartEvent` from a creature AI, e.g. Scourge Invasion), `HandleQuestComplete` counters, `HandleWorldEventGossip` | **defer** `GameEventAction`; counters that are per-player are own |
| `sInstanceSaveMgr` | `PlayerBindToInstance` via `InstanceMap::PermBindAllPlayers` from `InstanceScript::SetBossState`, `AddInstanceSave`, `DeleteInstanceSavedData`, `UnbindAllFor` | **defer** `InstanceBind{mapId, instanceId, playerGuids…, permanent}` → S2 |
| `sCharacterCache` | `UpdateCharacterLevel`/`Gender`/`Name`/`ArenaTeamId`/`GuildId`/`AccountId` from `Player::GiveLevel`, `SetGuild`, arena join | **mutex** inside `CharacterCache` (the store is a hash map; today unprotected) |
| `sAchievementMgr` | `SetRealmCompleted` from `Player::CompletedAchievement` | **mutex** on `m_allCompletedAchievements`; criteria tables read-only |
| `sGuildMgr` / `Guild` | `Guild::UpdateMemberData` (level, zone) from `Player::GiveLevel` / zone change, `Guild::SendUpdate*` packets, `HandleLeaveMember` (THREADUNSAFE) | **mutex** per `Guild` on the member map; packets are fine (see Packets) |
| `sGroupMgr` / `Group` | create/disband/`AddMember`/`RemoveMember` (THREADUNSAFE handlers, logout), `EndRoll`, `UpdatePlayerOutOfRange`, `SendUpdate` | between beats for membership; `GroupEndRoll`/`GroupBroadcast` defer for the rest |
| `sArenaTeamMgr` / `ArenaTeam` | rating changes in `Battleground::EndBattleground` | between beats (BG end is a `BattlegroundEvent` defer → world thread) |
| `sBattlegroundMgr` / `Battleground` | `UpdatePlayerScore`, `HandleKillPlayer/Unit`, `EventPlayerClickedOnFlag`, capture points from a GO task | **defer** `BattlegroundEvent` (09) |
| `sOutdoorPvPMgr` / zone scripts | `HandleKill`, capture point `Update` | **defer** `OutdoorPvPEvent` (09) |
| `sBattlefieldMgr` / `Battlefield` | `HandleKill` from `Unit::Kill` (13882), `HandlePlayerEnterZone` / `LeaveZone` from `Player` zone update, Wintergrasp script mutators in `zone_wintergrasp.cpp` reached from a creature or GO task | **defer** `BattlefieldEvent` (09) → S2. `Battlefield::Update` (capture-point tick, invites) stays on the world thread between beats, same as `OutdoorPvPMgr::Update`. `CanFlyIn` and `GetBattlefieldToZoneId` are reads |
| `sFormationMgr` | `LoadCreatureFormations` only. `CreatureGroupMap` and `CreatureGroupMembers` are not written after load. Assist indexes them with `operator[]`, which inserts on a miss | read-only; `ASSERT_NOT_IN_TASK` on `LoadCreatureFormations`. Those `operator[]` reads become `find` so a task cannot insert (step 13) |
| `sWorldState` / `sWorldStateMgr` | set/update from zone scripts | **defer** `WorldStateWrite` (09) |
| `sWorld` | `setWorldState`, `IncreasePlayerCount`/`DecreasePlayerCount`, `SendGlobalMessage`, config reload | `setWorldState` → `WorldStateWrite`; counts between beats (login/logout); `SendGlobalMessage` iterates sessions read-only and calls `SendPacket`: allowed |
| `sWorldSessionMgr` | add/remove session, kick | between beats |
| `ObjectAccessor` `HashMapHolder<T>` | `AddObject`/`RemoveObject` from `AddToWorld`/`RemoveFromWorld` | seal (`AddToMap`/`RemoveFromMap` defers apply there; the holder's own mutex stays for between-beat callers) |
| `sMapMgr` | `CreateMap`, `DestroyInstance`, `UnloadAll` | S2 or between beats; `MapChange` defer creates the destination in S2 |
| `sLFGMgr` | queue/proposal/kick from THREADUNSAFE handlers; `LFGMgr::Update` | between beats (step 12 moves it onto the world thread) |
| `sAuctionMgr`, `sMailMgr` (mail delivery), `sTicketMgr`, `sPetitionMgr`, `sCalendarMgr`, `sChannelMgr` | THREADUNSAFE handlers; world `Update` | between beats; `ASSERT_NOT_IN_TASK` on mutators. `Player::SendMailResult`-style packets are fine |
| `sScriptMgr`, `sSmartScriptMgr`, `sSpellMgr`, `sConditionMgr`, `sCreatureTextMgr`, `sTransportMgr`, `sGameGraveyard`, `sWaypointMgr`, `sAreaTriggerStore`, DBC/DB2 stores | reload commands only (GM, between beats) | read-only; `ASSERT_NOT_IN_TASK` on `Reload*`/`Load*` |
| `sWeatherMgr` | `Weather::Update` | map epilogue (S1); weather is per zone of one map |
| `sMetric`, `sLog`, `sConfigMgr` | — | already thread-safe |
| RNG (`urand`, `rand_norm`, `roll_chance_*`) | — | `thread_local` engine in `Random.cpp`; verify, and `ASSERT` one engine per thread in debug |
| `Spell`/`Aura` static caches (`SpellInfo::_GetExplicitTargetMask` etc.) | computed at load | read-only |

Anything not in this table that a task mutates is found by step 13 (`ASSERT_NOT_IN_TASK` placed on every `*Mgr` mutator the audit meets) and added here.

Step 09's `DeferKind` gains `BattlefieldEvent` (global, S2) and `ZoneDynamic` (per map, S1b). Spawn-group work reuses `Spawn` and `RemoveFromMap`; it does not add a kind.

## Packets

`MapSessionFilter` accepts `PROCESS_INPLACE` always and `PROCESS_THREADSAFE` when in world. `WorldSessionFilter` accepts `PROCESS_INPLACE` and `PROCESS_THREADUNSAFE`. `World::Update` runs `UpdateSessions` (line 1227) before `MapMgr::Update` (1254), so a queued `PROCESS_INPLACE` packet is usually taken on the world thread and a late one on the map side.

Under the beat loop the same filters hold. `PROCESS_THREADSAFE` and late `PROCESS_INPLACE` run inside the player's task. `PROCESS_THREADUNSAFE` runs in `UpdateSessions` between beats on the world thread. Handlers that run in a task and write another unit follow the record rule (cast → `SpellHit`, swing → `MeleeHit`, item use → spell rule). `CMSG_SPLIT_ITEM`, buy, and autostore loot take an item GUID: atomic.

## Spell and script rule

`Spell::cast`, `Spell::update`, aura effect handlers, `UnitAI::UpdateAI`, SmartAI, `ScriptsProcess` command bodies, and `sScriptMgr` hooks are not walked per spell. The rule: if the path can select a second unit, it is Record; if it can reach a map or global container, it is Defer. Step 07 moves the spell effect handlers onto the target with a compiler-enumerated audit; SmartAI and script bodies that write another unit directly are found by the step 13 `ASSERT`.

## Checklist

- [x] Every line of `Map::Update` and `DelayedUpdate` has a row with its destination.
- [x] Every `Map.h` container mutated in a tick has a seal-only destination.
- [x] Every other-unit mechanism has a record kind and a step.
- [x] Globals: handles allowed, field access through slots, container edits between beats.
- [x] Every singleton with a mutator reachable from a task or a map seal task has a row: read-only, mutex, defer, or between beats.
