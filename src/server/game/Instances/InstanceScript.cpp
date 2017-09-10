/*
 * Copyright (C) 2008-2012 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
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

#include "InstanceScript.h"
#include "DatabaseEnv.h"
#include "Map.h"
#include "Player.h"
#include "GameObject.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Log.h"
#include "LFGMgr.h"

InstanceScript::InstanceScript(Map* map)
{
    instance = map;
    completedEncounters = 0;

    m_InstanceGuid = MAKE_NEW_GUID(map->GetId(), 0, HighGuid::HIGHGUID_INSTANCE_SAVE);
    m_BeginningTime = 0;
    m_ScenarioID = 0;
    m_ScenarioStep = 0;
    m_EncounterTime = 0;
}

void InstanceScript::SaveToDB()
{
    std::string data = GetSaveData();
    if (data.empty())
        return;

    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_INSTANCE_DATA);
    stmt->setUInt32(0, GetCompletedEncounterMask());
    stmt->setString(1, data);
    stmt->setUInt32(2, instance->GetInstanceId());
    CharacterDatabase.Execute(stmt);
}

void InstanceScript::HandleGameObject(uint64 GUID, bool open, GameObject* go)
{
    if (!go)
        go = instance->GetGameObject(GUID);

    if (go)
        go->SetGoState(open ? GO_STATE_ACTIVE : GO_STATE_READY);
    else
        sLog->outDebug(LOG_FILTER_TSCR, "InstanceScript: HandleGameObject failed");
}

bool InstanceScript::IsEncounterInProgress() const
{
    if (bosses.empty())
        return false;

    for (std::vector<BossInfo>::const_iterator itr = bosses.begin(); itr != bosses.end(); ++itr)
        if (itr->state == IN_PROGRESS)
            return true;

    return false;
}

void InstanceScript::LoadMinionData(const MinionData* data)
{
    if (bosses.empty())
        return;

    while (data->entry)
    {
        if (data->bossId < bosses.size())
            minions.insert(std::make_pair(data->entry, MinionInfo(&bosses[data->bossId])));

        ++data;
    }
    sLog->outDebug(LOG_FILTER_TSCR, "InstanceScript::LoadMinionData: " UI64FMTD " minions loaded.", uint64(minions.size()));
}

void InstanceScript::LoadDoorData(const DoorData* data)
{
    if (bosses.empty())
        return;

    while (data->entry)
    {
        if (data->bossId < bosses.size())
            doors.insert(std::make_pair(data->entry, DoorInfo(&bosses[data->bossId], data->type, BoundaryType(data->boundary))));

        ++data;
    }
    sLog->outDebug(LOG_FILTER_TSCR, "InstanceScript::LoadDoorData: " UI64FMTD " doors loaded.", uint64(doors.size()));
}

void InstanceScript::UpdateMinionState(Creature* minion, EncounterState state)
{
    switch (state)
    {
        case NOT_STARTED:
            if (!minion->IsAlive())
                minion->Respawn();
            else if (minion->isInCombat())
                minion->AI()->EnterEvadeMode();
            break;
        case IN_PROGRESS:
            if (!minion->IsAlive())
                minion->Respawn();
            else if (!minion->getVictim())
                minion->AI()->DoZoneInCombat();
            break;

        default: break;
    }
}

void InstanceScript::UpdateDoorState(GameObject* door)
{
    DoorInfoMap::iterator lower = doors.lower_bound(door->GetEntry());
    DoorInfoMap::iterator upper = doors.upper_bound(door->GetEntry());
    if (lower == upper)
        return;

    bool open = true;

    for (DoorInfoMap::iterator itr = lower; itr != upper && open; ++itr)
    {
        switch (itr->second.type)
        {
            case DOOR_TYPE_ROOM:
                open = (itr->second.bossInfo->state != IN_PROGRESS);
                break;
            case DOOR_TYPE_PASSAGE:
                open = (itr->second.bossInfo->state == DONE);
                break;
            case DOOR_TYPE_SPAWN_HOLE:
                open = (itr->second.bossInfo->state == IN_PROGRESS);
                break;
            default:
                break;
        }
    }

    door->SetGoState(open ? GO_STATE_ACTIVE : GO_STATE_READY);
}

void InstanceScript::AddDoor(GameObject* door, bool add)
{
    DoorInfoMap::iterator lower = doors.lower_bound(door->GetEntry());
    DoorInfoMap::iterator upper = doors.upper_bound(door->GetEntry());
    if (lower == upper)
        return;

    for (DoorInfoMap::iterator itr = lower; itr != upper; ++itr)
    {
        DoorInfo const& data = itr->second;

        if (add)
        {
            data.bossInfo->door[data.type].insert(door);
            switch (data.boundary)
            {
                default:
                case BOUNDARY_NONE:
                    break;
                case BOUNDARY_N:
                case BOUNDARY_S:
                    data.bossInfo->boundary[data.boundary] = door->GetPositionX();
                    break;
                case BOUNDARY_E:
                case BOUNDARY_W:
                    data.bossInfo->boundary[data.boundary] = door->GetPositionY();
                    break;
                case BOUNDARY_NW:
                case BOUNDARY_SE:
                    data.bossInfo->boundary[data.boundary] = door->GetPositionX() + door->GetPositionY();
                    break;
                case BOUNDARY_NE:
                case BOUNDARY_SW:
                    data.bossInfo->boundary[data.boundary] = door->GetPositionX() - door->GetPositionY();
                    break;
            }
        }
        else data.bossInfo->door[data.type].erase(door);
    }

    if (add)
        UpdateDoorState(door);
}

void InstanceScript::AddMinion(Creature* minion, bool add)
{
    MinionInfoMap::iterator itr = minions.find(minion->GetEntry());
    if (itr == minions.end())
        return;

    if (add)
        itr->second.bossInfo->minion.insert(minion);
    else
        itr->second.bossInfo->minion.erase(minion);
}

bool InstanceScript::SetBossState(uint32 id, EncounterState state)
{
    if (bosses.empty())
        return false;

    if (id < bosses.size())
    {
        BossInfo* bossInfo = &bosses[id];
        BossScenarios* bossScenario = &bossesScenarios[id];
        if (!bossInfo)
            return false;

        if (bossInfo->state == TO_BE_DECIDED) // loading
        {
            bossInfo->state = state;
            //sLog->outError(LOG_FILTER_GENERAL, "Inialize boss %u state as %u.", id, (uint32)state);
            return false;
        }
        else
        {
            if (bossInfo->state == state)
                return false;

            if (state == DONE)
            {
                if (!bossInfo->minion.empty())
                    for (MinionSet::iterator i = bossInfo->minion.begin(); i != bossInfo->minion.end(); ++i)
                        if ((*i)->isWorldBoss() && (*i)->IsAlive())
                            return false;

                SendScenarioProgressUpdate(CriteriaProgressData(bossScenario->m_ScenarioID, 1, m_InstanceGuid, time(NULL), m_BeginningTime, 0));
                SendScenarioState(ScenarioData(m_ScenarioID, ++m_ScenarioStep));
            }

            bossInfo->state = state;
            SaveToDB();
        }

        for (uint32 type = 0; type < MAX_DOOR_TYPES; ++type)
            if (!bossInfo->door[type].empty())
                for (DoorSet::iterator i = bossInfo->door[type].begin(); i != bossInfo->door[type].end(); ++i)
                    UpdateDoorState(*i);

        if (!bossInfo->minion.empty())
            for (MinionSet::iterator i = bossInfo->minion.begin(); i != bossInfo->minion.end(); ++i)
                UpdateMinionState(*i, state);

        return true;
    }

    return false;
}

std::string InstanceScript::LoadBossState(const char * data)
{
    if (!data || bosses.empty())
        return "";

    std::istringstream loadStream(data);
    uint32 buff;
    uint32 bossId = 0;

    for (std::vector<BossInfo>::iterator i = bosses.begin(); i != bosses.end(); ++i, ++bossId)
    {
        loadStream >> buff;
        if (buff < TO_BE_DECIDED)
            SetBossState(bossId, (EncounterState)buff);
    }

    return loadStream.str();
}

std::string InstanceScript::GetBossSaveData()
{
    if (bosses.empty())
        return "";

    std::ostringstream saveStream;

    for (std::vector<BossInfo>::iterator i = bosses.begin(); i != bosses.end(); ++i)
        saveStream << (uint32)i->state << ' ';

    return saveStream.str();
}

void InstanceScript::DoUseDoorOrButton(uint64 uiGuid, uint32 uiWithRestoreTime, bool bUseAlternativeState)
{
    if (!uiGuid)
        return;

    GameObject* go = instance->GetGameObject(uiGuid);

    if (go)
    {
        if (go->GetGoType() == GAMEOBJECT_TYPE_DOOR || go->GetGoType() == GAMEOBJECT_TYPE_BUTTON)
        {
            if (go->getLootState() == GO_READY)
                go->UseDoorOrButton(uiWithRestoreTime, bUseAlternativeState);
            else if (go->getLootState() == GO_ACTIVATED)
                go->ResetDoorOrButton();
        }
        else sLog->outError(LOG_FILTER_GENERAL, "SD2: Script call DoUseDoorOrButton, but gameobject entry %u is type %u.", go->GetEntry(), go->GetGoType());
    }
}

void InstanceScript::DoRespawnGameObject(uint64 uiGuid, uint32 uiTimeToDespawn)
{
    if (GameObject* go = instance->GetGameObject(uiGuid))
    {
        //not expect any of these should ever be handled
        if (go->GetGoType() == GAMEOBJECT_TYPE_FISHINGNODE || go->GetGoType() == GAMEOBJECT_TYPE_DOOR ||
            go->GetGoType() == GAMEOBJECT_TYPE_BUTTON || go->GetGoType() == GAMEOBJECT_TYPE_TRAP)
            return;

        if (go->isSpawned())
            return;

        go->SetRespawnTime(uiTimeToDespawn);
        go->UpdateObjectVisibility();
    }
}

void InstanceScript::DoUpdateWorldState(uint32 uiStateId, uint32 uiStateData)
{
    Map::PlayerList const& lPlayers = instance->GetPlayers();

    if (!lPlayers.isEmpty())
    {
        for (Map::PlayerList::const_iterator itr = lPlayers.begin(); itr != lPlayers.end(); ++itr)
            if (Player* player = itr->getSource())
                player->SendUpdateWorldState(uiStateId, uiStateData);
    }
    else sLog->outDebug(LOG_FILTER_TSCR, "DoUpdateWorldState attempt send data but no players in map.");
}

// Send Notify to all players in instance
void InstanceScript::DoSendNotifyToInstance(char const* format, ...)
{
    InstanceMap::PlayerList const& players = instance->GetPlayers();

    if (!players.isEmpty())
    {
        va_list ap;
        va_start(ap, format);
        char buff[1024];
        vsnprintf(buff, 1024, format, ap);
        va_end(ap);
        for (Map::PlayerList::const_iterator i = players.begin(); i != players.end(); ++i)
            if (Player* player = i->getSource())
                if (WorldSession* session = player->GetSession())
                    session->SendNotification("%s", buff);
    }
}

// Reset Achievement Criteria for all players in instance
void InstanceScript::DoResetAchievementCriteria(AchievementCriteriaTypes type, uint64 miscValue1 /*= 0*/, uint64 miscValue2 /*= 0*/, bool evenIfCriteriaComplete /*= false*/)
{
    Map::PlayerList const &plrList = instance->GetPlayers();

    if (!plrList.isEmpty())
        for (Map::PlayerList::const_iterator i = plrList.begin(); i != plrList.end(); ++i)
            if (Player* pPlayer = i->getSource())
                pPlayer->GetAchievementMgr().ResetAchievementCriteria(type, miscValue1, miscValue2, evenIfCriteriaComplete);
}

// Complete Achievement for all players in instance
void InstanceScript::DoCompleteAchievement(uint32 achievement)
{
  AchievementEntry const* pAE = sAchievementStore.LookupEntry(achievement);
  Map::PlayerList const &plrList = instance->GetPlayers();
  if (!pAE)
      return;

  if (!plrList.isEmpty())
      for (Map::PlayerList::const_iterator i = plrList.begin(); i != plrList.end(); ++i)
          if (Player *pPlayer = i->getSource())
              pPlayer->CompletedAchievement(pAE);
}

// Update Achievement Criteria for all players in instance
void InstanceScript::DoUpdateAchievementCriteria(AchievementCriteriaTypes type, uint32 miscValue1 /*= 0*/, uint32 miscValue2 /*= 0*/, uint32 miscValue3 /*=0*/, Unit* unit /*= NULL*/)
{
    Map::PlayerList const &plrList = instance->GetPlayers();

    if (!plrList.isEmpty())
        for (Map::PlayerList::const_iterator i = plrList.begin(); i != plrList.end(); ++i)
            if (Player* pPlayer = i->getSource())
                pPlayer->UpdateAchievementCriteria(type, miscValue1, miscValue2, miscValue3, unit);
}

// Start timed achievement for all players in instance
void InstanceScript::DoStartTimedAchievement(AchievementCriteriaTimedTypes type, uint32 entry)
{
    Map::PlayerList const &PlayerList = instance->GetPlayers();

    if (!PlayerList.isEmpty())
        for (Map::PlayerList::const_iterator i = PlayerList.begin(); i != PlayerList.end(); ++i)
            if (Player* player = i->getSource())
                player->GetAchievementMgr().StartTimedAchievement(type, entry);
}

// Stop timed achievement for all players in instance
void InstanceScript::DoStopTimedAchievement(AchievementCriteriaTimedTypes type, uint32 entry)
{
    Map::PlayerList const &PlayerList = instance->GetPlayers();

    if (!PlayerList.isEmpty())
        for (Map::PlayerList::const_iterator i = PlayerList.begin(); i != PlayerList.end(); ++i)
            if (Player* player = i->getSource())
                player->GetAchievementMgr().RemoveTimedAchievement(type, entry);
}

// Remove Auras due to Spell on all players in instance
void InstanceScript::DoRemoveAurasDueToSpellOnPlayers(uint32 spell)
{
    Map::PlayerList const& PlayerList = instance->GetPlayers();
    if (!PlayerList.isEmpty())
    {
        for (Map::PlayerList::const_iterator itr = PlayerList.begin(); itr != PlayerList.end(); ++itr)
        {
            if (Player* player = itr->getSource())
            {
                player->RemoveAurasDueToSpell(spell);
                if (Pet* pet = player->GetPet())
                    pet->RemoveAurasDueToSpell(spell);
            }
        }
    }
}

// Remove aura from stack on all players in instance
void InstanceScript::DoRemoveAuraFromStackOnPlayers(uint32 spell, uint64 casterGUID, AuraRemoveMode mode, uint32 num)
{
    Map::PlayerList const& plrList = instance->GetPlayers();
    if (!plrList.isEmpty())
        for (Map::PlayerList::const_iterator itr = plrList.begin(); itr != plrList.end(); ++itr)
            if (Player* pPlayer = itr->getSource())
                pPlayer->RemoveAuraFromStack(spell, casterGUID, mode, num);
}

// Cast spell on all players in instance
void InstanceScript::DoCastSpellOnPlayers(uint32 spell)
{
    Map::PlayerList const &PlayerList = instance->GetPlayers();

    if (!PlayerList.isEmpty())
        for (Map::PlayerList::const_iterator i = PlayerList.begin(); i != PlayerList.end(); ++i)
            if (Player* player = i->getSource())
                player->CastSpell(player, spell, true);
}

// Add aura on all players in instance
void InstanceScript::DoAddAuraOnPlayers(uint32 spell)
{
    Map::PlayerList const &PlayerList = instance->GetPlayers();

    if (!PlayerList.isEmpty())
        for (Map::PlayerList::const_iterator i = PlayerList.begin(); i != PlayerList.end(); ++i)
            if (Player* player = i->getSource())
                player->AddAura(spell, player);
}

void InstanceScript::DoSetAlternatePowerOnPlayers(int32 value)
{
    Map::PlayerList const &plrList = instance->GetPlayers();

    if (!plrList.isEmpty())
        for (Map::PlayerList::const_iterator i = plrList.begin(); i != plrList.end(); ++i)
            if (Player* pPlayer = i->getSource())
                pPlayer->SetPower(POWER_ALTERNATE_POWER, value);
}

void InstanceScript::DoModifyPlayerCurrencies(uint32 id, int32 value)
{
    Map::PlayerList const &plrList = instance->GetPlayers();

    if (!plrList.isEmpty())
        for (Map::PlayerList::const_iterator i = plrList.begin(); i != plrList.end(); ++i)
            if (Player* pPlayer = i->getSource())
                pPlayer->ModifyCurrency(id, value);
}

void InstanceScript::DoNearTeleportPlayers(const Position pos, bool casting /*=false*/)
{
    Map::PlayerList const &plrList = instance->GetPlayers();

    if (!plrList.isEmpty())
        for (Map::PlayerList::const_iterator i = plrList.begin(); i != plrList.end(); ++i)
            if (Player* pPlayer = i->getSource())
                pPlayer->NearTeleportTo(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), pos.GetOrientation(), casting);
}

void InstanceScript::DoStartMovie(uint32 movieId)
{
    Map::PlayerList const &plrList = instance->GetPlayers();

    if (!plrList.isEmpty())
        for (Map::PlayerList::const_iterator i = plrList.begin(); i != plrList.end(); ++i)
            if (Player* pPlayer = i->getSource())
                pPlayer->SendMovieStart(movieId);

}

void InstanceScript::DoKilledMonsterKredit(uint32 questId, uint32 entry, uint64 guid/* =0*/)
{
    Map::PlayerList const &plrList = instance->GetPlayers();
    
    if (!plrList.isEmpty())
        for (Map::PlayerList::const_iterator i = plrList.begin(); i != plrList.end(); ++i)
            if (Player* pPlayer = i->getSource())
                if (pPlayer->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE)
                    pPlayer->KilledMonsterCredit(entry, guid);
}

bool InstanceScript::CheckAchievementCriteriaMeet(uint32 criteria_id, Player const* /*source*/, Unit const* /*target*/ /*= NULL*/, uint32 /*miscvalue1*/ /*= 0*/)
{
    sLog->outError(LOG_FILTER_GENERAL, "Achievement system call InstanceScript::CheckAchievementCriteriaMeet but instance script for map %u not have implementation for achievement criteria %u",
        instance->GetId(), criteria_id);
    return false;
}

bool InstanceScript::CheckRequiredBosses(uint32 /*bossId*/, Player const* player) const
{
    if (player && player->isGameMaster())
        return true;

    if (instance->GetPlayersCountExceptGMs() > instance->ToInstanceMap()->GetMaxPlayers())
        return false;

    return true;
}

void InstanceScript::SendEncounterUnit(uint32 type, Unit* unit /*= NULL*/, uint8 param1 /*= 0*/, uint8 param2 /*= 0*/)
{
    // size of this packet is at most 15 (usually less)
    WorldPacket data(SMSG_UPDATE_INSTANCE_ENCOUNTER_UNIT, 15);
    data << uint32(type);

    switch (type)
    {
        case ENCOUNTER_FRAME_ENGAGE:
        case ENCOUNTER_FRAME_DISENGAGE:
        case ENCOUNTER_FRAME_UPDATE_PRIORITY:
            if (!unit)
                return;
            data.append(unit->GetPackGUID());
            data << uint8(param1);
            break;
        case ENCOUNTER_FRAME_ADD_TIMER:
        case ENCOUNTER_FRAME_ENABLE_OBJECTIVE:
        case ENCOUNTER_FRAME_DISABLE_OBJECTIVE:
        case ENCOUNTER_FRAME_SET_COMBAT_RES_LIMIT:
            data << uint8(param1);
            break;
        case ENCOUNTER_FRAME_UPDATE_OBJECTIVE:
            data << uint8(param1);
            data << uint8(param2);
            break;
        case ENCOUNTER_FRAME_UNK7:
        case ENCOUNTER_FRAME_ADD_COMBAT_RES_LIMIT:
        case ENCOUNTER_FRAME_RESET_COMBAT_RES_LIMIT:
        default:
            break;
    }

    instance->SendToPlayers(&data);
}

bool InstanceScript::IsWipe()
{
    Map::PlayerList const& PlayerList = instance->GetPlayers();

    if (PlayerList.isEmpty())
        return true;

    for (Map::PlayerList::const_iterator Itr = PlayerList.begin(); Itr != PlayerList.end(); ++Itr)
    {
        Player* player = Itr->getSource();

        if (!player)
            continue;

        if (player->IsAlive() && !player->isGameMaster())
            return false;
    }

    return true;
}

void InstanceScript::UpdateEncounterState(EncounterCreditType type, uint32 creditEntry, Unit* source)
{
    DungeonEncounterList const* encounters = sObjectMgr->GetDungeonEncounterList(instance->GetId(), instance->GetDifficulty());
    if (!encounters)
        encounters = sObjectMgr->GetDungeonEncounterList(instance->GetId(), Difficulty(0));

    if (!encounters)
        return;

    for (DungeonEncounterList::const_iterator itr = encounters->begin(); itr != encounters->end(); ++itr)
    {
        if ((*itr)->creditType == type && (*itr)->creditEntry == creditEntry)
        {
            completedEncounters |= 1 << (*itr)->dbcEntry->encounterIndex;
            sLog->outDebug(LOG_FILTER_TSCR, "Instance %s (instanceId %u) completed encounter %s", instance->GetMapName(), instance->GetInstanceId(), (*itr)->dbcEntry->encounterName);
            if (uint32 dungeonId = (*itr)->lastEncounterDungeon)
            {
                Map::PlayerList const& players = instance->GetPlayers();
                if (!players.isEmpty())
                    for (Map::PlayerList::const_iterator i = players.begin(); i != players.end(); ++i)
                        if (Player* player = i->getSource())
                            if (!source || player->IsAtGroupRewardDistance(source))
                                sLFGMgr->RewardDungeonDoneFor(dungeonId, player);
            }
        }
    }
}

void InstanceScript::UpdatePhasing()
{
    PhaseUpdateData phaseUpdateData;
    phaseUpdateData.AddConditionType(CONDITION_INSTANCE_DATA);

    Map::PlayerList const& players = instance->GetPlayers();
    for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        if (Player* player = itr->getSource())
            player->GetPhaseMgr().NotifyConditionChanged(phaseUpdateData);
}

void InstanceScript::OnPlayerEnter(Player* p_Player)
{
    SendScenarioState(ScenarioData(m_ScenarioID, m_ScenarioStep), p_Player);
}

void InstanceScript::SetBossNumber(uint32 p_Number)
{
    bosses.resize(p_Number);
    bossesScenarios.resize(p_Number);
}

void InstanceScript::LoadScenariosInfos(BossScenarios const* p_Scenarios, uint32 p_ScenarioID)
{
    while (p_Scenarios->m_ScenarioID)
    {
        if (p_Scenarios->m_BossID < bosses.size())
            bossesScenarios[p_Scenarios->m_BossID] = BossScenarios(p_Scenarios->m_BossID, p_Scenarios->m_ScenarioID);

        ++p_Scenarios;
    }

    m_ScenarioID = p_ScenarioID;
}

void InstanceScript::ScheduleBeginningTimeUpdate(uint32 p_Diff)
{
    m_BeginningTime += p_Diff;
}

void InstanceScript::SendScenarioState(ScenarioData p_Data, Player* p_Player /*= nullptr*/)
{
    WorldPacket data(Opcodes::SMSG_SCENARIO_STATE);

    data << int32(0);
    data << int32(p_Data.m_StepID);
    data << int32(p_Data.m_ScenarioID);
    data << int32(0);
    data << int32(0);
    data << int32(0);
    data << int32(0);

    data.WriteBit(false);
    data.WriteBits(p_Data.m_CriteriaCount, 21);

    ByteBuffer byteBuffer;

    for (CriteriaProgressData progressData : p_Data.m_CriteriaProgress)
    {
        ObjectGuid guid = progressData.m_Guid;
        ObjectGuid quantity = progressData.m_Quantity;

        data.WriteBit(guid[2]);
        data.WriteBits(progressData.m_Flags, 4);
        data.WriteBit(guid[6]);
        data.WriteBit(quantity[2]);
        data.WriteBit(quantity[7]);
        data.WriteBit(guid[1]);
        data.WriteBit(quantity[3]);
        data.WriteBit(quantity[5]);
        data.WriteBit(guid[7]);
        data.WriteBit(guid[3]);
        data.WriteBit(guid[5]);
        data.WriteBit(quantity[4]);
        data.WriteBit(quantity[0]);
        data.WriteBit(quantity[1]);
        data.WriteBit(guid[4]);
        data.WriteBit(quantity[6]);
        data.WriteBit(guid[0]);

        byteBuffer.WriteByteSeq(guid[6]);
        byteBuffer.WriteByteSeq(quantity[3]);
        byteBuffer.WriteByteSeq(guid[3]);
        byteBuffer << uint32(0);
        byteBuffer << uint32(0);
        byteBuffer.WriteByteSeq(guid[0]);
        byteBuffer.WriteByteSeq(quantity[7]);
        byteBuffer.WriteByteSeq(guid[2]);
        byteBuffer.WriteByteSeq(quantity[1]);
        byteBuffer.WriteByteSeq(quantity[6]);
        byteBuffer.WriteByteSeq(quantity[0]);
        byteBuffer.WriteByteSeq(guid[1]);
        byteBuffer.WriteByteSeq(quantity[2]);
        byteBuffer.WriteByteSeq(guid[5]);
        byteBuffer.WriteByteSeq(quantity[4]);
        byteBuffer << uint32(0);
        byteBuffer.WriteByteSeq(guid[4]);
        byteBuffer.WriteByteSeq(quantity[5]);
        byteBuffer.WriteByteSeq(guid[7]);
    }

    data.WriteBit(false);

    if (byteBuffer.size())
        data.append(byteBuffer);

    if (p_Player == nullptr)
        instance->SendToPlayers(&data);
    else
        p_Player->SendDirectMessage(&data);
}

void InstanceScript::SendScenarioProgressUpdate(CriteriaProgressData p_Data, Player* p_Player /*= nullptr*/)
{
    ObjectGuid guid = p_Data.m_Guid;
    ObjectGuid quantity = p_Data.m_Quantity;
    WorldPacket data(Opcodes::SMSG_SCENARIO_PROGRESS_UPDATE);

    data.WriteBit(guid[7]);
    data.WriteBit(guid[0]);
    data.WriteBit(guid[4]);
    data.WriteBits(p_Data.m_Flags, 4);
    data.WriteBit(quantity[3]);
    data.WriteBit(quantity[4]);
    data.WriteBit(quantity[0]);
    data.WriteBit(guid[6]);
    data.WriteBit(quantity[2]);
    data.WriteBit(guid[3]);
    data.WriteBit(quantity[7]);
    data.WriteBit(guid[5]);
    data.WriteBit(quantity[6]);
    data.WriteBit(quantity[5]);
    data.WriteBit(quantity[1]);
    data.WriteBit(guid[2]);
    data.WriteBit(guid[1]);

    data.WriteByteSeq(guid[5]);
    data.WriteByteSeq(quantity[2]);
    data.WriteByteSeq(quantity[6]);
    data.WriteByteSeq(guid[4]);
    data.WriteByteSeq(quantity[4]);
    data.WriteByteSeq(guid[6]);
    data.WriteByteSeq(quantity[3]);
    data << uint32(secsToTimeBitFields(p_Data.m_Date));
    data.WriteByteSeq(guid[7]);
    data.WriteByteSeq(quantity[5]);
    data.WriteByteSeq(quantity[7]);
    data.WriteByteSeq(quantity[0]);
    data.WriteByteSeq(guid[1]);
    data.WriteByteSeq(quantity[1]);
    data.WriteByteSeq(guid[2]);
    data.WriteByteSeq(guid[3]);
    data << int32(p_Data.m_TimeFromStart);
    data << int32(p_Data.m_TimeFromCreate);
    data << int32(p_Data.m_ID);
    data.WriteByteSeq(guid[0]);

    if (p_Player == nullptr)
        instance->SendToPlayers(&data);
    else
        p_Player->SendDirectMessage(&data);
}

bool ScenarioStep::AddCriteria(uint32 criteriaId, uint32 totalCount)
{
    if (m_Criterias.find(criteriaId) != m_Criterias.end())
        return false;

    ScenarioStepCriteria& criteria = m_Criterias[criteriaId];
    criteria.CriteriaId = criteriaId;
    criteria.CurrentCount = 0;
    criteria.TotalCount = totalCount;

    return true;
}

bool ScenarioStep::UpdateCriteria(uint32 criteriaId, uint32 count)
{
    if (m_Criterias.find(criteriaId) == m_Criterias.end())
        return false;

    m_Criterias[criteriaId].CurrentCount += count;

    return true;
}

uint32 ScenarioStep::GetCriteriaCount(uint32 criteriaId)
{
    if (m_Criterias.find(criteriaId) == m_Criterias.end())
        return 0;

    return m_Criterias[criteriaId].CurrentCount;
}

bool ScenarioStep::IsCompleted() const
{
    for (ScenarioCriteriaMap::const_iterator itr = m_Criterias.begin(); itr != m_Criterias.end(); ++itr)
    {
        if (!itr->second.IsCompleted())
            return false;
    }

    return true;
}

ScenarioController::ScenarioController(Map* map, uint32 scenarioId, uint32 maxStep) :
m_Map(map), m_ScenarioId(scenarioId), m_MaxStep(maxStep)
{
    
    m_CurrentStep = STEP_1;
}

ScenarioStep& ScenarioController::GetStep(uint32 step)
{
    return m_Steps[step];
}

void ScenarioController::UpdateCurrentStepCriteria(uint32 criteriaId, uint32 count)
{
    if (m_Steps.find(m_CurrentStep) == m_Steps.end())
        return;

    ScenarioStep& step = m_Steps[m_CurrentStep];

    if (!step.UpdateCriteria(criteriaId, count))
        return;

    SendScenarioProgressToAll(criteriaId);

    if (step.IsCompleted())
    {
        if (m_CurrentStep < m_MaxStep)
        {
            m_CurrentStep++;

            SendScenarioStateToAll();
        }
    }
}

uint32 ScenarioController::GetCurrentStepCriteriaCount(uint32 criteriaId)
{
    if (m_Steps.find(m_CurrentStep) == m_Steps.end())
        return 0;

    ScenarioStep& step = m_Steps[m_CurrentStep];

    return step.GetCriteriaCount(criteriaId);
}

bool ScenarioController::IsCompleted()
{
    if (m_Steps.find(m_CurrentStep) == m_Steps.end())
        return false;

    return GetStep(m_CurrentStep).IsCompleted();
}

void ScenarioController::SendScenarioProgressToAll(uint32 criteriaId)
{
    ObjectGuid counter = GetCurrentStepCriteriaCount(criteriaId);

    for (Map::PlayerList::const_iterator itr = m_Map->GetPlayers().begin(); itr != m_Map->GetPlayers().end(); ++itr)
    {
        Player* player = itr->getSource();
        if (!player)
            continue;

        WorldPacket packet(SMSG_SCENARIO_PROGRESS_UPDATE);
        
        ObjectGuid playerGuid = player->GetGUID();

        uint32 bits4 = 0;

        uint32 uint32_1 = 0;
        uint32 uint32_2 = 0;

        packet.WriteBit(playerGuid[7]);
        packet.WriteBit(playerGuid[0]);
        packet.WriteBit(playerGuid[4]);
        packet.WriteBits(bits4, 4);
        packet.WriteBit(counter[3]);
        packet.WriteBit(counter[4]);
        packet.WriteBit(counter[0]);
        packet.WriteBit(playerGuid[6]);
        packet.WriteBit(counter[2]);
        packet.WriteBit(playerGuid[3]);
        packet.WriteBit(counter[7]);
        packet.WriteBit(playerGuid[5]);
        packet.WriteBit(counter[6]);
        packet.WriteBit(counter[5]);
        packet.WriteBit(counter[1]);
        packet.WriteBit(playerGuid[2]);
        packet.WriteBit(playerGuid[1]);

        packet.WriteByteSeq(playerGuid[5]);
        packet.WriteByteSeq(counter[2]);
        packet.WriteByteSeq(counter[6]);
        packet.WriteByteSeq(playerGuid[4]);
        packet.WriteByteSeq(counter[4]);
        packet.WriteByteSeq(playerGuid[6]);
        packet.WriteByteSeq(counter[3]);
        packet << getMSTime();
        packet.WriteByteSeq(playerGuid[7]);
        packet.WriteByteSeq(counter[5]);
        packet.WriteByteSeq(counter[7]);
        packet.WriteByteSeq(counter[0]);
        packet.WriteByteSeq(playerGuid[1]);
        packet.WriteByteSeq(counter[1]);
        packet.WriteByteSeq(playerGuid[2]);
        packet.WriteByteSeq(playerGuid[3]);

        packet << uint32_1;
        packet << uint32_2;
        packet << criteriaId;

        packet.WriteByteSeq(playerGuid[0]);

        player->GetSession()->SendPacket(&packet);
    }
}

void ScenarioController::SendScenarioProgressToAll(uint32 criteriaId, uint32 count)
{
    ObjectGuid counter = count;

    for (Map::PlayerList::const_iterator itr = m_Map->GetPlayers().begin(); itr != m_Map->GetPlayers().end(); ++itr)
    {
        Player* player = itr->getSource();
        if (!player)
            continue;

        WorldPacket packet(SMSG_SCENARIO_PROGRESS_UPDATE);
        
        ObjectGuid playerGuid = player->GetGUID();

        uint32 bits4 = 0;

        uint32 uint32_1 = 0;
        uint32 uint32_2 = 0;

        packet.WriteBit(playerGuid[7]);
        packet.WriteBit(playerGuid[0]);
        packet.WriteBit(playerGuid[4]);
        packet.WriteBits(bits4, 4);
        packet.WriteBit(counter[3]);
        packet.WriteBit(counter[4]);
        packet.WriteBit(counter[0]);
        packet.WriteBit(playerGuid[6]);
        packet.WriteBit(counter[2]);
        packet.WriteBit(playerGuid[3]);
        packet.WriteBit(counter[7]);
        packet.WriteBit(playerGuid[5]);
        packet.WriteBit(counter[6]);
        packet.WriteBit(counter[5]);
        packet.WriteBit(counter[1]);
        packet.WriteBit(playerGuid[2]);
        packet.WriteBit(playerGuid[1]);

        packet.WriteByteSeq(playerGuid[5]);
        packet.WriteByteSeq(counter[2]);
        packet.WriteByteSeq(counter[6]);
        packet.WriteByteSeq(playerGuid[4]);
        packet.WriteByteSeq(counter[4]);
        packet.WriteByteSeq(playerGuid[6]);
        packet.WriteByteSeq(counter[3]);
        packet << getMSTime();
        packet.WriteByteSeq(playerGuid[7]);
        packet.WriteByteSeq(counter[5]);
        packet.WriteByteSeq(counter[7]);
        packet.WriteByteSeq(counter[0]);
        packet.WriteByteSeq(playerGuid[1]);
        packet.WriteByteSeq(counter[1]);
        packet.WriteByteSeq(playerGuid[2]);
        packet.WriteByteSeq(playerGuid[3]);

        packet << uint32_1;
        packet << uint32_2;
        packet << criteriaId;

        packet.WriteByteSeq(playerGuid[0]);

        player->GetSession()->SendPacket(&packet);
    }
}

void ScenarioController::SendScenarioState(Player* player)
{
    WorldPacket packet(SMSG_SCENARIO_STATE);

    uint32 uint32_1 = 0;
    uint32 uint32_4 = 0;
    uint32 uint32_5 = 0;
    uint32 uint32_6 = 0;
    uint32 uint32_7 = 0; // bonus step

    bool bit1 = false;
    bool bit2 = false;

    uint32 bits19_1 = 0;

    packet << uint32_1;
    packet << GetCurrentStep(); 
    packet << m_ScenarioId; 
    packet << uint32_4;
    packet << uint32_5;
    packet << uint32_6;
    packet << uint32_7; // bonus step

    packet.WriteBit(bit1);

    packet.WriteBits(bits19_1, 19);
    
    /*guid1[i][2] = packet.ReadBit();
                bits18 = (int)packet.ReadBits(4);
                guid1[i][6] = packet.ReadBit();
                guid2[i][2] = packet.ReadBit();
                guid2[i][7] = packet.ReadBit();
                guid1[i][1] = packet.ReadBit();
                guid2[i][3] = packet.ReadBit();
                guid2[i][5] = packet.ReadBit();
                guid1[i][7] = packet.ReadBit();
                guid1[i][3] = packet.ReadBit();
                guid1[i][5] = packet.ReadBit();
                guid2[i][4] = packet.ReadBit();
                guid2[i][0] = packet.ReadBit();
                guid2[i][1] = packet.ReadBit();
                guid1[i][4] = packet.ReadBit();
                guid2[i][6] = packet.ReadBit();
                guid1[i][0] = packet.ReadBit();*/

    packet.WriteBit(bit2);

    /* for (var i = 0; i < bits10; ++i)
            {
                packet.ReadXORByte(guid1[i], 6);
                packet.ReadXORByte(guid2[i], 3);
                packet.ReadXORByte(guid1[i], 3);
                packet.ReadInt32("Int54");
                packet.ReadInt32("Int60");
                packet.ReadXORByte(guid1[i], 0);
                packet.ReadXORByte(guid2[i], 7);
                packet.ReadXORByte(guid1[i], 2);
                packet.ReadXORByte(guid2[i], 1);
                packet.ReadXORByte(guid2[i], 6);
                packet.ReadXORByte(guid2[i], 0);
                packet.ReadXORByte(guid1[i], 1);
                packet.ReadXORByte(guid2[i], 2);
                packet.ReadXORByte(guid1[i], 5);
                packet.ReadXORByte(guid2[i], 4);
                packet.ReadPackedTime("Time", i);
                packet.ReadXORByte(guid1[i], 4);
                packet.ReadXORByte(guid2[i], 5);
                packet.ReadInt32("Int14");
                packet.ReadXORByte(guid1[i], 7);
                packet.WriteGuid("Guid1", guid1[i], i);
                packet.WriteGuid("Guid2", guid2[i], i);
            }*/
    player->GetSession()->SendPacket(&packet);
}

void ScenarioController::SendScenarioStateToAll()
{
    WorldPacket packet(SMSG_SCENARIO_STATE);

    uint32 uint32_1 = 0;
    uint32 uint32_4 = 0;
    uint32 uint32_5 = 0;
    uint32 uint32_6 = 0;
    uint32 uint32_7 = 0; // bonus step

    bool bit1 = false;
    bool bit2 = false;

    uint32 bits19_1 = 0;

    packet << uint32_1;
    packet << GetCurrentStep(); 
    packet << m_ScenarioId; 
    packet << uint32_4;
    packet << uint32_5;
    packet << uint32_6;
    packet << uint32_7; // bonus step

    packet.WriteBit(bit1);

    packet.WriteBits(bits19_1, 19);
    
    /*guid1[i][2] = packet.ReadBit();
                bits18 = (int)packet.ReadBits(4);
                guid1[i][6] = packet.ReadBit();
                guid2[i][2] = packet.ReadBit();
                guid2[i][7] = packet.ReadBit();
                guid1[i][1] = packet.ReadBit();
                guid2[i][3] = packet.ReadBit();
                guid2[i][5] = packet.ReadBit();
                guid1[i][7] = packet.ReadBit();
                guid1[i][3] = packet.ReadBit();
                guid1[i][5] = packet.ReadBit();
                guid2[i][4] = packet.ReadBit();
                guid2[i][0] = packet.ReadBit();
                guid2[i][1] = packet.ReadBit();
                guid1[i][4] = packet.ReadBit();
                guid2[i][6] = packet.ReadBit();
                guid1[i][0] = packet.ReadBit();*/

    packet.WriteBit(bit2);

    /* for (var i = 0; i < bits10; ++i)
            {
                packet.ReadXORByte(guid1[i], 6);
                packet.ReadXORByte(guid2[i], 3);
                packet.ReadXORByte(guid1[i], 3);
                packet.ReadInt32("Int54");
                packet.ReadInt32("Int60");
                packet.ReadXORByte(guid1[i], 0);
                packet.ReadXORByte(guid2[i], 7);
                packet.ReadXORByte(guid1[i], 2);
                packet.ReadXORByte(guid2[i], 1);
                packet.ReadXORByte(guid2[i], 6);
                packet.ReadXORByte(guid2[i], 0);
                packet.ReadXORByte(guid1[i], 1);
                packet.ReadXORByte(guid2[i], 2);
                packet.ReadXORByte(guid1[i], 5);
                packet.ReadXORByte(guid2[i], 4);
                packet.ReadPackedTime("Time", i);
                packet.ReadXORByte(guid1[i], 4);
                packet.ReadXORByte(guid2[i], 5);
                packet.ReadInt32("Int14");
                packet.ReadXORByte(guid1[i], 7);
                packet.WriteGuid("Guid1", guid1[i], i);
                packet.WriteGuid("Guid2", guid2[i], i);
            }*/

    for (Map::PlayerList::const_iterator itr = m_Map->GetPlayers().begin(); itr != m_Map->GetPlayers().end(); ++itr)
    {
        itr->getSource()->GetSession()->SendPacket(&packet);
    }
}

void ScenarioController::SendScenarioPOI(uint32 criteriaTreeId, Player* player)
{

}

void ScenarioController::SendScenarioPOI(std::list<uint32>& criteriaTrees, Player* player)
{
    uint32 uint32_1 = 0;
    uint32 uint32_2 = 0;
    uint32 uint32_3 = 0;
    uint32 uint32_4 = 0;
    uint32 uint32_5 = 0;
    int32 uint32_6 = -3803;
    int32 uint32_7 = -4788;
    uint32 uint32_8 = 0;
    uint32 uint32_9 = 0;
    uint32 uint32_10 = 0;
    uint32 uint32_11 = 27242; // criteria tree id

    /*packet.ReadInt32("BlobID", i, j);
                    packet.ReadInt32("MapID", i, j);
                    packet.ReadInt32("WorldMapAreaID", i, j);
                    packet.ReadInt32("Floor", i, j);
                    packet.ReadInt32("Priority", i, j);
                    packet.ReadInt32("Flags", i, j);
                    packet.ReadInt32("WorldEffectID", i, j);
                    packet.ReadInt32("PlayerConditionID", i, j);*/

    for (std::list<uint32>::const_iterator itr = criteriaTrees.begin(); itr != criteriaTrees.end(); ++itr)
    {
        if ((*itr) == 27243)
        {
            WorldPacket packet(SMSG_SCENARIO_POI);
            
            packet.WriteBits(1, 21); // ScenarioPOIDataCount

            packet.WriteBits(1, 19); // ScenarioBlobDataCount

            packet.WriteBits(1, 21); // ScenarioPOIPointDataCount

            packet << uint32_1;
            packet << uint32_2;
            packet << uint32_3;
            packet << uint32_4;
            packet << uint32_5;

            packet << uint32_6;
            packet << uint32_7;

            packet << uint32_8;
            packet << uint32_9;
            packet << uint32_10;

            packet << uint32_11;

            player->GetSession()->SendPacket(&packet);
        }
    }
}
