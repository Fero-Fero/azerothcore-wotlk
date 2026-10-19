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

#ifndef _BIH_WRAP
#define _BIH_WRAP

#include "BoundingIntervalHierarchy.h"
#include "G3D/Array.h"
#include "G3D/Set.h"
#include "G3D/Table.h"
#include <atomic>
#include <mutex>

template<class T, class BoundsFunc = BoundsTrait<T>>
class BIHWrap
{
    template<class RayCallback>
    struct MDLCallback
    {
        T const* const* objects;
        RayCallback& _callback;
        uint32 objects_size;

        MDLCallback(RayCallback& callback, T const* const* objects_array, uint32 objects_size ) : objects(objects_array), _callback(callback), objects_size(objects_size) { }

        /// Intersect ray
        bool operator() (G3D::Ray const& ray, uint32 idx, float& maxDist, bool stopAtFirstHit)
        {
            if (idx >= objects_size)
            {
                return false;
            }
            if (T const* obj = objects[idx])
            {
                return _callback(ray, *obj, maxDist, stopAtFirstHit);
            }
            return false;
        }

        /// Intersect point
        void operator() (G3D::Vector3 const& p, uint32 idx)
        {
            if (idx >= objects_size)
            {
                return;
            }
            if (T const* obj = objects[idx])
            {
                _callback(p, *obj);
            }
        }
    };

    typedef G3D::Array<T const*> ObjArray;

    BIH m_tree;
    ObjArray m_objects;
    G3D::Table<T const*, uint32> m_obj2Idx;
    G3D::Set<T const*> m_objects_to_push;
    // Raised by insert/remove (never concurrent with a reader), cleared by balance() once the rebuild is done.
    // Readers rebuild lazily, and map update tasks read the same tree on several cores: the rebuild runs under
    // m_balanceMutex and the counter drops only after it, so a reader that sees 0 sees a finished tree.
    std::atomic<int> unbalanced_times;
    std::mutex m_balanceMutex;

public:
    BIHWrap() : unbalanced_times(0) { }

    void insert(T const& obj)
    {
        ++unbalanced_times;
        m_objects_to_push.insert(&obj);
    }

    void remove(T const& obj)
    {
        ++unbalanced_times;
        uint32 Idx = 0;
        T const* temp;
        if (m_obj2Idx.getRemove(&obj, temp, Idx))
        {
            m_objects[Idx] = nullptr;
        }
        else
        {
            m_objects_to_push.remove(&obj);
        }
    }

    void balance()
    {
        if (unbalanced_times.load(std::memory_order_acquire) == 0)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_balanceMutex);

        int const pending = unbalanced_times.load(std::memory_order_relaxed);
        if (pending == 0)
        {
            return; // another reader rebuilt while this one waited for the lock
        }

        m_objects.fastClear();
        m_obj2Idx.getKeys(m_objects);
        m_objects_to_push.getMembers(m_objects);
        //assert that m_obj2Idx has all the keys

        m_tree.build(m_objects, BoundsFunc::GetBounds2);

        unbalanced_times.fetch_sub(pending, std::memory_order_release);
    }

    template<typename RayCallback>
    void intersectRay(G3D::Ray const& ray, RayCallback& intersectCallback, float& maxDist, bool stopAtFirstHit)
    {
        balance();
        MDLCallback<RayCallback> temp_cb(intersectCallback, m_objects.getCArray(), m_objects.size());
        m_tree.intersectRay(ray, temp_cb, maxDist, stopAtFirstHit);
    }

    template<typename IsectCallback>
    void intersectPoint(G3D::Vector3 const& point, IsectCallback& intersectCallback)
    {
        balance();
        MDLCallback<IsectCallback> callback(intersectCallback, m_objects.getCArray(), m_objects.size());
        m_tree.intersectPoint(point, callback);
    }
};

#endif // _BIH_WRAP
