/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AcceptInvitationAction.h"
#include "Event.h"
#include "ObjectAccessor.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotOperations.h"
#include "PlayerbotSecurity.h"
#include "PlayerbotTextMgr.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "Playerbots.h"
#include "WorldPacket.h"

#include <mutex>
#include <unordered_set>

namespace
{
    std::mutex PendingAcceptLock;
    std::unordered_set<ObjectGuid> PendingAccept;
}

// CMSG_GROUP_ACCEPT is a world-thread opcode: accepting from the bot's map thread made Group::Create change the
// inviter, who may be on another map. The acceptance and what follows it run on the world thread a few milliseconds
// later. Ported from SquidBots (Zyth45/mod-playerbots 85ac93cc).
bool AcceptInvitationAction::Execute(Event /*event*/)
{
    ObjectGuid const guid = bot->GetGUID();
    {
        std::lock_guard<std::mutex> lock(PendingAcceptLock);
        if (!PendingAccept.insert(guid).second)
            return true;
    }

    auto accept = [guid](Player* player)
    {
        {
            std::lock_guard<std::mutex> lock(PendingAcceptLock);
            PendingAccept.erase(guid);
        }

        PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
        if (!ai)
            return false;

        AcceptInvitationAction action(ai);
        return action.AcceptNow();
    };

    auto acceptOp = std::make_unique<WorldThreadCallOperation>(guid, "AcceptInvitation", std::move(accept));
    if (PlayerbotWorldThreadProcessor::instance().QueueOperation(std::move(acceptOp)))
        return true;

    std::lock_guard<std::mutex> lock(PendingAcceptLock);
    PendingAccept.erase(guid);
    return false;
}

bool AcceptInvitationAction::AcceptNow()
{
    Group* grp = bot->GetGroupInvite();
    if (!grp)
        return false;
    Player* inviter = ObjectAccessor::FindPlayer(grp->GetLeaderGUID());
    if (!inviter)
        return false;

    if (!botAI->GetSecurity()->CheckLevelFor(PLAYERBOT_SECURITY_INVITE, false, inviter))
    {
        WorldPacket data(SMSG_GROUP_DECLINE, 10);
        data << bot->GetName();
        inviter->SendDirectMessage(&data);
        bot->UninviteFromGroup();
        return false;
    }

    if (bot->isAFK() && !IsSelfBot(bot))
        bot->ToggleAFK();

    WorldPacket p;
    uint32 roles_mask = 0;
    p << roles_mask;
    bot->GetSession()->HandleGroupAcceptOpcode(p);

    if (!bot->GetGroup() || !bot->GetGroup()->IsMember(inviter->GetGUID()))
        return false;

    if (sRandomPlayerbotMgr.IsRandomBot(bot))
        botAI->SetMaster(inviter);
    // else
    // PlayerbotRepository::instance().Save(botAI);

    botAI->ResetStrategies();
    botAI->ChangeStrategy("+follow,-lfg,-bg", BOT_STATE_NON_COMBAT);
    botAI->Reset();

    botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault("hello", "Hello", {}));

    if (sPlayerbotAIConfig.SummonWhenGroup && bot->GetDistance(inviter) > sPlayerbotAIConfig.SightDistance)
    {
        Teleport(inviter, bot, true);
    }
    return true;
}
