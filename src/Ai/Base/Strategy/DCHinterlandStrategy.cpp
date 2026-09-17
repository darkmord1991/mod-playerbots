/*
 * DarkChaos addition to mod-playerbots. See DCHinterlandStrategy.h.
 */

#include "DCHinterlandStrategy.h"

#include "Playerbots.h"

void DCHinterlandStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // "bg waiting" / "bg active" are battleground-type agnostic (they only read
    // Battleground::GetStatus), so they fire for HLBG unchanged.
    triggers.push_back(new TriggerNode("bg waiting", { NextAction("dc hinterland tactics", ACTION_BG) }));
    triggers.push_back(new TriggerNode("bg active", { NextAction("dc hinterland tactics", ACTION_BG) }));
    triggers.push_back(
        new TriggerNode("dead", { NextAction("dc hinterland reset objective", ACTION_EMERGENCY) }));

    // The stock "pvp" strategy already maps "enemy player near" to this same
    // action at 55, but that trigger is only looked at every 3 seconds. This
    // one is checked every tick and sits one notch above it, so a bot that has
    // just come into range of an enemy starts on it now rather than up to 20
    // yards further down the road. See DCHinterlandEnemyNearTrigger.
    triggers.push_back(new TriggerNode("dc hlbg enemy near", { NextAction("attack enemy player", 56.0f) }));
}
