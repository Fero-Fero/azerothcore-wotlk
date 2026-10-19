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

#include "MapMgr.h"
#include "BeatContext.h"
#include "BeatPool.h"
#include "MapTree.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "GridDefines.h"
#include "GridTerrainLoader.h"
#include "Group.h"
#include "InstanceSaveMgr.h"
#include "LFGMgr.h"
#include "Language.h"
#include "Log.h"
#include "MapInstanced.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "TC9Sidecar.h"
#include "Transport.h"
#include "UnitRecordLog.h"
#include "World.h"
#include "WorldPacket.h"
#include <algorithm>

MapMgr::MapMgr()
{
    SetMapUpdateInterval(sWorld->getIntConfig(CONFIG_INTERVAL_MAPUPDATE));
    _nextInstanceId = 0;
}

MapMgr::~MapMgr()
{
}

MapMgr* MapMgr::instance()
{
    static MapMgr instance;
    return &instance;
}

void MapMgr::Initialize()
{
    int num_threads(sWorld->getIntConfig(CONFIG_NUMTHREADS));

    // Step 14: the pool has MapUpdate.PhaseThreads cores (the world thread is core 0). The record logs are
    // indexed by BeatContext::core, which the startup grid preload threads (MapUpdate.Threads) also carry, so
    // they are sized for whichever set is larger; preloads never overlap a beat.
    uint32 const key = std::clamp<uint32>(sWorld->getIntConfig(CONFIG_MAPUPDATE_PHASE_THREADS), 1, 64);
    uint8 const cores = uint8(std::max<uint32>(key, std::clamp(num_threads, 0, 63) + 1));
    sRecordLogs->Init(cores);
    sRecordLogs->BeginBeat(BeatContext::beat.load(std::memory_order_relaxed));

    sBeatPool->Init(uint8(key));

    // Writes to the shared vmap tree / nav mesh (tile installs) are legal in a seal task or outside a beat.
    VMAP::SetGeometryWriteCheck([]() { return BeatContext::InSealTask() || !BeatContext::InBeat(); });

    // The updater threads only serve the grid preloads at startup.
    if (num_threads > 0)
        m_updater.activate(num_threads);
}

void MapMgr::StartBeatPool()
{
    // After the world loaded and the grid preloads finished: the workers exist from here to UnloadAll.
    if (m_updater.activated())
        m_updater.wait();
    sBeatPool->Start();
}

void MapMgr::InitializeVisibilityDistanceInfo()
{
    for (MapMapType::iterator iter = i_maps.begin(); iter != i_maps.end(); ++iter)
        (*iter).second->InitVisibilityDistance();
}

Map* MapMgr::CreateBaseMap(uint32 id)
{
    Map* map = FindBaseMap(id);

    if (!map)
    {
        std::lock_guard<std::mutex> guard(Lock);

        map = FindBaseMap(id);
        if (!map) // pussywizard: check again after acquiring mutex
        {
            MapEntry const* entry = sMapStore.LookupEntry(id);
            ASSERT(entry);

            if (entry->Instanceable())
                map = new MapInstanced(id);
            else
                map = new Map(id, 0, REGULAR_DIFFICULTY);

            i_maps[id] = map;

            if (!entry->Instanceable())
            {
                map->LoadRespawnTimes();
                map->LoadCorpseData();
            }

            map->OnCreateMap();
        }
    }

    ASSERT(map);
    return map;
}

Map* MapMgr::FindBaseNonInstanceMap(uint32 mapId) const
{
    Map* map = FindBaseMap(mapId);
    if (map && map->Instanceable())
        return nullptr;
    return map;
}

Map* MapMgr::CreateMap(uint32 id, Player* player)
{
    ASSERT_NOT_IN_TASK();
    Map* m = CreateBaseMap(id);

    if (m && m->Instanceable())
        m = ((MapInstanced*)m)->CreateInstanceForPlayer(id, player);

    return m;
}

Map* MapMgr::FindMap(uint32 mapid, uint32 instanceId) const
{
    Map* map = FindBaseMap(mapid);
    if (!map)
        return nullptr;

    if (!map->Instanceable())
        return instanceId == 0 ? map : nullptr;

    return ((MapInstanced*)map)->FindInstanceMap(instanceId);
}

Map::EnterState MapMgr::PlayerCannotEnter(uint32 mapid, Player* player, bool loginCheck)
{
    MapEntry const* entry = sMapStore.LookupEntry(mapid);
    if (!entry)
        return Map::CANNOT_ENTER_NO_ENTRY;

    if (!entry->IsDungeon())
        return Map::CAN_ENTER;

    InstanceTemplate const* instance = sObjectMgr->GetInstanceTemplate(mapid);
    if (!instance)
        return Map::CANNOT_ENTER_UNINSTANCED_DUNGEON;

    Difficulty targetDifficulty, requestedDifficulty;
    targetDifficulty = requestedDifficulty = player->GetDifficulty(entry->IsRaid());
    // Get the highest available difficulty if current setting is higher than the instance allows
    MapDifficulty const* mapDiff = GetDownscaledMapDifficultyData(entry->MapID, targetDifficulty);
    if (!mapDiff)
    {
        player->SendTransferAborted(mapid, TRANSFER_ABORT_DIFFICULTY, requestedDifficulty);
        return Map::CANNOT_ENTER_DIFFICULTY_UNAVAILABLE;
    }

    //Bypass checks for GMs
    if (player->IsGameMaster())
        return Map::CAN_ENTER;

    char const* mapName = entry->name[player->GetSession()->GetSessionDbcLocale()];

    if (!sScriptMgr->OnPlayerCanEnterMap(player, entry, instance, mapDiff, loginCheck))
        return Map::CANNOT_ENTER_UNSPECIFIED_REASON;

    Group* group = player->GetGroup();
    if (entry->IsRaid())
    {
        // can only enter in a raid group
        if ((!group || !group->isRaidGroup()) && !sWorld->getBoolConfig(CONFIG_INSTANCE_IGNORE_RAID))
        {
            // probably there must be special opcode, because client has this string constant in GlobalStrings.lua
            /// @todo: this is not a good place to send the message
            player->GetSession()->SendAreaTriggerMessage(LANG_INSTANCE_RAID_GROUP_ONLY, mapName);
            LOG_DEBUG("maps", "MAP: Player '{}' must be in a raid group to enter instance '{}'", player->GetName(), mapName);
            return Map::CANNOT_ENTER_NOT_IN_RAID;
        }
    }

    // xinef: dont allow LFG Group to enter other instance that is selected
    if (group)
        if (group->isLFGGroup())
            if (!sLFGMgr->inLfgDungeonMap(group->GetGUID(), mapid, targetDifficulty))
            {
                player->SendTransferAborted(mapid, TRANSFER_ABORT_MAP_NOT_ALLOWED);
                return Map::CANNOT_ENTER_UNSPECIFIED_REASON;
            }

    if (!player->IsAlive())
    {
        if (player->HasCorpse())
        {
            // let enter in ghost mode in instance that connected to inner instance with corpse
            uint32 corpseMap = player->GetCorpseLocation().GetMapId();
            do
            {
                if (corpseMap == mapid)
                    break;

                InstanceTemplate const* corpseInstance = sObjectMgr->GetInstanceTemplate(corpseMap);
                corpseMap = corpseInstance ? corpseInstance->Parent : 0;
            } while (corpseMap);

            if (!corpseMap)
            {
                WorldPacket data(SMSG_CORPSE_NOT_IN_INSTANCE, 0);
                player->SendDirectMessage(&data);
                LOG_DEBUG("maps", "MAP: Player '{}' does not have a corpse in instance '{}' and cannot enter.", player->GetName(), mapName);
                return Map::CANNOT_ENTER_CORPSE_IN_DIFFERENT_INSTANCE;
            }
            LOG_DEBUG("maps", "MAP: Player '{}' has corpse in instance '{}' and can enter.", player->GetName(), mapName);
        }
        else
        {
            LOG_DEBUG("maps", "Map::PlayerCannotEnter - player '{}' is dead but does not have a corpse!", player->GetName());
            return Map::CANNOT_ENTER_CORPSE_IN_DIFFERENT_INSTANCE;
        }
    }

    // if map exists - check for being full, etc.
    if (!loginCheck) // for login this is done by the calling function
    {
        uint32 destInstId = sInstanceSaveMgr->PlayerGetDestinationInstanceId(player, mapid, targetDifficulty);
        if (destInstId)
            if (Map* boundMap = FindMap(mapid, destInstId))
                if (Map::EnterState denyReason = boundMap->CannotEnter(player, loginCheck))
                    return denyReason;
    }

    // players are only allowed to enter 5 instances per hour
    if (entry->IsNonRaidDungeon() && (!group || !group->isLFGGroup() || !group->IsLfgRandomInstance()))
    {
        uint32 instaceIdToCheck = 0;
        if (InstanceSave* save = sInstanceSaveMgr->PlayerGetInstanceSave(player->GetGUID(), mapid, player->GetDifficulty(entry->IsRaid())))
            instaceIdToCheck = save->GetInstanceId();

        // instaceIdToCheck can be 0 if save not found - means no bind so the instance is new
        if (!player->CheckInstanceCount(instaceIdToCheck))
        {
            player->SendTransferAborted(mapid, TRANSFER_ABORT_TOO_MANY_INSTANCES);
            return Map::CANNOT_ENTER_TOO_MANY_INSTANCES;
        }
    }

    //Other requirements
    return player->Satisfy(sObjectMgr->GetAccessRequirement(mapid, targetDifficulty), mapid, true) ? Map::CAN_ENTER : Map::CANNOT_ENTER_UNSPECIFIED_REASON;
}

void MapMgr::Update(uint32 diff)
{
    // pussywizard: lfg compatibles update, before the maps so it is processed from the very beginning
    sLFGMgr->Update(diff, 1);

    // Step 12: one beat for every map once the map update interval elapsed. No per-type rotation, no
    // session-only ticks in between: a player's packets wait for its task like everything else.
    _beatAccum += diff;
    if (_beatAccum < _beatInterval)
        return;

    uint32 const beatDiff = _beatAccum;
    _beatAccum = 0;
    sBeatPool->RunBeat(beatDiff);
}

void MapMgr::CollectAllMaps(std::vector<Map*>& out)
{
    std::lock_guard<std::mutex> guard(Lock);

    for (auto const& mapPair : i_maps)
    {
        Map* map = mapPair.second;
        out.push_back(map);
        if (MapInstanced* mapInstanced = map->ToMapInstanced())
            for (auto const& instancePair : mapInstanced->GetInstancedMaps())
                out.push_back(instancePair.second);
    }
}

void MapMgr::DestroyExpiredInstances(uint32 diff)
{
    for (auto const& mapPair : i_maps)
        if (MapInstanced* mapInstanced = mapPair.second->ToMapInstanced())
            mapInstanced->DestroyExpiredInstances(diff);
}

void MapMgr::DoDelayedMovesAndRemoves()
{
}

bool MapMgr::ExistMapAndVMap(uint32 mapid, float x, float y)
{
    GridCoord p = Acore::ComputeGridCoord(x, y);
    return GridTerrainLoader::ExistMap(mapid, p.x_coord, p.y_coord) && GridTerrainLoader::ExistVMap(mapid, p.x_coord, p.y_coord);
}

bool MapMgr::IsValidMAP(uint32 mapid, bool startUp)
{
    MapEntry const* mEntry = sMapStore.LookupEntry(mapid);

    if (startUp)
    {
        return mEntry != nullptr;
    }
    else
    {
        return mEntry && (!mEntry->IsDungeon() || sObjectMgr->GetInstanceTemplate(mapid));
    }

    /// @todo: add check for battleground template
}

void MapMgr::UnloadAll()
{
    ASSERT_NOT_IN_TASK();
    // Workers first: the maps below are freed with no beat running and nothing to steal.
    sBeatPool->Stop();
    for (MapMapType::iterator iter = i_maps.begin(); iter != i_maps.end();)
    {
        iter->second->UnloadAll();
        delete iter->second;
        i_maps.erase(iter++);
    }

    if (m_updater.activated())
        m_updater.deactivate();

    // Step 16: the record logs' owner frees them once no map can append (an append after this re-initialises
    // a one-core set, as before Initialize).
    sRecordLogs->Shutdown();
}

void MapMgr::GetNumInstances(uint32& dungeons, uint32& battlegrounds, uint32& arenas)
{
    for (MapMapType::iterator itr = i_maps.begin(); itr != i_maps.end(); ++itr)
    {
        Map* map = itr->second;
        if (!map->Instanceable())
            continue;
        MapInstanced::InstancedMaps& maps = ((MapInstanced*)map)->GetInstancedMaps();
        for (MapInstanced::InstancedMaps::iterator mitr = maps.begin(); mitr != maps.end(); ++mitr)
        {
            if (mitr->second->IsDungeon()) dungeons++;
            else if (mitr->second->IsBattleground()) battlegrounds++;
            else if (mitr->second->IsBattleArena()) arenas++;
        }
    }
}

void MapMgr::GetNumPlayersInInstances(uint32& dungeons, uint32& battlegrounds, uint32& arenas, uint32& spectators)
{
    for (MapMapType::iterator itr = i_maps.begin(); itr != i_maps.end(); ++itr)
    {
        Map* map = itr->second;
        if (!map->Instanceable())
            continue;
        MapInstanced::InstancedMaps& maps = ((MapInstanced*)map)->GetInstancedMaps();
        for (MapInstanced::InstancedMaps::iterator mitr = maps.begin(); mitr != maps.end(); ++mitr)
        {
            if (mitr->second->IsDungeon()) dungeons += ((InstanceMap*)mitr->second)->GetPlayers().getSize();
            else if (mitr->second->IsBattleground()) battlegrounds += ((InstanceMap*)mitr->second)->GetPlayers().getSize();
            else if (mitr->second->IsBattleArena())
            {
                uint32 spect = 0;
                if (BattlegroundMap* bgmap = mitr->second->ToBattlegroundMap())
                    if (Battleground* bg = bgmap->GetBG())
                        spect = bg->GetSpectators().size();

                arenas += ((InstanceMap*)mitr->second)->GetPlayers().getSize() - spect;
                spectators += spect;
            }
        }
    }
}

void MapMgr::InitInstanceIds()
{
    _nextInstanceId = 1;

    QueryResult result = CharacterDatabase.Query("SELECT MAX(id) FROM instance");
    if (result)
    {
        uint32 maxId = (*result)[0].Get<uint32>();
        _instanceIds.resize(maxId + 1);
    }
}

void MapMgr::RegisterInstanceId(uint32 instanceId)
{
    // Allocation was done in InitInstanceIds()
    _instanceIds[instanceId] = true;

    // Instances are pulled in ascending order from db and _nextInstanceId is initialized with 1,
    // so if the instance id is used, increment
    if (_nextInstanceId == instanceId)
        ++_nextInstanceId;
}

uint32 MapMgr::GenerateInstanceId()
{
    if (sToCloud9Sidecar->ClusterModeEnabled())
        return sToCloud9Sidecar->GenerateInstanceGuid();

    uint32 newInstanceId = _nextInstanceId;

    // find the lowest available id starting from the current _nextInstanceId
    while (_nextInstanceId < 0xFFFFFFFF && ++_nextInstanceId < _instanceIds.size() && _instanceIds[_nextInstanceId]);

    if (_nextInstanceId == 0xFFFFFFFF)
    {
        LOG_ERROR("server.worldserver", "Instance ID overflow!! Can't continue, shutting down server. ");
        World::StopNow(ERROR_EXIT_CODE);
    }

    return newInstanceId;
}
