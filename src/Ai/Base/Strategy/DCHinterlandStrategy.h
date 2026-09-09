/*
 * DarkChaos addition to mod-playerbots.
 *
 * The Hinterland BG counterpart to WarsongStrategy / ArathiStrategy and
 * friends. Deliberately a separate strategy rather than an extra case inside
 * "battleground": that one drives BGTactics, whose objective selection is built
 * entirely from per-BG flag, node and waypoint tables that HLBG has none of.
 *
 * See DCHinterlandTactics.h for why HLBG needs roaming rather than objective
 * capture.
 */

#ifndef PLAYERBOTS_DCHINTERLANDSTRATEGY_H
#define PLAYERBOTS_DCHINTERLANDSTRATEGY_H

#include "Strategy.h"

class PlayerbotAI;

class DCHinterlandStrategy : public Strategy
{
public:
    DCHinterlandStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    uint32 GetType() const override { return STRATEGY_TYPE_NONCOMBAT; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    std::string const getName() override { return "dc hinterland"; }
};

#endif
