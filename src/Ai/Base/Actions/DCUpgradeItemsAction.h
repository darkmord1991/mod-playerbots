/*
 * DarkChaos addition to mod-playerbots.
 *
 * Bots earn the DC item upgrade currencies from the same quest / kill / PvP
 * hooks players do, but had no way to spend them: the only upgrade entry points
 * are the DC addon UI and the gossip NPCs, both of which need a real client. So
 * the currency piled up and bot gear never gained the tier stat multiplier while
 * players of the same level pulled ahead.
 *
 * This action is the bot's own decision to spend it, driven by the "dc upgrade"
 * strategy. The pricing, ownership checks and persistence stay on the DC side,
 * behind the game-library façade in src/server/game/DC/DCItemUpgradeApi.h --
 * `scripts` and `modules` are sibling libraries, so that header is the only
 * place the two can meet.
 */

#ifndef PLAYERBOTS_DCUPGRADEITEMSACTION_H
#define PLAYERBOTS_DCUPGRADEITEMSACTION_H

#include "Action.h"

class PlayerbotAI;

class DCUpgradeItemsAction : public Action
{
public:
    DCUpgradeItemsAction(PlayerbotAI* botAI) : Action(botAI, "dc upgrade items") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    // Per-bot floor on how often this may run. The action is created once per
    // bot AI context and kept, so this member is genuinely per-bot.
    //
    // The trigger alone is not enough pacing at scale: "seldom" fires roughly
    // once per bot per 600s, so a 1000-bot realm produces ~1.7 fires/sec, and
    // every upgrade costs a full stat re-apply plus three DB writes. Fires/sec
    // is (online bots / this cooldown), which is the number to size against.
    uint32 _lastRunMs = 0;
};

#endif
