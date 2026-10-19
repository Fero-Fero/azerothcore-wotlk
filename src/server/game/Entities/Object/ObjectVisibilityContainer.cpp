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

#include "ObjectVisibilityContainer.h"
#include "Object.h"
#include "Player.h"
#include "UnitRecordLog.h"
#include "UnitRecords.h"
#include <thread>

/*
* Some implementation notes:
* Non-player worldobjects do not have any concept of 'visibility', thus,
* the most important and mainly used map is 'VisibleWorldObjectsMap'
* which is only accessible for player objects. The 'VisiblePlayersMap'
* map is simply for managing the references so we can use direct pointers.
*/

ObjectVisibilityContainer::ObjectVisibilityContainer(WorldObject* selfObject) :
    _selfObject(selfObject)
{
}

ObjectVisibilityContainer::~ObjectVisibilityContainer()
{
    ASSERT(_visiblePlayersMap.empty());
    if (_visibleWorldObjectsMap)
        ASSERT((*_visibleWorldObjectsMap).empty());
}

VisiblePlayersMap& ObjectVisibilityContainer::GetVisiblePlayersMap()
{
    ASSERT_OWNED(_selfObject);
    return _visiblePlayersMap;
}

VisiblePlayersMap const& ObjectVisibilityContainer::GetVisiblePlayersMap() const
{
    ASSERT_OWNED(_selfObject);
    return _visiblePlayersMap;
}

VisibleWorldObjectsMap* ObjectVisibilityContainer::GetVisibleWorldObjectsMap()
{
    ASSERT_OWNED(_selfObject);
    return _visibleWorldObjectsMap.get();
}

VisibleWorldObjectsMap const* ObjectVisibilityContainer::GetVisibleWorldObjectsMap() const
{
    ASSERT_OWNED(_selfObject);
    return _visibleWorldObjectsMap.get();
}

void ObjectVisibilityContainer::InitForPlayer()
{
    _visibleWorldObjectsMap = std::make_unique<VisibleWorldObjectsMap>();
}

void ObjectVisibilityContainer::CleanVisibilityReferences()
{
    for (auto const& kvPair : _visiblePlayersMap)
        kvPair.second->GetObjectVisibilityContainer().DirectRemoveVisibilityReference(_selfObject->GetGUID());

    if (_visibleWorldObjectsMap)
    {
        for (auto const& kvPair : *_visibleWorldObjectsMap)
            kvPair.second->GetObjectVisibilityContainer().DirectRemoveVisiblePlayerReference(_selfObject->GetGUID());

        (*_visibleWorldObjectsMap).clear();
    }

    _visiblePlayersMap.clear();
}

void ObjectVisibilityContainer::LinkWorldObjectVisibility(WorldObject* worldObject)
{
    // Do not link self
    if (worldObject == _selfObject)
        return;

    // Transports are special and should not be added to our visibility map
    if (worldObject->IsGameObject() && worldObject->ToGameObject()->IsTransport())
        return;

    // Only players can link visibility
    if (!_visibleWorldObjectsMap)
        return;

    (*_visibleWorldObjectsMap).insert(std::make_pair(worldObject->GetGUID(), worldObject));
    LinkTargetSide(worldObject);
}

void ObjectVisibilityContainer::UnlinkWorldObjectVisibility(WorldObject* worldObject)
{
    // Only players can unlink visibility
    if (!_visibleWorldObjectsMap)
        return;

    UnlinkTargetSide(worldObject);
    (*_visibleWorldObjectsMap).erase(worldObject->GetGUID());
}

VisibleWorldObjectsMap::iterator ObjectVisibilityContainer::UnlinkVisibilityFromPlayer(WorldObject* worldObject, VisibleWorldObjectsMap::iterator itr)
{
    ASSERT(_visibleWorldObjectsMap); // Ensure we aren't for some reason calling this as a non-player object
    UnlinkTargetSide(worldObject);
    return (*_visibleWorldObjectsMap).erase(itr);
}

// Step 10: the target's half of a link. The record form (VisibilityLink / VisibilityUnlink to a foreign unit,
// applied from its inbox next beat) is wired but not used yet: an object removed at the end of this tick
// would be freed before the record lands, leaving the viewer's map pointing at it. The write stays direct;
// step 14 made it safe under concurrent view commits with the one-byte spinlock in the two Direct*Player* writers.
namespace
{
    constexpr bool USE_VISIBILITY_LINK_RECORDS = false;
}

void ObjectVisibilityContainer::LinkTargetSide(WorldObject* worldObject)
{
    if (USE_VISIBILITY_LINK_RECORDS && worldObject->IsUnit() && worldObject->IsForeignToTask())
    {
        VisibilityLinkRecord record;
        record.viewer = _selfObject->GetGUID();
        sRecordLogs->Append(_selfObject->GetGUID(), worldObject, RecordKind::VisibilityLink, record);
        return;
    }

    worldObject->GetObjectVisibilityContainer().DirectInsertVisiblePlayerReference(_selfObject->ToPlayer());
}

void ObjectVisibilityContainer::UnlinkTargetSide(WorldObject* worldObject)
{
    if (USE_VISIBILITY_LINK_RECORDS && worldObject->IsUnit() && worldObject->IsForeignToTask())
    {
        VisibilityUnlinkRecord record;
        record.viewer = _selfObject->GetGUID();
        sRecordLogs->Append(_selfObject->GetGUID(), worldObject, RecordKind::VisibilityUnlink, record);
        return;
    }

    worldObject->GetObjectVisibilityContainer().DirectRemoveVisiblePlayerReference(_selfObject->GetGUID());
}

void ObjectVisibilityContainer::ApplyLinkFromViewer(Player* viewer)
{
    DirectInsertVisiblePlayerReference(viewer);
}

void ObjectVisibilityContainer::ApplyUnlinkFromViewer(ObjectGuid viewer)
{
    DirectRemoveVisiblePlayerReference(viewer);
}

VisiblePlayersMap::iterator ObjectVisibilityContainer::UnlinkVisibilityFromWorldObject(Player* player, VisiblePlayersMap::iterator itr)
{
    player->GetObjectVisibilityContainer().DirectRemoveVisibilityReference(_selfObject->GetGUID());
    return _visiblePlayersMap.erase(itr);
}

void ObjectVisibilityContainer::DirectRemoveVisibilityReference(ObjectGuid guid)
{
    ASSERT(_visibleWorldObjectsMap);
    (*_visibleWorldObjectsMap).erase(guid);
}

namespace
{
    struct PlayersLockScope
    {
        explicit PlayersLockScope(std::atomic_flag& flag) : _flag(flag)
        {
            while (_flag.test_and_set(std::memory_order_acquire))
                std::this_thread::yield();
        }
        ~PlayersLockScope() { _flag.clear(std::memory_order_release); }
        std::atomic_flag& _flag;
    };
}

void ObjectVisibilityContainer::DirectInsertVisiblePlayerReference(Player* player)
{
    PlayersLockScope lock(_playersLock);
    _visiblePlayersMap.insert(std::make_pair(player->GetGUID(), player));
}

void ObjectVisibilityContainer::DirectRemoveVisiblePlayerReference(ObjectGuid guid)
{
    PlayersLockScope lock(_playersLock);
    _visiblePlayersMap.erase(guid);
}
