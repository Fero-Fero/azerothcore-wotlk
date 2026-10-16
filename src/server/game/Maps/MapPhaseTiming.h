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

#ifndef ACORE_MAP_PHASE_TIMING_H
#define ACORE_MAP_PHASE_TIMING_H

#include "Define.h"
#include <chrono>

class Map;

// Baseline measurement of the single-threaded Map::Update. Enabled by
// MapUpdate.PhaseTiming; off by default and changes nothing else.
enum class MapUpdatePhase : uint8
{
    DynamicTree = 0,
    Sessions,
    Respawn,
    Players,
    NonPlayers,
    ObjectUpdates,
    Scripts,
    Movement,
    Visibility,
    Tail,
    Count
};

struct MapPhaseTiming
{
    uint32 lastMs[uint8(MapUpdatePhase::Count)] = {};
    uint32 windowSumMs[uint8(MapUpdatePhase::Count)] = {};
    uint32 windowSamples = 0;
    uint32 lastLogMs = 0;

    void Add(MapUpdatePhase phase, uint32 ms);
    void MaybeLog(Map const& map, uint32 wallMs);
};

class MapPhaseClock
{
public:
    MapPhaseClock(MapPhaseTiming* sink, Map const& map);
    MapPhaseClock(MapPhaseClock const&) = delete;
    MapPhaseClock& operator=(MapPhaseClock const&) = delete;
    ~MapPhaseClock();

    // Closes the open phase into _tickUs and opens `phase`.
    void Next(MapUpdatePhase phase);
    // Discards this tick; the destructor then records and logs nothing.
    void Abandon();

private:
    void Close(std::chrono::steady_clock::time_point now);

    MapPhaseTiming* _sink;
    Map const* _map;
    MapUpdatePhase _phase = MapUpdatePhase::DynamicTree;
    bool _open = false;
    bool _abandoned = false;
    // Microseconds per phase; converted to ms with cumulative rounding in the
    // destructor so the ten values sum to the rounded wall time exactly.
    uint64 _tickUs[uint8(MapUpdatePhase::Count)] = {};
    std::chrono::steady_clock::time_point _mark;
    std::chrono::steady_clock::time_point _wallStart;
};

#endif
