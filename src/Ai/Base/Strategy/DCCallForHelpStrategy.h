/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * DarkChaos addition, see DCCallForHelpActions.h and DCHelpRequests.h.
 */

#ifndef PLAYERBOTS_DCCALLFORHELPSTRATEGY_H
#define PLAYERBOTS_DCCALLFORHELPSTRATEGY_H

#include "Strategy.h"

class PlayerbotAI;

class DCCallForHelpStrategy : public Strategy
{
public:
    DCCallForHelpStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    uint32 GetType() const override { return STRATEGY_TYPE_COMBAT; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "dc call for help"; }
};

class DCHelpNearbyStrategy : public Strategy
{
public:
    DCHelpNearbyStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    uint32 GetType() const override { return STRATEGY_TYPE_NONCOMBAT; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "dc help nearby"; }
};

#endif
