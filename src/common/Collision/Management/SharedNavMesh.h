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
#include "Errors.h"
#include "MapTree.h"
#include <atomic>
#include <memory>
#include <utility>

// One holder per map id for the dtNavMesh shared by every instance of that map.
//
// Step 14: readers (path searches, GM inspection) take nothing. Tiles install only inside a map seal task,
// when no unit task runs, so a writer never meets a reader; a counter or lock on the query path would be one
// cache line bouncing between every core that paths. In debug builds NavMeshQueryScope still counts the
// in-flight queries so Write can assert that there are none instead of waiting for them.
class SharedNavMesh
{
public:
    explicit SharedNavMesh(std::shared_ptr<dtNavMesh> mesh) : _mesh(std::move(mesh)) { }

    SharedNavMesh(SharedNavMesh const&) = delete;
    SharedNavMesh& operator=(SharedNavMesh const&) = delete;

    // Read side. Pair every EnterQuery with LeaveQuery; prefer NavMeshQueryScope.
    dtNavMesh const* EnterQuery() const
    {
#if defined(ACORE_DEBUG)
        _queries.fetch_add(1, std::memory_order_relaxed);
#endif
        return _mesh.get();
    }

    void LeaveQuery() const
    {
#if defined(ACORE_DEBUG)
        _queries.fetch_sub(1, std::memory_order_relaxed);
#endif
    }

    // Write side: legal in a seal task or outside a beat only, with no query in flight; then runs fn(dtNavMesh*).
    template <typename F>
    auto Write(F&& fn)
    {
        ASSERT(VMAP::GeometryWriteAllowed(), "nav mesh written outside a seal task while a beat runs");
#if defined(ACORE_DEBUG)
        ASSERT(_queries.load(std::memory_order_relaxed) == 0, "nav mesh written with {} queries in flight",
            _queries.load(std::memory_order_relaxed));
#endif
        return std::forward<F>(fn)(_mesh.get());
    }

    // The raw mesh. Safe wherever a query is: tiles only install in a seal task.
    dtNavMesh const* UnsafeGet() const { return _mesh.get(); }

private:
    std::shared_ptr<dtNavMesh> _mesh;
#if defined(ACORE_DEBUG)
    mutable std::atomic<uint32> _queries{0};
#endif
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
