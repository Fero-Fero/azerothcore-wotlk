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

#include "MapPhaseTiming.h"
#include "Log.h"
#include "Map.h"
#include "Timer.h"

namespace
{
    constexpr uint32 PHASE_TIMING_WALL_FLOOR_MS = 20;
    constexpr uint32 PHASE_TIMING_LOG_INTERVAL_MS = 5000;
    constexpr uint32 PHASE_TIMING_MISMATCH_MS = 1;

    inline uint64 ElapsedUs(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to)
    {
        return uint64(std::chrono::duration_cast<std::chrono::microseconds>(to - from).count());
    }

    inline uint32 RoundToMs(uint64 us)
    {
        return uint32((us + 500) / 1000);
    }
}

void MapPhaseTiming::Add(MapUpdatePhase phase, uint32 ms)
{
    uint8 const index = uint8(phase);
    lastMs[index] = ms;
    windowSumMs[index] += ms;
}

void MapPhaseTiming::MaybeLog(Map const& map, uint32 wallMs)
{
    if (wallMs < PHASE_TIMING_WALL_FLOOR_MS)
        return;

    ++windowSamples;

    uint32 const nowMs = getMSTime();
    if (getMSTimeDiff(lastLogMs, nowMs) < PHASE_TIMING_LOG_INTERVAL_MS)
        return;

    uint32 avg[uint8(MapUpdatePhase::Count)] = {};
    uint32 sum = 0;
    for (uint8 i = 0; i < uint8(MapUpdatePhase::Count); ++i)
    {
        avg[i] = windowSumMs[i] / windowSamples;
        sum += lastMs[i];
    }

    LOG_INFO("maps.timing", "map phase timing: map={} inst={} players={} updatable={} wall={} dyn={} sess={} rsp={} plr={} npc={} upd={} scr={} mov={} vis={} tail={}",
        map.GetId(), map.GetInstanceId(), map.GetPlayers().getSize(), map.GetUpdatableObjectCount(), wallMs,
        avg[uint8(MapUpdatePhase::DynamicTree)], avg[uint8(MapUpdatePhase::Sessions)], avg[uint8(MapUpdatePhase::Respawn)],
        avg[uint8(MapUpdatePhase::Players)], avg[uint8(MapUpdatePhase::NonPlayers)], avg[uint8(MapUpdatePhase::ObjectUpdates)],
        avg[uint8(MapUpdatePhase::Scripts)], avg[uint8(MapUpdatePhase::Movement)], avg[uint8(MapUpdatePhase::Visibility)],
        avg[uint8(MapUpdatePhase::Tail)]);

    uint32 const diff = sum > wallMs ? sum - wallMs : wallMs - sum;
    if (diff > PHASE_TIMING_MISMATCH_MS)
        LOG_INFO("maps.timing", "map phase timing mismatch: map={} wall={} sum={}", map.GetId(), wallMs, sum);

    for (uint8 i = 0; i < uint8(MapUpdatePhase::Count); ++i)
        windowSumMs[i] = 0;
    windowSamples = 0;
    lastLogMs = nowMs;
}

MapPhaseClock::MapPhaseClock(MapPhaseTiming* sink, Map const& map)
    : _sink(sink), _map(&map)
{
    if (!_sink)
        return;

    _wallStart = std::chrono::steady_clock::now();
    _mark = _wallStart;
}

MapPhaseClock::~MapPhaseClock()
{
    if (!_sink || _abandoned)
        return;

    auto const now = std::chrono::steady_clock::now();
    Close(now);

    // Cumulative rounding: phase ms = round(end) - round(start), so the sum of
    // the ten values is exactly the rounded total of the phases.
    uint64 cumulativeUs = 0;
    uint32 cumulativeMs = 0;
    for (uint8 i = 0; i < uint8(MapUpdatePhase::Count); ++i)
    {
        cumulativeUs += _tickUs[i];
        uint32 const endMs = RoundToMs(cumulativeUs);
        _sink->Add(MapUpdatePhase(i), endMs - cumulativeMs);
        cumulativeMs = endMs;
    }

    _sink->MaybeLog(*_map, RoundToMs(ElapsedUs(_wallStart, now)));
}

void MapPhaseClock::Next(MapUpdatePhase phase)
{
    if (!_sink)
        return;

    auto const now = std::chrono::steady_clock::now();
    Close(now);
    _phase = phase;
    _open = true;
    _mark = now;
}

void MapPhaseClock::Abandon()
{
    _abandoned = true;
}

void MapPhaseClock::Close(std::chrono::steady_clock::time_point now)
{
    if (!_open)
        return;

    _tickUs[uint8(_phase)] += ElapsedUs(_mark, now);
    _open = false;
}
