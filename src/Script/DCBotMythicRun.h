/*
 * DarkChaos addition to mod-playerbots.
 *
 * Bot-only dungeon runs. A run recruits idle random bots of one faction - a tank, a healer and
 * three damage dealers, or a raid's worth - none locked to the dungeon, groups them under the
 * tank, sets the difficulty and walks them into a fresh instance:
 *   - `.playerbots mplus start` sets Mythic difficulty and starts a keystone run through the DC
 *     Mythic+ run manager. It counts and pays out like any other; the live spectator list labels
 *     it "BOT".
 *   - `.playerbots dungeon start` runs Normal, Heroic or Mythic without a keystone.
 *   - With Playerbots.DCMythicBots.AutoRuns the world tick starts runs on its own: every few
 *     minutes it picks a dungeon that enough idle bots fit by level and faction.
 *
 * mod-dungeon-clear leads the group when it is loaded (DCBotRunEngine.h): it routes, pulls, runs
 * the scripted events and resurrects, owned by its headless driver character. Without it the
 * run's tank leads through the "dc mythic" strategy. Either way the runner recruits, enters,
 * revives, watches for stalls and sends every bot back to where it was recruited.
 *
 * The run manager lives in scripts.lib, which modules.lib does not link, so the keystone calls go
 * through the DCMythicPlusBots functions it exports (see
 * src/server/scripts/DC/MythicPlus/dc_mythicplus_run_manager.h).
 *
 * Threads: runs are created by the chat command and the scheduler and advanced by a WorldScript,
 * all on the world thread. IsManaged, UsesOwnLead, IsLeadingRunningRun and GetRemainingBossEntries
 * are also called by bot AI on map threads (AiFactory, the "dc mythic" strategy), so the registry
 * is locked and holds GUIDs only.
 */

#ifndef PLAYERBOTS_DCBOTMYTHICRUN_H
#define PLAYERBOTS_DCBOTMYTHICRUN_H

#include "ObjectGuid.h"

#include <string>
#include <unordered_map>
#include <vector>

class ChatHandler;
class Map;

namespace DCBotMythicRun
{
    struct Config
    {
        bool enabled = true;
        // Maps a run led by "dc mythic" may be started on (Playerbots.DCMythicBots.Dungeons).
        std::vector<uint32> dungeons;
        uint32 maxConcurrentRuns = 2;
        // Lowest bot level for a run outside mod-dungeon-clear's catalogue.
        uint32 botLevel = 80;
        uint32 maxKeystoneLevel = 10;
        uint32 enterTimeoutSeconds = 90;
        uint32 startDelaySeconds = 3;
        // "dc mythic" runs: give up when nobody fights, no boss dies and the tank does not move.
        uint32 stuckSeconds = 180;
        uint32 maxRunMinutes = 45;
        float pullRange = 30.0f;
        float regroupDistance = 40.0f;
        uint32 restHealthPct = 60;
        uint32 restManaPct = 50;
        bool reviveDeadBots = true;

        // mod-dungeon-clear leads when it is loaded.
        bool useDungeonClear = true;
        uint32 clearNoProgressSeconds = 600;
        uint32 clearMaxRunMinutes = 120;
        // Map id -> minutes, replacing clearMaxRunMinutes for dungeons too long for it (full Maraudon).
        std::unordered_map<uint32, uint32> clearMaxRunMinutesByMap;
        // A paused clear is resumed after this long; one paused at a door skips that boss first.
        uint32 clearPauseGraceSeconds = 60;
        // A clear stuck this long skips its current objective, at most clearMaxSkips times.
        uint32 clearStallGraceSeconds = 120;
        uint32 clearMaxSkips = 3;
        // A clear that reaches no new boss or objective, and whose tank reaches no new 40-yard square of the
        // dungeon, for this long skips its current one, then gives up.
        uint32 clearTargetMinutes = 20;
        // A clear that ended itself early (a wipe, a member left) is restarted this often.
        uint32 clearMaxRestarts = 3;

        // Automatic runs.
        bool autoRuns = true;
        uint32 autoIntervalSeconds = 120;
        // Catalogue tokens or map ids; empty takes every dungeon.
        std::vector<std::string> autoDungeons;
        std::vector<std::string> autoExcludeDungeons;
        // Map ids outside mod-dungeon-clear's catalogue, offered at their Heroic and Mythic levels.
        std::vector<uint32> autoExtraDungeons;
        uint32 levelBelow = 2;
        uint32 levelAbove = 10;
        uint32 normalWeight = 100;
        uint32 heroicWeight = 60;
        uint32 mythicWeight = 40;
        // Automatic keystone runs pay out and enter the leaderboards like a player's.
        uint32 keystoneWeight = 0;
        uint32 autoKeystoneLevel = 2;
        bool autoRaids = false;
        uint32 raidWeight = 20;
        uint32 raidSize = 10;
    };

    Config GetConfig();

    // True for a bot in a bot run, from recruitment until teardown.
    bool IsManaged(ObjectGuid guid);

    // True for a member of a run its tank leads through "dc mythic" (no mod-dungeon-clear).
    bool UsesOwnLead(ObjectGuid guid);

    // True for the tank of a "dc mythic" run while the run is under way (for a keystone run,
    // once its timer runs) and the dungeon is not yet cleared.
    bool IsLeadingRunningRun(ObjectGuid guid);

    // Creature entries of the bosses the member's run still has to kill, in the order to take
    // them. Empty when the member is in no run.
    void GetRemainingBossEntries(ObjectGuid member, Map* map, std::vector<uint32>& out);

    // `.playerbots mplus start [mapId] [keystoneLevel]`
    bool HandleStartCommand(ChatHandler* handler, char const* args);
    // `.playerbots dungeon start [token|mapId] [normal|heroic|mythic]`
    bool HandleDungeonStartCommand(ChatHandler* handler, char const* args);
    // `.playerbots dungeon auto [now|on|off]`
    bool HandleAutoCommand(ChatHandler* handler, char const* args);
    bool HandleStopCommand(ChatHandler* handler, char const* args);
    bool HandleStatusCommand(ChatHandler* handler, char const* args);
}

#endif
