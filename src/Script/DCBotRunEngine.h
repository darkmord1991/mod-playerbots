/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

/*
 * DarkChaos addition to mod-playerbots.
 *
 * The seam between bot-only dungeon runs (DCBotMythicRun.h) and mod-dungeon-clear, which clears
 * a dungeon far better than the runner's own "dc mythic" lead. mod-dungeon-clear links against
 * this module, not the other way round, so it registers these hooks from its own loader
 * (modules/mod-dungeon-clear/src/DarkChaos/DcBotRunEngine.cpp). Without it Get() stays null and
 * runs keep the "dc mythic" lead.
 *
 * Every hook runs on the world thread, between map updates.
 */

#ifndef PLAYERBOTS_DCBOTRUNENGINE_H
#define PLAYERBOTS_DCBOTRUNENGINE_H

#include "Define.h"

#include <string>
#include <vector>

class Player;

namespace DCBotRunEngine
{
    // One enterable dungeon; each wing of a split map (Scarlet Monastery, Dire Maul,
    // Blackrock Spire) is its own entry with its own entrance.
    struct Dungeon
    {
        std::string token;
        std::string name;
        uint32 mapId = 0;
        // Just inside the portal.
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float o = 0.0f;
        uint32 normalLevel = 0;
        // 0 when the dungeon has no heroic mode of its own.
        uint32 heroicLevel = 0;
        // Wing the clear must be told about (Blackrock Spire, Blackrock Depths, Maraudon);
        // empty when the entrance alone decides it.
        std::string wing;
        bool raid = false;
    };

    struct ClearStatus
    {
        bool enabled = false;
        bool paused = false;
        // Paused in front of a door it cannot open, waiting for it to open.
        bool pausedAtDoor = false;
        std::string pauseReason;
        // Set while the clear reports itself stuck; it keeps trying meanwhile.
        std::string stallReason;
        // Its boss list is known and nothing on it is left.
        bool finished = false;
        std::string next;
    };

    struct Hooks
    {
        bool (*available)() = nullptr;
        // The player every clear of a bot run is owned by: it authorises the clear and is each
        // member's master. Null while it is still being created or logging in; `why` says which.
        Player* (*owner)(std::string& why) = nullptr;
        std::vector<Dungeon> (*dungeons)() = nullptr;
        // Starts (or restarts) the clear of the group `leader` leads.
        bool (*start)(Player* leader, Player* owner, std::string const& wing, std::string& error) = nullptr;
        ClearStatus (*status)(Player* leader) = nullptr;
        bool (*skip)(Player* leader, Player* owner) = nullptr;
        bool (*resume)(Player* leader, Player* owner) = nullptr;
        void (*stop)(Player* leader, std::string const& reason) = nullptr;
        // Scripted area triggers on behalf of a party without a game client, one relay per run.
        void (*armRelay)(uint32 runId, uint32 mapId) = nullptr;
        void (*tickRelay)(uint32 runId, Player* leader) = nullptr;
        void (*disarmRelay)(uint32 runId) = nullptr;
    };

    // Called once, while the scripts load.
    void Register(Hooks const& hooks);

    // Null until an engine registered, and while it is switched off.
    Hooks const* Get();
}

#endif
