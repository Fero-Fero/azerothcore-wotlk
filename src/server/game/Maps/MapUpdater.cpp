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

#include "MapUpdater.h"
#include "BeatContext.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"

class UpdateRequest
{
public:
    UpdateRequest() = default;
    virtual ~UpdateRequest() = default;

    virtual void call() = 0;
};

class MapPreloadRequest : public UpdateRequest
{
public:
    MapPreloadRequest(uint32 mapId, MapUpdater& updater)
        : _mapId(mapId), _updater(updater)
    {
    }

    void call() override
    {
        // The map was created on the loading thread (respawn and corpse queries are synchronous); the worker
        // only loads the grids (step 11).
        if (Map* map = sMapMgr->FindBaseMap(_mapId))
        {
            LOG_INFO("server.loading", ">> Loading All Grids For Map {} ({})", map->GetId(), map->GetMapName());
            map->LoadAllGrids();
        }
        else
            LOG_ERROR("server.loading", "MapPreloadRequest: base map {} was not created before the preload", _mapId);

        _updater.update_finished();
    }

private:
    uint32 _mapId;
    MapUpdater& _updater;
};

MapUpdater::MapUpdater() : pending_requests(0), _cancelationToken(false)
{
}

void MapUpdater::activate(std::size_t num_threads)
{
    _workerThreads.reserve(num_threads);
    for (std::size_t i = 0; i < num_threads; ++i)
    {
        _workerThreads.push_back(std::thread(&MapUpdater::WorkerThread, this, uint8(i + 1)));
    }
}

void MapUpdater::deactivate()
{
    _cancelationToken = true;

    wait();  // This is where we wait for tasks to complete

    _queue.Cancel();  // Cancel the queue to prevent further task processing

    // Join all worker threads
    for (auto& thread : _workerThreads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
}

void MapUpdater::wait()
{
    std::unique_lock<std::mutex> guard(_lock);  // Guard lock for safe waiting

    // Wait until there are no pending requests
    _condition.wait(guard, [this] {
        return pending_requests.load(std::memory_order_acquire) == 0;
    });
}

void MapUpdater::schedule_task(UpdateRequest* request)
{
    // Atomic increment for pending_requests
    pending_requests.fetch_add(1, std::memory_order_release);
    _queue.Push(request);
}

void MapUpdater::schedule_map_preload(uint32 mapid)
{
    schedule_task(new MapPreloadRequest(mapid, *this));
}

bool MapUpdater::activated()
{
    return !_workerThreads.empty();
}

void MapUpdater::update_finished()
{
    // Atomic decrement for pending_requests
    if (pending_requests.fetch_sub(1, std::memory_order_acquire) == 1)
    {
        // Only notify when pending_requests becomes 0 (i.e., all tasks are finished)
        std::lock_guard<std::mutex> lock(_lock);  // Lock only for condition variable notification
        _condition.notify_all();  // Notify waiting threads that all requests are complete
    }
}

void MapUpdater::WorkerThread(uint8 coreIndex)
{
    BeatContext::core = coreIndex;

    LoginDatabase.WarnAboutSyncQueries(true);
    CharacterDatabase.WarnAboutSyncQueries(true);
    WorldDatabase.WarnAboutSyncQueries(true);

    while (!_cancelationToken)
    {
        UpdateRequest* request = nullptr;

        _queue.WaitAndPop(request);  // Wait for and pop a request from the queue

        if (!_cancelationToken && request)
        {
            request->call();  // Execute the request
            delete request;  // Clean up after processing
        }
    }
}
