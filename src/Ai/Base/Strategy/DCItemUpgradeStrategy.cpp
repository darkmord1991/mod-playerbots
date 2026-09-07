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

    // Heirlooms ride the same strategy rather than getting their own: a bot that
    // is due one is due the other, and they draw on different currencies
    // (heirlooms are paid in essence, ordinary upgrades in tokens/sap), so they
    // do not compete for the same balance.
    triggers.push_back(
        new TriggerNode(
            "seldom",
            {
                NextAction("dc heirloom upgrade", 0.9f)
            }
        )
    );
}
