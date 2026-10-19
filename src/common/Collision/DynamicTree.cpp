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

#include "DynamicTree.h"
#include "BoundingIntervalHierarchyWrapper.h"
#include "GameObjectModel.h"
#include "MapTree.h"
#include "ModelIgnoreFlags.h"
#include "ModelInstance.h"
#include "RegularGrid.h"
#include "VMapFactory.h"
#include "VMapMgr2.h"
#include "WorldModel.h"

#include <G3D/AABox.h>
#include <G3D/Ray.h>
#include <G3D/Vector3.h>

using VMAP::ModelInstance;

namespace
{
    // Step 12 residue: a rebalance is ~1 ms and used to run on every changed model. Balance does not affect
    // query correctness, so wait for a batch of edits or 32 beats, whichever comes first.
    constexpr int REBALANCE_DIRTY_THRESHOLD = 16;
    constexpr int REBALANCE_MAX_BEATS = 32;
}

template<> struct HashTrait< GameObjectModel>
{
    static std::size_t hashCode(GameObjectModel const& g) { return (size_t)(void*)&g; }
};

template<> struct PositionTrait< GameObjectModel>
{
    static void GetPosition(GameObjectModel const& g, G3D::Vector3& p) { p = g.GetPosition(); }
};

template<> struct BoundsTrait< GameObjectModel>
{
    static void GetBounds(GameObjectModel const& g, G3D::AABox& out) { out = g.GetBounds();}
    static void GetBounds2(GameObjectModel const* g, G3D::AABox& out) { out = g->GetBounds();}
};

typedef RegularGrid2D<GameObjectModel, BIHWrap<GameObjectModel>> ParentTree;

struct DynTreeImpl : public ParentTree
{
    typedef GameObjectModel Model;
    typedef ParentTree base;

    DynTreeImpl() :
        _dirtyCount(0),
        _beatsSinceBalance(0)
    {
    }

    void insert(Model const& mdl)
    {
        base::insert(mdl);
        ++_dirtyCount;
    }

    void remove(Model const& mdl)
    {
        base::remove(mdl);
        ++_dirtyCount;
    }

    void balance()
    {
        base::balance();
        _dirtyCount = 0;
        _beatsSinceBalance = 0;
    }

    void update(uint32 /*difftime*/)
    {
        if (!size())
            return;

        if (_dirtyCount <= 0)
        {
            _beatsSinceBalance = 0;
            return;
        }

        // Called once per beat from Map::BeatEpilogue.
        ++_beatsSinceBalance;
        if (_dirtyCount >= REBALANCE_DIRTY_THRESHOLD || _beatsSinceBalance >= REBALANCE_MAX_BEATS)
            balance();
    }

    int _dirtyCount;
    int _beatsSinceBalance;
};

DynamicMapTree::DynamicMapTree() : impl(new DynTreeImpl()) { }

DynamicMapTree::~DynamicMapTree()
{
    delete impl;
}

void DynamicMapTree::insert(GameObjectModel const& mdl)
{
    impl->insert(mdl);
}

void DynamicMapTree::remove(GameObjectModel const& mdl)
{
    impl->remove(mdl);
}

bool DynamicMapTree::contains(GameObjectModel const& mdl) const
{
    return impl->contains(mdl);
}

void DynamicMapTree::balance()
{
    impl->balance();
}

int DynamicMapTree::size() const
{
    return impl->size();
}

void DynamicMapTree::update(uint32 t_diff)
{
    impl->update(t_diff);
}

struct DynamicTreeIntersectionCallback
{
    DynamicTreeIntersectionCallback(uint32 phasemask, VMAP::ModelIgnoreFlags ignoreFlags) :
        _didHit(false), _phaseMask(phasemask), _ignoreFlags(ignoreFlags) { }

    bool operator()(G3D::Ray const& r, GameObjectModel const& obj, float& distance, bool stopAtFirstHit)
    {
        bool result = obj.intersectRay(r, distance, stopAtFirstHit, _phaseMask, _ignoreFlags);
        if (result)
        {
            _didHit = result;
        }
        return result;
    }

    [[nodiscard]] bool didHit() const
    {
        return _didHit;
    }

private:
    bool _didHit;
    uint32 _phaseMask;
    VMAP::ModelIgnoreFlags _ignoreFlags;
};

struct DynamicTreeLocationInfoCallback
{
    DynamicTreeLocationInfoCallback(uint32 phaseMask)
        : _phaseMask(phaseMask), _hitModel(nullptr) {}

    void operator()(G3D::Vector3 const& p, GameObjectModel const& obj)
    {
        if (obj.GetLocationInfo(p, _locationInfo, _phaseMask))
            _hitModel = &obj;
    }

    VMAP::LocationInfo& GetLocationInfo()
    {
        return _locationInfo;
    }
    GameObjectModel const* GetHitModel() const
    {
        return _hitModel;
    }

private:
    uint32                 _phaseMask;
    VMAP::LocationInfo     _locationInfo;
    GameObjectModel const* _hitModel;
};

bool DynamicMapTree::GetIntersectionTime(const uint32 phasemask, G3D::Ray const& ray, G3D::Vector3 const& endPos, float& maxDist) const
{
    float distance = maxDist;
    DynamicTreeIntersectionCallback callback(phasemask, VMAP::ModelIgnoreFlags::Nothing);
    impl->intersectRay(ray, callback, distance, endPos, false);
    if (callback.didHit())
    {
        maxDist = distance;
    }
    return callback.didHit();
}

bool DynamicMapTree::GetObjectHitPos(const uint32 phasemask, G3D::Vector3 const& startPos,
                                     G3D::Vector3 const& endPos, G3D::Vector3& resultHit,
                                     float modifyDist) const
{
    bool result = false;
    float maxDist = (endPos - startPos).magnitude();
    // valid map coords should *never ever* produce float overflow, but this would produce NaNs too
    ASSERT(maxDist < std::numeric_limits<float>::max());
    // prevent NaN values which can cause BIH intersection to enter infinite loop
    if (maxDist < 1e-10f)
    {
        resultHit = endPos;
        return false;
    }
    G3D::Vector3 dir = (endPos - startPos) / maxDist;            // direction with length of 1
    G3D::Ray ray(startPos, dir);
    float dist = maxDist;
    if (GetIntersectionTime(phasemask, ray, endPos, dist))
    {
        resultHit = startPos + dir * dist;
        if (modifyDist < 0)
        {
            if ((resultHit - startPos).magnitude() > -modifyDist)
            {
                resultHit = resultHit + dir * modifyDist;
            }
            else
            {
                resultHit = startPos;
            }
        }
        else
        {
            resultHit = resultHit + dir * modifyDist;
        }

        result = true;
    }
    else
    {
        resultHit = endPos;
        result = false;
    }
    return result;
}

bool DynamicMapTree::isInLineOfSight(float x1, float y1, float z1, float x2, float y2, float z2, uint32 phasemask, VMAP::ModelIgnoreFlags ignoreFlags) const
{
    G3D::Vector3 v1(x1, y1, z1), v2(x2, y2, z2);

    float maxDist = (v2 - v1).magnitude();

    if (!G3D::fuzzyGt(maxDist, 0))
    {
        return true;
    }

    G3D::Ray r(v1, (v2 - v1) / maxDist);
    DynamicTreeIntersectionCallback callback(phasemask, ignoreFlags);
    impl->intersectRay(r, callback, maxDist, v2, true);

    return !callback.didHit();
}

float DynamicMapTree::getHeight(float x, float y, float z, float maxSearchDist, uint32 phasemask) const
{
    G3D::Vector3 v(x, y, z);
    G3D::Ray r(v, G3D::Vector3(0, 0, -1));
    DynamicTreeIntersectionCallback callback(phasemask, VMAP::ModelIgnoreFlags::Nothing);
    impl->intersectZAllignedRay(r, callback, maxSearchDist);

    if (callback.didHit())
    {
        return v.z - maxSearchDist;
    }
    else
    {
        return -G3D::finf();
    }
}

bool DynamicMapTree::GetAreaAndLiquidData(float x, float y, float z, uint32 phasemask, Optional<uint8> reqLiquidType, VMAP::AreaAndLiquidData& data) const
{
    G3D::Vector3 v(x, y, z + 0.5f);
    DynamicTreeLocationInfoCallback intersectionCallBack(phasemask);
    impl->intersectPoint(v, intersectionCallBack);
    if (intersectionCallBack.GetLocationInfo().hitModel)
    {
        data.floorZ = intersectionCallBack.GetLocationInfo().ground_Z;
        uint32 liquidType = intersectionCallBack.GetLocationInfo().hitModel->GetLiquidType();
        float liquidLevel;
        if (!reqLiquidType || (dynamic_cast<VMAP::VMapMgr2*>(VMAP::VMapFactory::createOrGetVMapMgr())->GetLiquidFlagsPtr(liquidType) & *reqLiquidType))
            if (intersectionCallBack.GetHitModel()->GetLiquidLevel(v, intersectionCallBack.GetLocationInfo(), liquidLevel))
                data.liquidInfo.emplace(liquidType, liquidLevel);

        data.areaInfo.emplace(intersectionCallBack.GetLocationInfo().hitModel->GetWmoID(),
            0,
            intersectionCallBack.GetLocationInfo().rootId,
            intersectionCallBack.GetLocationInfo().hitModel->GetMogpFlags(),
            0);
        return true;
    }
    return false;
}
