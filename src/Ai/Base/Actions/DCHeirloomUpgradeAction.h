/*
 * DarkChaos addition to mod-playerbots.
 *
 * The heirloom half of the DC item upgrade system. Heirlooms are the artifact
 * tiers the ordinary "dc upgrade items" action refuses: they are priced from
 * dc_heirloom_upgrade_costs (essence, mostly), and instead of a stat multiplier
 * they carry a stat-package enchant, id 900000 + package*100 + level.
 *
 * So there are two decisions here that the ordinary path does not have:
 *   - WHICH stat package, out of the twelve, suits this bot. Picked from its
 *     role, since a tank and a healer want very different heirlooms.
 *   - Whether the bot owns a heirloom at all. The starter heirloom is only
 *     obtainable from the onboarding quest chain, which playerbots never run,
 *     so without a grant this action would have nothing to work on.
 */

#ifndef PLAYERBOTS_DCHEIRLOOMUPGRADEACTION_H
#define PLAYERBOTS_DCHEIRLOOMUPGRADEACTION_H

#include "Action.h"

class PlayerbotAI;

class DCHeirloomUpgradeAction : public Action
{
public:
    DCHeirloomUpgradeAction(PlayerbotAI* botAI) : Action(botAI, "dc heirloom upgrade") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    // Per-bot floor between runs; see DCUpgradeItemsAction.h for why the trigger
    // alone is not enough pacing across a full roster.
    uint32 _lastRunMs = 0;
};

#endif
