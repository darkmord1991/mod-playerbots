/*
 * DarkChaos addition to mod-playerbots.
 *
 * Bot-only dungeon runs. A GM command recruits five idle random bots - a tank,
 * a healer and three damage dealers of one faction, none locked to the
 * dungeon - groups them under the tank, sets the difficulty and walks them
 * into a fresh instance. Two kinds of run share everything else:
 *   - `.playerbots mplus start` sets Mythic difficulty and starts a keystone
 *     run through the DC Mythic+ run manager. It counts and pays out like any
 *     other; the live spectator list labels it "BOT".
 *   - `.playerbots dungeon start` runs Normal or Heroic without a keystone.
 *     The bosses are the dungeon's own encounters (instance_encounters and
 *     DungeonEncounter.dbc); dead bots are revived beside the tank once the
 *     fighting stops, and a cleared dungeon is left after a looting break.
 *     It lists itself in the Spectate tab through the Mythic+ spectator
 *     (RegisterSpectatableDungeonRun), labelled "BOT" as well.
 *
 * The run manager lives in scripts.lib, which modules.lib does not link, so the
 * keystone calls go through the DCMythicPlusBots functions it exports (see
 * src/server/scripts/DC/MythicPlus/dc_mythicplus_run_manager.h). The .cpp files
 * that call them re-declare those functions.
 *
 * Threads: runs are created by the chat command and advanced by a WorldScript,
 * both on the world thread. IsManaged, IsLeadingRunningRun and
 * GetRemainingBossEntries are also called by bot AI on map threads (AiFactory,
 * the "dc mythic" strategy), so the registry is locked and holds GUIDs only.
 */

#ifndef PLAYERBOTS_DCBOTMYTHICRUN_H
#define PLAYERBOTS_DCBOTMYTHICRUN_H

#include "ObjectGuid.h"

#include <vector>

class ChatHandler;
class Map;

namespace DCBotMythicRun
{
    struct Config
    {
        bool enabled = true;
        // Maps a bot run may be started on, keyed or not
        // (AiPlayerbot.DCMythicBots.Dungeons).
        std::vector<uint32> dungeons;
        uint32 maxConcurrentRuns = 1;
        uint32 botLevel = 80;
        uint32 maxKeystoneLevel = 10;
        // Tank first, then the rest: each stage must finish within this.
        uint32 enterTimeoutSeconds = 90;
        // Pause between everyone arriving and the tank moving off, so the
        // entrance grid (and its Font of Power) is loaded.
        uint32 startDelaySeconds = 3;
        // Abort when the group neither fights, kills a boss nor moves on.
        uint32 stuckSeconds = 180;
        uint32 maxRunMinutes = 45;
        // The tank pulls the nearest visible hostile within this range.
        float pullRange = 30.0f;
        // The tank waits until every member is within this range, alive and
        // above these health / mana percentages.
        float regroupDistance = 40.0f;
        uint32 restHealthPct = 60;
        uint32 restManaPct = 50;
        // Runs without a keystone: revive dead bots beside the tank and bring
        // back members that ended up outside the instance.
        bool reviveDeadBots = true;
    };

    Config GetConfig();

    // True for a bot in a bot run, from recruitment until teardown.
    bool IsManaged(ObjectGuid guid);

    // True for a run's tank while the run is under way (for a keystone run,
    // once its timer runs) and the dungeon is not yet cleared.
    bool IsLeadingRunningRun(ObjectGuid guid);

    // Creature entries of the bosses the member's run still has to kill, in
    // the order to take them. Empty when the member is in no run.
    void GetRemainingBossEntries(ObjectGuid member, Map* map, std::vector<uint32>& out);

    // `.playerbots mplus start [mapId] [keystoneLevel]`
    bool HandleStartCommand(ChatHandler* handler, char const* args);
    // `.playerbots dungeon start [mapId] [normal|heroic]`
    bool HandleDungeonStartCommand(ChatHandler* handler, char const* args);
    bool HandleStopCommand(ChatHandler* handler, char const* args);
    bool HandleStatusCommand(ChatHandler* handler, char const* args);
}

#endif
