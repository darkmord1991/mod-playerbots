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
}
