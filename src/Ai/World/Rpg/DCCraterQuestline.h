/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DCCRATERQUESTLINE_H
#define PLAYERBOTS_DCCRATERQUESTLINE_H

#include "Define.h"
#include "TravelMgr.h"
#include <limits>
#include <vector>

class Player;

// DarkChaos: the Azshara Crater quests, in the order the zone is meant to be played.
//
// The crater quests have no quest_template_addon chains, so nothing stops a bot from taking them in any
// order, and the New RPG strategy does exactly that: it accepts whatever a quest giver it happens to walk
// past offers, works on a random quest from its log, and never looks for the next hub. The questline
// fixes the order instead: hub by hub as listed in AiPlayerbot.DCCraterQuestline.Hubs, and within a hub
// by quest level, with the quest that sends the player on to the next hub last.
//
// Built once, on first use, from ObjectMgr; read-only afterwards, so safe from the map threads.
namespace DCCraterQuestline
{
    constexpr uint32 NOT_IN_QUESTLINE = std::numeric_limits<uint32>::max();

    // True when the questline is enabled and the bot is in the crater zone.
    bool AppliesTo(Player const* bot);

    // Where the quest stands in the questline, 0 first, or NOT_IN_QUESTLINE.
    uint32 GetRank(uint32 questId);

    // The questline's quests, index = rank.
    std::vector<uint32> const& GetQuests();

    // False for a questline quest with an objective whose creature or game object has no spawn on the
    // crater. Such a quest cannot be finished, so bots neither take it nor work on it. True for any quest
    // outside the questline, and for every quest while the questline is disabled.
    bool IsPlayable(uint32 questId);

    // Crater spawn positions of the creatures that give the quest, that take it back, and that count
    // for kill (or use) objective `objectiveIdx`. Empty when there are none.
    std::vector<WorldPosition> const& GetStarterPositions(uint32 questId);
    std::vector<WorldPosition> const& GetEnderPositions(uint32 questId);
    std::vector<WorldPosition> const& GetObjectivePositions(uint32 questId, uint32 objectiveIdx);
}

#endif
