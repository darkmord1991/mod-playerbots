/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * DarkChaos addition, see DCCallForHelpActions.h.
 */

#include "DCCallForHelpActions.h"

#include "Creature.h"
#include "DCHelpRequests.h"
#include "ObjectAccessor.h"
#include "Playerbots.h"

namespace
{
    constexpr float ENGAGE_DISTANCE = 30.0f;    // a helper this close, in line of sight, attacks
    constexpr float APPROACH_DISTANCE = 15.0f;  // otherwise it walks this close first
    constexpr float FLEE_STEP = 20.0f;          // each flee move ends this far from where the bot stands
}

bool DCHopelessAttackerTrigger::IsActive()
{
    return DCHelpRequests::CanFlee(bot) && DCHelpRequests::FindHopelessAttacker(botAI, bot);
}

bool DCHeavyAttackerTrigger::IsActive()
{
    return DCHelpRequests::CanCall(bot) && DCHelpRequests::FindHeavyAttacker(botAI, bot);
}

bool DCHelpRequestNearbyTrigger::IsActive()
{
    ObjectGuid target;
    return DCHelpRequests::CanHelp(bot) && DCHelpRequests::Find(bot, target, false);
}

bool DCCallForHelpAction::Execute(Event /*event*/)
{
    bool eliteOrRare = false;
    Creature* attacker = DCHelpRequests::FindHeavyAttacker(botAI, bot, &eliteOrRare);
    if (!attacker)
        return false;

    DCHelpRequests::Post(bot, attacker->GetGUID(), eliteOrRare);
    return true;
}

bool DCCallForHelpAction::isUseful()
{
    return DCHelpRequests::CanCall(bot);
}

bool DCFleeHopelessAttackerAction::Execute(Event /*event*/)
{
    Creature* attacker = DCHelpRequests::FindHopelessAttacker(botAI, bot);
    if (!attacker)
        return false;

    if (bot->GetVictim())
        bot->AttackStop();

    if (bot->IsNonMeleeSpellCast(false))
        bot->InterruptNonMeleeSpells(false);

    context->GetValue<Unit*>("current target")->Set(nullptr);
    return MoveAway(attacker, FLEE_STEP);
}

bool DCFleeHopelessAttackerAction::isUseful()
{
    return DCHelpRequests::CanFlee(bot) && IsMovingAllowed();
}

bool DCHelpNearbyBotAction::Execute(Event /*event*/)
{
    ObjectGuid targetGuid;
    if (!DCHelpRequests::Find(bot, targetGuid, true))
        return false;

    Creature* target = ObjectAccessor::GetCreature(*bot, targetGuid);
    if (!target || !target->IsInWorld() || !target->IsAlive() || !bot->InSamePhase(target) ||
        !bot->IsValidAttackTarget(target))
        return false;

    if (bot->GetExactDist(target) > ENGAGE_DISTANCE || !bot->IsWithinLOSInMap(target))
        return MoveNear(target, APPROACH_DISTANCE);

    return Attack(target);
}

bool DCHelpNearbyBotAction::isUseful()
{
    return DCHelpRequests::CanHelp(bot);
}
