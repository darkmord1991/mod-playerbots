/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTSTARTLOCATION_H
#define PLAYERBOTS_BOTSTARTLOCATION_H

#include "Define.h"

// DarkChaos: `playercreateinfo` points every race at the onboarding hub on map 37, which is
// deliberate for human players but useless for bots -- a bot parked there has no starter quests,
// no graveyard and no travel-graph node, so the whole levelling pipeline stalls. These are the
// stock 3.3.5a start positions, kept in code so bots use them without touching the table that
// real characters are created from.
struct BotStartLocation
{
    uint32 mapId;
    uint32 zoneId;
    float  x;
    float  y;
    float  z;
    float  o;
};

namespace BotStartLocations
{
    // Stock start position for the given race/class, ignoring any server-side
    // `playercreateinfo` override. Death knights (class 6) always start at Ebon Hold.
    // Returns nullptr when the race has no mapping.
    BotStartLocation const* Get(uint8 race, uint8 cls);

    // Race whose start position `race` borrows. Stock races map to themselves; the
    // DarkChaos-only races map onto a faction-appropriate stock race. Returns 0 if unmapped.
    uint8 GetDonorRace(uint8 race);

    // The DarkChaos onboarding hub on Azshara Crater -- the position `playercreateinfo` gives every
    // race. Unlike the stock starts this one is handed out to only a limited number of bots, because
    // the crater is a small zone: see AiPlayerbot.AzsharaCraterMaxBots.
    BotStartLocation const& GetCraterStart();

    // Level a death knight bot starts at when it is placed at the Ebon Hold start above rather than
    // on the crater.
    //
    // The realm sets StartHeroicPlayerLevel to 1 so a player's death knight begins on the onboarding
    // hub alongside every other class. That is fine on the crater, but the Ebon Hold start sits in
    // the Scarlet Enclave, whose mobs are level 55-58 -- a level 1 bot dropped there dies on sight,
    // revives at its corpse and dies again for as long as it stays logged in. Bots that start there
    // get the stock death knight level instead, so they spawn as ordinary death knights.
    //
    // Never below the realm's own heroic start level: raising StartHeroicPlayerLevel past this
    // setting should raise bots with it.
    uint32 GetDeathKnightStartLevel();
}

#endif
