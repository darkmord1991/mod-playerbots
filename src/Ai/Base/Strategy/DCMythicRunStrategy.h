/*
 * DarkChaos addition to mod-playerbots.
 *
 * Carried by every bot in a bot-only Mythic+ run (see DCBotMythicRun.h); only
 * the run's tank ever acts on it. Added in AiFactory rather than with
 * ChangeStrategy, so a strategy reset rebuilds it.
 */

#ifndef PLAYERBOTS_DCMYTHICRUNSTRATEGY_H
#define PLAYERBOTS_DCMYTHICRUNSTRATEGY_H

#include "Strategy.h"

class PlayerbotAI;

class DCMythicRunStrategy : public Strategy
{
public:
    DCMythicRunStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    uint32 GetType() const override { return STRATEGY_TYPE_NONCOMBAT; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "dc mythic"; }
};

#endif
