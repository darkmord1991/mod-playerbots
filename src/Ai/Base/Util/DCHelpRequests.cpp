/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * DarkChaos addition, see DCHelpRequests.h.
 */

#include "DCHelpRequests.h"

#include "Creature.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "Timer.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace
{
    struct Request
    {
        ObjectGuid caller;
        ObjectGuid target;
        TeamId team;
        uint8 level;
        float x;
        float y;
        uint32 refreshedMs;
        bool eliteOrRare;
        std::vector<ObjectGuid> helpers;
    };

    constexpr uint32 REQUEST_LIFETIME_MS = 6 * IN_MILLISECONDS;  // the caller refreshes its request every 2 s
    constexpr uint32 PRUNE_INTERVAL_MS = 30 * IN_MILLISECONDS;
    constexpr uint32 SUMMARY_INTERVAL_MINUTES = 10;
    constexpr int32 ELITE_LEVELS_BELOW = 3;  // an elite this many levels below the bot still counts
    constexpr float MIN_HELPER_HEALTH_PCT = 50.0f;
    constexpr float NORMAL_FLEE_HEALTH_PCT = 50.0f;  // a bot runs from a normal creature only below this health

    std::shared_mutex sLock;
    std::unordered_map<uint64, std::vector<Request>> sRequests;
    std::atomic<uint32> sStored{0};  // live and expired requests not yet pruned; 0 lets helpers skip the lock
    uint32 sPosted = 0;               // the rest is guarded by sLock
    uint32 sClaimed = 0;
    uint32 sPruneMs = 0;
    uint32 sSummaryMs = 0;

    uint64 MapKey(Player const* player)
    {
        return (uint64(player->GetMapId()) << 32) | player->GetInstanceId();
    }

    bool IsExpired(Request const& request, uint32 now)
    {
        return getMSTimeDiff(request.refreshedMs, now) > REQUEST_LIFETIME_MS;
    }

    bool IsSoloOpenWorldRandomBot(Player* bot)
    {
        return bot->IsAlive() && !bot->GetGroup() && !bot->GetMap()->Instanceable() &&
               RandomPlayerbotMgr::instance().IsRandomBot(bot);
    }

    bool IsEliteRank(uint32 rank)
    {
        return rank == CREATURE_ELITE_ELITE || rank == CREATURE_ELITE_RAREELITE || rank == CREATURE_ELITE_WORLDBOSS;
    }

    // A creature the bot runs from: an elite FleeLevelGap or more levels above it, any other creature
    // FleeNormalAlwaysLevelGap or more above it, or FleeNormalLevelGap or more above it once the bot is losing to it.
    // 0 turns the respective check off. Creatures run faster than players, so running from a normal creature the bot
    // is beating only gets it killed.
    bool IsHopeless(Creature const* creature, Player const* bot)
    {
        bool const elite = IsEliteRank(creature->GetCreatureTemplate()->rank);
        int32 const levelDiff = int32(creature->GetLevel()) - int32(bot->GetLevel());
        int32 const alwaysGap = int32(sPlayerbotAIConfig.DCCallForHelpFleeNormalAlwaysLevelGap);
        if (!elite && alwaysGap > 0 && levelDiff >= alwaysGap)
            return true;

        int32 const fleeGap = int32(elite ? sPlayerbotAIConfig.DCCallForHelpFleeLevelGap
                                          : sPlayerbotAIConfig.DCCallForHelpFleeNormalLevelGap);
        if (fleeGap <= 0 || levelDiff < fleeGap)
            return false;

        if (elite)
            return true;

        float const botHealth = bot->GetHealthPct();
        return botHealth < NORMAL_FLEE_HEALTH_PCT && botHealth < creature->GetHealthPct();
    }

    Creature* LiveHostileCreature(PlayerbotAI* botAI, ObjectGuid guid)
    {
        Unit* unit = botAI->GetUnit(guid);
        Creature* creature = unit ? unit->ToCreature() : nullptr;
        if (!creature || !creature->IsAlive() || creature->GetCharmerOrOwnerPlayerOrPlayerItself())
            return nullptr;

        return creature;
    }

    void Prune(std::vector<Request>& requests, uint32 now)
    {
        size_t const before = requests.size();
        requests.erase(std::remove_if(requests.begin(), requests.end(),
                                      [now](Request const& request) { return IsExpired(request, now); }),
                       requests.end());
        sStored -= uint32(before - requests.size());
    }

    // Caller holds the unique lock.
    void Housekeeping(uint32 now)
    {
        if (!sPruneMs || getMSTimeDiff(sPruneMs, now) >= PRUNE_INTERVAL_MS)
        {
            for (auto& entry : sRequests)
                Prune(entry.second, now);

            sPruneMs = now;
        }

        if (!sSummaryMs)
            sSummaryMs = now;
        else if (getMSTimeDiff(sSummaryMs, now) >= SUMMARY_INTERVAL_MINUTES * MINUTE * IN_MILLISECONDS)
        {
            LOG_INFO("playerbots", "Call for help: {} requests, {} helpers sent in the last {} minutes.", sPosted,
                     sClaimed, SUMMARY_INTERVAL_MINUTES);
            sPosted = 0;
            sClaimed = 0;
            sSummaryMs = now;
        }
    }

    // Index of the nearest request the helper may answer, or -1.
    int32 PickRequest(std::vector<Request> const& requests, Player* helper, uint32 now)
    {
        ObjectGuid const self = helper->GetGUID();
        int32 const levelsBelow = int32(sPlayerbotAIConfig.DCCallForHelpLevelsBelow);
        int32 const levelsAbove = int32(sPlayerbotAIConfig.DCCallForHelpLevelsAbove);
        size_t const maxHelpers = sPlayerbotAIConfig.DCCallForHelpMaxHelpers;
        float bestDistance = 0.0f;
        int32 best = -1;

        for (size_t i = 0; i < requests.size(); ++i)
        {
            Request const& request = requests[i];
            if (IsExpired(request, now) || request.caller == self || request.team != helper->GetTeamId())
                continue;

            int32 const levelsOverCaller = int32(helper->GetLevel()) - int32(request.level);
            if (levelsOverCaller < -levelsBelow || levelsOverCaller > levelsAbove)
                continue;

            float const range = request.eliteOrRare ? sPlayerbotAIConfig.DCCallForHelpRange
                                                    : sPlayerbotAIConfig.DCCallForHelpLevelGapRange;
            float const distance = helper->GetExactDist2d(request.x, request.y);
            if (distance > range || (best >= 0 && distance >= bestDistance))
                continue;

            bool const holdsSlot =
                std::find(request.helpers.begin(), request.helpers.end(), self) != request.helpers.end();
            if (!holdsSlot && request.helpers.size() >= maxHelpers)
                continue;

            best = int32(i);
            bestDistance = distance;
        }

        return best;
    }
}

Creature* DCHelpRequests::FindHeavyAttacker(PlayerbotAI* botAI, Player* bot, bool* eliteOrRare)
{
    int32 const levelGap = int32(sPlayerbotAIConfig.DCCallForHelpLevelGap);
    Creature* byLevel = nullptr;
    GuidVector const attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();

    for (ObjectGuid const guid : attackers)
    {
        Creature* creature = LiveHostileCreature(botAI, guid);
        if (!creature || IsHopeless(creature, bot))
            continue;

        int32 const levelDiff = int32(creature->GetLevel()) - int32(bot->GetLevel());
        switch (creature->GetCreatureTemplate()->rank)
        {
            case CREATURE_ELITE_ELITE:
            case CREATURE_ELITE_RAREELITE:
            case CREATURE_ELITE_WORLDBOSS:
                if (levelDiff >= -ELITE_LEVELS_BELOW)
                {
                    if (eliteOrRare)
                        *eliteOrRare = true;
                    return creature;
                }
                break;
            case CREATURE_ELITE_RARE:
                if (levelDiff >= 0)
                {
                    if (eliteOrRare)
                        *eliteOrRare = true;
                    return creature;
                }
                break;
            default:
                if (levelGap > 0 && levelDiff >= levelGap && !byLevel)
                    byLevel = creature;
                break;
        }
    }

    // An elite or rare among the attackers wins over a normal creature that only counts by its level.
    if (byLevel && eliteOrRare)
        *eliteOrRare = false;

    return byLevel;
}

Creature* DCHelpRequests::FindHopelessAttacker(PlayerbotAI* botAI, Player* bot)
{
    GuidVector const attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const guid : attackers)
    {
        Creature* creature = LiveHostileCreature(botAI, guid);
        if (creature && IsHopeless(creature, bot))
            return creature;
    }

    return nullptr;
}

bool DCHelpRequests::CanCall(Player* bot)
{
    return sPlayerbotAIConfig.DCCallForHelp && bot->IsInCombat() && IsSoloOpenWorldRandomBot(bot);
}

bool DCHelpRequests::CanFlee(Player* bot)
{
    bool const flees = sPlayerbotAIConfig.DCCallForHelpFleeLevelGap > 0 ||
                       sPlayerbotAIConfig.DCCallForHelpFleeNormalLevelGap > 0 ||
                       sPlayerbotAIConfig.DCCallForHelpFleeNormalAlwaysLevelGap > 0;
    return flees && CanCall(bot);
}

bool DCHelpRequests::CanHelp(Player* bot)
{
    return sPlayerbotAIConfig.DCCallForHelp && sStored.load(std::memory_order_relaxed) && !bot->IsInCombat() &&
           IsSoloOpenWorldRandomBot(bot) && !bot->IsInFlight() && !bot->IsBeingTeleported() &&
           bot->GetHealthPct() >= MIN_HELPER_HEALTH_PCT;
}

void DCHelpRequests::Post(Player* caller, ObjectGuid target, bool eliteOrRare)
{
    uint32 const now = getMSTime();
    ObjectGuid const self = caller->GetGUID();

    std::unique_lock lock(sLock);
    std::vector<Request>& requests = sRequests[MapKey(caller)];

    auto itr = std::find_if(requests.begin(), requests.end(),
                            [self](Request const& request) { return request.caller == self; });
    if (itr == requests.end())
    {
        requests.push_back({ self, target, caller->GetTeamId(), uint8(caller->GetLevel()), caller->GetPositionX(),
                             caller->GetPositionY(), now, eliteOrRare, {} });
        ++sStored;
        ++sPosted;
    }
    else
    {
        if (itr->target != target)
        {
            itr->target = target;
            itr->helpers.clear();
            ++sPosted;
        }

        itr->level = uint8(caller->GetLevel());
        itr->eliteOrRare = eliteOrRare;
        itr->x = caller->GetPositionX();
        itr->y = caller->GetPositionY();
        itr->refreshedMs = now;
    }

    Housekeeping(now);
}

bool DCHelpRequests::Find(Player* helper, ObjectGuid& target, bool claim)
{
    if (!sStored.load(std::memory_order_relaxed))
        return false;

    uint32 const now = getMSTime();
    uint64 const key = MapKey(helper);

    if (!claim)
    {
        std::shared_lock lock(sLock);
        auto itr = sRequests.find(key);
        if (itr == sRequests.end())
            return false;

        int32 const index = PickRequest(itr->second, helper, now);
        if (index < 0)
            return false;

        target = itr->second[index].target;
        return true;
    }

    std::unique_lock lock(sLock);
    auto itr = sRequests.find(key);
    if (itr == sRequests.end())
        return false;

    int32 const index = PickRequest(itr->second, helper, now);
    if (index < 0)
        return false;

    Request& request = itr->second[index];
    if (std::find(request.helpers.begin(), request.helpers.end(), helper->GetGUID()) == request.helpers.end())
    {
        request.helpers.push_back(helper->GetGUID());
        ++sClaimed;
    }

    target = request.target;
    return true;
}
