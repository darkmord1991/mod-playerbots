/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * DarkChaos addition: solo random bots call idle bots nearby into fights they cannot win alone, see
 * DCHelpRequests.h. "dc call for help" (combat) posts the request, "dc help nearby" (non-combat) answers it.
 */

#ifndef PLAYERBOTS_DCCALLFORHELPACTIONS_H
#define PLAYERBOTS_DCCALLFORHELPACTIONS_H

#include "AttackAction.h"
#include "Trigger.h"

class PlayerbotAI;

// An elite, a rare, or a creature far above the bot's level is attacking a solo random bot.
class DCHeavyAttackerTrigger : public Trigger
{
public:
    DCHeavyAttackerTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dc heavy attacker", 2 * IN_MILLISECONDS) {}

    bool IsActive() override;
};

// A creature far above a solo random bot's level is attacking it (an elite 3+, any other 5+ levels by default).
// Checked every tick: the flee has to win each tick against the rotation, or the bot turns back to fight.
class DCHopelessAttackerTrigger : public Trigger
{
public:
    DCHopelessAttackerTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dc hopeless attacker") {}

    bool IsActive() override;
};

// An idle solo random bot is near a call for help it may answer.
class DCHelpRequestNearbyTrigger : public Trigger
{
public:
    DCHelpRequestNearbyTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dc help request nearby", 3 * IN_MILLISECONDS) {}

    bool IsActive() override;
};

class DCCallForHelpAction : public Action
{
public:
    DCCallForHelpAction(PlayerbotAI* botAI) : Action(botAI, "dc call for help") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

// Stops fighting and runs from the creature until it gives up the chase (it leashes once the bot is far enough from
// its home).
class DCFleeHopelessAttackerAction : public MovementAction
{
public:
    DCFleeHopelessAttackerAction(PlayerbotAI* botAI) : MovementAction(botAI, "dc flee hopeless attacker") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

class DCHelpNearbyBotAction : public AttackAction
{
public:
    DCHelpNearbyBotAction(PlayerbotAI* botAI) : AttackAction(botAI, "dc help nearby bot") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
