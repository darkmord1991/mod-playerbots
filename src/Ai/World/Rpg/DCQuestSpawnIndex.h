/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DCQUESTSPAWNINDEX_H
#define PLAYERBOTS_DCQUESTSPAWNINDEX_H

#include <utility>
#include <vector>

#include "Define.h"
#include "TravelMgr.h"

// DarkChaos: spawn data for the quests on the leveling continents of Playerbots.DCLevelAreas.QuestSpawnMaps.
//
// Those maps copy stock zones. Many of their quests have no POI there, and some name a turn-in or target the copy
// never spawned, which a bot only finds out after five minutes at a POI. Both are answered from the map's own
// spawns. Built once by Init() at startup (or on first use, without the item rule, if Init never ran); read-only
// afterwards, so safe from the map threads.
namespace DCQuestSpawnIndex
{
// Loads, from the world database, the items each indexed map's creatures and chests can give, then builds the
// index. Call once at startup on the world thread, before any bot update.
void Init();

// False when the quest has a giver or turn-in on an indexed map but cannot be finished there: no turn-in is
// spawned, or nothing spawned counts for one of its kill or object objectives. A quest GIVEN there is also
// unplayable when it is on Playerbots.DCLevelAreas.SkipQuests, when its QuestSortID is a zone of another map (a
// battleground, dungeon, raid or another continent's city), or when it needs an item it does not hand out and
// nothing on the map drops. True for any other quest or map.
bool IsPlayable(uint32 questId, uint32 mapId);

// Spawn positions on the map of the quest's turn-ins, and of what counts for kill/object objective
// `objectiveIdx`. Empty when there are none or the map is not indexed.
std::vector<WorldPosition> const& GetEnderPositions(uint32 questId, uint32 mapId);
std::vector<WorldPosition> const& GetObjectivePositions(uint32 questId, uint32 mapId, uint32 objectiveIdx);

// Spawn positions on the map of the creatures (loot, skinning, pickpocket) and chests that give a quest item.
// Empty when nothing there does, the item is no quest's objective, or the map is not indexed.
std::vector<WorldPosition> const& GetItemSourcePositions(uint32 itemId, uint32 mapId);

// (map, quest) of every quest the index found unplayable, for the quest stats dump.
std::vector<std::pair<uint32, uint32>> GetUnplayable();
}  // namespace DCQuestSpawnIndex

#endif
