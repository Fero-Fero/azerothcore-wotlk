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

#ifndef _OBJECTVISIBILITYCONTAINER_H
#define _OBJECTVISIBILITYCONTAINER_H

#include "Common.h"
#include "ObjectGuid.h"
#include <atomic>
#include <memory>
#include <unordered_map>

class Player;
class WorldObject;

typedef std::unordered_map<ObjectGuid, WorldObject*> VisibleWorldObjectsMap;
typedef std::unordered_map<ObjectGuid, Player*> VisiblePlayersMap;

// Class that manages the visibility containers of a worldobject
class ObjectVisibilityContainer
{
public:
    ObjectVisibilityContainer(WorldObject* selfObject);
    ~ObjectVisibilityContainer();

    // Creates the _visibleWorldObjectsMap map if we are a player
    void InitForPlayer();

    // Cleans up all visibility references from other worldobjects,
    // this is used before a worldobject is deleted to prevent any dangling references
    void CleanVisibilityReferences();

    void LinkWorldObjectVisibility(WorldObject* worldObject);
    void UnlinkWorldObjectVisibility(WorldObject* worldObject);

    // These helpers aren't ideal, but needed in a few spots for cleaning up references
    VisibleWorldObjectsMap::iterator UnlinkVisibilityFromPlayer(WorldObject* worldObject, VisibleWorldObjectsMap::iterator itr);
    VisiblePlayersMap::iterator UnlinkVisibilityFromWorldObject(Player* player, VisiblePlayersMap::iterator itr);

    // Step 10: appliers of the VisibilityLink / VisibilityUnlink records (the target's half, on its own task).
    void ApplyLinkFromViewer(Player* viewer);
    void ApplyUnlinkFromViewer(ObjectGuid viewer);

    // Returns a list of all players who can see us
    // Step 13: the maps are the owner's; a foreign task reads the viewer's own map or a slot instead
    // (ASSERT_OWNED(_selfObject) in debug builds, out of line because WorldObject is incomplete here).
    VisiblePlayersMap& GetVisiblePlayersMap();
    VisiblePlayersMap const& GetVisiblePlayersMap() const;

    // Returns a list of all worldobjects who we can see
    // Warning: This is for player objects only, all other objects will return a nullptr
    VisibleWorldObjectsMap* GetVisibleWorldObjectsMap();
    VisibleWorldObjectsMap const* GetVisibleWorldObjectsMap() const;

private:

    // The target's half of a link: a record for a foreign unit, a direct write otherwise.
    void LinkTargetSide(WorldObject* worldObject);
    void UnlinkTargetSide(WorldObject* worldObject);

    // Directly removes visibility reference. This is to be ONLY used as
    // a more efficient method for cleaning up visibility references.
    // Warning: Improper use will leave dangling references and result in crashes.
    void DirectRemoveVisibilityReference(ObjectGuid guid);

    // Directly inserts player visibility reference.
    // Warning: Improper use will leave dangling references and result in crashes.
    void DirectInsertVisiblePlayerReference(Player* player);

    // Directly removes player visibility reference.
    // Warning: Improper use will leave dangling references and result in crashes.
    void DirectRemoveVisiblePlayerReference(ObjectGuid guid);

    WorldObject* _selfObject;

    // List of all worldobjects that are visible to us (including other players)
    // Only players contain this map, thus we will only allocate it as needed.
    std::unique_ptr<VisibleWorldObjectsMap> _visibleWorldObjectsMap;

    // List of players who are currently able to see this worldobject.
    // All worldobjects will contain this map
    VisiblePlayersMap _visiblePlayersMap;

    // Step 14: view commits run concurrently in U2 and several viewers may link / unlink the same target in
    // the same phase. The target's map is only ever read by its owner (U1, seal), never in U2, so a one-byte
    // spinlock around the two direct writes is the whole protocol; held for one hash-map op.
    std::atomic_flag _playersLock = ATOMIC_FLAG_INIT;
};

#endif
