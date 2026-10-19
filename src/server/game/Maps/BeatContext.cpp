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
#include "Item.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Unit.h"
#include "Vehicle.h"

Map* BeatContext::DeferMap()
{
    if (!InBeat())
        return nullptr;
    if (root)
        return root->FindMap();
    return sealMap;
}

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

    // Same shape as MayAccess: items and objects outside the world have no other task to race with.
    if (!o->IsWorldObject() || !static_cast<WorldObject const*>(o)->IsInWorld())
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

char const* BeatContext::SingletonLockName(SingletonLock kind)
{
    switch (kind)
    {
        case LOCK_CHARACTER_CACHE:
            return "CharacterCache";
        case LOCK_ACHIEVEMENT_REALM_FIRST:
            return "AchievementGlobalMgr::m_allCompletedAchievements";
        case LOCK_GUILD_MEMBER:
            return "Guild member data";
        default:
            return "?";
    }
}

namespace
{
    // An item belongs to the task of the player holding it. Items without an owner (being created by a
    // spell, in the mail or auction stores) belong to whoever holds the pointer.
    bool MayAccessItem(Item const* item)
    {
        ObjectGuid const ownerGuid = item->GetOwnerGUID();
        if (!ownerGuid)
            return true;
        if (BeatContext::owner && BeatContext::owner->GetGUID() == ownerGuid)
            return true;

        Player const* holder = ObjectAccessor::FindConnectedPlayer(ownerGuid);
        return !holder || BeatContext::MayAccess(holder);
    }
}

bool BeatContext::MayAccess(Object const* o)
{
    // Outside a beat everything is single-threaded. inBeat stays false until step 12.
    if (!inBeat.load(std::memory_order_relaxed))
        return true;

    // Resolving an item's owner reads the item's own guid fields; do not audit those.
    static thread_local bool resolving = false;
    if (resolving)
        return true;

    if (!o->IsWorldObject())
    {
        if (o->isType(TYPEMASK_ITEM))
        {
            resolving = true;
            bool const ok = MayAccessItem(static_cast<Item const*>(o));
            resolving = false;
            return ok;
        }
        // Plain Objects (none today) and whatever else is not placed in a map: no task owns them.
        return true;
    }

    WorldObject const* wo = static_cast<WorldObject const*>(o);

    // Not in the world: being built by the task that will add it, or already torn down by the task that
    // held it. Same rule as IsForeign.
    if (!wo->IsInWorld())
        return true;

    // Unit phase: the owner, or any member of the owner's task group.
    if (owner)
        return owner == o || wo->TaskRoot() == root;

    // Seal phase S1a: a grid task may touch only the objects routed to its grid this beat.
    if (sealMap && sealGrid >= 0)
        return wo->_pendingInGrid == sealGrid && wo->FindMap() == sealMap;

    // Seal phase S1b: a map seal task may touch anything on its own map (maps are disjoint, no unit task runs).
    if (sealMap)
        return wo->FindMap() == sealMap;

    // Inside a beat on a thread that runs neither a unit task nor a seal task: the coordinator between
    // tasks. It holds no object; a live-field access here is a bug in the pool.
    return false;
}
