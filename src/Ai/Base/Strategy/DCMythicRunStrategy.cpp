/*
 * DarkChaos addition to mod-playerbots. See DCMythicRunStrategy.h.
 */

#include "DCMythicRunStrategy.h"

#include "Playerbots.h"

void DCMythicRunStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Low on purpose: eating, drinking, buffing and looting between pulls should
    // win over leading the group on to the next one.
    triggers.push_back(new TriggerNode("dc mythic lead", { NextAction("dc mythic advance", ACTION_DEFAULT) }));
}
