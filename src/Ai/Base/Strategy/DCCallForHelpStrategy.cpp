/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * DarkChaos addition, see DCCallForHelpStrategy.h.
 */

#include "DCCallForHelpStrategy.h"

#include "Playerbots.h"

void DCCallForHelpStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Posting the request costs the bot one tick every 2 s of the fight; above the rotation so it is not starved.
    triggers.push_back(new TriggerNode("dc heavy attacker", { NextAction("dc call for help", ACTION_HIGH + 5) }));

    // Running from a creature far above the bot beats everything but the pull sequence, or the rotation turns it back.
    triggers.push_back(
        new TriggerNode("dc hopeless attacker", { NextAction("dc flee hopeless attacker", ACTION_EMERGENCY + 2) }));
}

void DCHelpNearbyStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Above questing, grinding and looting: a bot that fights on alone dies in most of these fights.
    triggers.push_back(
        new TriggerNode("dc help request nearby", { NextAction("dc help nearby bot", ACTION_HIGH + 5) }));
}
