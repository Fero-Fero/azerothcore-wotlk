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

#ifndef ACORE_AREA_AURA_RECORDS_H
#define ACORE_AREA_AURA_RECORDS_H

#include "AuraLaunchData.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <type_traits>

// Step 08 §3: area auras as per-target clones.
//
// The owner's Aura (a unit's Devotion Aura, a Consecration dynobj) computes its membership in UpdateTargetMap
// on the owner's task and keeps a guid set of the foreign members. Each foreign member owns a clone: a UnitAura
// of the same spell and caster, flagged IsAreaClone(), built from these numbers exactly like a hit builds an
// aura from AuraLaunchData. The clone ticks on its owner's task; its caster-side writes are PeriodicLanded
// records to the aura's caster like any other periodic aura.
//
// The owner refreshes every member each UPDATE_TARGET_MAP_INTERVAL. A clone that has not been refreshed for
// AREA_CLONE_TIMEOUT_MS removes itself (owner gone, owner stopped updating). The client-visible duration is
// the owner's, carried in launch.maxDuration / launch.duration.
constexpr uint32 AREA_CLONE_TIMEOUT_MS = 1500;   // 3 x UPDATE_TARGET_MAP_INTERVAL

struct AreaAuraApplyRecord
{
    uint32 spellId = 0;
    ObjectGuid areaOwner;           // the owner of the spreading aura (unit or dynamic object)
    ObjectGuid caster;              // Aura::m_casterGuid
    ObjectGuid castItemGuid;
    uint32 castItemEntry = 0;
    uint8 stackAmount = 1;
    AuraLaunchData launch;          // effMask = effects this member gets after immunities
};

struct AreaAuraRemoveRecord
{
    uint32 spellId = 0;
    ObjectGuid areaOwner;
    ObjectGuid caster;
    uint8 removeMode = 0;           // AuraRemoveMode
};

static_assert(std::is_trivially_copyable_v<AreaAuraApplyRecord>, "record payloads are plain values");
static_assert(std::is_trivially_copyable_v<AreaAuraRemoveRecord>, "record payloads are plain values");

#endif
