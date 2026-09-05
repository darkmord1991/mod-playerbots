/*
 * DarkChaos addition to mod-playerbots.
 *
 * Kept as its own strategy rather than folded into "maintenance": that one is
 * commented out of AiFactory and defaults to off (RandomBotNonCombatStrategies
 * is empty), so an action hung off it would never run. This one is added to the
 * always-on non-combat list instead, and can still be toggled by name.
 */

#ifndef PLAYERBOTS_DCITEMUPGRADESTRATEGY_H
#define PLAYERBOTS_DCITEMUPGRADESTRATEGY_H

#include "NonCombatStrategy.h"

class PlayerbotAI;

class DCItemUpgradeStrategy : public NonCombatStrategy
{
public:
    DCItemUpgradeStrategy(PlayerbotAI* botAI) : NonCombatStrategy(botAI) {}

    std::string const getName() override { return "dc upgrade"; }
    uint32 GetType() const override { return STRATEGY_TYPE_NONCOMBAT; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

#endif
