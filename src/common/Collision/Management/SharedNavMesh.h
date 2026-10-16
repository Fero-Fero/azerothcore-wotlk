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

#ifndef _SHARED_NAV_MESH_H
#define _SHARED_NAV_MESH_H

#include "Define.h"
#include "DetourNavMesh.h"
#include <atomic>
#include <memory>
#include <shared_mutex>
#include <thread>
#include <utility>

// One holder per map id for the dtNavMesh shared by every instance of that map.
//
// Readers (path searches, GM inspection) never touch the mutex: they bump an
// in-flight counter through NavMeshQueryScope. Writers (tile loads) take the
// mutex exclusively and then wait for the counter to drain before touching the
// mesh. Tile loads happen on grid creation only, so writer starvation is not a
// practical concern.
//
// Lock order: MapGridManager::_gridLock -> StaticMapTree::_mutex -> this.
// Nothing takes _gridLock while inside Write().
class SharedNavMesh
{
public:
    explicit SharedNavMesh(std::shared_ptr<dtNavMesh> mesh) : _mesh(std::move(mesh)) { }

    SharedNavMesh(SharedNavMesh const&) = delete;
    SharedNavMesh& operator=(SharedNavMesh const&) = delete;

    // Read side. Pair every EnterQuery with LeaveQuery; prefer NavMeshQueryScope.
    dtNavMesh const* EnterQuery() const
    {
        _queries.fetch_add(1, std::memory_order_acq_rel);
        return _mesh.get();
    }

    void LeaveQuery() const
    {
        _queries.fetch_sub(1, std::memory_order_release);
    }

    // Write side: exclusive lock, drain in-flight queries, then run fn(dtNavMesh*).
    template <typename F>
    auto Write(F&& fn)
    {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        while (_queries.load(std::memory_order_acquire) != 0)
            std::this_thread::yield();

        return std::forward<F>(fn)(_mesh.get());
    }

    // Unguarded access. Only for code that runs when no query can be in flight.
    dtNavMesh const* UnsafeGet() const { return _mesh.get(); }

private:
    std::shared_ptr<dtNavMesh> _mesh;
    mutable std::shared_mutex _mutex;
    mutable std::atomic<uint32> _queries{0};
};

// RAII around EnterQuery/LeaveQuery. `ptr` is valid for the lifetime of the scope only.
struct NavMeshQueryScope
{
    SharedNavMesh const& mesh;
    dtNavMesh const* ptr;

    explicit NavMeshQueryScope(SharedNavMesh const& m) : mesh(m), ptr(m.EnterQuery()) { }
    ~NavMeshQueryScope() { mesh.LeaveQuery(); }

    NavMeshQueryScope(NavMeshQueryScope const&) = delete;
    NavMeshQueryScope& operator=(NavMeshQueryScope const&) = delete;
};

#endif // _SHARED_NAV_MESH_H
