/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotStartLocation.h"

#include "DatabaseEnv.h"
#include "Log.h"
#include "PlayerbotAIConfig.h"
#include "QueryResult.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "World.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace
{
    // Verbatim from data/sql/base/db_world/playercreateinfo.sql (stock AzerothCore 3.3.5a).
    std::unordered_map<uint8, BotStartLocation> const StockStarts =
    {
        { RACE_HUMAN,          {   0,   12, -8949.95f,   -132.493f,   83.5312f, 0.0f      } }, // Northshire, Elwynn Forest
        { RACE_ORC,            {   1,   14,  -618.518f,  -4251.67f,   38.718f,  0.0f      } }, // Valley of Trials, Durotar
        { RACE_DWARF,          {   0,    1, -6240.32f,     331.033f, 382.758f,  6.17716f  } }, // Coldridge Valley, Dun Morogh
        { RACE_NIGHTELF,       {   1,  141, 10311.3f,      832.463f, 1326.41f,  5.69632f  } }, // Shadowglen, Teldrassil
        { RACE_UNDEAD_PLAYER,  {   0,   85,  1676.71f,    1678.31f,  121.67f,   2.70526f  } }, // Deathknell, Tirisfal Glades
        { RACE_TAUREN,         {   1,  215, -2917.58f,    -257.98f,   52.9968f, 0.0f      } }, // Camp Narache, Mulgore
        { RACE_GNOME,          {   0,    1, -6240.32f,     331.033f, 382.758f,  0.0f      } }, // Coldridge Valley, Dun Morogh
        { RACE_TROLL,          {   1,   14,  -618.518f,  -4251.67f,   38.718f,  0.0f      } }, // Valley of Trials, Durotar
        { RACE_BLOODELF,       { 530, 3431, 10349.6f,   -6357.29f,    33.4026f, 5.31605f  } }, // Sunstrider Isle, Eversong Woods
        { RACE_DRAENEI,        { 530, 3526, -3961.64f, -13931.2f,    100.615f,  2.08364f  } }, // Ammen Vale, Azuremyst Isle
    };

    // The realm's own start, verbatim from `playercreateinfo` -- all 180 race/class rows share it.
    BotStartLocation const CraterStart = { 37, 268, 131.82f, 1025.28f, 296.27f, 5.483f };

    // Death knights ignore their race's start entirely -- stock puts every one of them in
    // Ebon Hold. The per-race jitter in playercreateinfo is cosmetic; one spot is enough.
    BotStartLocation const DeathKnightStart = { 609, 4298, 2356.21f, -5662.21f, 426.026f, 3.65997f };

    // DarkChaos-only races have no stock start of their own, so each borrows one from a stock
    // race of the SAME faction. Change the right-hand side to move a race somewhere else.
    std::unordered_map<uint8, uint8> const DonorRaces =
    {
        { RACE_GOBLIN,            RACE_TROLL    }, // Horde    -> Valley of Trials, Durotar
        { RACE_WORGEN,            RACE_HUMAN    }, // Alliance -> Northshire, Elwynn Forest
        { RACE_PANDAREN_ALLIANCE, RACE_NIGHTELF }, // Alliance -> Shadowglen, Teldrassil
        { RACE_PANDAREN_HORDE,    RACE_TAUREN   }, // Horde    -> Camp Narache, Mulgore
        { RACE_VULPERA,           RACE_TROLL    }, // Horde    -> Valley of Trials, Durotar
        { RACE_ZANDALARI_TROLL,   RACE_TROLL    }, // Horde    -> Valley of Trials, Durotar
        { RACE_KUL_TIRAN,         RACE_HUMAN    }, // Alliance -> Northshire, Elwynn Forest
        { RACE_DARK_IRON_DWARF,   RACE_DWARF    }, // Alliance -> Coldridge Valley, Dun Morogh
    };

    std::mutex CraterRosterMutex;
    std::unordered_set<uint32> CraterResidents;
}

BotStartLocation const& BotStartLocations::GetCraterStart() { return CraterStart; }

uint32 BotStartLocations::GetDeathKnightStartLevel()
{
    return std::max<uint32>(sPlayerbotAIConfig.deathKnightStartLevel,
                            sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL));
}

uint8 BotStartLocations::GetDonorRace(uint8 race)
{
    if (StockStarts.find(race) != StockStarts.end())
        return race;

    auto const itr = DonorRaces.find(race);
    return itr != DonorRaces.end() ? itr->second : 0;
}

BotStartLocation const* BotStartLocations::Get(uint8 race, uint8 cls)
{
    if (cls == CLASS_DEATH_KNIGHT)
        return &DeathKnightStart;

    uint8 const donor = GetDonorRace(race);
    if (!donor)
        return nullptr;

    auto const itr = StockStarts.find(donor);
    return itr != StockStarts.end() ? &itr->second : nullptr;
}

void CraterRoster::Load()
{
    uint32 const cap = sPlayerbotAIConfig.azsharaCraterMaxBots;

    std::string prefix = sPlayerbotAIConfig.randomBotAccountPrefix;
    CharacterDatabase.EscapeString(prefix);

    // Most recently played first: the bots that were actually using the crater keep their slots, so a
    // restart rebuilds the roster that was in use rather than an arbitrary one.
    std::string const sql = Acore::StringFormat(
        "SELECT guid FROM characters WHERE map = {} AND account IN "
        "(SELECT id FROM {}.account WHERE username LIKE '{}%') ORDER BY logout_time DESC, guid",
        CraterStart.mapId, LoginDatabase.GetConnectionInfo()->database, prefix);

    std::unordered_set<uint32> residents;
    uint32 surplus = 0;
    if (QueryResult result = CharacterDatabase.Query(sql))
    {
        do
        {
            if (residents.size() < cap)
                residents.insert((*result)[0].Get<uint32>());
            else
                ++surplus;
        } while (result->NextRow());
    }

    LOG_INFO("playerbots", "{} of {} Azshara Crater bot slots are taken.", residents.size(), cap);
    if (surplus)
        LOG_INFO("playerbots",
                 "{} more bots are saved on Azshara Crater without a slot and will be moved off it "
                 "as they log in.",
                 surplus);

    std::lock_guard<std::mutex> lock(CraterRosterMutex);
    CraterResidents = std::move(residents);
}

bool CraterRoster::IsResident(uint32 guid)
{
    std::lock_guard<std::mutex> lock(CraterRosterMutex);
    return CraterResidents.count(guid) != 0;
}

bool CraterRoster::HasFreeSlot()
{
    std::lock_guard<std::mutex> lock(CraterRosterMutex);
    return CraterResidents.size() < sPlayerbotAIConfig.azsharaCraterMaxBots;
}

bool CraterRoster::Claim(uint32 guid)
{
    std::lock_guard<std::mutex> lock(CraterRosterMutex);
    if (CraterResidents.count(guid))
        return true;

    if (CraterResidents.size() >= sPlayerbotAIConfig.azsharaCraterMaxBots)
        return false;

    CraterResidents.insert(guid);
    return true;
}

void CraterRoster::Release(uint32 guid)
{
    std::lock_guard<std::mutex> lock(CraterRosterMutex);
    CraterResidents.erase(guid);
}
