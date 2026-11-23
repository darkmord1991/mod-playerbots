/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGHELPERS_H
#define PLAYERBOTS_NEWRPGHELPERS_H

#include "ObjectGuid.h"

class Object;
class Player;
class PlayerbotAI;
class Quest;
class WorldObject;
struct FactionTemplateEntry;
struct QuestStatusData;

// Quest checks shared by the new RPG actions and values. They encode new RPG questing policy
// (solo, non-repeatable, level window), not general quest rules.
namespace NewRpgHelpers
{
// Radius within which a quest giver with something to accept or reward pulls the bot over.
constexpr float QUEST_GIVER_SEARCH_DISTANCE = 80.0f;

// Player::CanInteractWithQuestGiver without the distance check.
bool CanInteractWithQuestGiver(Player* bot, Object* questGiver);
bool HasQuestToAcceptOrReward(Player* bot, WorldObject* object);
bool IsQuestWorthDoing(Player* bot, Quest const* quest);
bool IsQuestCapableDoing(Player* bot, Quest const* quest);

// Minimum level of the highest creature the quest asks a player of `playerFaction` to kill, 0 for none. Skips the
// objectives `progress` has done (null: none done), credit dummies and NPCs the player cannot fight.
uint32 GetHighestKillTargetLevel(Quest const* quest, FactionTemplateEntry const* playerFaction,
                                 QuestStatusData const* progress);
// An open kill objective more than QUEST_MAX_LEVELS_ABOVE_BOT levels above the bot. Stock creatures placed on a
// custom map keep their stock level, so a quest of the bot's level can still send it at much higher mobs.
bool HasKillTargetAboveBot(Player* bot, Quest const* quest);

// Nearest giver in the new RPG target caches with something to accept or reward, skipping givers
// the bot gave up on. A distanceLimit of 0 searches the whole cache.
ObjectGuid FindQuestGiver(PlayerbotAI* botAI, float distanceLimit = 0.0f);
}  // namespace NewRpgHelpers

#endif
