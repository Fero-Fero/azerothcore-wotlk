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

#ifndef ACORE_UNIT_VIEW_H
#define ACORE_UNIT_VIEW_H

#include "Define.h"
#include "ObjectGuid.h"
#include "ObjectSlot.h"
#include "Position.h"
#include "SpellAuraDefines.h"

class Unit;

// Read-only view of a unit for code running on another unit's task (plan 07 §9). Reads come from the live
// object when the running task may access it (its own task, or no task at all) and from the published slot
// otherwise. There is no mutator: a call site that needs one does not compile, which is the audit.
//
// This slice wraps the live caster on the land step; the slot path is exercised from slice 3 on.
class UnitView
{
public:
    explicit UnitView(Unit const* handle);

    // Identity. The handle is for guid compares and for APIs that take a Unit* as a key; never for fields.
    [[nodiscard]] Unit const* Handle() const { return _handle; }
    [[nodiscard]] bool IsValid() const { return _handle != nullptr; }
    [[nodiscard]] bool IsLive() const { return _live; }
    [[nodiscard]] ObjectGuid GetGUID() const;

    [[nodiscard]] bool IsPlayer() const;
    [[nodiscard]] bool IsCreature() const;
    [[nodiscard]] uint32 GetEntry() const;
    [[nodiscard]] uint8 GetLevel() const;
    [[nodiscard]] uint32 GetHealth() const;
    [[nodiscard]] uint32 GetMaxHealth() const;
    [[nodiscard]] uint32 GetFaction() const;
    [[nodiscard]] bool IsAlive() const;
    [[nodiscard]] bool IsInCombat() const;
    [[nodiscard]] bool HasUnitState(uint32 state) const;
    [[nodiscard]] bool HasUnitFlag(uint32 flags) const;
    [[nodiscard]] ObjectGuid GetOwnerGUID() const;
    [[nodiscard]] ObjectGuid GetCharmerOrOwnerGUID() const;
    [[nodiscard]] Position GetPosition() const;

    [[nodiscard]] bool HasAura(uint32 spellId, ObjectGuid casterGuid = ObjectGuid::Empty) const;
    [[nodiscard]] int32 GetTotalAuraModifier(AuraType type) const;

    // Reaction against the owner of the running task. Live: Unit::IsFriendlyTo. Slot: faction template only.
    [[nodiscard]] bool IsFriendlyTo(Unit const* target) const;
    [[nodiscard]] bool IsHostileTo(Unit const* target) const;

private:
    [[nodiscard]] SlotReader Slot() const;

    Unit const* _handle;
    bool _live;
};

using CasterView = UnitView;

#endif
