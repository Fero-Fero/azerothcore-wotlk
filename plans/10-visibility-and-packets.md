# 10 — Visibility and update packets from slots

Today a changed object pushes itself into `Map::_updateObjects` and `SendObjectUpdates` (`Map.cpp:1730`) builds one `UpdateData` per viewer from the object's live fields; visibility scans run partly inside `PlayerRelocation` and partly in `HandleDelayedVisibility` on the map. Under the beat loop a viewer cannot read a target's live fields and the map cannot run a serial pass over every object. This step inverts both: each player's own task scans from slots, builds its own packet from slots, and the only write into another object is the `VisibilityLink` record into the target's `_visiblePlayersMap`.

## Files

- `Object.h/.cpp` — `BuildValuesUpdateFromSlot`, `BuildCreateUpdateBlockFromSlot`, movement block pre-serialisation in `FillSlot`; `AddToObjectUpdate`/`RemoveFromObjectUpdate`/`m_objectUpdated` removed; `ClearUpdateMask` called by the owner after `PublishSlot`.
- `Unit.cpp`, `GameObject.cpp`, `DynamicObject.cpp`, `Corpse.cpp`, `Player.cpp` — their `BuildValuesUpdate` overrides get the slot variant.
- `Map.h/.cpp` — `_updateObjects`, `AddUpdateObject`, `RemoveUpdateObject`, `SendObjectUpdates`, `i_objectsForDelayedVisibility`, `HandleDelayedVisibility` deleted.
- `Unit.cpp:521–534` — `m_delayed_unit_relocation_timer` fires `ExecuteDelayedUnitRelocationEvent()` inline (the commented-out line at 530) instead of inserting into the map set.
- `PlayerUpdates.cpp:1629–1770` — `UpdateVisibilityForPlayer`, `UpdateObjectVisibility`, `UpdateVisibilityOf` read slots; back-link becomes a record.
- `GridNotifiers.h/.cpp` — `VisibleNotifier`, `PlayerRelocationNotifier`, `CreatureRelocationNotifier`, `DelayedUnitRelocation`, `AIRelocationNotifier` read `ReadSlot()` for the visited object.
- `ObjectVisibilityContainer.cpp` — `LinkWorldObjectVisibility` splits into the viewer's own insert and a `VisibilityLink` record.
- New record kinds in `UnitRecords.h`: `VisibilityLink { ObjectGuid viewer; }`, `VisibilityUnlink { ObjectGuid viewer; }`.

## 1. Who scans

Each `Unit::Update` runs its own relocation notifier when its delay timer fires (today's `ExecuteDelayedUnitRelocationEvent`, which already exists and already does the right scan per unit):

- A **player**: `Acore::PlayerRelocationNotifier` visits the grid cells around the player (live cell lists, read-only during a beat since relinks are defers), and for each visited object calls `UpdateVisibilityOf(target, data, visibleNow)` with the target's **slot** as the data source. The player writes its own `_visibleWorldObjectsMap` and `UpdateData`, and appends `VisibilityLink`/`VisibilityUnlink` to the target.
- A **creature**: `CreatureRelocationNotifier` for AI (`MoveInLineOfSight`, which reads the visited unit's slot and writes own AI state) and the player-side half: a creature that moved into a player's range must become visible to that player. Today the creature notifier also calls `player->UpdateVisibilityOf(creature)` on each player it finds — a write to the player. Under the record rule the creature instead appends `VisibilityPoke { creatureGuid }` to each player in range whose `_visiblePlayersMap` does not already contain it... simpler and cheaper: the player's own scan runs every beat at the recheck interval anyway, and a creature entering range is picked up by the player's next scan. Accept the one-interval delay (same as today's `m_delayed_unit_relocation_timer` granularity) and drop the creature-side player write. Creatures therefore do not write any player. `CreatureRelocationNotifier` keeps only the AI half.
- `MarkNearbyCellsOf` stays a `CellMark` defer (step 09).

`Map::PlayerRelocation` no longer calls `UpdateObjectVisibility(false)`; `Player::Update` reaches the scan through the existing timer, and `UpdateObjectVisibility(true)` (forced) sets `bRequestForcedVisibilityUpdate` as today and the next `Player::Update` runs the scan inline.

## 2. The back-link

`ObjectVisibilityContainer::LinkWorldObjectVisibility(target)` (viewer side) today inserts into the viewer's `_visibleWorldObjectsMap` **and** into `target->_visiblePlayersMap` (`DirectInsertVisiblePlayerReference`). Split:

```cpp
void ObjectVisibilityContainer::LinkWorldObjectVisibility(WorldObject* target)
{
    _visibleWorldObjectsMap->emplace(target->GetGUID(), target);            // own
    sRecordLogs->Current().Append(_selfObject->GetGUID(), target, RecordKind::VisibilityLink, VisibilityLink{ _selfObject->GetGUID() });
}
// target side, from ApplyRecords:
void ObjectVisibilityContainer::ApplyVisibilityLink(ObjectGuid viewer) { _visiblePlayersMap.emplace(viewer, ObjectAccessor::FindPlayer(viewer)); }
```

`_visiblePlayersMap` holds `Player*` handles keyed by guid; the handle is valid during a beat (players are removed from the map only at the seal / between beats).

`CleanVisibilityReferences` at object delete: an object whose removal was requested from its **own task** (death, despawn, `DespawnOrUnsummon`, player logout request) appends `VisibilityUnlink` to every guid in its `_visiblePlayersMap` and every object in its `_visibleWorldObjectsMap` *during that task*, as part of `PrepareRemoval()` (step 12); the seal-time cleanup then only clears its own containers and is grid-local. An object removed by the seal itself (map unload, S1b script despawn) runs today's two-sided body in S1b, where the whole map is live.

The creature's `IsUpdateNeeded` (`Creature.cpp:4010`) reads `_visiblePlayersMap.empty()` — its own field, one beat late. Fine.

## 3. Update packets

### Owner side

After `PublishSlot()` the owner calls `ClearUpdateMask(false)`. The slot holds the mask for the beat it was published in. `Object::SetUInt32Value` and friends no longer call `AddToObjectUpdateIfNeeded`; `m_objectUpdated`, `AddToObjectUpdate`, `RemoveFromObjectUpdate` are deleted along with `Map::_updateObjects`.

`WorldObject::FillSlot` pre-serialises the movement block once per publish: `BuildMovementUpdate(&buf, m_updateFlag & ~UPDATEFLAG_SELF)` into `slot.movementBlock`. It is per object, not per viewer (the only viewer-dependent flag is `UPDATEFLAG_SELF`, which the viewer adds for itself from live data).

### Viewer side

At the end of a player task, after its own `Update` and scan (on this step; from step 12 on this is `ViewBuild` + `ViewCommit` in phase U2, see below):

```cpp
void Player::BuildAndSendUpdatesFromSlots()      // = ViewBuild(vw, 0) + ViewCommit(vw) with one chunk
{
    UpdateData& data = _pendingUpdateData;          // resident
    uint32 prevBeat = BeatContext::beat.load(relaxed) - 1;   // slots readable this beat were published last beat
    for (auto const& [guid, obj] : *GetObjectVisibilityContainer().GetVisibleWorldObjectsMap())
    {
        ObjectSlot const& s = obj->ReadSlot();
        if (s.beat != prevBeat) continue;            // not due last beat → nothing changed
        if (!UpdateMask::AnyBit(s.changesMask, s.valuesCount)) continue;
        obj->BuildValuesUpdateFromSlot(s, &data, this);    // viewer-specific field visibility applied here, as in GetUpdateFieldData
    }
    // self: live, as today
    if (_changesMask.AnyBit()) BuildValuesUpdateBlockForPlayer(&data, this);
    if (data.HasData()) { data.BuildPacket(_packetScratch); SendDirectMessage(&_packetScratch); data.Clear(); }
}
```

`BuildValuesUpdateFromSlot(ObjectSlot const&, UpdateData*, Player* viewer)` is `Object::BuildValuesUpdate(UPDATETYPE_VALUES, ...)` with `m_uint32Values` → `slot.values` and `_changesMask` → `slot.changesMask`. The per-type overrides (`Unit::BuildValuesUpdate` for owner-only flags and `UNIT_DYNAMIC_FLAGS` tap/loot bits per viewer, `GameObject::BuildValuesUpdate` for `GAMEOBJECT_DYNAMIC` quest state, `Player::BuildValuesUpdate` for party fields) get the same substitution. Viewer-dependent decisions (`GetUpdateFieldData(target, flags)`, `IsInSameRaidWith`, loot recipient check `IsTappedBy(viewer)` → `SlotReader::Guid(UNIT_FIELD_TARGET)`-style fields, quest giver status → `sObjectMgr` static data plus the viewer's own quest log) read the viewer live (owner) and the target from the slot.

Create blocks on first sight: `BuildCreateUpdateBlockFromSlot(s, &data, viewer)` = values (full block from `s.values`) + `s.movementBlock` + the viewer's own `UPDATEFLAG_SELF` handling. Out-of-range blocks (`BuildOutOfRangeUpdateBlock`) only need the guid.

A viewer that sees an object published two beats ago (object not due last beat) gets no values update, which is correct: nothing changed.

### What this deletes

`Map::SendObjectUpdates` (1730–1756), `Object::BuildUpdate`, `Object::BuildFieldsUpdate`, `WorldObject::BuildUpdate` iterating `_visiblePlayersMap`, `sScriptMgr->CanSendObjectUpdatesToPlayer` (moved into the viewer-side function).

### Cost

Today's cost is O(changed objects × their viewers). The viewer-side pull is O(visible objects) per viewer with an early-out on `s.beat` and the mask. In a crowded cell both are O(N²) over the cell; the pull does the work on the viewer's core, which is what lets it spread. The `packet` field of the pool clock measures this function. Step 17 is the escape if it dominates.

### The view phase (step 12 moves it)

On this step the scan and the packet build run at the end of the player's own task and read slots published **last** beat. Step 12 moves both into a second unit phase, **U2**, that starts after every U1 task has published: the scan and the build then read **this** beat's slots (zero-lag visibility and values, one beat fresher than above), and a crowded player's U2 work becomes chunkable across cores (step 14) so one player in a 500-unit crowd is not the beat's straggler. To make the move a relocation and not a rewrite, structure the code now as:

```cpp
struct ViewWork                                   // resident per player
{
    uint32 slotBeat;                              // which slot stamp counts as "changed this beat"
    uint32 chunkCount; std::atomic<uint32> chunksDone;   // step 14: > 1 for a crowded viewer; 1 here
    std::vector<UpdateData> chunkData;            // one per chunk: values blocks for that chunk's bucket range of _visibleWorldObjectsMap
    VisibilityDelta delta;                        // objects entering / leaving view, from ViewScan (chunk 0 only)
};

void Player::ViewScan(ViewWork&);                 // today's relocation notifier body, reading slots; fills `delta`; writes nothing else
void Player::ViewBuild(ViewWork&, uint32 chunk);  // BuildValuesUpdateFromSlot for bucket range `chunk` of the *current* visible map into chunkData[chunk]
void Player::ViewCommit(ViewWork&);               // create / out-of-range blocks for `delta`, apply delta to _visibleWorldObjectsMap, append VisibilityLink/Unlink records, concatenate chunkData, send
```

`ViewScan` and `ViewBuild` write only `ViewWork`; the visible map is read-only until `ViewCommit`, which is the only function that writes the player's containers and appends records. Because chunks iterate bucket ranges of the existing visible map (not a snapshot), chunk 1..K−1 can run concurrently with chunk 0's scan, and the chunk count is fixed before the phase starts. That separation is what step 14 needs. `slotBeat` is `beat − 1` on this step and `beat` from step 12 on. Create blocks for newly visible objects are built in the commit from the same slots; they are a few per beat.

## 4. Slot additions

`ObjectSlot` gains: `movementBlock` (filled here), `uint32 m_updateFlag`, `SlotImmunity[]` (already added in 07), `uint16 cellId` (05). `Player` slots gain `groupGuid`, `raidGroupId` (for party-field visibility checks by other viewers), `teamId`, `isGameMaster`, `isGMVisible`, the stealth/invisibility detect values (`m_invisibility`, `m_invisibilityDetect`, `m_stealth`, `m_stealthDetect` flag-and-value arrays — `FlagArray32<TOTAL_*_TYPES>` copied by value), and `m_serverSideVisibility`/`Detect`. `CanSeeOrDetect(target, ...)` (`Object.cpp`) reads those from the target: slot overload `CanSeeOrDetect(ObjectSlot const&, ...)`.

## 5. Slices

1. Owner clears its mask after publish; `_updateObjects` and `SendObjectUpdates` removed; viewer-side build from slots. Check: two characters see each other's health, buffs, equipment changes, movement; a third sees both; `.server info` in band.
2. Scan on the unit's own task; `HandleDelayedVisibility` and `i_objectsForDelayedVisibility` removed; `PlayerRelocation` without the inline scan; `VisibilityLink`/`Unlink` records; creature notifier keeps only the AI half. Check: walk toward a creature, it appears within one relocation interval; walk away, it disappears; a creature walking toward a standing player appears within one interval; `_visiblePlayersMap` edited only in `ApplyRecords` and at the seal (debug counter).
3. `CanSeeOrDetect` from slot: stealth detection, invisibility, GM visibility, phase mask. Check: a stealthed rogue at the detection boundary; `.gm visible off`.

## Must not

- No push from a changed object to its viewers.
- No read of a visited object's live fields in a notifier.
- No creature writing a player's visible set.
- No per-beat allocation of `UpdateData` or the packet (resident per player).

## Checklist

- [x] `ClearUpdateMask` after `PublishSlot`; `AddToObjectUpdate*`, `_updateObjects`, `SendObjectUpdates`, `BuildUpdate`, `BuildFieldsUpdate` deleted.
- [x] `BuildValuesUpdateFromSlot` / `BuildCreateUpdateBlockFromSlot` for every object type; movement block pre-serialised at publish.
- [x] Player task builds and sends its own packet; `packet` field measured. (resident `ViewWork::chunkData`; no measurement yet)
- [x] Scan on the unit's task; map-side visibility pass deleted.
- [ ] `VisibilityLink`/`Unlink` are the only writes to `_visiblePlayersMap` during a tick. (records wired, direct write kept — see Status)
- [x] `CanSeeOrDetect` slot overload covers stealth, invisibility, GM, phase.

## Status

Not built since these edits (the user builds). `codestyle-cpp.py` passes. Not played.

### Done

- `ObjectSlot.h`: `SlotFlaggedValues<N>`, `SlotVisibilityFlags`, `SlotVisibility vis` (stealth / invisibility /
  server-side arrays and their detect sides, team, group / summoner / vehicle-base / owner / farsight guids, combat
  reach, level), `updateFlag`, `movementBlock` (+ `EnsureMovementCapacity`, doubling from 64 like auras),
  `AnyChange()` / `Changed(index)`. `PublishSlot` fills `vis` (per-type `FillSlot` adds the unit / player / GO
  parts), serialises the movement block with `m_updateFlag` (+ `HAS_TARGET`, never `SELF`), commits, then
  `ClearUpdateMask(false)`: the mask lives in the slot from here on.
- `UpdateValuesSource { values, changesMask }` replaces `UpdateDataMapType`. `BuildValuesUpdateFrom(type, buf,
  viewer, src)` is the one virtual values builder (Object / Unit / GameObject / Corpse); the live
  `BuildValuesUpdate` packs the owner's mask and calls it. `Unit::_valuesUpdateCache` is owner-only (live path).
  `BuildValuesUpdateFromSlot`, `BuildCreateUpdateBlockFromSlot` (self → live create block; others from
  `slot.updateFlag`, viewer pet guid, GO type / `vis.ownerGuid`, appended `movementBlock`).
  `BuildUpdate` / `BuildFieldsUpdate` on Object, WorldObject, Item, Motion/StaticTransport deleted.
- `Map::_updateObjects`, `AddUpdateObject`, `RemoveUpdateObject`, `SendObjectUpdates`, `HandleDelayedVisibility`,
  `i_objectsForDelayedVisibility` deleted. `WorldObject::AddToObjectUpdate` only nudges a non-player into the
  updatable list so it publishes; `WorldObject::AddToWorld` publishes a first slot. `Item::AddToObjectUpdate`
  → owner's `_changedItems`.
- `Player::ViewWork` (resident: `slotBeat`, `chunkCount` / `chunksDone` / `chunkData` for step 14, `delta`,
  `seen`, `pokes`, `scanDue` / `scanForced` / `gobjOnly`), `ViewCollect` (slot `CanSeeOrDetect`, decision into
  `delta`, nudges the object into the update list), `ViewScan` (viewpoint + minimum-move check, grid visit via
  `VisibleNotifier` / `PlayerRelocationNotifier`, `CollectRest` for zone-wide objects and the out-of-range sweep),
  `ViewBuild(chunk)` (chunk 0: own slot, changed items, transports; every chunk: a bucket range of
  `_visibleWorldObjectsMap`, values from any slot stamped `slotBeat` with a non-empty mask), `ViewCommit`
  (pet removal on leave, out-of-range / create blocks, unlink / link, chunk merge, `CanSendObjectUpdatesToPlayer`,
  one packet, `GetInitialVisiblePackets` for entered units), `RunViewPhase` (scan if due, pokes, build, commit).
- `Map::RunViewPhase()` after `ApplyDefers` in both the `t_diff == 0` and the normal path (`Visibility` phase
  label): every object published, so each slot's mask is sent exactly once; `LoadGridsInRange` for a scanning
  player runs there, outside the player's task, so grid loads stay off the defer path.
- Scan on the unit's task: `Unit::Update` calls `ExecuteDelayedUnitRelocationEvent` inline; a player
  `RequestViewScan(false)`, a shared-vision unit `PokeViewScan`s its viewers, a creature runs
  `CreatureRelocationNotifier` (AI half only; the player half is the player's own scan). `Map::PlayerRelocation`
  arms `NOTIFY_VISIBILITY_CHANGED` in and out of a task.
- Pokes replace every push to another player's visible set: `VisibleChangesNotifier`, the player half of
  `PlayerRelocationNotifier`, the out-of-range sweep, `Player::UpdateVisibilityOf(WorldObject*)` in a task.
  `VisibilityPokeRecord { object }` (empty guid = scan request) to a foreign player, inline otherwise; applied
  in `ApplyVisibilityPoke`, consumed by the next `RunViewPhase` (`ViewCollect` dedups against the scan).
- `UpdateVisibilityForPlayer(mapChange)`: in a task a forced scan request (`gobjOnly` never narrows a pending
  full scan); outside one (login, teleport) `ViewScan` + `ViewCommit` now, no `ViewBuild`, so the beat's masks are
  still sent once by the view phase.
- Records: `VisibilityLinkRecord` / `VisibilityUnlinkRecord { viewer }`, `VisibilityPoke` kind + name,
  `WorldObject::ApplyCommonRecord` (link / unlink / poke / wake) used by the base `ApplyRecords` and by the
  `Unit::ApplyRecord` default. `ObjectVisibilityContainer::LinkTargetSide` / `UnlinkTargetSide` hold the routing;
  `ApplyLinkFromViewer` / `ApplyUnlinkFromViewer` are the appliers.
- `CanSeeOrDetect(ObjectSlot const&, obj, …)`, `CanDetectInvisibilityOf(slot, …)`, `CanDetectStealthOf(slot, …)`:
  never-visible, phase (vehicle exception), spectator, summoner-only, ghost / corpse, vehicle base, arena pet,
  far-sight arc, distance (slot position, combat reach as size), GM, ghost team / group, despawn, invisibility and
  stealth arrays all from the slot; AI `CanBeSeen`, visibility conditions, `IsAlwaysVisibleFor` /
  `IsAlwaysDetectableFor` / `CanAlwaysSee`, group membership, detect-stealth auras through the handle.

### Open

- `VisibilityLink` / `Unlink` records are not used (`USE_VISIBILITY_LINK_RECORDS = false` in
  `ObjectVisibilityContainer.cpp`): an object removed at this tick's end would be freed before the record lands
  next beat and the viewer's map would keep the dead pointer. The target side is still written directly; safe
  while the view phase has no concurrent task (until step 14). Step 12 (seal after inbox) can flip the constant.
- `Pet::Remove` on leaving visibility (`ViewCommit`) writes the pet from the owner's task, as before.
- `CanSeeOrDetect` from slot still reads a few live fields through the handle (`IsPet`, `GetObjectSize` of
  non-units, GO owner level for stealth, `CanBeSeen`, conditions); the slot `beat` cross-check is not asserted.
- `ViewBuild` chunk > 0 and `chunksDone` exist but only chunk 0 runs (step 14).
- No measurement of packet size / build time; `Must not`'s "no per-beat allocation" holds for `UpdateData`
  (resident) but `WorldPacket` is still built per send.
- Transports: a static transport's values go to players in range that can see it; a motion transport's to every
  player of the map (as `HaveAtClient` always reports it), both only when its slot changed this beat.

### Behaviour

- Values changed after an object's publish in the same tick (scripts, movement phase) ship next beat.
- An object entering view gets its create block from the slot (position / movement as of its last publish).
- Visibility changes requested by another object (pokes) land at that viewer's view phase next beat; its own
  timer-driven scans unchanged (`DynamicVisibilityMgr` delays and move distance).
- `UpdateVisibilityOf(target)` off-task sends immediately; in a task it is a poke (one beat later).
