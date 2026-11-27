/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTSTARTLOCATION_H
#define PLAYERBOTS_BOTSTARTLOCATION_H

#include "Define.h"

class Player;

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
    // the crater is a small zone: see Playerbots.AzsharaCraterMaxBots.
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

    // Lowest level a death knight bot may be set to, by any re-roll, reset or level bracket move: the realm's heroic
    // start level while it holds a crater slot, GetDeathKnightStartLevel() anywhere else.
    uint32 GetDeathKnightLevelFloor(Player const* bot);
}

// DarkChaos: the bots that hold one of the Playerbots.AzsharaCraterMaxBots slots on Azshara Crater.
//
// A slot belongs to a character whether it is logged in or not, from the moment it is created on the
// crater or teleported onto it until it is teleported off. The cap used to be checked against the
// bots standing on the map, which let the crater keep taking bots while its residents were logged
// out -- and a resident never leaves before it graduates, so the population only ever grew.
//
// Called from the world thread and from bot AI on the map threads, so every member locks.
class CraterRoster
{
public:
    // Rebuilds the roster from the characters database: the bot characters saved on the crater, most
    // recently played first, up to the cap. Any beyond it get no slot and are moved off the crater the
    // next time the random bot manager processes them. Startup only (world thread, blocking query).
    static void Load();

    static bool IsResident(uint32 guid);
    static bool HasFreeSlot();

    // True when `guid` already holds a slot, or a slot was free and is now its.
    static bool Claim(uint32 guid);
    static void Release(uint32 guid);
};

// DarkChaos: the bot slots on the leveling continents capped by Playerbots.DCLevelAreas.MapBotCaps (maps 750 and
// 751). Unlike the crater's, a slot is held only while the bot is online: it is taken when the bot is teleported
// onto the map or logs in there (RandomPlayerbotMgr::ProcessBot), and given up when it is teleported to any other
// map or logs out. Counting offline characters too, as the crater does, left 168 of map 750's 500 slots to bots
// that were not logged in (2026-11-11) while 174 online bots of its levels waited elsewhere. A bot holds at most
// one slot.
//
// Called from the world thread and from bot AI on the map threads, so every member locks.
class DCMapRoster
{
public:
    // Empties every capped map's roster and logs how many bots are saved on each. Startup only (world thread,
    // blocking query).
    static void Load();
    static bool IsCapped(uint32 mapId);
    // True when the map is not capped, the bot already holds a slot there, or one is free.
    static bool CanEnter(uint32 mapId, uint32 guid);
    // Takes a slot on a capped map, giving up any slot held on another. False when the map is full.
    static bool Claim(uint32 mapId, uint32 guid);
    // Gives up the bot's slot, whichever map it is on.
    static void Release(uint32 guid);
};

#endif
