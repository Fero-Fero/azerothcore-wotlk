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

#include "BeatContext.h"
#include "Object.h"
#include "Unit.h"
#include "Vehicle.h"

bool BeatContext::IsForeign(WorldObject const* o)
{
    if (!owner || owner == o)
        return false;
    // Not in the world yet (being built by the task that will add it) or already out of it: nobody else's task.
    if (!o->IsInWorld())
        return false;
    return o->TaskRoot() != root;
}

namespace
{
    // The pairs step 12 folds into one task group. They write each other every beat and are not audited.
    bool IsGroupPeer(Object const* ownerObj, Object const* o)
    {
        Unit const* a = ownerObj->ToUnit();
        Unit const* b = o->ToUnit();
        if (!a || !b)
            return false;

        ObjectGuid const bGuid = b->GetGUID();
        if (a->GetCharmerGUID() == bGuid || a->GetCharmGUID() == bGuid)
            return true;
        if (Unit const* base = a->GetVehicleBase())
            if (base == b)
                return true;
        if (Vehicle const* kit = a->GetVehicleKit())
            for (auto const& [seatId, seat] : kit->Seats)
                if (seat.Passenger.Guid == bGuid)
                    return true;
        return false;
    }
}

void BeatContext::CountForeignRead(Object const* o)
{
    if (!owner || owner == o)
        return;

    // The exclusion check reads guid fields itself; do not count those.
    static thread_local bool counting = false;
    if (counting)
        return;
    counting = true;
    bool const peer = IsGroupPeer(owner, o);
    counting = false;
    if (!peer)
        ++foreignReads;
}

void BeatContext::FlushForeignReads()
{
    if (uint64 n = foreignReads)
    {
        foreignReads = 0;
        foreignReadsTotal.fetch_add(n, std::memory_order_relaxed);
    }
}

bool BeatContext::MayAccess(Object const* o)
{
    // Outside a beat everything is single-threaded. inBeat stays false until step 12.
    if (!inBeat.load(std::memory_order_relaxed))
        return true;

    // Unit phase: the owner, or any member of the owner's task group.
    if (owner)
        return owner == o || (o->IsWorldObject() && static_cast<WorldObject const*>(o)->TaskRoot() == root);

    // Seal phase S1a: a grid task may touch only the objects routed to its grid this beat.
    if (sealMap && sealGrid >= 0)
    {
        if (!o->IsWorldObject())
            return false;

        WorldObject const* wo = static_cast<WorldObject const*>(o);
        return wo->_pendingInGrid == sealGrid && wo->FindMap() == sealMap;
    }

    // Seal phase S1b: a map seal task may touch anything on its own map (maps are disjoint, no unit task runs).
    if (sealMap)
        return !o->IsWorldObject() || static_cast<WorldObject const*>(o)->FindMap() == sealMap;

    return false;
}
