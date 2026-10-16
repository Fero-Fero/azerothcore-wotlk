# 05 — Object slot

Every `WorldObject` gets two slots and a `publishedBeat` stamp. After its `Update` the object copies its readable state into the slot that is **not** currently readable and then advances the stamp. Anyone else reads the stamped slot. On this step there is one thread and the slot is only written, never read; it is the rehearsal and the cost measurement. Steps 06–10 start reading it.

## Files

- New `src/server/game/Entities/Object/ObjectSlot.h` and `ObjectSlot.cpp`.
- `src/server/game/Entities/Object/Object.h` — `WorldObject` gains `ObjectSlotPair _slots;` and the publish/read API.
- `src/server/game/Entities/Object/Object.cpp` — allocation in `Object::_InitValues` (values count is known there), `WorldObject::PublishSlot`.
- `src/server/game/Entities/Unit/Unit.cpp` — `Unit::FillSlot` (aura list, unit-only fields).
- `src/server/game/Entities/Player/Player.cpp` — `Player::FillSlot` (nothing extra yet; hook exists for step 10).
- `src/server/game/Maps/Map.cpp` — `Map::Update`: `PublishSlot` after each `player->Update` (line 489) and after each `obj->Update` in `UpdateNonPlayerObjects` (lines 557, 578). Also in the zero-diff branch after `player->Update(s_diff)` (454).
- `src/server/scripts/Commands/cs_debug.cpp` — `.debug slot <guid>` dump command for the check.

## Layout

```cpp
struct SlotAura
{
    uint32 spellId;
    ObjectGuid casterGuid;
    uint8  effectMask;         // AuraApplication::GetEffectMask()
    uint8  stackAmount;        // Aura::GetStackAmount()
    uint8  charges;            // Aura::GetCharges()
    uint8  flags;              // bit0 positive (AuraApplication::IsPositive()), bit1 passive, bit2 isSelfCast
    int32  amount[MAX_SPELL_EFFECTS];     // AuraEffect::GetAmount() per effect, 0 where effect absent
    int32  duration;           // Aura::GetDuration()
    int32  maxDuration;        // Aura::GetMaxDuration()
};

struct ObjectSlot
{
    uint32 beat = 0;                   // beat this slot was published in (debug cross-check)
    uint16 valuesCount = 0;            // == Object::m_valuesCount
    uint32* values = nullptr;          // valuesCount words, owned by ObjectSlotPair storage
    uint32* changesMask = nullptr;     // (valuesCount + 31) / 32 words; the owner's _changesMask at publish

    // WorldObject
    Position pos;                      // m_positionX/Y/Z/O
    uint32 phaseMask;
    uint32 zoneId, areaId;
    uint16 cellId;                     // Cell(pos).GetCellCoord().GetId(), for isCellMarked-style checks

    // Unit only (zero for others)
    uint32 movementFlags;              // m_movementInfo.flags
    uint16 movementFlags2;             // m_movementInfo.flags2
    uint32 unitState;                  // Unit::m_state
    uint8  deathState;                 // Unit::m_deathState
    uint8  isInCombat;                 // Unit::IsInCombat()
    uint32 lastSanctuaryTime;          // Unit::m_lastSanctuaryTime
    ObjectGuid victimGuid;             // GetVictim() ? GetVictim()->GetGUID() : Empty
    ObjectGuid charmerOrOwnerGuid;     // GetCharmerOrOwnerGUID()
    uint32 auraCount;
    SlotAura* auras;                   // auraCount entries, capacity in the pair

    // Movement update block pre-serialised for viewers (step 10 fills it; empty until then)
    uint16 movementBlockSize;
    uint8* movementBlock;
};

class ObjectSlotPair
{
public:
    void Init(uint16 valuesCount, bool isUnit);  // one allocation for both slots: values*2, masks*2, aura arrays*2
    ObjectSlot& WriteSlot();                      // slot[(publishedBeat + 1) & 1]
    ObjectSlot const& ReadSlot() const;           // slot[publishedBeat.load(acquire) & 1]
    void Commit(uint32 beat);                     // publishedBeat.store(beat, release)
    uint32 PublishedBeat() const { return _publishedBeat.load(std::memory_order_relaxed); }
    uint32 SlotIndex() const { return _slotIndex; }   // dense index, step 06

private:
    ObjectSlot _slot[2];
    std::atomic<uint32> _publishedBeat{0};
    uint32 _slotIndex = 0;
    std::unique_ptr<uint8[]> _storage;  // one block; aura capacity grows only here, never per beat
    uint32 _auraCapacity = 0;
};
```

### Why the write slot is `(publishedBeat + 1) & 1` and not `beat & 1`

If a unit is not due in beat *k+1* it does not publish, so its stamp stays *k* and readers in beat *k+2* still read `slot[k & 1]`. A task in beat *k+2* that wrote `slot[(k+2) & 1] == slot[k & 1]` would overwrite the slot being read. Writing the slot opposite to the stamped one is always safe because the stamp advances only when this object publishes.

### Size

Player `m_valuesCount` is `PLAYER_END` (≈ 1326 words → 5.3 KB per slot, 10.6 KB per pair plus auras). Creature `UNIT_END` (≈ 148 words → 600 B per slot). Aura capacity starts at 16 for creatures and 64 for players and grows by doubling at `FillSlot`, never shrinks. 1 000 players + 50 000 creatures ≈ 11 MB + 60 MB. Resident, allocated once per object in `_InitValues`.

## Publish

```cpp
void WorldObject::PublishSlot()
{
    ObjectSlot& s = _slots.WriteSlot();
    s.beat = BeatContext::beat.load(std::memory_order_relaxed);
    std::memcpy(s.values, m_uint32Values, m_valuesCount * sizeof(uint32));
    _changesMask.CopyTo(s.changesMask);           // new UpdateMask helper; raw word copy
    s.pos = GetPosition();                         // the four floats
    s.phaseMask = GetPhaseMask();
    s.zoneId = m_zoneId; s.areaId = m_areaId;      // the cached ids, not a recompute
    s.cellId = Cell(s.pos.GetPositionX(), s.pos.GetPositionY()).GetCellCoord().GetId();
    FillSlot(s);                                   // virtual; Unit and Player add their parts
    _slots.Commit(s.beat);
}

void Unit::FillSlot(ObjectSlot& s)
{
    s.movementFlags = m_movementInfo.flags; s.movementFlags2 = m_movementInfo.flags2;
    s.unitState = m_state; s.deathState = m_deathState; s.isInCombat = IsInCombat();
    s.lastSanctuaryTime = m_lastSanctuaryTime;
    s.victimGuid = m_attacking ? m_attacking->GetGUID() : ObjectGuid::Empty;
    s.charmerOrOwnerGuid = GetCharmerOrOwnerGUID();
    _slots.EnsureAuraCapacity(m_appliedAuras.size());
    uint32 n = 0;
    for (auto const& [id, app] : m_appliedAuras)   // AuraApplicationMap, Unit.h:2175
    {
        Aura const* a = app->GetBase();
        SlotAura& sa = s.auras[n++];
        sa.spellId = id; sa.casterGuid = a->GetCasterGUID(); sa.effectMask = app->GetEffectMask();
        sa.stackAmount = a->GetStackAmount(); sa.charges = a->GetCharges();
        sa.flags = (app->IsPositive() ? 1 : 0) | (a->IsPassive() ? 2 : 0) | (a->GetCasterGUID() == GetGUID() ? 4 : 0);
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            sa.amount[i] = (sa.effectMask & (1 << i)) ? a->GetEffect(i)->GetAmount() : 0;
        sa.duration = a->GetDuration(); sa.maxDuration = a->GetMaxDuration();
    }
    s.auraCount = n;
}
```

`Player::FillSlot` calls `Unit::FillSlot` and does nothing more in this step.

The owner does **not** clear `_changesMask` here. Today `ClearUpdateMask` runs in `BuildUpdate` / `SendObjectUpdates`. Step 10 moves the clear to right after `PublishSlot` and removes `_updateObjects`. Until then both paths coexist and the mask in the slot is informational.

## Read API

```cpp
ObjectSlot const& WorldObject::ReadSlot() const { return _slots.ReadSlot(); }

// Slot accessors with the same semantics as the Object getters, no ASSERT (slots are always readable).
struct SlotReader
{
    ObjectSlot const& s;
    uint32 U32(uint16 i) const { return s.values[i]; }
    int32  I32(uint16 i) const { return int32(s.values[i]); }
    float  F(uint16 i) const  { float f; std::memcpy(&f, &s.values[i], 4); return f; }
    uint64 U64(uint16 i) const { return uint64(s.values[i]) | (uint64(s.values[i + 1]) << 32); }
    ObjectGuid Guid(uint16 i) const { return ObjectGuid(U64(i)); }
    uint8  Byte(uint16 i, uint8 off) const { return (s.values[i] >> (off * 8)) & 0xFF; }
    bool   Flag(uint16 i, uint32 f) const { return (s.values[i] & f) != 0; }
    bool   HasAura(uint32 spellId, ObjectGuid caster = ObjectGuid::Empty) const;
    SlotAura const* FindAura(uint32 spellId, ObjectGuid caster = ObjectGuid::Empty) const;
    int32  TotalAuraModifier(AuraType type) const;  // sum over slot auras of amount[i] where SpellInfo->Effects[i].ApplyAuraName == type
    uint32 Health() const { return U32(UNIT_FIELD_HEALTH); }
    uint32 MaxHealth() const { return U32(UNIT_FIELD_MAXHEALTH); }
    uint8  Level() const { return U32(UNIT_FIELD_LEVEL); }
    uint32 Faction() const { return U32(UNIT_FIELD_FACTIONTEMPLATE); }
    bool   IsAlive() const { return s.deathState == ALIVE; }
};
```

`UnitView` (step 07) is a `SlotReader` with a `Unit const*` handle attached, plus the reaction logic (`IsFriendlyTo`, `IsHostileTo`) implemented from the faction template, unit flags, PvP bytes, and the owner chain resolved through `ObjectAccessor::GetUnit(...)->ReadSlot()`.

## Slot index

`ObjectSlotPair::_slotIndex` is a dense per-process index used by the record log's `head[]` arrays (step 06). (The claim flag of step 14 is a per-object atomic, not indexed.) Allocated from a free list in `WorldObject::AddToWorld`, returned in seal phase S2 of the beat whose `RemoveFromMap` defer removed the object (step 12, `ReturnSlotIndices` runs after `FreeRemoved`). A record addressed to it during that beat is in a log that the same S2 resets, so nothing is misdelivered; the index may be reused from the next beat on. `SlotIndexAllocator` is a `std::vector<uint32> free` plus `uint32 next`, guarded by a mutex that is only taken at add/remove, which happen between beats or at the seal.

## Check

`.debug slot <guid>` prints, for the readable slot: beat, pos, health/maxhealth/level/faction from the slot and from live, aura count from slot and `m_appliedAuras.size()`, and a per-word diff count between `values` and `m_uint32Values`. After a tick with no intervening write, the diff count is 0 and both aura counts match. `.server info` on an empty server stays in the step 01 band; a jump of tens of milliseconds means `FillSlot` is doing more than a copy.

## Must not

- No reader of a slot yet except the debug command.
- No per-tick allocation. Aura capacity grows inside `FillSlot` only when exceeded, and then doubles.
- No change to `_changesMask` clearing yet.
- No `Unit*` or `Aura*` in a slot.

## Checklist

- [x] `ObjectSlotPair::Init` called from `_InitValues` with the right `isUnit` (via the `_OnValuesInitialized` hook).
- [x] Write slot is `(publishedBeat + 1) & 1`; commit is a release store; read is an acquire load.
- [x] `PublishSlot` after every `player->Update` and every `obj->Update`, including the zero-diff branch.
- [x] `SlotAura` carries casterGuid, effectMask, stack, charges, amounts, duration, maxDuration.
- [ ] `.debug slot` shows zero diff after a tick. (needs a build and a login)
- [ ] `.server info` in band. (needs a build and a login)

Deviations from the layout above: `cellId` is `uint32` (a map has 512×512 cells, so the id does not fit `uint16`);
the slot index is released in `RemoveFromWorld` until step 12 moves it to S2.
