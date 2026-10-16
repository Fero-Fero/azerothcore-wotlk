/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Ported from TrinityCore with modifications for AzerothCore
 * Original authors:
 *   Treeston <treeston.mmoc@gmail.com> - Combat/Threat system rewrite (PR #19930)
 *   Shauren <shauren.trinity@gmail.com>
 *   Spp <spp@jorge.gr>
 */

#include "CombatManager.h"
#include "BeatContext.h"
#include "Containers.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ThreatManager.h"
#include "Unit.h"
#include "UnitAI.h"
#include "UnitRecordLog.h"

/*static*/ bool CombatManager::CanBeginCombat(Unit const* a, Unit const* b)
{
    // Checks combat validity before initial reference creation.
    // For the combat to be valid...
    // ...the two units need to be different
    if (a == b)
        return false;
    // ...the two units need to be in the world
    if (!a->IsInWorld() || !b->IsInWorld())
        return false;
    // ...the two units need to both be alive
    if (!a->IsAlive() || !b->IsAlive())
        return false;
    // ...the two units need to be on the same map
    if (a->GetMap() != b->GetMap())
        return false;
    // ...the two units need to be in the same phase
    if (!a->InSamePhase(b))
        return false;
    if (a->HasUnitState(UNIT_STATE_EVADE) || b->HasUnitState(UNIT_STATE_EVADE))
        return false;
    if (a->HasUnitState(UNIT_STATE_IN_FLIGHT) || b->HasUnitState(UNIT_STATE_IN_FLIGHT))
        return false;
    // ... both units must be allowed to enter combat
    if (a->IsCombatDisallowed() || b->IsCombatDisallowed())
        return false;
    // ...not friendly, unless one side is hostile (asymmetric aggressor wins)
    if ((a->IsFriendlyTo(b) || b->IsFriendlyTo(a)) && !a->IsHostileTo(b) && !b->IsHostileTo(a))
        return false;
    Player const* playerA = a->GetCharmerOrOwnerPlayerOrPlayerItself();
    Player const* playerB = b->GetCharmerOrOwnerPlayerOrPlayerItself();
    // ...neither of the two units must be (owned by) a player with .gm on
    if ((playerA && playerA->IsGameMaster()) || (playerB && playerB->IsGameMaster()))
        return false;
    return true;
}

CombatReference::CombatReference(Unit* me, ObjectGuid other, bool pvp)
    : owner(me), otherGuid(other), _isPvP(pvp),
      _otherCache(nullptr), _otherCacheBeat(~0u)
{
}

Unit* CombatReference::GetOther() const
{
    uint32 const beat = BeatContext::beat.load(std::memory_order_relaxed);
    if (_otherCacheBeat != beat)
    {
        _otherCache = ObjectAccessor::GetUnit(*owner, otherGuid);
        _otherCacheBeat = beat;
    }
    if (_otherCache && !_otherCache->IsInWorld())
        return nullptr;
    return _otherCache;
}

Unit* CombatReference::GetOther(Unit const* me) const
{
    if (me == owner)
        return GetOther();
    if (me && me->GetGUID() == otherGuid)
        return owner;
    return GetOther();
}

void CombatReference::EndCombat()
{
    // sequencing matters here - AI might do nasty stuff, so make sure this half is gone before we hand off
    owner->GetThreatMgr().ClearThreat(otherGuid);
    owner->GetCombatManager().PurgeReference(otherGuid, _isPvP);
    bool const needAI = owner->GetCombatManager().UpdateOwnerCombatState();

    if (Unit* other = ObjectAccessor::GetUnit(*owner, otherGuid))
    {
        if (other->IsForeignToTask())
        {
            CombatEndRecord record;
            record.otherGuid = owner->GetGUID();
            record.isPvP = _isPvP;
            sRecordLogs->Append(owner->GetGUID(), other, RecordKind::CombatEnd, record);
        }
        else
            other->GetCombatManager().ApplyCombatEnd(owner->GetGUID(), _isPvP);
    }

    if (needAI)
        if (UnitAI* ai = owner->GetAI())
            ai->JustExitedCombat();

    delete this;
}

void CombatReference::Refresh()
{
    if (!_suppressed)
        return;

    _suppressed = false;
    if (owner->GetCombatManager().UpdateOwnerCombatState())
        if (Unit* other = GetOther())
            CombatManager::NotifyAICombat(owner, other);
}

void CombatReference::SuppressFor(Unit* who)
{
    if (!who)
        return;

    if (who != owner)
    {
        // Other half: write it inline when it is not foreign (tests, no running task).
        if (who->GetGUID() == otherGuid && !who->IsForeignToTask())
            if (CombatReference* other = Acore::Containers::MapGetValuePtr(
                    who->GetCombatManager()._pveRefs, owner->GetGUID()))
            {
                other->SuppressFor(who);
                return;
            }
            else if (CombatReference* otherPvp = Acore::Containers::MapGetValuePtr(
                    who->GetCombatManager()._pvpRefs, owner->GetGUID()))
            {
                otherPvp->SuppressFor(who);
                return;
            }
        return;
    }

    Suppress();
    if (who->GetCombatManager().UpdateOwnerCombatState())
        if (UnitAI* ai = who->GetAI())
            ai->JustExitedCombat();
}

bool PvPCombatReference::Update(uint32 tdiff)
{
    if (_combatTimer <= tdiff)
        return false;
    _combatTimer -= tdiff;
    return true;
}

void PvPCombatReference::RefreshTimer()
{
    _combatTimer = PVP_COMBAT_TIMEOUT;
}

CombatManager::~CombatManager()
{
    ASSERT(_pveRefs.empty(), "CombatManager::~CombatManager - %s: we still have %zu PvE combat references, one of them is with %s", _owner->GetGUID().ToString().c_str(), _pveRefs.size(), _pveRefs.begin()->first.ToString().c_str());
    ASSERT(_pvpRefs.empty(), "CombatManager::~CombatManager - %s: we still have %zu PvP combat references, one of them is with %s", _owner->GetGUID().ToString().c_str(), _pvpRefs.size(), _pvpRefs.begin()->first.ToString().c_str());
}

void CombatManager::Update(uint32 tdiff)
{
    // Update evade timer
    if (_evadeTimer)
    {
        if (_evadeTimer <= tdiff)
        {
            _evadeTimer = 0;
            // Timer expired - trigger evade check via AI
            if (UnitAI* ai = _owner->GetAI())
                ai->EvadeTimerExpired();
        }
        else
            _evadeTimer -= tdiff;
    }

    auto it = _pvpRefs.begin(), end = _pvpRefs.end();
    while (it != end)
    {
        PvPCombatReference* const ref = it->second;
        if (!ref->Update(tdiff))
        {
            it = _pvpRefs.erase(it), end = _pvpRefs.end(); // remove it from our refs first to prevent invalidation
            ref->EndCombat(); // this will erase the other half (inline or as CombatEnd)
        }
        else
            ++it;
    }
}

bool CombatManager::HasPvECombat() const
{
    for (auto const& [guid, ref] : _pveRefs)
        if (!ref->IsSuppressedFor(_owner))
            return true;
    return false;
}

bool CombatManager::HasPvECombatWithPlayers() const
{
    for (std::pair<ObjectGuid const, CombatReference*> const& reference : _pveRefs)
        if (!reference.second->IsSuppressedFor(_owner))
            if (Unit* other = reference.second->GetOther())
                if (other->IsPlayer())
                    return true;

    return false;
}

bool CombatManager::HasPvPCombat() const
{
    for (auto const& pair : _pvpRefs)
        if (!pair.second->IsSuppressedFor(_owner))
            return true;
    return false;
}

Unit* CombatManager::GetAnyTarget() const
{
    for (auto const& pair : _pveRefs)
        if (!pair.second->IsSuppressedFor(_owner))
            if (Unit* other = pair.second->GetOther())
                return other;
    for (auto const& pair : _pvpRefs)
        if (!pair.second->IsSuppressedFor(_owner))
            if (Unit* other = pair.second->GetOther())
                return other;
    return nullptr;
}

bool CombatManager::SetInCombatWith(Unit* who, bool addSecondUnitSuppressed)
{
    bool const pvp = _owner->IsControlledByPlayer() && who->IsControlledByPlayer();

    // Are we already in combat? If yes, refresh (and tell the other half, as a record when they are foreign)
    if (pvp)
    {
        if (PvPCombatReference* existingPvpRef = Acore::Containers::MapGetValuePtr(_pvpRefs, who->GetGUID()))
        {
            existingPvpRef->RefreshTimer();
            existingPvpRef->Refresh();
            if (who->IsForeignToTask())
            {
                CombatRefRecord record;
                record.otherGuid = _owner->GetGUID();
                record.isPvP = 1;
                record.suppressed = 0;
                sRecordLogs->Append(_owner->GetGUID(), who, RecordKind::CombatRef, record);
            }
            else if (PvPCombatReference* other = Acore::Containers::MapGetValuePtr(who->GetCombatManager()._pvpRefs, _owner->GetGUID()))
            {
                other->RefreshTimer();
                other->Refresh();
            }
            return true;
        }
    }
    if (CombatReference* existingPveRef = Acore::Containers::MapGetValuePtr(_pveRefs, who->GetGUID()))
    {
        existingPveRef->Refresh();
        if (who->IsForeignToTask())
        {
            CombatRefRecord record;
            record.otherGuid = _owner->GetGUID();
            record.isPvP = 0;
            record.suppressed = 0;
            sRecordLogs->Append(_owner->GetGUID(), who, RecordKind::CombatRef, record);
        }
        else if (CombatReference* other = Acore::Containers::MapGetValuePtr(who->GetCombatManager()._pveRefs, _owner->GetGUID()))
            other->Refresh();
        return true;
    }

    if (!CombatManager::CanBeginCombat(_owner, who))
        return false;

    PutOwnHalf(who->GetGUID(), pvp, false);

    bool const needSelfAI = UpdateOwnerCombatState();
    if (needSelfAI)
        NotifyAICombat(_owner, who);

    if (who->IsForeignToTask())
    {
        CombatRefRecord record;
        record.otherGuid = _owner->GetGUID();
        record.isPvP = pvp;
        record.suppressed = addSecondUnitSuppressed;
        sRecordLogs->Append(_owner->GetGUID(), who, RecordKind::CombatRef, record);
    }
    else
    {
        who->GetCombatManager().PutOwnHalf(_owner->GetGUID(), pvp, addSecondUnitSuppressed);
        if (who->GetCombatManager().UpdateOwnerCombatState())
            NotifyAICombat(who, _owner);
    }

    return IsInCombatWith(who);
}

bool CombatManager::IsInCombatWith(ObjectGuid const& guid) const
{
    return (_pveRefs.find(guid) != _pveRefs.end()) || (_pvpRefs.find(guid) != _pvpRefs.end());
}

bool CombatManager::IsInCombatWith(Unit const* who) const
{
    return IsInCombatWith(who->GetGUID());
}

void CombatManager::InheritCombatStatesFrom(Unit const* who)
{
    CombatManager const& mgr = who->GetCombatManager();
    for (auto& ref : mgr._pveRefs)
    {
        if (!IsInCombatWith(ref.first))
        {
            Unit* target = ref.second->GetOther();
            if (!target)
                continue;
            if ((_owner->IsImmuneToPC() && target->HasUnitFlag(UNIT_FLAG_PLAYER_CONTROLLED)) ||
                (_owner->IsImmuneToNPC() && !target->HasUnitFlag(UNIT_FLAG_PLAYER_CONTROLLED)))
                continue;
            SetInCombatWith(target);
        }
    }
    for (auto& ref : mgr._pvpRefs)
    {
        Unit* target = ref.second->GetOther();
        if (!target)
            continue;
        if ((_owner->IsImmuneToPC() && target->HasUnitFlag(UNIT_FLAG_PLAYER_CONTROLLED)) ||
            (_owner->IsImmuneToNPC() && !target->HasUnitFlag(UNIT_FLAG_PLAYER_CONTROLLED)))
            continue;
        SetInCombatWith(target);
    }
}

void CombatManager::EndCombatBeyondRange(float range, bool includingPvP)
{
    auto it = _pveRefs.begin(), end = _pveRefs.end();
    while (it != end)
    {
        CombatReference* const ref = it->second;
        Unit* other = ref->GetOther();
        if (!other || !_owner->IsWithinDistInMap(other, range))
        {
            it = _pveRefs.erase(it), end = _pveRefs.end(); // erase manually here to avoid iterator invalidation
            ref->EndCombat();
        }
        else
            ++it;
    }

    if (!includingPvP)
        return;

    auto it2 = _pvpRefs.begin(), end2 = _pvpRefs.end();
    while (it2 != end2)
    {
        CombatReference* const ref = it2->second;
        Unit* other = ref->GetOther();
        if (!other || !_owner->IsWithinDistInMap(other, range))
        {
            it2 = _pvpRefs.erase(it2), end2 = _pvpRefs.end(); // erase manually here to avoid iterator invalidation
            ref->EndCombat();
        }
        else
            ++it2;
    }
}

void CombatManager::SuppressPvPCombat()
{
    for (auto const& pair : _pvpRefs)
        pair.second->Suppress();
    if (UpdateOwnerCombatState())
        if (UnitAI* ownerAI = _owner->GetAI())
            ownerAI->JustExitedCombat();
}

void CombatManager::EndAllPvECombat()
{
    // cannot have threat without combat
    _owner->GetThreatMgr().RemoveMeFromThreatLists();
    _owner->GetThreatMgr().ClearAllThreat();
    while (!_pveRefs.empty())
        _pveRefs.begin()->second->EndCombat();
}

void CombatManager::RevalidateCombat()
{
    auto it = _pveRefs.begin(), end = _pveRefs.end();
    while (it != end)
    {
        CombatReference* const ref = it->second;
        Unit* other = ref->GetOther();
        if (!other || !CanBeginCombat(_owner, other))
        {
            it = _pveRefs.erase(it), end = _pveRefs.end(); // erase manually here to avoid iterator invalidation
            ref->EndCombat();
        }
        else
            ++it;
    }

    auto it2 = _pvpRefs.begin(), end2 = _pvpRefs.end();
    while (it2 != end2)
    {
        CombatReference* const ref = it2->second;
        Unit* other = ref->GetOther();
        if (!other || !CanBeginCombat(_owner, other))
        {
            it2 = _pvpRefs.erase(it2), end2 = _pvpRefs.end(); // erase manually here to avoid iterator invalidation
            ref->EndCombat();
        }
        else
            ++it2;
    }
}

void CombatManager::EndAllPvPCombat()
{
    while (!_pvpRefs.empty())
        _pvpRefs.begin()->second->EndCombat();
}

/*static*/ void CombatManager::NotifyAICombat(Unit* me, Unit* other)
{
    if (UnitAI* ai = me->GetAI())
        ai->JustEnteredCombat(other);
}

void CombatManager::PutOwnHalf(ObjectGuid const& guid, bool pvp, bool suppressed)
{
    CombatReference* ref = pvp
        ? static_cast<CombatReference*>(new PvPCombatReference(_owner, guid))
        : new CombatReference(_owner, guid, false);
    if (suppressed)
        ref->Suppress();
    PutReference(guid, ref);
}

void CombatManager::ApplyCombatRef(ObjectGuid const& otherGuid, bool isPvP, bool suppressed)
{
    if (isPvP)
    {
        if (PvPCombatReference* existing = Acore::Containers::MapGetValuePtr(_pvpRefs, otherGuid))
        {
            existing->RefreshTimer();
            existing->Refresh();
            return;
        }
    }
    else if (CombatReference* existing = Acore::Containers::MapGetValuePtr(_pveRefs, otherGuid))
    {
        existing->Refresh();
        return;
    }

    Unit* other = ObjectAccessor::GetUnit(*_owner, otherGuid);
    if (!other || !other->IsInWorld() || !CanBeginCombat(_owner, other))
        return;

    PutOwnHalf(otherGuid, isPvP, suppressed);
    if (UpdateOwnerCombatState())
        NotifyAICombat(_owner, other);
}

void CombatManager::ApplyCombatEnd(ObjectGuid const& otherGuid, bool isPvP)
{
    _owner->GetThreatMgr().ClearThreat(otherGuid);

    CombatReference* ref = nullptr;
    if (isPvP)
    {
        auto it = _pvpRefs.find(otherGuid);
        if (it == _pvpRefs.end())
            return;
        ref = it->second;
        _pvpRefs.erase(it);
    }
    else
    {
        auto it = _pveRefs.find(otherGuid);
        if (it == _pveRefs.end())
            return;
        ref = it->second;
        _pveRefs.erase(it);
    }

    bool const needAI = UpdateOwnerCombatState();
    if (needAI)
        if (UnitAI* ai = _owner->GetAI())
            ai->JustExitedCombat();

    delete ref;
}

void CombatManager::PutReference(ObjectGuid const& guid, CombatReference* ref)
{
    if (ref->_isPvP)
    {
        auto& inMap = _pvpRefs[guid];
        ASSERT(!inMap, "Duplicate combat state at %p being inserted for %s vs %s - memory leak!", (void*)ref, _owner->GetGUID().ToString().c_str(), guid.ToString().c_str());
        inMap = static_cast<PvPCombatReference*>(ref);
    }
    else
    {
        auto& inMap = _pveRefs[guid];
        ASSERT(!inMap, "Duplicate combat state at %p being inserted for %s vs %s - memory leak!", (void*)ref, _owner->GetGUID().ToString().c_str(), guid.ToString().c_str());
        inMap = ref;
    }
}

void CombatManager::PurgeReference(ObjectGuid const& guid, bool pvp)
{
    if (pvp)
        _pvpRefs.erase(guid);
    else
        _pveRefs.erase(guid);
}

bool CombatManager::UpdateOwnerCombatState() const
{
    bool const combatState = HasCombat();
    if (combatState == _owner->IsInCombat())
        return false;

    if (combatState)
    {
        _owner->SetUnitFlag(UNIT_FLAG_IN_COMBAT);
        _owner->AtEnterCombat();
        if (!_owner->IsCreature())
            _owner->AtEngage(GetAnyTarget());

        if (Player* player = _owner->ToPlayer())
            sScriptMgr->OnPlayerEnterCombat(player, GetAnyTarget());
    }
    else
    {
        _owner->RemoveUnitFlag(UNIT_FLAG_IN_COMBAT);
        _owner->AtExitCombat();
        if (!_owner->IsCreature())
            _owner->AtDisengage();

        if (Player* player = _owner->ToPlayer())
            sScriptMgr->OnPlayerLeaveCombat(player);
    }

    if (Unit* master = _owner->GetCharmerOrOwner())
        master->UpdatePetCombatState();

    return true;
}

void CombatManager::SetEvadeState(EvadeState state)
{
    if (_evadeState == state)
        return;

    if (state == EVADE_STATE_NONE)
        _evadeTimer = 0;

    _evadeState = state;

    // Propagate evade state to controlled units (pets, guardians, etc.)
    if (!_owner->HasUnitFlag(UNIT_FLAG_PLAYER_CONTROLLED))
    {
        for (ObjectGuid const& guid : _owner->m_Controlled)
            if (Unit* controlled = ObjectAccessor::GetUnit(*_owner, guid))
                controlled->GetCombatManager().SetEvadeState(state);
    }
}

void CombatManager::StopEvade()
{
    _evadeTimer = 0;
    SetEvadeState(EVADE_STATE_NONE);
}
