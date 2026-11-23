/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GRINDTARGETVALUE_H
#define PLAYERBOTS_GRINDTARGETVALUE_H

#include "PlayerbotAIConfig.h"
#include "TargetValue.h"

#include <algorithm>

class PlayerbotAI;
class Unit;

class GrindTargetValue : public TargetValue
{
public:
    // Several triggers and actions read the grind target in one tick: one search per reaction cycle. Adapted from
    // SquidBots (Zyth45/mod-playerbots 5af1ce9f, after bissakov 43d3482e).
    GrindTargetValue(PlayerbotAI* botAI, std::string const name = "grind target") : TargetValue(botAI, name)
    {
        checkInterval = std::max<uint32>(2, sPlayerbotAIConfig.ReactDelay);
    }

    Unit* Get() override;
    Unit* Calculate() override;

private:
    uint32 GetTargetingPlayerCount(Unit* unit);
    Unit* FindTargetForGrinding(uint32 assistCount);
    bool needForQuest(Unit* target);
};

#endif
