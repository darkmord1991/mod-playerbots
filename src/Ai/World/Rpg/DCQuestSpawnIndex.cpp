/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DCQuestSpawnIndex.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "PlayerbotAIConfig.h"
#include "QueryResult.h"
#include "QuestDef.h"
#include "StringFormat.h"

namespace
{
// A spawn and the phases it is in (CreatureData / GameObjectData::phaseMask).
struct Spawn
{
    WorldPosition pos;
    uint32 phaseMask;
};

using SpawnList = std::vector<Spawn>;
using SpawnPositions = std::unordered_map<uint32, SpawnList>;  // entry -> spawns

// The rule of the header: phase 1, or a phase the bot is in.
bool IsInSight(Spawn const& spawn, uint32 phaseMask) { return (spawn.phaseMask & (phaseMask | PHASEMASK_NORMAL)) != 0; }

std::vector<WorldPosition> InSight(SpawnList const& spawns, uint32 phaseMask)
{
    std::vector<WorldPosition> positions;
    positions.reserve(spawns.size());
    for (Spawn const& spawn : spawns)
        if (IsInSight(spawn, phaseMask))
            positions.push_back(spawn.pos);

    return positions;
}

// Quest items each indexed map can give: the loot, skinning and pickpocket tables of its creature spawns and the
// loot of its chests and fishing holes, one reference level deep. ItemSources holds the items, ItemDroppers the
// creature (false) or gameobject (true) entries giving each. Written by Init() before the index is built; no entry
// for a map means the item rule is off there.
std::unordered_map<uint32, std::unordered_set<uint32>> ItemSources;
std::unordered_map<uint32, std::unordered_map<uint32, std::vector<std::pair<bool, uint32>>>> ItemDroppers;

// Rows of (item, creature entry) for the creatures spawned on the map.
std::string CreatureItemQuery(uint32 mapId)
{
    std::string sql;
    auto const add = [&sql](std::string const& part) { sql += (sql.empty() ? "" : " UNION ") + part; };

    std::pair<char const*, char const*> const creatureLoot[] = {{"creature_loot_template", "lootid"},
                                                                {"skinning_loot_template", "skinloot"},
                                                                {"pickpocketing_loot_template", "pickpocketloot"}};
    for (auto const& [table, column] : creatureLoot)
    {
        add(
            Acore::StringFormat("SELECT l.Item, t.entry FROM creature c JOIN creature_template t ON t.entry = c.id "
                                "JOIN {} l ON l.Entry = t.{} WHERE c.map = {} AND t.{} > 0 AND l.Reference = 0",
                                table, column, mapId, column));
        add(
            Acore::StringFormat("SELECT r.Item, t.entry FROM creature c JOIN creature_template t ON t.entry = c.id "
                                "JOIN {} l ON l.Entry = t.{} AND l.Reference > 0 "
                                "JOIN reference_loot_template r ON r.Entry = l.Reference WHERE c.map = {} AND t.{} > 0",
                                table, column, mapId, column));
    }
    return sql;
}

// Rows of (item, gameobject entry) for the chests (type 3) and fishing holes (type 25, loot id in data1 for both)
// spawned on the map.
std::string GameObjectItemQuery(uint32 mapId)
{
    return Acore::StringFormat(
        "SELECT l.Item, t.entry FROM gameobject g JOIN gameobject_template t ON t.entry = g.id "
        "JOIN gameobject_loot_template l ON l.Entry = t.Data1 "
        "WHERE g.map = {} AND t.type IN (3, 25) AND t.Data1 > 0 AND l.Reference = 0 "
        "UNION SELECT r.Item, t.entry FROM gameobject g "
        "JOIN gameobject_template t ON t.entry = g.id "
        "JOIN gameobject_loot_template l ON l.Entry = t.Data1 AND l.Reference > 0 "
        "JOIN reference_loot_template r ON r.Entry = l.Reference "
        "WHERE g.map = {} AND t.type IN (3, 25) AND t.Data1 > 0",
        mapId, mapId);
}

// Rows of (item, creature entry) over every creature template: loot, skinning and pickpocket, one reference level
// deep, for the items some quest asks for.
std::string AllCreatureQuestItemQuery()
{
    std::string items;
    for (uint32 i = 1; i <= QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        items += Acore::StringFormat("{}SELECT RequiredItemId{} AS Item FROM quest_template WHERE RequiredItemId{} > 0",
                                     items.empty() ? "" : " UNION ", i, i);

    std::string sql;
    std::pair<char const*, char const*> const creatureLoot[] = {{"creature_loot_template", "lootid"},
                                                                {"skinning_loot_template", "skinloot"},
                                                                {"pickpocketing_loot_template", "pickpocketloot"}};
    for (auto const& [table, column] : creatureLoot)
    {
        sql += Acore::StringFormat(
            "{}SELECT l.Item, t.entry FROM creature_template t JOIN {} l ON l.Entry = t.{} "
            "JOIN ({}) q ON q.Item = l.Item WHERE t.{} > 0 AND l.Reference = 0 "
            "UNION SELECT r.Item, t.entry FROM creature_template t JOIN {} l ON l.Entry = t.{} AND l.Reference > 0 "
            "JOIN reference_loot_template r ON r.Entry = l.Reference JOIN ({}) q ON q.Item = r.Item WHERE t.{} > 0",
            sql.empty() ? "" : " UNION ", table, column, items, column, table, column, items, column);
    }
    return sql;
}

// Rows of (item, gameobject entry) over every chest (type 3) and fishing hole (type 25) template.
std::string AllGameObjectItemQuery()
{
    return "SELECT l.Item, t.entry FROM gameobject_template t JOIN gameobject_loot_template l ON l.Entry = t.Data1 "
           "WHERE t.type IN (3, 25) AND t.Data1 > 0 AND l.Reference = 0 "
           "UNION SELECT r.Item, t.entry FROM gameobject_template t "
           "JOIN gameobject_loot_template l ON l.Entry = t.Data1 AND l.Reference > 0 "
           "JOIN reference_loot_template r ON r.Entry = l.Reference WHERE t.type IN (3, 25) AND t.Data1 > 0";
}

// The quest is filed under a zone of another map: a battleground, dungeon, raid or a city of another continent.
// The map the copy was made from (Playerbots.DCLevelAreas.QuestPoiMapAlias) counts as this map.
bool IsSortedToAnotherMap(Quest const* quest, uint32 mapId, uint32 stockMapId)
{
    int32 const sort = quest->GetZoneOrSort();
    if (sort <= 0)
        return false;

    AreaTableEntry const* area = sAreaTableStore.LookupEntry(static_cast<uint32>(sort));
    return area && area->mapid != mapId && area->mapid != stockMapId;
}

// The quest needs an item it does not hand out, and nothing on the map gives it. On 750/751 every such quest bots
// took in the 2026-11-10 runs (28 of them) was never finished: crafted items, items from dungeons, raids or
// battlegrounds, or items made by using a quest item.
bool NeedsUnobtainableItem(Quest const* quest, std::unordered_set<uint32> const& mapItems)
{
    for (uint32 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
    {
        uint32 const item = quest->RequiredItemId[i];
        if (!item || !quest->RequiredItemCount[i] || mapItems.count(item) || item == quest->GetSrcItemId())
            continue;

        if (std::find(std::begin(quest->ItemDrop), std::end(quest->ItemDrop), item) == std::end(quest->ItemDrop))
            return true;
    }

    return false;
}

struct QuestSpawns
{
    bool playable{true};
    SpawnList enders;
    std::array<SpawnList, QUEST_OBJECTIVES_COUNT> objectives;
};

struct Index
{
    std::unordered_map<uint32, std::unordered_map<uint32, QuestSpawns>> byMap;
    // map -> quest item -> spawns of the creatures and objects on the map that give it
    std::unordered_map<uint32, std::unordered_map<uint32, SpawnList>> itemPositionsByMap;

    void Build();
    void BuildMap(uint32 mapId);
};

std::vector<uint32> const NoTriggers{};

// Every map: where the targets of kill, object and item objectives stand, and the area triggers of exploration
// objectives.
struct ObjectiveSpawns
{
    SpawnPositions creatures;    // entry -> spawns, for the entries some objective needs
    SpawnPositions gameObjects;  // likewise
    std::unordered_map<uint32, std::vector<uint32>> creditedBy;                    // kill credit -> creatures
    std::unordered_map<uint32, std::vector<std::pair<bool, uint32>>> itemSources;  // item -> (is object, entry)
    std::unordered_map<uint32, std::vector<uint32>> exploreTriggers;               // quest -> area triggers
};

// The nearest of `spawns` on the map, in sight of `phaseMask`, within the distance `nearestDistSq` (squared, 2D)
// of (x, y).
void KeepNearest(SpawnList const& spawns, uint32 mapId, uint32 phaseMask, float x, float y,
                 WorldPosition const*& nearest, float& nearestDistSq)
{
    for (Spawn const& spawn : spawns)
    {
        if (spawn.pos.GetMapId() != mapId || !IsInSight(spawn, phaseMask))
            continue;

        float const dx = spawn.pos.GetPositionX() - x;
        float const dy = spawn.pos.GetPositionY() - y;
        float const distSq = dx * dx + dy * dy;
        if (distSq <= nearestDistSq)
        {
            nearestDistSq = distSq;
            nearest = &spawn.pos;
        }
    }
}

void AppendSpawns(SpawnList& out, SpawnPositions const& spawns, uint32 entry)
{
    auto itr = spawns.find(entry);
    if (itr != spawns.end())
        out.insert(out.end(), itr->second.begin(), itr->second.end());
}

void Index::Build()
{
    for (uint32 mapId : sPlayerbotAIConfig.DCQuestSpawnMaps)
        BuildMap(mapId);
}

void Index::BuildMap(uint32 mapId)
{
    SpawnPositions creatureSpawns;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        if (data.mapid != mapId)
            continue;

        Spawn const spawn{WorldPosition(mapId, data.posX, data.posY, data.posZ, data.orientation), data.phaseMask};
        for (uint32 entry : {data.id, data.id2, data.id3})
            if (entry)
                creatureSpawns[entry].push_back(spawn);
    }

    SpawnPositions goSpawns;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
        if (data.mapid == mapId)
            goSpawns[data.id].push_back(
                {WorldPosition(mapId, data.posX, data.posY, data.posZ, data.orientation), data.phaseMask});

    // Killing a creature whose template names a kill credit counts for that entry too. The DC continents rely on
    // this: their clone creatures credit the stock entries the stock quests ask for.
    std::unordered_map<uint32, std::vector<uint32>> creditedBy;
    for (auto const& [entry, creatureTemplate] : *sObjectMgr->GetCreatureTemplates())
        for (uint32 credit : creatureTemplate.KillCredit)
            if (credit && creatureSpawns.count(entry))
                creditedBy[credit].push_back(entry);

    std::unordered_set<uint32> quests;
    std::unordered_set<uint32> givenHere;  // the item and sort rules apply only to these
    std::unordered_map<uint32, std::vector<uint32>> creatureEnders;
    std::unordered_map<uint32, std::vector<uint32>> goEnders;
    for (auto const& [entry, questId] : *sObjectMgr->GetCreatureQuestRelationMap())
        if (creatureSpawns.count(entry))
            givenHere.insert(questId);
    for (auto const& [entry, questId] : *sObjectMgr->GetGOQuestRelationMap())
        if (goSpawns.count(entry))
            givenHere.insert(questId);
    quests = givenHere;
    for (auto const& [entry, questId] : *sObjectMgr->GetCreatureQuestInvolvedRelationMap())
    {
        creatureEnders[questId].push_back(entry);
        if (creatureSpawns.count(entry))
            quests.insert(questId);
    }
    for (auto const& [entry, questId] : *sObjectMgr->GetGOQuestInvolvedRelationMap())
    {
        goEnders[questId].push_back(entry);
        if (goSpawns.count(entry))
            quests.insert(questId);
    }

    // Where to go for an item objective: the spawns on this map of what gives the item.
    auto const droppers = ItemDroppers.find(mapId);
    if (droppers != ItemDroppers.end())
    {
        std::unordered_map<uint32, SpawnList>& itemPositions = itemPositionsByMap[mapId];
        for (auto const& [item, sources] : droppers->second)
        {
            SpawnList& positions = itemPositions[item];
            for (auto const& [isGameObject, entry] : sources)
                AppendSpawns(positions, isGameObject ? goSpawns : creatureSpawns, entry);
        }
    }

    auto const alias = sPlayerbotAIConfig.DCQuestPoiMapAlias.find(mapId);
    uint32 const stockMapId = alias != sPlayerbotAIConfig.DCQuestPoiMapAlias.end() ? alias->second : mapId;
    auto const itemSources = ItemSources.find(mapId);
    std::unordered_set<uint32> const* mapItems = itemSources != ItemSources.end() ? &itemSources->second : nullptr;
    std::vector<uint32> const& skipList = sPlayerbotAIConfig.DCSkipQuests;
    uint32 skipped = 0;
    uint32 sortedElsewhere = 0;
    uint32 missingItem = 0;

    std::unordered_map<uint32, QuestSpawns>& byQuest = byMap[mapId];
    std::vector<uint32> unplayable;
    for (uint32 questId : quests)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;

        QuestSpawns& spawns = byQuest[questId];
        for (uint32 ender : creatureEnders[questId])
            AppendSpawns(spawns.enders, creatureSpawns, ender);
        for (uint32 ender : goEnders[questId])
            AppendSpawns(spawns.enders, goSpawns, ender);

        if (spawns.enders.empty())
            spawns.playable = false;

        for (uint32 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            int32 const target = quest->RequiredNpcOrGo[i];
            if (!target)
                continue;

            SpawnList& positions = spawns.objectives[i];
            if (target > 0)
            {
                AppendSpawns(positions, creatureSpawns, static_cast<uint32>(target));
                for (uint32 source : creditedBy[static_cast<uint32>(target)])
                    AppendSpawns(positions, creatureSpawns, source);
            }
            else
                AppendSpawns(positions, goSpawns, static_cast<uint32>(-target));

            if (positions.empty())
                spawns.playable = false;
        }

        // Only for quests given here: a quest merely turned in here may have been done elsewhere.
        if (spawns.playable && givenHere.count(questId))
        {
            if (std::find(skipList.begin(), skipList.end(), questId) != skipList.end())
            {
                spawns.playable = false;
                ++skipped;
            }
            else if (IsSortedToAnotherMap(quest, mapId, stockMapId))
            {
                spawns.playable = false;
                ++sortedElsewhere;
            }
            else if (mapItems && NeedsUnobtainableItem(quest, *mapItems))
            {
                spawns.playable = false;
                ++missingItem;
            }
        }

        if (!spawns.playable)
            unplayable.push_back(questId);
    }

    std::sort(unplayable.begin(), unplayable.end());
    std::string sample;
    for (size_t i = 0; i < unplayable.size() && i < 40; ++i)
        sample += (sample.empty() ? "" : " ") + std::to_string(unplayable[i]);

    LOG_INFO("playerbots",
             "DC quest spawn index: map {}: {} quests, {} cannot be finished there ({} on the skip list, {} sorted to "
             "another map, {} need an item nothing there gives{}); {} quest items have known sources{}{}",
             mapId, byQuest.size(), unplayable.size(), skipped, sortedElsewhere, missingItem,
             mapItems ? "" : "; item check off", itemPositionsByMap[mapId].size(),
             unplayable.empty() ? "" : " - bots skip them: ", sample);
}

Index const& GetIndex()
{
    static Index index;
    static std::once_flag built;
    std::call_once(built, [] { index.Build(); });
    return index;
}

// Spawns of the turn-ins of every quest, on every map.
std::unordered_map<uint32, SpawnList> BuildEnderSpawns()
{
    std::unordered_map<uint32, std::vector<uint32>> creatureQuests;
    for (auto const& [entry, questId] : *sObjectMgr->GetCreatureQuestInvolvedRelationMap())
        creatureQuests[entry].push_back(questId);
    std::unordered_map<uint32, std::vector<uint32>> goQuests;
    for (auto const& [entry, questId] : *sObjectMgr->GetGOQuestInvolvedRelationMap())
        goQuests[entry].push_back(questId);

    std::unordered_map<uint32, SpawnList> enders;
    size_t spawns = 0;
    auto const add = [&enders, &spawns](std::vector<uint32> const& quests, Spawn const& spawn)
    {
        for (uint32 questId : quests)
        {
            enders[questId].push_back(spawn);
            ++spawns;
        }
    };
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        Spawn const spawn{WorldPosition(data.mapid, data.posX, data.posY, data.posZ, data.orientation), data.phaseMask};
        for (uint32 entry : {data.id, data.id2, data.id3})
        {
            auto const quests = entry ? creatureQuests.find(entry) : creatureQuests.end();
            if (quests != creatureQuests.end())
                add(quests->second, spawn);
        }
    }
    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
    {
        auto const quests = goQuests.find(data.id);
        if (quests != goQuests.end())
            add(quests->second,
                {WorldPosition(data.mapid, data.posX, data.posY, data.posZ, data.orientation), data.phaseMask});
    }

    LOG_INFO("playerbots", "Quest turn-in spawns: {} quests, {} spawns", enders.size(), spawns);
    return enders;
}

// The item sources and exploration triggers come from the world database, so only Init() (world thread, startup)
// reads them; a build on first use from a map thread leaves them out.
ObjectiveSpawns BuildObjectiveSpawns(bool withDatabase)
{
    ObjectiveSpawns out;
    std::unordered_set<uint32> creatureEntries;
    std::unordered_set<uint32> goEntries;
    std::unordered_set<uint32> questItems;
    for (auto const& [questId, quest] : sObjectMgr->GetQuestTemplates())
    {
        for (uint32 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            int32 const target = quest->RequiredNpcOrGo[i];
            if (target > 0)
                creatureEntries.insert(static_cast<uint32>(target));
            else if (target < 0)
                goEntries.insert(static_cast<uint32>(-target));
        }
        for (uint32 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredItemId[i])
                questItems.insert(quest->RequiredItemId[i]);
    }

    for (auto const& [entry, creatureTemplate] : *sObjectMgr->GetCreatureTemplates())
        for (uint32 credit : creatureTemplate.KillCredit)
            if (credit && credit != entry && creatureEntries.count(credit))
                out.creditedBy[credit].push_back(entry);
    for (auto const& [credit, sources] : out.creditedBy)
        creatureEntries.insert(sources.begin(), sources.end());

    if (withDatabase)
    {
        for (bool const isGameObject : {false, true})
        {
            // sql-ok: table and column names are literals
            QueryResult result =
                WorldDatabase.Query(isGameObject ? AllGameObjectItemQuery() : AllCreatureQuestItemQuery());
            if (!result)
                continue;

            do
            {
                uint32 const item = (*result)[0].Get<uint32>();
                if (!questItems.count(item))
                    continue;

                uint32 const entry = (*result)[1].Get<uint32>();
                out.itemSources[item].emplace_back(isGameObject, entry);
                (isGameObject ? goEntries : creatureEntries).insert(entry);
            } while (result->NextRow());
        }

        if (QueryResult result = WorldDatabase.Query("SELECT id, quest FROM areatrigger_involvedrelation"))
        {
            do
                out.exploreTriggers[(*result)[1].Get<uint32>()].push_back((*result)[0].Get<uint32>());
            while (result->NextRow());
        }
    }

    size_t spawns = 0;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        for (uint32 entry : {data.id, data.id2, data.id3})
        {
            if (!entry || !creatureEntries.count(entry))
                continue;

            out.creatures[entry].push_back(
                {WorldPosition(data.mapid, data.posX, data.posY, data.posZ, data.orientation), data.phaseMask});
            ++spawns;
        }
    }
    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
    {
        if (!goEntries.count(data.id))
            continue;

        out.gameObjects[data.id].push_back(
            {WorldPosition(data.mapid, data.posX, data.posY, data.posZ, data.orientation), data.phaseMask});
        ++spawns;
    }

    LOG_INFO("playerbots",
             "Quest objective spawns: {} creature and {} object entries, {} spawns, {} quest items "
             "with sources, {} quests with exploration triggers",
             out.creatures.size(), out.gameObjects.size(), spawns, out.itemSources.size(), out.exploreTriggers.size());
    return out;
}

bool ObjectiveSpawnsFromInit = false;

ObjectiveSpawns const& GetObjectiveSpawns()
{
    static ObjectiveSpawns spawns;
    static std::once_flag built;
    std::call_once(built, [] { spawns = BuildObjectiveSpawns(ObjectiveSpawnsFromInit); });
    return spawns;
}

std::unordered_map<uint32, SpawnList> const& GetEnderSpawns()
{
    static std::unordered_map<uint32, SpawnList> enders;
    static std::once_flag built;
    std::call_once(built, [] { enders = BuildEnderSpawns(); });
    return enders;
}

QuestSpawns const* FindQuest(uint32 questId, uint32 mapId)
{
    if (sPlayerbotAIConfig.DCQuestSpawnMaps.empty())
        return nullptr;

    Index const& index = GetIndex();
    auto map = index.byMap.find(mapId);
    if (map == index.byMap.end())
        return nullptr;

    auto quest = map->second.find(questId);
    return quest != map->second.end() ? &quest->second : nullptr;
}
}  // namespace

void DCQuestSpawnIndex::Init()
{
    // Only items some quest asks for are kept.
    std::unordered_set<uint32> questItems;
    for (auto const& [questId, quest] : sObjectMgr->GetQuestTemplates())
        for (uint32 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredItemId[i])
                questItems.insert(quest->RequiredItemId[i]);

    for (uint32 mapId : sPlayerbotAIConfig.DCQuestSpawnMaps)
    {
        std::unordered_set<uint32>& items = ItemSources[mapId];
        std::unordered_map<uint32, std::vector<std::pair<bool, uint32>>>& droppers = ItemDroppers[mapId];
        for (bool const isGameObject : {false, true})
        {
            // sql-ok: table and column names are literals, the map id is a number from the config
            std::string const sql = isGameObject ? GameObjectItemQuery(mapId) : CreatureItemQuery(mapId);
            QueryResult result = WorldDatabase.Query(sql);
            if (!result)
                continue;

            do
            {
                uint32 const item = (*result)[0].Get<uint32>();
                if (!questItems.count(item))
                    continue;

                items.insert(item);
                droppers[item].emplace_back(isGameObject, (*result)[1].Get<uint32>());
            } while (result->NextRow());
        }
    }

    GetIndex();
    GetEnderSpawns();
    ObjectiveSpawnsFromInit = true;
    GetObjectiveSpawns();
}

std::vector<WorldPosition> DCQuestSpawnIndex::GetItemSourcePositions(uint32 itemId, uint32 mapId, uint32 phaseMask)
{
    if (sPlayerbotAIConfig.DCQuestSpawnMaps.empty())
        return {};

    Index const& index = GetIndex();
    auto const map = index.itemPositionsByMap.find(mapId);
    if (map == index.itemPositionsByMap.end())
        return {};

    auto const item = map->second.find(itemId);
    return item != map->second.end() ? InSight(item->second, phaseMask) : std::vector<WorldPosition>{};
}

bool DCQuestSpawnIndex::IsSkipped(uint32 questId)
{
    std::vector<uint32> const& skipList = sPlayerbotAIConfig.DCSkipQuests;
    return std::binary_search(skipList.begin(), skipList.end(), questId);
}

WorldPosition const* DCQuestSpawnIndex::FindEnderNear(uint32 questId, uint32 mapId, uint32 phaseMask, float x, float y,
                                                      float radius)
{
    std::unordered_map<uint32, SpawnList> const& enders = GetEnderSpawns();
    auto const quest = enders.find(questId);
    if (quest == enders.end())
        return nullptr;

    WorldPosition const* nearest = nullptr;
    float nearestDistSq = radius * radius;
    KeepNearest(quest->second, mapId, phaseMask, x, y, nearest, nearestDistSq);
    return nearest;
}

WorldPosition const* DCQuestSpawnIndex::FindObjectiveTargetNear(uint32 questId, int32 objectiveIdx, uint32 mapId,
                                                                uint32 phaseMask, float x, float y, float radius)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest || objectiveIdx < 0 || objectiveIdx >= EXPLORE_OBJECTIVE_IDX)
        return nullptr;

    ObjectiveSpawns const& spawns = GetObjectiveSpawns();
    WorldPosition const* nearest = nullptr;
    float nearestDistSq = radius * radius;
    auto const consider = [&](SpawnPositions const& positions, uint32 entry)
    {
        auto const itr = positions.find(entry);
        if (itr != positions.end())
            KeepNearest(itr->second, mapId, phaseMask, x, y, nearest, nearestDistSq);
    };

    if (objectiveIdx < QUEST_OBJECTIVES_COUNT)
    {
        int32 const target = quest->RequiredNpcOrGo[objectiveIdx];
        if (target > 0)
        {
            consider(spawns.creatures, static_cast<uint32>(target));
            auto const credits = spawns.creditedBy.find(static_cast<uint32>(target));
            if (credits != spawns.creditedBy.end())
                for (uint32 source : credits->second)
                    consider(spawns.creatures, source);
        }
        else if (target < 0)
            consider(spawns.gameObjects, static_cast<uint32>(-target));

        return nearest;
    }

    uint32 const item = quest->RequiredItemId[objectiveIdx - QUEST_OBJECTIVES_COUNT];
    auto const sources = item ? spawns.itemSources.find(item) : spawns.itemSources.end();
    if (sources != spawns.itemSources.end())
        for (auto const& [isGameObject, entry] : sources->second)
            consider(isGameObject ? spawns.gameObjects : spawns.creatures, entry);

    return nearest;
}

std::vector<uint32> const& DCQuestSpawnIndex::GetExploreTriggers(uint32 questId)
{
    ObjectiveSpawns const& spawns = GetObjectiveSpawns();
    auto const itr = spawns.exploreTriggers.find(questId);
    return itr != spawns.exploreTriggers.end() ? itr->second : NoTriggers;
}

bool DCQuestSpawnIndex::IsPlayable(uint32 questId, uint32 mapId)
{
    if (IsSkipped(questId))
        return false;

    QuestSpawns const* quest = FindQuest(questId, mapId);
    return !quest || quest->playable;
}

std::vector<WorldPosition> DCQuestSpawnIndex::GetEnderPositions(uint32 questId, uint32 mapId, uint32 phaseMask)
{
    QuestSpawns const* quest = FindQuest(questId, mapId);
    return quest ? InSight(quest->enders, phaseMask) : std::vector<WorldPosition>{};
}

std::vector<WorldPosition> DCQuestSpawnIndex::GetObjectivePositions(uint32 questId, uint32 mapId, uint32 objectiveIdx,
                                                                    uint32 phaseMask)
{
    QuestSpawns const* quest = FindQuest(questId, mapId);
    return quest && objectiveIdx < QUEST_OBJECTIVES_COUNT ? InSight(quest->objectives[objectiveIdx], phaseMask)
                                                          : std::vector<WorldPosition>{};
}

std::vector<std::pair<uint32, uint32>> DCQuestSpawnIndex::GetUnplayable()
{
    std::vector<std::pair<uint32, uint32>> result;
    if (sPlayerbotAIConfig.DCQuestSpawnMaps.empty())
        return result;

    for (auto const& [mapId, byQuest] : GetIndex().byMap)
        for (auto const& [questId, spawns] : byQuest)
            if (!spawns.playable)
                result.emplace_back(mapId, questId);

    std::sort(result.begin(), result.end());
    return result;
}
