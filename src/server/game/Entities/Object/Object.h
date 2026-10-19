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

#ifndef _OBJECT_H
#define _OBJECT_H

#include "AreaDefines.h"
#include "BeatContext.h"
#include "Common.h"
#include "DataMap.h"
#include "EventProcessor.h"
#include "G3D/Vector3.h"
#include "GridDefines.h"
#include "GridReference.h"
#include "Map.h"
#include "ModelIgnoreFlags.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectSlot.h"
#include "Optional.h"
#include "Position.h"
#include "SpellDefines.h"
#include "UnitRecords.h"
#include "UnitDefines.h"
#include "UpdateData.h"
#include "UpdateMask.h"
#include "ObjectVisibilityContainer.h"
#include <memory>
#include <set>
#include <sstream>
#include <string>

#include "UpdateFields.h"

class ALEEventProcessor;

enum TempSummonType
{
    TEMPSUMMON_TIMED_OR_DEAD_DESPAWN       = 1,             // despawns after a specified time OR when the creature disappears
    TEMPSUMMON_TIMED_OR_CORPSE_DESPAWN     = 2,             // despawns after a specified time OR when the creature dies
    TEMPSUMMON_TIMED_DESPAWN               = 3,             // despawns after a specified time
    TEMPSUMMON_TIMED_DESPAWN_OUT_OF_COMBAT = 4,             // despawns after a specified time after the creature is out of combat
    TEMPSUMMON_CORPSE_DESPAWN              = 5,             // despawns instantly after death
    TEMPSUMMON_CORPSE_TIMED_DESPAWN        = 6,             // despawns after a specified time after death
    TEMPSUMMON_DEAD_DESPAWN                = 7,             // despawns when the creature disappears
    TEMPSUMMON_MANUAL_DESPAWN              = 8,             // despawns when UnSummon() is called
    TEMPSUMMON_DESPAWNED                   = 9,             // xinef: DONT USE, INTERNAL USE ONLY
    TEMPSUMMON_TIMED_DESPAWN_OOC_ALIVE     = 10,            // despawns after a specified time after the creature is out of combat and alive
};

enum PhaseMasks
{
    PHASEMASK_NORMAL   = 0x00000001,
    PHASEMASK_ANYWHERE = 0xFFFFFFFF
};

enum NotifyFlags
{
    NOTIFY_NONE                     = 0x00,
    NOTIFY_AI_RELOCATION            = 0x01,
    NOTIFY_VISIBILITY_CHANGED       = 0x02,
    NOTIFY_ALL                      = 0xFF
};

enum GOSummonType
{
    GO_SUMMON_TIMED_OR_CORPSE_DESPAWN = 0,      // despawns after a specified time OR when the summoner dies
    GO_SUMMON_TIMED_DESPAWN = 1                 // despawns after a specified time
};

class WorldPacket;
class UpdateData;
class ByteBuffer;
class WorldSession;
class Creature;
class Player;
class InstanceScript;
class GameObject;
class TempSummon;
class Vehicle;
class CreatureAI;
class ZoneScript;
class Unit;
class Transport;
class StaticTransport;
class MotionTransport;
struct FactionTemplateEntry;
class Spell;
class SpellInfo;
struct CastSpellTargetArg;
enum SpellCastResult : uint8;

struct PositionFullTerrainStatus;

// Field values and change mask an update block is built from: the owner's live arrays, or a published slot
// when a viewer builds on its own task (step 10). The mask is packed one bit per field (UpdateMask::CopyTo).
struct UpdateValuesSource
{
    uint32 const* values;
    uint32 const* changesMask;
    // The published slot a viewer builds from; nullptr for the owner's live build. Everything a build needs
    // beyond the field array (auras, owner guids) comes from here so no live member of a foreign object is read.
    ObjectSlot const* slot = nullptr;
    // Extra UF_FLAG_* forced into the block for this build only, the slot-side equivalent of the owner's
    // SetFieldNotifyFlag/RemoveFieldNotifyFlag pair (a viewer must not write a foreign object's notify flags).
    uint16 notifyFlags = 0;

    [[nodiscard]] bool IsLive() const { return slot == nullptr; }

    [[nodiscard]] bool Changed(uint16 index) const { return (changesMask[index >> 5] >> (index & 31)) & 1; }
    [[nodiscard]] float F(uint16 index) const { float f; std::memcpy(&f, &values[index], sizeof(float)); return f; }
};

static constexpr Milliseconds HEARTBEAT_INTERVAL = 5s + 200ms;

class Object
{
public:
    virtual ~Object();

    [[nodiscard]] bool IsInWorld() const { return m_inWorld; }

    virtual void AddToWorld();
    virtual void RemoveFromWorld();

    [[nodiscard]] static ObjectGuid GetGUID(Object const* o) { return o ? o->GetGUID() : ObjectGuid::Empty; }
    [[nodiscard]] ObjectGuid GetGUID() const { return GetGuidValue(OBJECT_FIELD_GUID); }
    [[nodiscard]] PackedGuid const& GetPackGUID() const { return m_PackGUID; }
    [[nodiscard]] uint32 GetEntry() const { return GetUInt32Value(OBJECT_FIELD_ENTRY); }
    void SetEntry(uint32 entry) { SetUInt32Value(OBJECT_FIELD_ENTRY, entry); }

    [[nodiscard]] float GetObjectScale() const { return GetFloatValue(OBJECT_FIELD_SCALE_X); }
    virtual void SetObjectScale(float scale) { SetFloatValue(OBJECT_FIELD_SCALE_X, scale); }

    virtual uint32 GetDynamicFlags() const { return 0; }
    bool HasDynamicFlag(uint32 flag) const { return (GetDynamicFlags() & flag) != 0; }
    virtual void SetDynamicFlag(uint32 flag) { ReplaceAllDynamicFlags(GetDynamicFlags() | flag); }
    virtual void RemoveDynamicFlag(uint32 flag) { ReplaceAllDynamicFlags(GetDynamicFlags() & ~flag); }
    virtual void ReplaceAllDynamicFlags([[maybe_unused]] uint32 flag) { }

    [[nodiscard]] TypeID GetTypeId() const { return m_objectTypeId; }
    [[nodiscard]] bool isType(uint16 mask) const { return (mask & m_objectType); }

    virtual void BuildCreateUpdateBlockForPlayer(UpdateData* data, Player* target);
    void SendUpdateToPlayer(Player* player);

    void BuildValuesUpdateBlockForPlayer(UpdateData* data, Player* target);
    void BuildOutOfRangeUpdateBlock(UpdateData* data) const;
    void BuildMovementUpdateBlock(UpdateData* data, uint32 flags = 0) const;

    virtual void DestroyForPlayer(Player* target, bool onDeath = false) const;

    [[nodiscard]] int32 GetInt32Value(uint16 index) const;
    [[nodiscard]] uint32 GetUInt32Value(uint16 index) const;
    [[nodiscard]] uint64 GetUInt64Value(uint16 index) const;
    [[nodiscard]] float GetFloatValue(uint16 index) const;
    [[nodiscard]] uint8 GetByteValue(uint16 index, uint8 offset) const;
    [[nodiscard]] uint16 GetUInt16Value(uint16 index, uint8 offset) const;
    [[nodiscard]] ObjectGuid GetGuidValue(uint16 index) const;

    void SetInt32Value(uint16 index, int32 value);
    void SetUInt32Value(uint16 index, uint32 value);
    void UpdateUInt32Value(uint16 index, uint32 value);
    void SetUInt64Value(uint16 index, uint64 value);
    void SetFloatValue(uint16 index, float value);
    void SetByteValue(uint16 index, uint8 offset, uint8 value);
    void SetUInt16Value(uint16 index, uint8 offset, uint16 value);
    void SetInt16Value(uint16 index, uint8 offset, int16 value) { SetUInt16Value(index, offset, (uint16)value); }
    void SetGuidValue(uint16 index, ObjectGuid value);
    void SetStatFloatValue(uint16 index, float value);
    void SetStatInt32Value(uint16 index, int32 value);

    bool AddGuidValue(uint16 index, ObjectGuid value);
    bool RemoveGuidValue(uint16 index, ObjectGuid value);

    void ApplyModUInt32Value(uint16 index, int32 val, bool apply);
    void ApplyModInt32Value(uint16 index, int32 val, bool apply);
    void ApplyModUInt64Value(uint16 index, int32 val, bool apply);
    void ApplyModPositiveFloatValue(uint16 index, float val, bool apply);
    void ApplyModSignedFloatValue(uint16 index, float val, bool apply);

    void SetFlag(uint16 index, uint32 newFlag);
    void RemoveFlag(uint16 index, uint32 oldFlag);
    void ToggleFlag(uint16 index, uint32 flag);
    [[nodiscard]] bool HasFlag(uint16 index, uint32 flag) const;
    void ApplyModFlag(uint16 index, uint32 flag, bool apply);

    void SetByteFlag(uint16 index, uint8 offset, uint8 newFlag);
    void RemoveByteFlag(uint16 index, uint8 offset, uint8 newFlag);
    [[nodiscard]] bool HasByteFlag(uint16 index, uint8 offset, uint8 flag) const;

    void SetFlag64(uint16 index, uint64 newFlag);
    void RemoveFlag64(uint16 index, uint64 oldFlag);
    void ToggleFlag64(uint16 index, uint64 flag);
    [[nodiscard]] bool HasFlag64(uint16 index, uint64 flag) const;
    void ApplyModFlag64(uint16 index, uint64 flag, bool apply);

    void ClearUpdateMask(bool remove);

    [[nodiscard]] uint16 GetValuesCount() const { return m_valuesCount; }

    [[nodiscard]] virtual bool hasQuest(uint32 /* quest_id */) const { return false; }
    [[nodiscard]] virtual bool hasInvolvedQuest(uint32 /* quest_id */) const { return false; }

    void SetFieldNotifyFlag(uint16 flag) { _fieldNotifyFlags |= flag; }
    void RemoveFieldNotifyFlag(uint16 flag) { _fieldNotifyFlags &= ~flag; }

    // FG: some hacky helpers
    void ForceValuesUpdateAtIndex(uint32);

    [[nodiscard]] inline bool IsPlayer() const { return GetTypeId() == TYPEID_PLAYER; }
    Player* ToPlayer() { if (IsPlayer()) return reinterpret_cast<Player*>(this); else return nullptr; }
    [[nodiscard]] Player const* ToPlayer() const { if (IsPlayer()) return reinterpret_cast<Player const*>(this); else return nullptr; }

    [[nodiscard]] inline bool IsCreature() const { return GetTypeId() == TYPEID_UNIT; }
    Creature* ToCreature() { if (IsCreature()) return reinterpret_cast<Creature*>(this); else return nullptr; }
    [[nodiscard]] Creature const* ToCreature() const { if (IsCreature()) return reinterpret_cast<Creature const*>(this); else return nullptr; }

    [[nodiscard]] inline bool IsUnit() const { return isType(TYPEMASK_UNIT); }
    Unit* ToUnit() { if (IsCreature() || IsPlayer()) return reinterpret_cast<Unit*>(this); else return nullptr; }
    [[nodiscard]] Unit const* ToUnit() const { if (IsCreature() || IsPlayer()) return reinterpret_cast<Unit const*>(this); else return nullptr; }

    [[nodiscard]] inline bool IsGameObject() const { return GetTypeId() == TYPEID_GAMEOBJECT; }
    GameObject* ToGameObject() { if (IsGameObject()) return reinterpret_cast<GameObject*>(this); else return nullptr; }
    [[nodiscard]] GameObject const* ToGameObject() const { if (IsGameObject()) return reinterpret_cast<GameObject const*>(this); else return nullptr; }

    [[nodiscard]] inline bool IsCorpse() const { return GetTypeId() == TYPEID_CORPSE; }
    Corpse* ToCorpse() { if (IsCorpse()) return reinterpret_cast<Corpse*>(this); else return nullptr; }
    [[nodiscard]] Corpse const* ToCorpse() const { if (IsCorpse()) return reinterpret_cast<Corpse const*>(this); else return nullptr; }

    [[nodiscard]] inline bool IsDynamicObject() const { return GetTypeId() == TYPEID_DYNAMICOBJECT; }
    DynamicObject* ToDynObject() { if (IsDynamicObject()) return reinterpret_cast<DynamicObject*>(this); else return nullptr; }
    [[nodiscard]] DynamicObject const* ToDynObject() const { if (IsDynamicObject()) return reinterpret_cast<DynamicObject const*>(this); else return nullptr; }

    [[nodiscard]] inline bool IsItem() const { return GetTypeId() == TYPEID_ITEM; }
    [[nodiscard]] virtual bool IsWorldObject() const { return false; }

    virtual void Heartbeat() {}

    virtual std::string GetDebugInfo() const;

    DataMap CustomData;

    template<typename... T>
    [[nodiscard]] bool EntryEquals(T... entries) const
    {
        return ((GetEntry() == entries) || ...);
    }

protected:
    Object();

    void _InitValues();
    // Runs at the end of _InitValues, when m_valuesCount is final. WorldObject sizes its slots here.
    virtual void _OnValuesInitialized() { }
    void _Create(ObjectGuid::LowType guidlow, uint32 entry, HighGuid guidhigh);
    void _Create(ObjectGuid guid);
    [[nodiscard]] std::string _ConcatFields(uint16 startIndex, uint16 size) const;
    bool _LoadIntoDataField(std::string const& data, uint32 startOffset, uint32 count);

    uint32 GetUpdateFieldData(Player const* target, uint32*& flags, UpdateValuesSource const& src) const;

    void BuildMovementUpdate(ByteBuffer* data, uint16 flags) const;
    // Live values and the live mask.
    void BuildValuesUpdate(uint8 updateType, ByteBuffer* data, Player* target);
    // The values builder proper. `src` is either the live fields or a published slot (step 10); the per-type
    // overrides read every field value through it and keep only viewer-dependent decisions on the handle.
    virtual void BuildValuesUpdateFrom(uint8 updateType, ByteBuffer* data, Player* target, UpdateValuesSource const& src);

    uint16 m_objectType;

    TypeID m_objectTypeId;
    uint16 m_updateFlag;

    union
    {
        int32*  m_int32Values;
        uint32* m_uint32Values;
        float*  m_floatValues;
    };

    UpdateMask _changesMask;

    uint16 m_valuesCount;

    uint16 _fieldNotifyFlags;

    // A field changed while in world. A WorldObject makes sure it updates (and so publishes) this tick; an
    // Item joins its owner's changed-item list. Viewers pull the values from the slot (step 10).
    virtual void AddToObjectUpdate() = 0;
    virtual void RemoveFromObjectUpdate() = 0;
    void AddToObjectUpdateIfNeeded();

    bool m_objectUpdated;

private:
    bool m_inWorld;

    PackedGuid m_PackGUID;

    // for output helpfull error messages from asserts
    [[nodiscard]] bool PrintIndexError(uint32 index, bool set) const;
    Object(Object const&);                              // prevent generation copy constructor
    Object& operator=(Object const&);                   // prevent generation assigment operator
};

struct MovementInfo
{
    // common
    ObjectGuid guid;
    uint32 flags{0};
    uint16 flags2{0};
    Position pos;
    uint32 time{0};

    // transport
    struct TransportInfo
    {
        void Reset()
        {
            guid.Clear();
            pos.Relocate(0.0f, 0.0f, 0.0f, 0.0f);
            seat = -1;
            time = 0;
            time2 = 0;
        }

        ObjectGuid guid;
        Position pos;
        int8 seat;
        uint32 time;
        uint32 time2;
    } transport;

    // swimming/flying
    float pitch{0.0f};

    // falling
    uint32 fallTime{0};

    // jumping
    struct JumpInfo
    {
        void Reset()
        {
            zspeed = sinAngle = cosAngle = xyspeed = 0.0f;
        }

        float zspeed, sinAngle, cosAngle, xyspeed;
    } jump;

    // spline
    float splineElevation{0.0f};

    MovementInfo()
    {
        pos.Relocate(0.0f, 0.0f, 0.0f, 0.0f);
        transport.Reset();
        jump.Reset();
    }

    [[nodiscard]] uint32 GetMovementFlags() const { return flags; }
    void SetMovementFlags(uint32 flag) { flags = flag; }
    void AddMovementFlag(uint32 flag) { flags |= flag; }
    void RemoveMovementFlag(uint32 flag) { flags &= ~flag; }
    [[nodiscard]] bool HasMovementFlag(uint32 flag) const { return flags & flag; }

    [[nodiscard]] UnitMoveType GetSpeedType() const { return GetSpeedType(flags); }
    [[nodiscard]] static UnitMoveType GetSpeedType(uint32 moveFlags);

    [[nodiscard]] uint16 GetExtraMovementFlags() const { return flags2; }
    void AddExtraMovementFlag(uint16 flag) { flags2 |= flag; }
    [[nodiscard]] bool HasExtraMovementFlag(uint16 flag) const { return flags2 & flag; }

    void SetFallTime(uint32 newFallTime) { fallTime = newFallTime; }

    void OutDebug();
};

template<class T>
class GridObject
{
public:
    bool IsInGrid() const
    {
        return _gridRef.isValid();
    }
    void AddToGrid(GridRefMgr<T>& m)
    {
        ASSERT(!IsInGrid());
        _gridRef.link(&m, (T*)this);
    }
    void RemoveFromGrid()
    {
        ASSERT(IsInGrid());
        _gridRef.unlink();
    }
private:
    GridReference<T> _gridRef;
};

template <class T_VALUES, class T_FLAGS, class FLAG_TYPE, uint8 ARRAY_SIZE>
class FlaggedValuesArray32
{
public:
    FlaggedValuesArray32()
    {
        memset(&m_values, 0x00, sizeof(T_VALUES) * ARRAY_SIZE);
        m_flags = 0;
    }

    [[nodiscard]] T_FLAGS GetFlags() const { return m_flags; }
    [[nodiscard]] bool HasFlag(FLAG_TYPE flag) const { return m_flags & (1 << flag); }
    void AddFlag(FLAG_TYPE flag) { m_flags |= (1 << flag); }
    void DelFlag(FLAG_TYPE flag) { m_flags &= ~(1 << flag); }

    [[nodiscard]] T_VALUES GetValue(FLAG_TYPE flag) const { return m_values[flag]; }
    void SetValue(FLAG_TYPE flag, T_VALUES value) { m_values[flag] = value; }
    void AddValue(FLAG_TYPE flag, T_VALUES value) { m_values[flag] += value; }

private:
    T_VALUES m_values[ARRAY_SIZE];
    T_FLAGS m_flags;
};

enum MapObjectCellMoveState
{
    MAP_OBJECT_CELL_MOVE_NONE, //not in move list
    MAP_OBJECT_CELL_MOVE_ACTIVE, //in move list
    MAP_OBJECT_CELL_MOVE_INACTIVE, //in move list but should not move
};

class MovableMapObject
{
    friend class Map; //map for moving creatures
    friend class ObjectGridLoader; //grid loader for loading creatures
    template<class T> friend class RandomMovementGenerator;

protected:
    MovableMapObject()  = default;

    [[nodiscard]] Cell const& GetCurrentCell() const { return _currentCell; }

private:
    void SetCurrentCell(Cell const& cell) { _currentCell = cell; }

    Cell _currentCell;
    MapObjectCellMoveState _moveState{MAP_OBJECT_CELL_MOVE_NONE};
};

class UpdatableMapObject
{
    friend class Map;

public:
    enum UpdateState : uint8
    {
        NotUpdating,
        PendingAdd,
        Updating
    };

protected:
    UpdatableMapObject() : _mapUpdateListOffset(0), _mapUpdateState(NotUpdating) { }

private:
    void SetMapUpdateListOffset(std::size_t const offset)
    {
        ASSERT(_mapUpdateState == Updating, "Attempted to set update list offset when object is not in map update list");
        _mapUpdateListOffset = offset;
    }

    size_t GetMapUpdateListOffset() const
    {
        ASSERT(_mapUpdateState == Updating, "Attempted to get update list offset when object is not in map update list");
        return _mapUpdateListOffset;
    }

    void SetUpdateState(UpdateState state)
    {
        _mapUpdateState = state;
    }

    UpdateState GetUpdateState() const
    {
        return _mapUpdateState;
    }

private:
    std::size_t _mapUpdateListOffset;
    UpdateState _mapUpdateState;
};

class WorldObject : public Object, public WorldLocation
{
protected:
    explicit WorldObject();
public:
    ~WorldObject() override;

    virtual void Update(uint32 diff);

    [[nodiscard]] bool IsWorldObject() const override { return true; }

    // Ungrouped objects are their own root. Step 12 points a passenger or a charmed unit at the group root.
    [[nodiscard]] WorldObject* TaskRoot() { return _taskRoot ? _taskRoot : this; }
    [[nodiscard]] WorldObject const* TaskRoot() const { return _taskRoot ? _taskRoot : this; }
    [[nodiscard]] bool IsTaskRoot() const { return !_taskRoot; }
    [[nodiscard]] std::vector<WorldObject*> const& TaskMembers() const { return _taskMembers; }
    // Step 12 task groups, written only at the seal (TaskGroupJoin / TaskGroupLeave defers) and at removal.
    void JoinTaskGroup(WorldObject* root);
    void LeaveTaskGroup();
    // A root whose task ran this beat republishes itself when: a player; IsUpdateNeeded; a pending record;
    // or any member says so. A member is never due on its own.
    [[nodiscard]] bool IsDueNextBeat();
    [[nodiscard]] uint32 LastTaskNs() const { return _lastTaskNs; }

    // Copies the readable state into the write slot and advances the stamp. Called once after each Update.
    void PublishSlot();
    [[nodiscard]] ObjectSlot const& ReadSlot() const { return _slots.ReadSlot(); }
    [[nodiscard]] ObjectSlotPair const& Slots() const { return _slots; }
    [[nodiscard]] uint32 SlotIndex() const { return _slots.SlotIndex(); }

    // Record inbox. Records written to this object in the previous beat are collected from every core's log
    // named in _writtenBy, sorted by (source, sequence) and handed to ApplyRecords before this object's Update.
    void ApplyInbox();
    [[nodiscard]] bool HasInbox() const { return _writtenBy.load(std::memory_order_relaxed) != 0; }
    // Called by UnitRecordLog::Append for the first record of a (core, target, beat). Sets the mask bit and
    // wakes the target so its inbox is scanned next beat.
    void OnFirstRecordOfBeat(uint8 core);
    // True when IsForeign(this): the running task is not this object's and must write it through a record.
    [[nodiscard]] bool IsForeignToTask() const { return BeatContext::IsForeign(this); }

    // A removal requested on this object's own task: runs the own half of the cleanup inline, emits the foreign
    // half as records, marks the object removing and publishes its final slot. The seal then only unlinks.
    void PrepareRemoval();
    [[nodiscard]] bool IsRemoving() const { return _removing; }

protected:
    // Foreign half of a task-originated removal: records to every unit this object would have written.
    virtual void PrepareRemovalForeign() { }
    // Own half: everything in today's CleanupsBeforeDelete that writes this. Must not RemoveFromWorld.
    virtual void PrepareRemovalOwn();

public:

    void _Create(ObjectGuid::LowType guidlow, HighGuid guidhigh, uint32 phaseMask);

    void AddToWorld() override;
    void RemoveFromWorld() override;

    void GetNearPoint2D(WorldObject const* searcher, float& x, float& y, float distance, float absAngle, Position const* startPos = nullptr) const;
    void GetNearPoint2D(float& x, float& y, float distance, float absAngle, Position const* startPos = nullptr) const;
    void GetNearPoint(WorldObject const* searcher, float& x, float& y, float& z, float searcher_size, float distance2d, float absAngle, float controlZ = 0, Position const* startPos = nullptr) const;
    void GetVoidClosePoint(float& x, float& y, float& z, float size, float distance2d = 0, float relAngle = 0, float controlZ = 0) const;
    bool GetClosePoint(float& x, float& y, float& z, float size, float distance2d = 0, float angle = 0, WorldObject const* forWho = nullptr, bool force = false) const;
    void MovePosition(Position& pos, float dist, float angle);
    Position GetNearPosition(float dist, float angle);
    void MovePositionToFirstCollision(Position& pos, float dist, float angle);
    Position GetFirstCollisionPosition(float startX, float startY, float startZ, float destX, float destY);
    Position GetFirstCollisionPosition(float destX, float destY, float destZ);
    Position GetFirstCollisionPosition(float dist, float angle);
    Position GetRandomNearPosition(float radius);

    void GetContactPoint(WorldObject const* obj, float& x, float& y, float& z, float distance2d = CONTACT_DISTANCE) const;
    void GetChargeContactPoint(WorldObject const* obj, float& x, float& y, float& z, float distance2d = CONTACT_DISTANCE) const;

    [[nodiscard]] float GetObjectSize() const;

    [[nodiscard]] virtual float GetCombatReach() const { return 0.0f; } // overridden (only) in Unit
    void UpdateGroundPositionZ(float x, float y, float& z) const;
    void UpdateAllowedPositionZ(float x, float y, float& z, float* groundZ = nullptr) const;

    void GetRandomPoint(Position const& srcPos, float distance, float& rand_x, float& rand_y, float& rand_z) const;
    [[nodiscard]] Position GetRandomPoint(Position const& srcPos, float distance) const;

    [[nodiscard]] uint32 GetInstanceId() const { return m_InstanceId; }

    virtual void SetPhaseMask(uint32 newPhaseMask, bool update);
    [[nodiscard]] uint32 GetPhaseMask() const { return m_phaseMask; }
    bool InSamePhase(WorldObject const* obj) const { return InSamePhase(obj->GetPhaseMask()); }
    [[nodiscard]] bool InSamePhase(uint32 phasemask) const { return m_useCombinedPhases ? GetPhaseMask() & phasemask : GetPhaseMask() == phasemask; }

    [[nodiscard]] uint32 GetZoneId() const;
    [[nodiscard]] uint32 GetAreaId() const;
    void GetZoneAndAreaId(uint32& zoneid, uint32& areaid) const;
    [[nodiscard]] bool IsOutdoors() const;
    [[nodiscard]] LiquidData const& GetLiquidData() const;

    [[nodiscard]] InstanceScript* GetInstanceScript() const;

    [[nodiscard]] std::string const& GetName() const { return m_name; }
    void SetName(std::string const& newname) { m_name = newname; }

    [[nodiscard]] virtual std::string const& GetNameForLocaleIdx(LocaleConstant /*locale_idx*/) const { return m_name; }

    float GetDistance(WorldObject const* obj) const;
    [[nodiscard]] float GetDistance(Position const& pos) const;
    [[nodiscard]] float GetDistance(float x, float y, float z) const;
    float GetDistance2d(WorldObject const* obj) const;
    [[nodiscard]] float GetDistance2d(float x, float y) const;
    float GetDistanceZ(WorldObject const* obj) const;

    bool IsSelfOrInSameMap(WorldObject const* obj) const;
    bool IsInMap(WorldObject const* obj) const;
    [[nodiscard]] bool IsWithinDist3d(float x, float y, float z, float dist) const;
    bool IsWithinDist3d(Position const* pos, float dist) const;
    [[nodiscard]] bool IsWithinDist2d(float x, float y, float dist) const;
    bool IsWithinDist2d(Position const* pos, float dist) const;
    virtual bool IsWithinSightRange(Position const& pos, float dist) const;
    // use only if you will sure about placing both object at same map
    bool IsWithinDist(WorldObject const* obj, float dist2compare, bool is3D = true, bool incOwnRadius = true, bool incTargetRadius = true) const;
    bool IsWithinDistInMap(WorldObject const* obj, float dist2compare, bool is3D = true, bool incOwnRadius = true, bool incTargetRadius = true) const;
    [[nodiscard]] bool IsWithinLOS(float x, float y, float z, VMAP::ModelIgnoreFlags ignoreFlags = VMAP::ModelIgnoreFlags::Nothing, LineOfSightChecks checks = LINEOFSIGHT_ALL_CHECKS) const;
    [[nodiscard]] bool IsWithinLOSInMap(WorldObject const* obj, VMAP::ModelIgnoreFlags ignoreFlags = VMAP::ModelIgnoreFlags::Nothing, LineOfSightChecks checks = LINEOFSIGHT_ALL_CHECKS, Optional<float> collisionHeight = { }, Optional<float> combatReach = { }) const;
    [[nodiscard]] Position GetHitSpherePointFor(Position const& dest, Optional<float> collisionHeight = { }, Optional<float> combatReach = { }) const;
    void GetHitSpherePointFor(Position const& dest, float& x, float& y, float& z, Optional<float> collisionHeight = { }, Optional<float> combatReach = { }) const;
    bool GetDistanceOrder(WorldObject const* obj1, WorldObject const* obj2, bool is3D = true) const;
    bool IsInRange(WorldObject const* obj, float minRange, float maxRange, bool is3D = true) const;
    [[nodiscard]] bool IsInRange2d(float x, float y, float minRange, float maxRange) const;
    [[nodiscard]] bool IsInRange3d(float x, float y, float z, float minRange, float maxRange) const;
    bool isInFront(WorldObject const* target, float arc = M_PI) const;
    bool isInBack(WorldObject const* target, float arc = M_PI) const;

    bool IsInBetween(WorldObject const* obj1, WorldObject const* obj2, float size = 0) const;

    virtual void CleanupsBeforeDelete(bool finalCleanup = true);  // used in destructor or explicitly before mass creature delete to remove cross-references to already deleted units

    virtual void SendMessageToSet(WorldPacket const* data, bool self) const;
    virtual void SendMessageToSetInRange(WorldPacket const* data, float dist, bool self) const;
    virtual void SendMessageToSet(WorldPacket const* data, Player const* skipped_rcvr) const;

    virtual uint8 getLevelForTarget(WorldObject const* /*target*/) const { return 1; }

    void PlayDistanceSound(uint32 sound_id, Player* target = nullptr);
    void PlayDirectSound(uint32 sound_id, Player* target = nullptr);
    void PlayRadiusSound(uint32 sound_id, float radius);
    void PlayDirectMusic(uint32 music_id, Player* target = nullptr);
    void PlayRadiusMusic(uint32 music_id, float radius);

    // Warning: Possible iterator invalidation in uses that may modify visibility map
    template<typename Worker>
    void DoForAllVisiblePlayers(Worker&& worker)
    {
        for (auto const& kvPair : GetObjectVisibilityContainer().GetVisiblePlayersMap())
            worker(kvPair.second);
    }

    // Warning: Possible iterator invalidation in uses that may modify visibility map
    template<typename Worker>
    void DoForAllVisibleWorldObjects(Worker&& worker)
    {
        // Not a player, no access to this map
        VisibleWorldObjectsMap const* visibleWorldObjectsMap = GetObjectVisibilityContainer().GetVisibleWorldObjectsMap();
        if (!visibleWorldObjectsMap)
            return;

        for (auto const& kvPair : *visibleWorldObjectsMap)
            worker(kvPair.second);
    }

    void DestroyForVisiblePlayers();

    void SendObjectDeSpawnAnim(ObjectGuid guid);

    virtual void SaveRespawnTime() {}
    void AddObjectToRemoveList();

    [[nodiscard]] float GetGridActivationRange() const;
    [[nodiscard]] float GetVisibilityRange() const;
    virtual float GetSightRange(WorldObject const* target = nullptr) const;

    [[nodiscard]] static float GetLeewayBonusRangeForTargets(Player const* player, Unit const* target);
    [[nodiscard]] float GetLeewayBonusRange(Unit const* target) const;
    [[nodiscard]] float GetLeewayBonusRadius() const;

    //bool CanSeeOrDetect(WorldObject const* obj, bool ignoreStealth = false, bool distanceCheck = false) const;
    bool CanSeeOrDetect(WorldObject const* obj, bool ignoreStealth = false, bool distanceCheck = false, bool checkAlert = false) const;
    // Step 10: the same decision with the target's state read from its published slot. Script hooks
    // (AI CanBeSeen, visibility conditions) still go through the handle.
    bool CanSeeOrDetect(ObjectSlot const& slot, WorldObject const* obj, bool ignoreStealth = false, bool distanceCheck = false, bool checkAlert = false) const;
    bool CanDetectInvisibilityOf(ObjectSlot const& slot, WorldObject const* obj) const;
    bool CanDetectStealthOf(ObjectSlot const& slot, WorldObject const* obj, bool checkAlert = false) const;

    FlaggedValuesArray32<int32, uint32, StealthType, TOTAL_STEALTH_TYPES> m_stealth;
    FlaggedValuesArray32<int32, uint32, StealthType, TOTAL_STEALTH_TYPES> m_stealthDetect;

    FlaggedValuesArray32<int32, uint32, InvisibilityType, TOTAL_INVISIBILITY_TYPES> m_invisibility;
    FlaggedValuesArray32<int32, uint32, InvisibilityType, TOTAL_INVISIBILITY_TYPES> m_invisibilityDetect;

    FlaggedValuesArray32<int32, uint32, ServerSideVisibilityType, TOTAL_SERVERSIDE_VISIBILITY_TYPES> m_serverSideVisibility;
    FlaggedValuesArray32<int32, uint32, ServerSideVisibilityType, TOTAL_SERVERSIDE_VISIBILITY_TYPES> m_serverSideVisibilityDetect;

    // Low Level Packets
    void SendPlayMusic(uint32 Music, bool OnlySelf);

    virtual void SetMap(Map* map);
    virtual void ResetMap();
    [[nodiscard]] Map* GetMap() const { ASSERT(m_currMap); return m_currMap; }
    [[nodiscard]] Map* FindMap() const { return m_currMap; }
    //used to check all object's GetMap() calls when object is not in world!

    void SetZoneScript();
    void ClearZoneScript();
    [[nodiscard]] ZoneScript* GetZoneScript() const { return m_zoneScript; }

    TempSummon* SummonCreature(uint32 id, Position const& pos, TempSummonType spwtype = TEMPSUMMON_MANUAL_DESPAWN, uint32 despwtime = 0, uint32 vehId = 0, SummonPropertiesEntry const* properties = nullptr, bool visibleBySummonerOnly = false) const;
    TempSummon* SummonCreature(uint32 id, float x, float y, float z, float ang = 0, TempSummonType spwtype = TEMPSUMMON_MANUAL_DESPAWN, uint32 despwtime = 0, SummonPropertiesEntry const* properties = nullptr, bool visibleBySummonerOnly = false);
    GameObject* SummonGameObject(uint32 entry, float x, float y, float z, float ang, float rotation0, float rotation1, float rotation2, float rotation3, uint32 respawnTime, bool checkTransport = true, GOSummonType summonType = GO_SUMMON_TIMED_OR_CORPSE_DESPAWN);
    Creature*   SummonTrigger(float x, float y, float z, float ang, uint32 dur, bool setLevel = false, CreatureAI * (*GetAI)(Creature*) = nullptr);
    void SummonCreatureGroup(uint8 group, std::list<TempSummon*>* list = nullptr);
    void SummonGameObjectGroup(uint8 group, std::list<GameObject*>* list = nullptr);

    [[nodiscard]] Creature*   FindNearestCreature(uint32 entry, float range, bool alive = true) const;
    [[nodiscard]] GameObject* FindNearestGameObject(uint32 entry, float range, bool onlySpawned = false) const;
    [[nodiscard]] GameObject* FindNearestGameObjectOfType(GameobjectTypes type, float range) const;

    [[nodiscard]] Player* SelectNearestPlayer(float distance = 0) const;
    void GetGameObjectListWithEntryInGrid(std::list<GameObject*>& lList, uint32 uiEntry, float fMaxSearchRange) const;
    void GetGameObjectListWithEntryInGrid(std::list<GameObject*>& gameobjectList, std::vector<uint32> const& entries, float maxSearchRange) const;
    void GetCreatureListWithEntryInGrid(std::list<Creature*>& lList, uint32 uiEntry, float fMaxSearchRange) const;
    void GetCreatureListWithEntryInGrid(std::list<Creature*>& creatureList, std::vector<uint32> const& entries, float maxSearchRange) const;
    void GetDeadCreatureListInGrid(std::list<Creature*>& lList, float maxSearchRange, bool alive = false) const;

    virtual void UpdateObjectVisibility(bool forced = true, bool fromUpdate = false);
    virtual void UpdateObjectVisibilityOnCreate() { UpdateObjectVisibility(true); }

    // Step 10: a viewer builds this object's blocks from a published slot on its own task.
    void BuildValuesUpdateFromSlot(ObjectSlot const& slot, UpdateData* data, Player* viewer, uint16 notifyFlags = 0);
    void BuildCreateUpdateBlockFromSlot(ObjectSlot const& slot, UpdateData* data, Player* viewer);
    void GetCreaturesWithEntryInRange(std::list<Creature*>& creatureList, float radius, uint32 entry);

    void SetPositionDataUpdate();
    void UpdatePositionData();
    [[nodiscard]] bool IsPositionDataUpdatePending() const { return _updatePositionData; }

    void AddToObjectUpdate() override;
    void RemoveFromObjectUpdate() override;

    //relocation and visibility system functions
    void AddToNotify(uint16 f);
    void RemoveFromNotify(uint16 f) { m_notifyflags &= ~f; }
    [[nodiscard]] bool isNeedNotify(uint16 f) const { return m_notifyflags & f;}
    [[nodiscard]] uint16 GetNotifyFlags() const { return m_notifyflags; }
    [[nodiscard]] bool NotifyExecuted(uint16 f) const { return m_executed_notifies & f;}
    void SetNotified(uint16 f) { m_executed_notifies |= f;}
    void ResetAllNotifies() { m_notifyflags = 0; m_executed_notifies = 0; }

    [[nodiscard]] bool isActiveObject() const { return m_isActive; }
    void setActive(bool isActiveObject);
    VisibilityDistanceType GetVisibilityOverrideType() const { return _visibilityDistanceOverrideType; }
    bool IsVisibilityOverridden() const { return _visibilityDistanceOverrideType > VisibilityDistanceType::Normal; }
    bool IsZoneWideVisible() const { return _visibilityDistanceOverrideType == VisibilityDistanceType::Infinite; }
    bool IsFarVisible() const { return _visibilityDistanceOverrideType == VisibilityDistanceType::Large || _visibilityDistanceOverrideType == VisibilityDistanceType::Gigantic; }
    float GetVisibilityOverrideDistance() const;
    void SetVisibilityDistanceOverride(VisibilityDistanceType type);

    [[nodiscard]] bool IsInWintergrasp() const
    {
        return GetMapId() == MAP_NORTHREND && GetPositionX() > 3733.33331f && GetPositionX() < 5866.66663f && GetPositionY() > 1599.99999f && GetPositionY() < 4799.99997f;
    }

    uint32  LastUsedScriptID;

    // Transports
    [[nodiscard]] Transport* GetTransport() const { return m_transport; }
    [[nodiscard]] float GetTransOffsetX() const { return m_movementInfo.transport.pos.GetPositionX(); }
    [[nodiscard]] float GetTransOffsetY() const { return m_movementInfo.transport.pos.GetPositionY(); }
    [[nodiscard]] float GetTransOffsetZ() const { return m_movementInfo.transport.pos.GetPositionZ(); }
    [[nodiscard]] float GetTransOffsetO() const { return m_movementInfo.transport.pos.GetOrientation(); }
    [[nodiscard]] uint32 GetTransTime()   const { return m_movementInfo.transport.time; }
    [[nodiscard]] int8 GetTransSeat()     const { return m_movementInfo.transport.seat; }
    [[nodiscard]] virtual ObjectGuid GetTransGUID()   const;
    void SetTransport(Transport* t) { m_transport = t; }

    MovementInfo m_movementInfo;

    [[nodiscard]] virtual float GetStationaryX() const { return GetPositionX(); }
    [[nodiscard]] virtual float GetStationaryY() const { return GetPositionY(); }
    [[nodiscard]] virtual float GetStationaryZ() const { return GetPositionZ(); }
    [[nodiscard]] virtual float GetStationaryO() const { return GetOrientation(); }

    [[nodiscard]] float GetMapWaterOrGroundLevel(Position pos, float* ground = nullptr) const
    {
        return GetMapWaterOrGroundLevel(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), ground);
    };
    [[nodiscard]] float GetMapWaterOrGroundLevel(float x, float y, float z, float* ground = nullptr) const;
    [[nodiscard]] float GetMapHeight(float x, float y, float z, bool vmap = true, float distanceToSearch = 50.0f) const; // DEFAULT_HEIGHT_SEARCH in map.h

    [[nodiscard]] float GetFloorZ() const;
    [[nodiscard]] float GetMinHeightInWater() const;

    [[nodiscard]] virtual float GetCollisionHeight() const { return 0.0f; }
    [[nodiscard]] virtual float GetCollisionWidth() const { return GetObjectSize(); }
    [[nodiscard]] virtual float GetCollisionRadius() const { return GetObjectSize() / 2; }

    void AddAllowedLooter(ObjectGuid guid);
    void ResetAllowedLooters();
    void SetAllowedLooters(GuidUnorderedSet const looters);
    [[nodiscard]] bool HasAllowedLooter(ObjectGuid guid) const;
    [[nodiscard]] GuidUnorderedSet const& GetAllowedLooters() const;
    void RemoveAllowedLooter(ObjectGuid guid);

    virtual bool IsUpdateNeeded();
    bool CanBeAddedToMapUpdateList();

    std::string GetDebugInfo() const override;

    ObjectVisibilityContainer& GetObjectVisibilityContainer() { return _objectVisibilityContainer; }
    ObjectVisibilityContainer const& GetObjectVisibilityContainer() const { return _objectVisibilityContainer; }

    // Event handler
    ALEEventProcessor* ALEEvents;
    EventProcessor m_Events;

    // CastSpell's third arg can be a variety of things, see CastSpellExtraArgs' constructors.
    SpellCastResult CastSpell(CastSpellTargetArg const& targets, uint32 spellId,
                              CastSpellExtraArgs const& args = {});
    SpellCastResult CastSpell(CastSpellTargetArg const& targets, SpellInfo const* info,
                              CastSpellExtraArgs const& args = {});

    // Ownership, faction and target validation at the WorldObject level so
    // non-Unit casters (GameObjects, DynamicObjects, Corpses) can resolve them.
    // Unit and GameObject override the virtuals with their field-based versions.
    [[nodiscard]] virtual ObjectGuid GetOwnerGUID() const { return ObjectGuid::Empty; }
    [[nodiscard]] virtual ObjectGuid GetCharmerOrOwnerGUID() const { return GetOwnerGUID(); }
    [[nodiscard]] Unit* GetOwnerUnit() const;
    [[nodiscard]] Unit* GetCharmerOrOwnerUnit() const;
    [[nodiscard]] Unit* GetCharmerOrOwnerOrSelfUnit() const;
    [[nodiscard]] Player* GetCharmerOrOwnerPlayerOrPlayerItself() const;
    [[nodiscard]] Player* GetAffectingPlayer() const;
    [[nodiscard]] virtual Player* GetSpellModOwner() const;

    [[nodiscard]] virtual uint32 GetFaction() const { return 0; }
    [[nodiscard]] FactionTemplateEntry const* GetFactionTemplateEntry() const;
    [[nodiscard]] ReputationRank GetReactionTo(WorldObject const* target) const;
    static ReputationRank GetFactionReactionTo(FactionTemplateEntry const* factionTemplateEntry, WorldObject const* target);
    [[nodiscard]] bool IsHostileTo(WorldObject const* target) const;
    [[nodiscard]] bool IsFriendlyTo(WorldObject const* target) const;

    [[nodiscard]] float GetSpellMaxRangeForTarget(Unit const* target, SpellInfo const* spellInfo) const;
    [[nodiscard]] float GetSpellMinRangeForTarget(Unit const* target, SpellInfo const* spellInfo) const;

    [[nodiscard]] bool IsValidAttackTarget(WorldObject const* target, SpellInfo const* bySpell = nullptr) const;
    [[nodiscard]] bool IsValidAssistTarget(WorldObject const* target, SpellInfo const* bySpell = nullptr) const;
    Unit* GetMagicHitRedirectTarget(Unit* victim, SpellInfo const* spellInfo);
    Unit* GetMeleeHitRedirectTarget(Unit* victim, SpellInfo const* spellInfo);

    // Spell calculations usable by any WorldObject caster. Unit-specific
    // bonuses apply when the caster is (or is owned by) a Unit.
    int32 CalculateSpellDamage(Unit const* target, SpellInfo const* spellProto, uint8 effect_index, int32 const* basePoints = nullptr) const;
    int32 CalcSpellDuration(SpellInfo const* spellProto) const;
    int32 ModSpellDuration(SpellInfo const* spellProto, Unit const* target, int32 duration, bool positive, uint32 effectMask) const;
    void ModSpellCastTime(SpellInfo const* spellProto, int32& castTime, Spell* spell = nullptr) const;

    [[nodiscard]] virtual SpellMissInfo MeleeSpellHitResult(Unit* /*victim*/, SpellInfo const* /*spellInfo*/) { return SPELL_MISS_NONE; }
    SpellMissInfo MagicSpellHitResult(Unit* victim, SpellInfo const* spellInfo);
    SpellMissInfo SpellHitResult(Unit* victim, SpellInfo const* spellInfo, bool canReflect = false);
    SpellMissInfo SpellHitResult(Unit* victim, Spell const* spell, bool canReflect = false);
    void SendSpellMiss(Unit* target, uint32 spellID, SpellMissInfo missInfo);
    void SendSpellNonMeleeDamageLog(Unit* target, SpellInfo const* spellInfo, uint32 damage, SpellSchoolMask schoolMask, uint32 absorb, uint32 resist);

protected:
    std::string m_name;
    bool m_isActive;
    VisibilityDistanceType _visibilityDistanceOverrideType;
    ZoneScript* m_zoneScript;

    virtual void ProcessPositionDataChanged(PositionFullTerrainStatus const& data);
    uint32 _zoneId;
    uint32 _areaId;
    float _floorZ;
    bool _outdoors;
    LiquidData _liquidData;
    bool _updatePositionData;

    // transports
    Transport* m_transport;

    //these functions are used mostly for Relocate() and Corpse/Player specific stuff...
    //use them ONLY in LoadFromDB()/Create() funcs and nowhere else!
    //mapId/instanceId should be set in SetMap() function!
    void SetLocationMapId(uint32 _mapId) { m_mapId = _mapId; }
    void SetLocationInstanceId(uint32 _instanceId) { m_InstanceId = _instanceId; }

    void _OnValuesInitialized() override;
    // Unit and Player add their parts of the slot. The WorldObject part is filled by PublishSlot.
    virtual void FillSlot(ObjectSlot& /*slot*/) { }
    ObjectSlotPair _slots;

    // Applies the sorted records of one beat. The base handles the kinds every WorldObject receives
    // (visibility links, step 10) and drops the rest.
    virtual void ApplyRecords(std::vector<RecordHeader const*> const& records);
    // Kinds any WorldObject can receive. Returns false when the kind is not one of them.
    bool ApplyCommonRecord(RecordHeader const& header, uint8 const* payload);

    // Set by PrepareRemoval. Never cleared: the object is a dead handle until the seal frees it.
    bool _removing{false};

    [[nodiscard]] virtual bool IsNeverVisible() const { return !IsInWorld(); }
    virtual bool IsAlwaysVisibleFor(WorldObject const* /*seer*/) const { return false; }
    [[nodiscard]] virtual bool IsInvisibleDueToDespawn() const { return false; }
    //difference from IsAlwaysVisibleFor: 1. after distance check; 2. use owner or charmer as seer
    virtual bool IsAlwaysDetectableFor(WorldObject const* /*seer*/) const { return false; }
private:
    Map* m_currMap;                                    //current object's Map location
    Milliseconds _heartbeatTimer;
    //uint32 m_mapId;                                     // object at map with map_id
    uint32 m_InstanceId;                                // in map copy with instance id
    uint32 m_phaseMask;                                 // in area phase state
    bool m_useCombinedPhases;                           // true (default): use phaseMask as bit mask combining up to 32 phases
    // false: use phaseMask to represent single phases only (up to 4294967295 phases)

    uint16 m_notifyflags;
    uint16 m_executed_notifies;

    virtual bool _IsWithinDist(WorldObject const* obj, float dist2compare, bool is3D, bool incOwnRadius = true, bool incTargetRadius = true) const;

    bool CanNeverSee(WorldObject const* obj) const;
    virtual bool CanAlwaysSee(WorldObject const* /*obj*/) const { return false; }
    //bool CanDetect(WorldObject const* obj, bool ignoreStealth, bool checkClient) const;
    bool CanDetect(WorldObject const* obj, bool ignoreStealth, bool checkClient, bool checkAlert = false) const;
    bool CanDetectInvisibilityOf(WorldObject const* obj) const;
    //bool CanDetectStealthOf(WorldObject const* obj) const;
    bool CanDetectStealthOf(WorldObject const* obj, bool checkAlert = false) const;

    void RemoveFromMapVisibilityOverrideContainers();
    void AddToMapVisibilityOverrideContainers();

    GuidUnorderedSet _allowedLooters;

    ObjectVisibilityContainer _objectVisibilityContainer;

    // Written only at the seal (step 12). nullptr means this object is its own task root.
    WorldObject* _taskRoot{nullptr};
    // Non-empty on a root with members (a vehicle's seated units, a charmer's charmed unit).
    // Owner (step 16 table): the root, by value; allocated on the first TaskGroupJoin, never shrunk (seats <= 8),
    // freed with the object.
    std::vector<WorldObject*> _taskMembers;
    // Grid the S1a router tagged this object for. -1 unless it is in that grid's pending list.
    int32 _pendingInGrid{-1};
    // Beat this root is scheduled for (BeatPool::Schedule CAS); 0 when not in any ring.
    std::atomic<uint32> _scheduledBeat{0};
    // Step 14: set by the core that popped this root's item for the duration of its task body. A duplicate
    // handle that loses the exchange is dropped (second line of defence behind _scheduledBeat).
    alignas(8) std::atomic<uint8> _claimed{0};
    bool TryClaim()
    {
        uint8 expected = 0;
        return _claimed.compare_exchange_strong(expected, 1, std::memory_order_acquire, std::memory_order_relaxed);
    }
    void Unclaim() { _claimed.store(0, std::memory_order_release); }
    // Duration of this root's last task body, for the heavy ring.
    uint32 _lastTaskNs{0};

    // Bit per core that appended a record to this object in the current beat. Written only by
    // OnFirstRecordOfBeat (fetch_or) and ApplyInbox (exchange to 0).
    std::atomic<uint64> _writtenBy{0};

    friend struct BeatContext;
    friend class BeatPool;
};

namespace Acore
{
    // Binary predicate to sort WorldObjects based on the distance to a reference WorldObject
    class ObjectDistanceOrderPred
    {
    public:
        ObjectDistanceOrderPred(WorldObject const* pRefObj, bool ascending = true) : m_refObj(pRefObj), m_ascending(ascending) {}
        bool operator()(WorldObject const* pLeft, WorldObject const* pRight) const
        {
            return m_ascending ? m_refObj->GetDistanceOrder(pLeft, pRight) : !m_refObj->GetDistanceOrder(pLeft, pRight);
        }
    private:
        WorldObject const* m_refObj;
        const bool m_ascending;
    };
}

#endif
