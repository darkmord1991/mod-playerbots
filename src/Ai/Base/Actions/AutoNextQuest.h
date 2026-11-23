/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_AUTONEXTQUEST_H
#define PLAYERBOTS_AUTONEXTQUEST_H

#include "Object.h"
#include "Player.h"
#include "QuestDef.h"

// A player who turns a quest in is handed its auto-accept follow-up at once (WorldSession::
// HandleQuestgiverChooseRewardOpcode). Bots turn in with Player::RewardQuest, which skips that handler, so every chain
// behind such a quest stopped (767 Rite of Vision needs 763). Call right after bot->RewardQuest. Ported from SquidBots
// (Zyth45/mod-playerbots ebf73cc6), extended to game object quest givers as the core does.
inline bool TakeAutoNextQuest(Player* bot, Quest const* quest, Object* questGiver)
{
    if (!bot || !quest || !questGiver || (!questGiver->IsCreature() && !questGiver->IsGameObject()))
        return false;

    Quest const* next = bot->GetNextQuest(questGiver->GetGUID(), quest);
    if (!next || !next->IsAutoAccept() || !bot->CanTakeQuest(next, false) || !bot->CanAddQuest(next, false))
        return false;

    bot->AddQuestAndCheckCompletion(next, questGiver);
    return true;
}

#endif
