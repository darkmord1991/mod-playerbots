/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DPSTARGETVALUE_H
#define PLAYERBOTS_DPSTARGETVALUE_H

#include "RtiTargetValue.h"

class PlayerbotAI;

// Kept for one AI pass: "not dps target active" and then the action searched it again in the same pass (PerfMon at
// 3,000 bots: 286,721 searches for 303,635 trigger checks). Ported from SquidBots (Zyth45/mod-playerbots 85b5c5f6).
constexpr uint32 DPS_TARGET_CACHE_MS = 100;

class DpsTargetValue : public RtiTargetValue
{
public:
    DpsTargetValue(PlayerbotAI* botAI, std::string const type = "rti", std::string const name = "dps target")
        : RtiTargetValue(botAI, type, name)
    {
        checkInterval = DPS_TARGET_CACHE_MS;
    }

    Unit* Calculate() override;

    // A kept target that died is searched again at once.
    Unit* Get() override
    {
        Unit* target = RtiTargetValue::Get();
        if (target && !target->IsAlive())
        {
            Reset();
            target = RtiTargetValue::Get();
        }

        return target;
    }
};

class DpsAoeTargetValue : public RtiTargetValue
{
public:
    DpsAoeTargetValue(PlayerbotAI* botAI, std::string const type = "rti", std::string const name = "dps aoe target")
        : RtiTargetValue(botAI, type, name)
    {
    }

    Unit* Calculate() override;
};

#endif
