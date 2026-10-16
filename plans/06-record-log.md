# 06 — Record log

A write to another unit becomes a plain-value record appended to the writer's log and applied by the target at the start of its next task. This step builds the log, the per-target chains, the `writtenBy` mask, the inbox apply, the wake rule, and the non-spell record kinds. The spell kinds are step 07, melee and periodic step 08, visibility step 10.

On this step there is one thread and one log. The structure is already the per-core one so step 14 only adds instances.

## Files

- New `src/server/game/Maps/UnitRecordLog.h`, `UnitRecordLog.cpp`.
- New `src/server/game/Maps/UnitRecords.h` — the record structs, one per kind.
- `src/server/game/Entities/Object/Object.h` — `WorldObject` gains `std::atomic<uint64> _writtenBy{0};` and `void ApplyInbox();`.
- `src/server/game/Entities/Unit/Unit.cpp`, `Unit.h` — appliers `Unit::ApplyRecord(RecordHeader const&, uint8 const* payload)` and the writer-side changes listed under "Kinds".
- `src/server/game/Combat/ThreatMgr.cpp`, `CombatManager.cpp` — back-reference halves become records.
- `src/server/game/Maps/Map.cpp` — `Map::Update`: at the very top, before `GetDynamicTree().update`, run `ApplyInboxes()` for every object with a non-zero `_writtenBy`. At the very end, `sRecordLogs->ResetAll()`.
- `src/server/game/Maps/MapMgr.h` — owns `RecordLogSet` (one log on this step; `cores` logs in step 14).

## Layout

```cpp
enum class RecordKind : uint8
{
    SpellHit, SpellLanded, DeferredSpellHit,          // 07
    MeleeHit, MeleeLanded,                            // 08
    PeriodicLanded, AreaAuraApply, AreaAuraRemove,    // 08
    ThreatBackRef, CombatRef, CombatEnd,              // 06
    AuraRemove, Knockback,                            // 06 (charm and vehicle seating are task groups, step 12, not records)
    ThreatRemove, MinionUnlink, GameObjectUnlink,     // 06: removal prepared on the dying unit's own task (see §Removal)
    VisibilityLink, VisibilityUnlink,                 // 10
    Wake                                              // 06: empty record, only forces a task
};

struct RecordHeader
{
    ObjectGuid target;
    ObjectGuid source;
    uint32 nextInTarget;   // byte offset of the previous record for the same target in this log, or ~0u
    uint32 sequence;       // per-log monotonic, for stable sort after source guid
    uint16 size;           // payload bytes, 8-aligned
    RecordKind kind;
    uint8 pad;
};
// payload follows immediately, 8-byte aligned

struct TargetHead { uint32 beat; uint32 offset; };   // offset valid only if beat == current beat

class UnitRecordLog
{
public:
    void Init(uint8 coreIndex, uint32 slotCapacity, size_t bytes);  // resident storage; grows only on spill
    template <typename T> void Append(ObjectGuid source, WorldObject* target, RecordKind kind, T const& payload);
    void Reset();                                                   // size = 0, sequence = 0; heads stay (beat-tagged)
    TargetHead const& Head(uint32 slotIndex) const;
    RecordHeader const* At(uint32 offset) const;
    void GrowHeads(uint32 slotCapacity);                            // seal-only, when the slot index space grows

private:
    uint8 _core;
    std::vector<uint8> _bytes;        // reserve(bytes) once; high-water
    size_t _size = 0;
    uint32 _sequence = 0;
    std::vector<TargetHead> _head;    // indexed by ObjectSlotPair::SlotIndex()
};
```

`Append`:

```cpp
uint32 beat = BeatContext::beat.load(relaxed);
uint32 idx = target->SlotIndex();
TargetHead& h = _head[idx];
RecordHeader hdr{ target->GetGUID(), source, h.beat == beat ? h.offset : ~0u, _sequence++, uint16(sizeof(T)), kind, 0 };
size_t off = _size; _size += sizeof(RecordHeader) + alignUp8(sizeof(T));
if (_size > _bytes.capacity()) Spill();        // one extra resident block; a spill every beat means the reserve is short
memcpy(&_bytes[off], &hdr, sizeof hdr); memcpy(&_bytes[off + sizeof hdr], &payload, sizeof(T));
bool first = (h.beat != beat);
h = { beat, uint32(off) };
if (first)
    target->_writtenBy.fetch_or(1ull << _core, std::memory_order_release);   // one shared write per (core, target, beat)
if (first && !target->IsDueNextBeat())
    BeatScheduler::Wake(target);                                            // step 12; no-op before it
```

The `fetch_or` is the only shared write in the hot path. A boss hit by 40 players takes at most `cores` of them per beat.

## Inbox apply

```cpp
void WorldObject::ApplyInbox()
{
    uint64 mask = _writtenBy.exchange(0, std::memory_order_acquire);
    if (!mask) return;
    thread_local std::vector<RecordHeader const*> collected;  // capacity kept
    collected.clear();
    uint32 beat = BeatContext::beat.load(relaxed) - 1;        // records were written in the previous beat
    for (; mask; mask &= mask - 1)
    {
        UnitRecordLog const& log = sRecordLogs->Log(std::countr_zero(mask));
        TargetHead h = log.Head(SlotIndex());
        if (h.beat != beat) continue;
        for (uint32 off = h.offset; off != ~0u; off = log.At(off)->nextInTarget)
            collected.push_back(log.At(off));
    }
    std::sort(collected.begin(), collected.end(), [](auto a, auto b) {
        return a->source != b->source ? a->source < b->source : a->sequence < b->sequence; });
    ApplyRecords(collected);      // virtual: Unit, GameObject, DynamicObject, Corpse
}
```

`Unit::ApplyRecords` runs two passes so damage to one target in one beat is one health write:

1. **Sum pass.** For every `SpellHit`/`MeleeHit`/`PeriodicLanded`-to-me record that carries damage or heal, run the land step up to and including mitigation (absorb, resist, block, armor) but stop before `DealDamage`. Accumulate `totalDamage`, `totalHeal`, and keep each record's mitigated numbers.
2. **Apply pass.** One `ModifyHealth(totalHeal − totalDamage)` with the death check once. Then per record, in order: aura creation, effect handlers, procs (victim half), logs, feedback records. A record whose target died earlier in the same pass is still applied for its feedback (the attacker needs its proc and credit) but skips aura creation on a dead unit, as today's `DoAllEffectOnTarget` does via `IsAlive() != target->alive`.

Records whose target is gone (`!IsInWorld()`) are dropped at apply. A record is never carried into a second beat.

On this step `Map::Update` calls `ApplyInbox()` for every object in `m_mapRefMgr` and `_updatableObjectList` whose `_writtenBy` is non-zero, before any `Update`. That is O(objects) on a scan; step 12 replaces it with the wake rule (only woken or due objects have tasks).

## Timing rule

A record appended in beat *k* is applied at the start of the target's task in beat *k+1*. Feedback it produces is applied in beat *k+2*. The same at key 1 and key 13. There is no same-tick flush.

## Kinds in this step

Each row: the call today, the writer-side change, the record payload, the applier.

| Kind | Today | Writer side | Payload | Applier on target |
| --- | --- | --- | --- | --- |
| `ThreatBackRef` | `ThreatMgr::AddThreat` on the victim calls `attacker->GetThreatMgr().PutThreatenedByMeRef(ref)` | Victim's task adds to its own threat list and appends `ThreatBackRef` to the attacker | `victimGuid`, `refId` (per-victim counter) | Attacker inserts a `ThreatenedByMe` entry by guid. `Unit::m_threatenedByMe` becomes guid-keyed; `ThreatReference` holds guids, resolves to `Unit*` via `ObjectAccessor::GetUnit` at use (a handle lookup, allowed) |
| `CombatRef` | `CombatManager::SetInCombatWith(a, b)` creates one `CombatReference` stored in both managers | The caller's side inserts its half and appends `CombatRef` to the other side | `otherGuid`, `isPvP`, `suppressed` | Other side inserts its half. `CombatReference` becomes two half-objects keyed by guid; `EndCombat` is `CombatEnd` to the other side |
| `CombatEnd` | `CombatReference::EndCombat` erases from both | Own erase, record to other | `otherGuid` | Erase own half, run `AtExitCombat` if the map is now empty |
| `AuraRemove` | `target->RemoveAurasDueToSpell(id, casterGuid)` etc. from a unit that is not `target` | Append | `spellId`, `casterGuid`, `removeMode`, `effMask` | `RemoveAurasDueToSpell(...)` on self |
| `Knockback` | `victim->KnockbackFrom(x, y, speedXY, speedZ)` from the caster | Append | `x`, `y`, `speedXY`, `speedZ` | `KnockbackFrom` on self (which writes own motion and sends to own client) |
| `Wake` | — | `Append` when the first record to a non-due target lands (automatic) | none | nothing; the target just has a task |

Charm (`SetCharmedBy`/`RemoveCharmedBy`) and vehicle seating (`Vehicle::AddPassenger`/`RemovePassenger`, `RelocatePassengers`) are **not** records. They are the two places where two units write each other every beat, so step 12 runs them as one **task group**; joining and leaving the group is a `TaskGroupJoin`/`TaskGroupLeave` defer (step 09). In this step (single thread, pre-beat-loop) those calls stay inline as today. `TaskScope` must therefore not be asserted around them yet; the foreign-read counter below excludes a unit's charmer/charmed and vehicle/passengers from the count (`GetCharmerGUID`, `GetCharmGUID`, `GetVehicleBase`, `GetVehicleKit` passenger guids are compared) so the step 13 estimate is not inflated by a mechanism that will not be audited.

### Removal prepared on the unit's own task

`Unit::CleanupsBeforeDelete` / `WorldObject::CleanupsBeforeDelete` today write other units: every attacker's `m_threatenedByMe` and threat list, every combat partner's `CombatManager`, the owner's `m_Controlled` (`Minion::RemoveFromWorld` → `SetMinion(this, false)`), the owner's `m_gameObj` (`GameObject::RemoveFromOwner`), and every viewer's visible set. If that stayed at the seal, the seal could not be sharded by grid (step 12), because a removal in one grid would write units in any other. So a removal that originates on the unit's own task — death, despawn, `DespawnOrUnsummon`, `TempSummon` expiry, `Player` logout request — calls `PrepareRemoval()`, which does **both halves of the cleanup that the unit itself may do**, and only then appends the `RemoveFromMap` defer:

1. The **foreign half** as records (table below).
2. The **own half** inline, because the unit is the owner: `RemoveAllAuras()`, `CombatStop()` own side, `GetThreatMgr().ClearAllThreat()` own side, `GetMotionMaster()->Clear()`, `m_Events.KillAllEvents(false)`, `RemoveAllGameObjects()` own list, `RemoveAllControlled()` own list, `UnsummonAllTotems`, the AI's `OnDespawn`/`JustDespawned`-style own hooks, `ClearComboPoints`, interrupt own spells — everything in today's `CleanupsBeforeDelete` and `RemoveFromWorld` that writes `this`. It then sets `_removing = true`, publishes a final slot (so viewers' next scan sees it gone), and does not republish.

After `PrepareRemoval` the object is a dead handle: still addressable for the rest of the beat, never updated again (a `Wake` to a `_removing` object is dropped by `IsDueNextBeat`; `ApplyInbox` on it is never called). What remains for the seal is mechanical and cheap: the grid item (step 12 S1a) does `RemoveFromGrid()` — one intrusive list unlink, ~100 ns — and the map item (S1b) erases it from `_objectsStore` and `HashMapHolder` and pushes it onto `_freeAtSeal`. A hundred creatures dying in one grid is therefore ~10 µs of grid work, not a hundred aura-removal passes serialised in one item.

| Kind | Writer side (dying unit's task) | Payload | Applier |
| --- | --- | --- | --- |
| `ThreatRemove` | For each guid in own `m_threatenedByMe` (units that have us in their threat list) and each victim in own threat list (units whose `m_threatenedByMe` has us) | `deadGuid` | Erase the entry keyed by `deadGuid`; if own threat list is now empty, today's `ThreatMgr::ClearAllThreat`-side effects (evade check is the unit's own AI next update) |
| `CombatEnd` (exists) | To each combat partner | `otherGuid` | as above |
| `MinionUnlink` | To `GetOwnerGUID()` if we are a minion/guardian/totem | `minionGuid`, `isPet` | Owner erases from `m_Controlled` (now a `GuidSet`), clears `m_SummonSlot[]` entry, pet-specific bookkeeping that `SetMinion(false)` does on the owner |
| `GameObjectUnlink` | From a `GameObject` to its owner | `goGuid` | Owner erases from `m_gameObj` (now a guid list) |
| `VisibilityUnlink` (step 10) | To each viewer and each viewed object | `guid` | Erase from the visible set |

`Unit::m_Controlled` becomes `GuidSet`; `Unit::m_gameObj` becomes `GuidList`; iteration resolves through `ObjectAccessor::GetUnit`/`Map::GetGameObject` (handles). A removal that originates at the seal (map unload, `S1b` script despawn, `RemoveAllObjectsInRemoveList` for an object that was never prepared) runs today's two-sided body in S1b, where no grid task runs and the whole map is live. `RemoveFromMap{guid, prepared}` carries the flag so the applier knows which body to run (step 09).

`Unit::AddThreat(victim, ...)` (`Unit.cpp:11214`) is called by the attacker and writes the victim's threat list. Under the record rule the attacker cannot write the victim. Threat generated by a hit is therefore part of the land step (step 07, the target adds threat for the caster to its own list), and `AddThreat` called from scripts/AI against a foreign unit appends a `Threat` payload (`amount`, `schoolMask`, `spellId`) to the victim, which the victim applies to its own list and answers with `ThreatBackRef`. Add `Threat` to the enum.

## Foreign-read detection in this step

Nothing asserts yet (step 13). Add a debug-only counter: `Object::GetUInt32Value` increments `sForeignReads` when `BeatContext::owner && BeatContext::owner != this`. `BeatContext::owner` is set by `TaskScope` around each `player->Update` / `obj->Update` call in `Map::Update` starting this step. `.debug foreignreads` prints and clears the counter. The number is the size of the step 13 audit.

## Check

1. Two characters duel. Each melee swing is still inline in this step (step 08), but `SetInCombatWith` now produces a `CombatRef` record: the second unit enters combat on the next tick, visible in `.debug slot` as `isInCombat` flipping one tick after the first.
2. A creature taunted by a spell script `AddThreat` from a foreign unit gets threat next tick; the attacker's `m_threatenedByMe` has the entry the tick after.
3. `.server info` in band on an empty server. Record log `Reset` at the end of every tick; `_size` after reset is 0.
4. `.debug foreignreads` after one minute of play is written into this file's exit note.

## Must not

- No `Unit*`, `Aura*`, or `Spell*` in a payload.
- No same-tick apply, no generation loop.
- No per-record allocation. `Spill` adds one resident block.
- No second writer of `_writtenBy` besides `Append` and the target's `exchange`.

## Checklist

- [ ] `UnitRecordLog` resident; `Append` one `fetch_or` per (core, target, beat).
- [ ] `ApplyInbox` collects by mask bits, sorts by `(source, sequence)`, two-pass health.
- [ ] `ApplyInbox` runs before any `Update` in `Map::Update`; `ResetAll` at the end.
- [ ] `ThreatBackRef`, `CombatRef`, `CombatEnd`, `Threat`, `AuraRemove`, `Knockback`, `Wake` implemented and applied.
- [ ] `PrepareRemoval()` emits `ThreatRemove`, `CombatEnd`, `MinionUnlink`, `GameObjectUnlink`, `VisibilityUnlink` and runs the own half of `CleanupsBeforeDelete` inline; sets `_removing`, final slot publish, no republish; `m_Controlled` and `m_gameObj` are guid-keyed.
- [ ] `CleanupsBeforeDelete(prepared = true)` is `RemoveFromGrid` only; debug `ASSERT(_removing)` at its entry.
- [ ] Threat and combat references are guid-keyed.
- [ ] `TaskScope` around every object update; foreign-read counter available.
- [ ] Dropped-at-apply for targets not in world.

## Exit note

(foreign-read count after one minute of play)
