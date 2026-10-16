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

#ifndef ACORE_UNIT_RECORD_LOG_H
#define ACORE_UNIT_RECORD_LOG_H

#include "Define.h"
#include "UnitRecords.h"
#include <array>
#include <vector>

class WorldObject;

// Head of a target's chain inside one log. The offset is valid only if beat matches the beat being read.
struct TargetHead
{
    uint32 beat = ~0u;
    uint32 offset = RecordHeader::NO_RECORD;
};

// Append-only byte log of records written by one core in one beat. Resident: the storage is reserved once
// and grows only on spill, never per record. Heads are indexed by the target's dense slot index.
class UnitRecordLog
{
public:
    void Init(uint8 coreIndex, uint32 slotCapacity, std::size_t bytes);

    template <typename T>
    void Append(ObjectGuid source, WorldObject* target, RecordKind kind, T const& payload)
    {
        static_assert(std::is_trivially_copyable_v<T>, "record payloads are plain values");
        static_assert(sizeof(T) <= 0xFFFF, "record payload too large");
        AppendRaw(source, target, kind, &payload, sizeof(T));
    }

    // size = 0, sequence = 0. Heads stay: they are beat-tagged, a stale head is simply never matched.
    void Reset();

    [[nodiscard]] TargetHead Head(uint32 slotIndex) const
    {
        return slotIndex < _head.size() ? _head[slotIndex] : TargetHead();
    }

    [[nodiscard]] RecordHeader const* At(uint32 offset) const
    {
        return reinterpret_cast<RecordHeader const*>(_bytes.data() + offset);
    }

    // Grows the head array when the slot index space grows. Single-threaded on this step.
    void GrowHeads(uint32 slotCapacity);

    [[nodiscard]] std::size_t Size() const { return _size; }
    [[nodiscard]] std::size_t Capacity() const { return _bytes.size(); }
    [[nodiscard]] uint32 Count() const { return _sequence; }
    [[nodiscard]] uint32 Spills() const { return _spills; }

private:
    void AppendRaw(ObjectGuid source, WorldObject* target, RecordKind kind, void const* payload, std::size_t size);
    void Spill(std::size_t needed);

    uint8 _core = 0;
    std::vector<uint8> _bytes;        // sized once; _size is the write cursor
    std::size_t _size = 0;
    std::size_t _block = 0;           // initial reservation; a spill adds one more block
    uint32 _sequence = 0;
    uint32 _spills = 0;
    std::vector<TargetHead> _head;    // indexed by ObjectSlotPair::SlotIndex()
};

// One log per core and per beat parity. Records appended in beat k go to parity k and are read in
// beat k + 1 by the targets' inboxes; parity k is reset again at the start of beat k + 2.
class RecordLogSet
{
public:
    static RecordLogSet* instance();

    static constexpr std::size_t DEFAULT_BYTES_PER_LOG = 1024 * 1024;

    void Init(uint8 cores, std::size_t bytesPerLog = DEFAULT_BYTES_PER_LOG);
    [[nodiscard]] uint8 Cores() const { return _cores; }
    [[nodiscard]] bool IsInitialized() const { return _cores != 0; }

    // Called once per beat on the world thread after the beat counter advanced: resets this beat's write logs.
    void BeginBeat(uint32 beat);

    // The log this thread appends to in the current beat.
    UnitRecordLog& WriteLog();
    // The log a core wrote in the previous beat.
    [[nodiscard]] UnitRecordLog const& ReadLog(uint8 core) const;

    template <typename T>
    void Append(ObjectGuid source, WorldObject* target, RecordKind kind, T const& payload)
    {
        WriteLog().Append(source, target, kind, payload);
    }

    // Empty record: only forces a task on the target.
    void Wake(ObjectGuid source, WorldObject* target);

    // Sum over all logs of the current write parity.
    [[nodiscard]] uint32 PendingCount() const;

private:
    uint8 _cores = 0;
    std::array<std::vector<UnitRecordLog>, 2> _logs;
};

#define sRecordLogs RecordLogSet::instance()

#endif
