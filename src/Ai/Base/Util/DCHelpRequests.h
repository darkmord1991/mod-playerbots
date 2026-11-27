/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * DarkChaos addition: the help requests behind the "dc call for help" / "dc help nearby" strategies.
 */

#ifndef PLAYERBOTS_DCHELPREQUESTS_H
#define PLAYERBOTS_DCHELPREQUESTS_H

#include "ObjectGuid.h"

class Creature;
class Player;
class PlayerbotAI;

// A solo random bot in a fight it cannot win (an elite, a rare, or a creature far above its level) posts a request;
// idle solo bots of its faction and level nearby answer it by attacking the same creature. Requests are kept per map
// instance and expire unless the caller refreshes them, and a bot only ever reads the requests of its own map, so every
// unit it resolves belongs to the map thread it runs on.
namespace DCHelpRequests
{
    // The attacker a bot may call for help against, or nullptr. `eliteOrRare` tells whether it counts by its rank (an
    // elite, rare elite, world boss or rare) or only by its level (Playerbots.DCCallForHelp.LevelGap). An elite the bot
    // runs from (FindHopelessAttacker) is not one.
    Creature* FindHeavyAttacker(PlayerbotAI* botAI, Player* bot, bool* eliteOrRare = nullptr);

    // An elite, rare elite or world boss at least FleeLevelGap levels above the bot, any other creature at least
    // FleeNormalAlwaysLevelGap levels above it, or one at least FleeNormalLevelGap levels above it while the bot is
    // below half health and below the creature's health: the bot breaks off and runs instead.
    Creature* FindHopelessAttacker(PlayerbotAI* botAI, Player* bot);

    // Whether this bot calls for help at all / runs from hopeless attackers at all / answers calls at all (checked
    // before any attacker or request is looked at).
    bool CanCall(Player* bot);
    bool CanFlee(Player* bot);
    bool CanHelp(Player* bot);

    // Adds or refreshes the bot's request against `target`.
    void Post(Player* caller, ObjectGuid target, bool eliteOrRare);

    // The nearest request this bot may answer: same map and faction, the helper between HelperLevelsBelow below and
    // HelperLevelsAbove above the caller, within Range (elites and rares) or LevelGapRange, and a free helper slot or
    // one this bot already holds. `claim` takes the slot.
    bool Find(Player* helper, ObjectGuid& target, bool claim);
}

#endif
