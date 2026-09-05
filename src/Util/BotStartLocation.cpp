/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotStartLocation.h"

#include "SharedDefines.h"

#include <unordered_map>

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
