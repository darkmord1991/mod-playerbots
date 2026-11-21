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
#include "QuestDef.h"
#include "TravelMgr.h"

// DarkChaos: spawn data for the quests on the leveling continents of Playerbots.DCLevelAreas.QuestSpawnMaps.
//
// Those maps copy stock zones. Many of their quests have no POI there, and some name a turn-in or target the copy
// never spawned, which a bot only finds out after five minutes at a POI. Both are answered from the map's own
// spawns. Built once by Init() at startup (or on first use, without the item rule, if Init never ran); read-only
// afterwards, so safe from the map threads.
namespace DCQuestSpawnIndex
{
// Objective index of a quest's exploration objective (an area trigger), after the kill/object and item ones.
inline constexpr int32 EXPLORE_OBJECTIVE_IDX = QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT;

// Loads, from the world database, the items each indexed map's creatures and chests can give, then builds the
// index. Call once at startup on the world thread, before any bot update.
void Init();

// False when the quest has a giver or turn-in on an indexed map but cannot be finished there: no turn-in is
// spawned, or nothing spawned counts for one of its kill or object objectives. A quest GIVEN there is also
// unplayable when it is on Playerbots.DCLevelAreas.SkipQuests, when its QuestSortID is a zone of another map (a
// battleground, dungeon, raid or another continent's city), or when it needs an item it does not hand out and
// nothing on the map drops. True for any other quest or map.
bool IsPlayable(uint32 questId, uint32 mapId);

// True for a quest on Playerbots.DCLevelAreas.SkipQuests. The list holds on every map: IsPlayable and
// DCCraterQuestline::IsPlayable both ask here, so a bot neither takes such a quest nor works on one it carries.
// The config keeps the list sorted.
bool IsSkipped(uint32 questId);

// Every lookup below takes the phase mask of the bot asking and leaves out the spawns it cannot see: a spawn
// counts when it is in phase 1 or in a phase the bot is in. Quest phasing (the DarkChaos quest phase layer, phase
// auras) puts copies of an NPC in other phases, which a bot outside them would walk to and never find. Phase 1
// always counts: only a phase aura of the area takes it away, and the bot may be on its way out of that area.

// Spawn positions on the map of the quest's turn-ins, and of what counts for kill/object objective
// `objectiveIdx`. Empty when there are none or the map is not indexed.
std::vector<WorldPosition> GetEnderPositions(uint32 questId, uint32 mapId, uint32 phaseMask);
std::vector<WorldPosition> GetObjectivePositions(uint32 questId, uint32 mapId, uint32 objectiveIdx, uint32 phaseMask);

// The spawn of one of the quest's turn-ins on the map nearest to (x, y), when one stands within `radius` yards of
// it (2D); nullptr otherwise. Unlike the rest of the index this covers every map: the ground height at a turn-in
// point is the top surface, and a turn-in in a cave, a cellar or a building under a hill stands far below it.
WorldPosition const* FindEnderNear(uint32 questId, uint32 mapId, uint32 phaseMask, float x, float y, float radius);

// The spawn on the map nearest to (x, y), within `radius` yards (2D), of something that counts for the quest's
// objective `objectiveIdx`: for a kill or object objective the creature or object and the creatures that
// kill-credit it, for an item objective the creatures (loot, skinning, pickpocket) and chests that give the item.
// nullptr when none stands there. Covers every map; item sources only when Init() ran.
WorldPosition const* FindObjectiveTargetNear(uint32 questId, int32 objectiveIdx, uint32 mapId, uint32 phaseMask,
                                             float x, float y, float radius);

// The area triggers that complete the quest's exploration objective (areatrigger_involvedrelation). Empty when
// there are none or Init() never ran.
std::vector<uint32> const& GetExploreTriggers(uint32 questId);

// Spawn positions on the map of the creatures (loot, skinning, pickpocket) and chests that give a quest item.
// Empty when nothing there does, the item is no quest's objective, or the map is not indexed.
std::vector<WorldPosition> GetItemSourcePositions(uint32 itemId, uint32 mapId, uint32 phaseMask);

// (map, quest) of every quest the index found unplayable, for the quest stats dump.
std::vector<std::pair<uint32, uint32>> GetUnplayable();
}  // namespace DCQuestSpawnIndex

#endif
