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

#include "UnitView.h"
#include "BeatContext.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Unit.h"
#include "UpdateFields.h"

UnitView::UnitView(Unit const* handle)
    : _handle(handle), _live(handle && (!BeatContext::InTask() || BeatContext::MayAccess(handle)))
{
}

SlotReader UnitView::Slot() const
{
    return SlotReader(_handle->ReadSlot());
}

ObjectGuid UnitView::GetGUID() const
{
    return _handle ? _handle->GetGUID() : ObjectGuid::Empty;
}

bool UnitView::IsPlayer() const
{
    return _handle && _handle->IsPlayer();
}

bool UnitView::IsCreature() const
{
    return _handle && _handle->IsCreature();
}

uint32 UnitView::GetEntry() const
{
    if (!_handle)
        return 0;
    return _live ? _handle->GetEntry() : Slot().U32(OBJECT_FIELD_ENTRY);
}

uint8 UnitView::GetLevel() const
{
    if (!_handle)
        return 0;
    return _live ? _handle->GetLevel() : Slot().Level();
}

uint32 UnitView::GetHealth() const
{
    if (!_handle)
        return 0;
    return _live ? _handle->GetHealth() : Slot().Health();
}

uint32 UnitView::GetMaxHealth() const
{
    if (!_handle)
        return 0;
    return _live ? _handle->GetMaxHealth() : Slot().MaxHealth();
}

uint32 UnitView::GetFaction() const
{
    if (!_handle)
        return 0;
    return _live ? _handle->GetFaction() : Slot().Faction();
}

bool UnitView::IsAlive() const
{
    if (!_handle)
        return false;
    return _live ? _handle->IsAlive() : Slot().IsAlive();
}

bool UnitView::IsInCombat() const
{
    if (!_handle)
        return false;
    return _live ? _handle->IsInCombat() : _handle->ReadSlot().isInCombat != 0;
}

bool UnitView::HasUnitState(uint32 state) const
{
    if (!_handle)
        return false;
    return _live ? _handle->HasUnitState(state) : (_handle->ReadSlot().unitState & state) != 0;
}

bool UnitView::HasUnitFlag(uint32 flags) const
{
    if (!_handle)
        return false;
    return _live ? _handle->HasFlag(UNIT_FIELD_FLAGS, flags) : Slot().Flag(UNIT_FIELD_FLAGS, flags);
}

ObjectGuid UnitView::GetOwnerGUID() const
{
    if (!_handle)
        return ObjectGuid::Empty;
    return _live ? _handle->GetOwnerGUID() : Slot().Guid(UNIT_FIELD_SUMMONEDBY);
}

ObjectGuid UnitView::GetCharmerOrOwnerGUID() const
{
    if (!_handle)
        return ObjectGuid::Empty;
    return _live ? _handle->GetCharmerOrOwnerGUID() : _handle->ReadSlot().charmerOrOwnerGuid;
}

Position UnitView::GetPosition() const
{
    if (!_handle)
        return Position();
    return _live ? _handle->GetPosition() : _handle->ReadSlot().pos;
}

uint32 UnitView::GetCreatureType() const
{
    if (!_handle)
        return CREATURE_TYPE_HUMANOID;
    if (_live)
        return _handle->GetCreatureType();
    if (_handle->IsPlayer())
    {
        // Unit::GetShapeshiftForm reads UNIT_FIELD_BYTES_2 byte 3
        ShapeshiftForm form = ShapeshiftForm(Slot().Byte(UNIT_FIELD_BYTES_2, 3));
        SpellShapeshiftFormEntry const* ssEntry = sSpellShapeshiftFormStore.LookupEntry(form);
        if (ssEntry && ssEntry->creatureType > 0)
            return ssEntry->creatureType;
        return CREATURE_TYPE_HUMANOID;
    }
    // template data is immutable while the creature exists
    return _handle->ToCreature()->GetCreatureTemplate()->type;
}

uint32 UnitView::GetCreatureTypeMask() const
{
    uint32 creatureType = GetCreatureType();
    return (creatureType >= 1) ? (1 << (creatureType - 1)) : 0;
}

bool UnitView::HasAura(uint32 spellId, ObjectGuid casterGuid) const
{
    if (!_handle)
        return false;
    return _live ? _handle->HasAura(spellId, casterGuid) : Slot().HasAura(spellId, casterGuid);
}

int32 UnitView::GetTotalAuraModifier(AuraType type) const
{
    if (!_handle)
        return 0;
    return _live ? _handle->GetTotalAuraModifier(type) : Slot().TotalAuraModifier(type);
}

bool UnitView::IsFriendlyTo(Unit const* target) const
{
    if (!_handle || !target)
        return false;
    if (_live)
        return _handle->IsFriendlyTo(target);

    FactionTemplateEntry const* mine = sFactionTemplateStore.LookupEntry(GetFaction());
    FactionTemplateEntry const* theirs = target->GetFactionTemplateEntry();
    return mine && theirs && mine->IsFriendlyTo(*theirs);
}

bool UnitView::IsHostileTo(Unit const* target) const
{
    if (!_handle || !target)
        return false;
    if (_live)
        return _handle->IsHostileTo(target);

    FactionTemplateEntry const* mine = sFactionTemplateStore.LookupEntry(GetFaction());
    FactionTemplateEntry const* theirs = target->GetFactionTemplateEntry();
    return mine && theirs && mine->IsHostileTo(*theirs);
}
