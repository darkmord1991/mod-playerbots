/*
 * DarkChaos addition to mod-playerbots. See DCItemUpgradeStrategy.h.
 */

#include "DCItemUpgradeStrategy.h"
#include "Playerbots.h"

void DCItemUpgradeStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // "seldom" is a RandomTrigger with a 300 probability, so a bot reaches the
    // upgrade vendor's business rarely and the world thread never sees a burst.
    // The action itself buys a bounded number of levels per run
    // (AiPlayerbot.DCItemUpgrade.StepsPerRun) and bails out early when the bot
    // holds no currency.
    triggers.push_back(
        new TriggerNode(
            "seldom",
            {
                NextAction("dc upgrade items", 1.0f)
            }
        )
    );
}
