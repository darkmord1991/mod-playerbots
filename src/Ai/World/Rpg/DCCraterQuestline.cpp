/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DCCraterQuestline.h"
#include "BotStartLocation.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "QuestDef.h"
#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace
{
    using SpawnPositions = std::unordered_map<uint32, std::vector<WorldPosition>>;
    using EntriesByQuest = std::unordered_map<uint32, std::vector<uint32>>;

    struct QuestlineQuest
    {
        uint32 rank{0};
        bool playable{true};
        std::vector<WorldPosition> starters;
        std::vector<WorldPosition> enders;
        std::array<std::vector<WorldPosition>, QUEST_OBJECTIVES_COUNT> objectives;
    };

    struct Questline
    {
        uint32 mapId{0};
        uint32 zoneId{0};
        std::vector<uint32> quests;
        std::unordered_map<uint32, QuestlineQuest> byQuest;

        void Build();
    };

    std::vector<WorldPosition> const NoPositions{};

    void AppendSpawns(std::vector<WorldPosition>& out, SpawnPositions const& spawns, uint32 entry)
    {
        auto itr = spawns.find(entry);
        if (itr != spawns.end())
            out.insert(out.end(), itr->second.begin(), itr->second.end());
    }

    void CollectRelations(EntriesByQuest& out, QuestRelations const& relations)
    {
        for (auto const& [entry, questId] : relations)
            out[questId].push_back(entry);
    }

    void Questline::Build()
    {
        BotStartLocation const& crater = BotStartLocations::GetCraterStart();
        mapId = crater.mapId;
        zoneId = crater.zoneId;

        std::vector<std::vector<uint32>> const& hubs = sPlayerbotAIConfig.dcCraterQuestlineHubs;

        // Every spawn on the crater by entry. A creature spawn with creature_multispawn alternates counts
        // for each of them.
        SpawnPositions creatureSpawns;
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            if (data.mapid != mapId)
                continue;

            WorldPosition const pos(mapId, data.posX, data.posY, data.posZ, data.orientation);
            for (uint32 entry : {data.id, data.id2, data.id3})
                if (entry)
                    creatureSpawns[entry].push_back(pos);
        }

        SpawnPositions goSpawns;
        for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
            if (data.mapid == mapId)
                goSpawns[data.id].push_back(WorldPosition(mapId, data.posX, data.posY, data.posZ, data.orientation));

        // Killing a creature whose template names a kill credit counts for that entry as well.
        std::unordered_map<uint32, std::vector<uint32>> creditedBy;
        for (auto const& [entry, creatureTemplate] : *sObjectMgr->GetCreatureTemplates())
            for (uint32 credit : creatureTemplate.KillCredit)
                if (credit && creatureSpawns.count(entry))
                    creditedBy[credit].push_back(entry);

        EntriesByQuest creatureStarters, creatureEnders, goStarters, goEnders;
        CollectRelations(creatureStarters, *sObjectMgr->GetCreatureQuestRelationMap());
        CollectRelations(creatureEnders, *sObjectMgr->GetCreatureQuestInvolvedRelationMap());
        CollectRelations(goStarters, *sObjectMgr->GetGOQuestRelationMap());
        CollectRelations(goEnders, *sObjectMgr->GetGOQuestInvolvedRelationMap());

        // Hub by hub; within a hub by quest level, except that a quest turned in outside the hub -- the
        // one that sends the player on -- comes after the others.
        struct Candidate
        {
            size_t hub;
            bool leavesHub;
            int32 level;
            uint32 questId;
        };
        std::vector<Candidate> candidates;
        std::unordered_set<uint32> seen;
        for (size_t hub = 0; hub < hubs.size(); ++hub)
        {
            for (uint32 npc : hubs[hub])
            {
                QuestRelationBounds bounds = sObjectMgr->GetCreatureQuestRelationBounds(npc);
                for (auto itr = bounds.first; itr != bounds.second; ++itr)
                {
                    uint32 const questId = itr->second;
                    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                    // A hub NPC can also give quests that belong to another zone.
                    if (!quest || quest->GetZoneOrSort() != static_cast<int32>(zoneId) || !seen.insert(questId).second)
                        continue;

                    std::vector<uint32> const& enders = creatureEnders[questId];
                    bool const leavesHub = std::none_of(enders.begin(), enders.end(), [&](uint32 ender)
                    {
                        return std::find(hubs[hub].begin(), hubs[hub].end(), ender) != hubs[hub].end();
                    });
                    candidates.push_back({hub, leavesHub, quest->GetQuestLevel(), questId});
                }
            }
        }

        std::sort(candidates.begin(), candidates.end(), [](Candidate const& a, Candidate const& b)
        {
            return std::tie(a.hub, a.leavesHub, a.level, a.questId) < std::tie(b.hub, b.leavesHub, b.level, b.questId);
        });

        std::string unplayable;
        for (Candidate const& candidate : candidates)
        {
            uint32 const questId = candidate.questId;
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            QuestlineQuest& entry = byQuest[questId];
            entry.rank = static_cast<uint32>(quests.size());
            quests.push_back(questId);

            for (uint32 starter : creatureStarters[questId])
                AppendSpawns(entry.starters, creatureSpawns, starter);
            for (uint32 starter : goStarters[questId])
                AppendSpawns(entry.starters, goSpawns, starter);
            for (uint32 ender : creatureEnders[questId])
                AppendSpawns(entry.enders, creatureSpawns, ender);
            for (uint32 ender : goEnders[questId])
                AppendSpawns(entry.enders, goSpawns, ender);

            if (entry.enders.empty())
                entry.playable = false;

            for (uint32 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            {
                int32 const target = quest->RequiredNpcOrGo[i];
                if (!target)
                    continue;

                std::vector<WorldPosition>& positions = entry.objectives[i];
                if (target > 0)
                {
                    AppendSpawns(positions, creatureSpawns, static_cast<uint32>(target));
                    for (uint32 source : creditedBy[static_cast<uint32>(target)])
                        AppendSpawns(positions, creatureSpawns, source);
                }
                else
                    AppendSpawns(positions, goSpawns, static_cast<uint32>(-target));

                if (positions.empty())
                    entry.playable = false;
            }

            if (!entry.playable)
                unplayable += (unplayable.empty() ? "" : " ") + std::to_string(questId);
        }

        LOG_INFO("playerbots", "DC crater questline: {} quests in {} hubs on map {} zone {}", quests.size(),
                 hubs.size(), mapId, zoneId);
        if (!unplayable.empty())
            LOG_INFO("playerbots",
                     "DC crater questline: bots skip these quests, their quest ender or an objective creature/object "
                     "has no spawn on the crater: {}",
                     unplayable);
    }

    Questline const& GetQuestline()
    {
        static Questline questline;
        static std::once_flag built;
        std::call_once(built, [] { questline.Build(); });
        return questline;
    }

    QuestlineQuest const* FindQuest(uint32 questId)
    {
        if (!sPlayerbotAIConfig.dcCraterQuestlineEnabled)
            return nullptr;

        Questline const& questline = GetQuestline();
        auto itr = questline.byQuest.find(questId);
        return itr != questline.byQuest.end() ? &itr->second : nullptr;
    }
}

bool DCCraterQuestline::AppliesTo(Player const* bot)
{
    if (!sPlayerbotAIConfig.dcCraterQuestlineEnabled)
        return false;

    Questline const& questline = GetQuestline();
    return bot->GetMapId() == questline.mapId && bot->GetZoneId() == questline.zoneId;
}

uint32 DCCraterQuestline::GetRank(uint32 questId)
{
    QuestlineQuest const* quest = FindQuest(questId);
    return quest ? quest->rank : NOT_IN_QUESTLINE;
}

std::vector<uint32> const& DCCraterQuestline::GetQuests()
{
    static std::vector<uint32> const none{};
    return sPlayerbotAIConfig.dcCraterQuestlineEnabled ? GetQuestline().quests : none;
}

bool DCCraterQuestline::IsPlayable(uint32 questId)
{
    QuestlineQuest const* quest = FindQuest(questId);
    return !quest || quest->playable;
}

std::vector<WorldPosition> const& DCCraterQuestline::GetStarterPositions(uint32 questId)
{
    QuestlineQuest const* quest = FindQuest(questId);
    return quest ? quest->starters : NoPositions;
}

std::vector<WorldPosition> const& DCCraterQuestline::GetEnderPositions(uint32 questId)
{
    QuestlineQuest const* quest = FindQuest(questId);
    return quest ? quest->enders : NoPositions;
}

std::vector<WorldPosition> const& DCCraterQuestline::GetObjectivePositions(uint32 questId, uint32 objectiveIdx)
{
    QuestlineQuest const* quest = FindQuest(questId);
    return quest && objectiveIdx < QUEST_OBJECTIVES_COUNT ? quest->objectives[objectiveIdx] : NoPositions;
}
