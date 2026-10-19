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

#include "KillRewarder.h"
#include "BeatContext.h"
#include "CombatRecords.h"
#include "Creature.h"
#include "Formulas.h"
#include "Group.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraDefines.h"
#include "SpellAuraEffects.h"
#include "UnitRecordLog.h"

// KillRewarder incapsulates logic of rewarding player upon kill with:
// * XP;
// * honor;
// * reputation;
// * kill credit (for quest objectives).
// Rewarding is initiated in two cases: when player kills unit in Unit::Kill()
// and on battlegrounds in Battleground::RewardXPAtKill().
//
// Rewarding algorithm is:
// 1. Initialize internal variables to default values.
// 2. In case when player is in group, initialize variables necessary for group calculations:
// 2.1. _count - number of alive group members within reward distance;
// 2.2. _aliveSumLevel - sum of levels of alive group members within reward distance;
// 2.3. _sumLevel - sum of levels of group members within reward distance;
// 2.4. _maxLevel - maximum level of alive group member within reward distance;
// 2.5. _maxNotGrayMember - maximum level of alive group member within reward distance,
//      for whom victim is not gray;
// 2.6. _isFullXP - flag identifying that for all group members victim is not gray,
//      so 100% XP will be rewarded (50% otherwise).
// 3. Reward killer (and group, if necessary).
// 3.1. If killer is in group, reward group.
// 3.1.1. Initialize initial XP amount based on maximum level of group member,
//        for whom victim is not gray.
// 3.1.2. Alter group rate if group is in raid (not for battlegrounds).
// 3.1.3. Reward each group member (even dead) within reward distance (see 4. for more details).
// 3.2. Reward single killer (not group case).
// 3.2.1. Initialize initial XP amount based on killer's level.
// 3.2.2. Reward killer (see 4. for more details).
// 4. Reward player.
// 4.1. Give honor (player must be alive and not on BG).
// 4.2. Give XP.
// 4.2.1. If player is in group, adjust XP:
//        * set to 0 if player's level is more than maximum level of not gray member;
//        * cut XP in half if _isFullXP is false.
// 4.2.2. Apply auras modifying rewarded XP.
// 4.2.3. Give XP to player.
// 4.2.4. If player has pet, reward pet with XP (100% for single player, 50% for group case).
// 4.3. Give reputation (player must not be on BG).
// 4.4. Give kill credit (player must not be in group, or he must be alive or without corpse).
// 5. Credit instance encounter.
KillRewarder::KillRewarder(Player* killer, Unit* victim, bool isBattleGround) :
// 1. Initialize internal variables to default values.
        _killer(killer), _victim(victim), _group(killer->GetGroup()),
        _groupRate(1.0f), _maxNotGrayMember(nullptr), _maxNotGrayMemberLevel(0), _count(0), _aliveSumLevel(0), _sumLevel(0), _xp(0),
        _isFullXP(false), _maxLevel(0), _isBattleGround(isBattleGround), _isPvP(false),
        _fromCredit(false), _creditInGroup(false), _creditVictimEntry(victim->GetEntry()), _creditVictimLevel(victim->GetLevel())
{
    // mark the credit as pvp if victim is player
    if (victim->IsPlayer())
        _isPvP = true;
        // or if its owned by player and its not a vehicle
    else if (victim->GetCharmerOrOwnerGUID().IsPlayer())
        _isPvP = !victim->IsVehicle();

    _InitGroupData();
}

KillRewarder::KillRewarder(Player* member, Unit* victim, KillCreditRecord const& credit) :
        _killer(member), _victim(victim), _group(nullptr),
        _groupRate(credit.groupRate), _maxNotGrayMember(nullptr), _maxNotGrayMemberLevel(credit.maxNotGrayMemberLevel),
        _count(credit.count), _aliveSumLevel(credit.aliveSumLevel), _sumLevel(0), _xp(credit.xp),
        _isFullXP(credit.isFullXP != 0), _maxLevel(credit.maxLevel), _isBattleGround(credit.isBattleGround != 0), _isPvP(credit.isPvP != 0),
        _fromCredit(true), _creditInGroup(credit.inGroup != 0), _creditVictimEntry(credit.victimEntry), _creditVictimLevel(credit.victimLevel)
{
    // _maxNotGrayMember is only compared for null in _RewardXP; the member stands in for "there is one".
    if (credit.hasMaxNotGrayMember)
        _maxNotGrayMember = member;
}

void KillRewarder::RewardFromCredit(Player* member, Unit* victim, KillCreditRecord const& credit)
{
    KillRewarder rewarder(member, victim, credit);
    rewarder._RewardPlayer(member, credit.isDungeon != 0);
}

void KillRewarder::_FillCredit(KillCreditRecord& credit, bool isDungeon) const
{
    credit.victim = _victim ? _victim->GetGUID() : ObjectGuid::Empty;
    credit.killer = _killer->GetGUID();
    credit.victimEntry = _creditVictimEntry;
    credit.victimLevel = _creditVictimLevel;
    credit.xp = _xp;
    credit.aliveSumLevel = _aliveSumLevel;
    credit.count = _count;
    credit.groupRate = _groupRate;
    credit.maxLevel = _maxLevel;
    credit.maxNotGrayMemberLevel = _maxNotGrayMemberLevel;
    credit.hasMaxNotGrayMember = _maxNotGrayMember != nullptr;
    credit.isFullXP = _isFullXP;
    credit.isDungeon = isDungeon;
    credit.isBattleGround = _isBattleGround;
    credit.isPvP = _isPvP;
    credit.inGroup = _fromCredit ? _creditInGroup : _group != nullptr;
}

void KillRewarder::_RewardOrRecord(Player* player, bool isDungeon)
{
    // Kill runs on the victim's task; a member outside it gets its share as a KillCreditRecord (plan §7.3a).
    if (!_victim || !BeatContext::IsForeign(player))
    {
        _RewardPlayer(player, isDungeon);
        return;
    }

    KillCreditRecord credit;
    _FillCredit(credit, isDungeon);
    sRecordLogs->Append(_victim->GetGUID(), player, RecordKind::KillCredit, credit);
}

void KillRewarder::_InitGroupData()
{
    if (_group)
    {
        // 2. In case when player is in group, initialize variables necessary for group calculations:
        for (GroupReference* itr = _group->GetFirstMember(); itr != nullptr; itr = itr->next())
            if (Player* member = itr->GetSource())
                if ((_killer == member || member->IsAtGroupRewardDistance(_victim)))
                {
                    const uint8 lvl = _GetPlayerLevel(member);
                    if (member->IsAlive())
                    {
                        // 2.1. _count - number of alive group members within reward distance;
                        ++_count;
                        // 2.2. _aliveSumLevel - sum of levels of alive group members within reward distance;
                        _aliveSumLevel += lvl;
                        // 2.3. _maxLevel - maximum level of alive group member within reward distance;
                        if (_maxLevel < lvl)
                        {
                            _maxLevel = lvl;
                        }
                        // 2.4. _maxNotGrayMember - maximum level of alive group member within reward distance,
                        //      for whom victim is not gray;
                        uint32 grayLevel = Acore::XP::GetGrayLevel(lvl);
                        if (_victim->GetLevel() > grayLevel && (!_maxNotGrayMember || _maxNotGrayMemberLevel < lvl))
                        {
                            _maxNotGrayMember = member;
                            _maxNotGrayMemberLevel = lvl;
                        }
                    }
                    // 2.5. _sumLevel - sum of levels of group members within reward distance;
                    _sumLevel += lvl;
                }
        // 2.6. _isFullXP - flag identifying that for all group members victim is not gray,
        //      so 100% XP will be rewarded (50% otherwise).
        _isFullXP = _maxNotGrayMember && (_maxLevel == _maxNotGrayMemberLevel);
    }
    else
        _count = 1;
}

void KillRewarder::_InitXP(Player* player)
{
    // Get initial value of XP for kill.
    // XP is given:
    // * on battlegrounds;
    // * otherwise, not in PvP;
    // * not if killer is on vehicle.
    if (_victim && (_isBattleGround || (!_isPvP && !_killer->GetVehicle())))
        _xp = Acore::XP::Gain(player, _victim, _isBattleGround);

    if (_xp && !_isBattleGround && _victim) // pussywizard: npcs with relatively low hp give lower exp
        if (_victim->IsCreature())
            if (CreatureTemplate const* ct = _victim->ToCreature()->GetCreatureTemplate())
                if (ct->ModHealth <= 0.75f && ct->ModHealth >= 0.0f)
                    _xp = uint32(_xp * ct->ModHealth);
}

void KillRewarder::_RewardHonor(Player* player)
{
    // Rewarded player must be alive.
    if (player->IsAlive() && _victim)
        player->RewardHonor(_victim, _count, -1);
}

void KillRewarder::_RewardXP(Player* player, float rate)
{
    bool const inGroup = _fromCredit ? _creditInGroup : _group != nullptr;
    uint32 xp(_xp);
    if (inGroup)
    {
        // 4.2.1. If player is in group, adjust XP:
        //        * set to 0 if player's level is more than maximum level of not gray member;
        //        * cut XP in half if _isFullXP is false.
        if (_maxNotGrayMember && player->IsAlive() &&
            _maxNotGrayMemberLevel >= _GetPlayerLevel(player))
            xp = _isFullXP ?
                 uint32(xp * rate) :             // Reward FULL XP if all group members are not gray.
                 uint32(xp * rate / 2) + 1;      // Reward only HALF of XP if some of group members are gray.
        else
            xp = 0;
    }

    // An ungrouped helper who out-levels everyone eligible for the kill and for
    // whom the victim is gray halves the tagger's XP, matching the group
    // gray-member penalty. Closes a power-leveling exploit.
    if (xp && _victim)
        if (Creature* creature = _victim->ToCreature())
        {
            uint8 const referenceLevel = inGroup ? _maxLevel : player->GetLevel();
            uint8 const highestLevel = creature->GetHighestPlayerAttackerLevel();
            if (highestLevel > referenceLevel && creature->GetLevel() <= Acore::XP::GetGrayLevel(highestLevel))
                xp = xp / 2 + 1;
        }

    if (xp)
    {
        // 4.2.2. Apply auras modifying rewarded XP (SPELL_AURA_MOD_XP_PCT).
        xp *= player->GetTotalAuraMultiplier(SPELL_AURA_MOD_XP_PCT);

        // 4.2.3. Give XP to player.
        sScriptMgr->OnPlayerGiveXP(player, xp, _victim, PlayerXPSource::XPSOURCE_KILL);
        player->GiveXP(xp, _victim, _groupRate);
        if (Pet* pet = player->GetPet())
            // 4.2.4. If player has pet, reward pet with XP (100% for single player, 50% for group case).
            pet->GivePetXP(inGroup ? xp / 2 : xp);
    }
}

void KillRewarder::_RewardReputation(Player* player)
{
    // 4.3. Give reputation (player must not be on BG).
    // Even dead players and corpses are rewarded.
    // A KillCredit whose victim already left the world (a summon despawning on death) gives no reputation.
    if (_victim)
        player->RewardReputation(_victim);
}

void KillRewarder::_RewardKillCredit(Player* player)
{
    bool const inGroup = _fromCredit ? _creditInGroup : _group != nullptr;
    // 4.4. Give kill credit (player must not be in group, or he must be alive or without corpse).
    if (!inGroup || player->IsAlive() || !player->GetCorpse())
    {
        if (!_victim)
        {
            // KillCredit with the victim gone: credit by entry and the guid the record carries.
            if (CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(_creditVictimEntry))
            {
                player->KilledMonster(ct, ObjectGuid::Empty);
                player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_KILL_CREATURE_TYPE, ct->type, 1, nullptr);
            }
            return;
        }

        if (Creature* target = _victim->ToCreature())
        {
            player->KilledMonster(target->GetCreatureTemplate(), target->GetGUID());
            player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_KILL_CREATURE_TYPE, target->GetCreatureType(), 1, target);
        }
    }
}

void KillRewarder::_RewardPlayer(Player* player, bool isDungeon)
{
    bool const inGroup = _fromCredit ? _creditInGroup : _group != nullptr;
    // 4. Reward player.
    if (!_isBattleGround)
    {
        // 4.1. Give honor (player must be alive and not on BG).
        _RewardHonor(player);
        // 4.1.1 Send player killcredit for quests with PlayerSlain
        if (_victim ? _victim->IsPlayer() : _isPvP)
            player->KilledPlayerCredit();
    }

    // Give XP only in PvE or in battlegrounds.
    // Give reputation and kill credit only in PvE.
    if (!_isPvP || _isBattleGround)
    {
        float xpRate = inGroup && _aliveSumLevel ? _groupRate * float(_GetPlayerLevel(player)) / _aliveSumLevel : /*Personal rate is 100%.*/ 1.0f; // Group rate depends on the sum of levels.
        sScriptMgr->OnPlayerRewardKillRewarder(player, this, isDungeon, xpRate);                                                // Personal rate is 100%.

        if (_xp)
        {
            // 4.2. Give XP.
            _RewardXP(player, xpRate);
        }
        if (!_isBattleGround)
        {
            // If killer is in dungeon then all members receive full reputation at kill.
            _RewardReputation(player);
            _RewardKillCredit(player);
        }
    }
}

void KillRewarder::_RewardGroup()
{
    if (_maxLevel)
    {
        if (_maxNotGrayMember)
            // 3.1.1. Initialize initial XP amount based on maximum level of group member,
            //        for whom victim is not gray.
            _InitXP(_maxNotGrayMember);
        // To avoid unnecessary calculations and calls,
        // proceed only if XP is not ZERO or player is not on battleground
        // (battleground rewards only XP, that's why).
        if (!_isBattleGround || _xp)
        {
            const bool isDungeon = !_isPvP && sMapStore.LookupEntry(_killer->GetMapId())->IsDungeon();
            if (!_isBattleGround)
            {
                // 3.1.2. Alter group rate if group is in raid (not for battlegrounds).
                const bool isRaid = !_isPvP && sMapStore.LookupEntry(_killer->GetMapId())->IsRaid() && _group->isRaidGroup();
                _groupRate = Acore::XP::xp_in_group_rate(_count, isRaid);
            }

            // 3.1.3. Reward each group member (even dead or corpse) within reward distance.
            for (GroupReference* itr = _group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                if (Player* member = itr->GetSource())
                {
                    if (_killer == member || member->IsAtGroupRewardDistance(_victim))
                    {
                        _RewardOrRecord(member, isDungeon);
                        // Xinef: only count players
                        //if (_victim->IsPlayer())
                        //    member->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_SPECIAL_PVP_KILL, 1, 0, _victim);
                    }
                }
            }
        }
    }
}

uint8 KillRewarder::_GetPlayerLevel(Player const* player)
{
    uint8 level = player->GetLevel();
    sScriptMgr->OnPlayerBeforeGetLevelForXPGain(player, level);
    return level;
}

void KillRewarder::Reward()
{
    // 3. Reward killer (and group, if necessary).
    if (_group)
        // 3.1. If killer is in group, reward group.
        _RewardGroup();
    else
    {
        // 3.2. Reward single killer (not group case).
        // 3.2.1. Initialize initial XP amount based on killer's level.
        _InitXP(_killer);
        // To avoid unnecessary calculations and calls,
        // proceed only if XP is not ZERO or player is not on battleground
        // (battleground rewards only XP, that's why).
        if (!_isBattleGround || _xp)
            // 3.2.2. Reward killer.
            if (_killer->IsInMap(_victim)) // pussywizard: killer may be on other map (crashfix), when killing in a group same map is required, so its not a problem
                _RewardOrRecord(_killer, false);
    }

    // 5. Credit instance encounter.
    if (Creature* victim = _victim->ToCreature())
        if (victim->IsDungeonBoss())
            if (Map* map = _victim->FindMap())
                map->UpdateEncounterState(ENCOUNTER_CREDIT_KILL_CREATURE, _victim->GetEntry(), _victim);
}

Unit* KillRewarder::GetVictim()
{
    return _victim;
}

Player* KillRewarder::GetKiller()
{
    return _killer;
}
