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

#ifndef _MAPTREE_H
#define _MAPTREE_H

#include "BoundingIntervalHierarchy.h"
#include "Define.h"
#include "Errors.h"
#include <atomic>
#include <unordered_map>

namespace VMAP
{
    // Step 14: the vmap tree has no read lock. Tiles install only inside a map seal task, when no unit task
    // runs, so a reader on the query path takes nothing (a shared_lock or fetch_add there would be one cache
    // line bouncing between every core doing line of sight). The server installs a check that says whether
    // the running thread may write (seal task, or no beat running); it is consulted by the debug guards only.
    using GeometryWriteCheck = bool (*)();
    void SetGeometryWriteCheck(GeometryWriteCheck check);
    bool GeometryWriteAllowed();

    // Debug-only RAII around a tile load / unload: flags the tree as writing so a concurrent reader asserts.
    struct GeometryWriteScope
    {
        std::atomic<bool>& flag;
        explicit GeometryWriteScope(std::atomic<bool>& f) : flag(f)
        {
            ASSERT(GeometryWriteAllowed(), "geometry written outside a seal task while a beat runs");
            flag.store(true, std::memory_order_relaxed);
        }
        ~GeometryWriteScope() { flag.store(false, std::memory_order_relaxed); }
    };
}

#if defined(ACORE_DEBUG)
#define GEOMETRY_READ_GUARD(flag) ASSERT(!(flag).load(std::memory_order_relaxed), "geometry read during a tile load")
#define GEOMETRY_WRITE_SCOPE(flag) VMAP::GeometryWriteScope _geometryWrite(flag)
#else
#define GEOMETRY_READ_GUARD(flag) ((void)0)
#define GEOMETRY_WRITE_SCOPE(flag) ((void)0)
#endif

namespace VMAP
{
    class ModelInstance;
    class GroupModel;
    class VMapMgr2;
    enum class ModelIgnoreFlags : uint32;
    enum class LoadResult : uint8;

    struct GroupLocationInfo
    {
        GroupModel const* hitModel = nullptr;
        int32 rootId = -1;
    };

    struct LocationInfo
    {
        LocationInfo():  ground_Z(-G3D::inf()) { }
        ModelInstance const* hitInstance{nullptr};
        GroupModel const* hitModel{nullptr};
        float ground_Z;
        int32 rootId = -1;
    };

    // Plain-value copy of a location query, filled under the tree's read lock.
    // Holds no pointer into the tree, so it stays valid after the lock is released.
    struct LocationQueryResult
    {
        bool hit = false;
        float groundZ = -G3D::inf();
        bool hasLiquid = false;
        uint32 liquidType = 0;
        float liquidLevel = 0.0f;
        uint32 wmoId = 0;
        int32 adtId = 0;
        int32 rootId = -1;
        uint32 mogpFlags = 0;
        uint32 modelInstanceId = 0;
    };

    class StaticMapTree
    {
        typedef std::unordered_map<uint32, bool> loadedTileMap;
        typedef std::unordered_map<uint32, uint32> loadedSpawnMap;
    private:
        uint32 iMapID;
        bool iIsTiled;
        BIH iTree;
        ModelInstance* iTreeValues; // the tree entries
        uint32 iNTreeValues;

        // Store all the map tile idents that are loaded for that map
        // some maps are not splitted into tiles and we have to make sure, not removing the map before all tiles are removed
        // empty tiles have no tile file, hence map with bool instead of just a set (consistency check)
        loadedTileMap iLoadedTiles;
        std::string iBasePath;

        // Set while a tile load / unload edits iTree / iTreeValues / iLoadedTiles (debug guard only; writers
        // run inside a seal task, when no reader can). Two instances of one map id serialise their tile loads
        // on MapGridManager::_gridLock of the parent.
        mutable std::atomic<bool> _writing{false};

    private:
        bool GetIntersectionTime(G3D::Ray const& pRay, float& pMaxDist, bool StopAtFirstHit, ModelIgnoreFlags ignoreFlags) const;
        bool GetLocationInfoUnlocked(G3D::Vector3 const& pos, LocationInfo& info) const;
        //bool containsLoadedMapTile(unsigned int pTileIdent) const { return(iLoadedMapTiles.containsKey(pTileIdent)); }
    public:
        static std::string getTileFileName(uint32 mapID, uint32 tileX, uint32 tileY);
        static uint32 packTileID(uint32 tileX, uint32 tileY) { return tileX << 16 | tileY; }
        static void unpackTileID(uint32 ID, uint32& tileX, uint32& tileY) { tileX = ID >> 16; tileY = ID & 0xFF; }
        static LoadResult CanLoadMap(std::string const& basePath, uint32 mapID, uint32 tileX, uint32 tileY);

        StaticMapTree(uint32 mapID, std::string const& basePath);
        ~StaticMapTree();

        [[nodiscard]] bool isInLineOfSight(G3D::Vector3 const& pos1, G3D::Vector3 const& pos2, ModelIgnoreFlags ignoreFlags) const;
        bool GetObjectHitPos(G3D::Vector3 const& pos1, G3D::Vector3 const& pos2, G3D::Vector3& pResultHitPos, float pModifyDist) const;
        [[nodiscard]] float getHeight(G3D::Vector3 const& pPos, float maxSearchDist) const;
        bool GetLocationInfo(G3D::Vector3 const& pos, LocationInfo& info) const;
        // Same query as GetLocationInfo but the answer is copied out as plain values;
        // use this when liquid or area data of the hit model is needed after return.
        [[nodiscard]] LocationQueryResult QueryLocation(G3D::Vector3 const& pos) const;

        bool InitMap(std::string const& fname);
        void UnloadMap();
        bool LoadMapTile(uint32 tileX, uint32 tileY);
        void UnloadMapTile(uint32 tileX, uint32 tileY);
        [[nodiscard]] bool isTiled() const { return iIsTiled; }
        [[nodiscard]] uint32 numLoadedTiles() const { return iLoadedTiles.size(); }
        void GetModelInstances(ModelInstance*& models, uint32& count);
    };

    struct AreaInfo
    {
        AreaInfo():  ground_Z(-G3D::inf()) { }
        bool result{false};
        float ground_Z;
        uint32 flags{0};
        int32 adtId{0};
        int32 rootId{0};
        int32 groupId{0};
    };
}                                                           // VMAP

#endif // _MAPTREE_H
