/*
 * Dark Chaos - Hinterland BG bot monitoring.
 *
 * HLBG bot problems are invisible from inside the game: a bot that never leaves
 * its base looks exactly like a bot that is defending it, and a team that loses
 * 2600 resources over 20 minutes gives no clue about where they went. This is
 * the readout - per-team bot counts, how many are actually moving, how far they
 * have got from their own base, how the resource pools are draining, and which
 * bots are wedged and where.
 *
 * Two ways in:
 *   - a periodic LOG_INFO("playerbots.hlbg") report while a match runs, plus a
 *     final one at the horn, so a finished match leaves a trace in the log;
 *   - `.playerbots hlbg`, which prints the latest sample for every live HLBG.
 *
 * The unstick counters are fed by DCHinterlandTacticsAction, which is the only
 * code that can tell a stuck bot from a stationary one: it runs on the bot's own
 * map thread and owns its roaming target.
 */

#ifndef PLAYERBOTS_DCHINTERLANDMONITOR_H
#define PLAYERBOTS_DCHINTERLANDMONITOR_H

#include "Define.h"

class ChatHandler;
class Player;

namespace DCHinterlandMonitor
{
    // Called by the roaming action when a bot has failed to make any progress
    // towards its objective for the configured stall window. `strikes` is how
    // many times in a row this has now happened to that bot, so the report can
    // separate "repicked once and carried on" from "wedged".
    void NoteStall(Player* bot, uint32 strikes);

    // Called when the escalation fired and the bot was teleported back to its
    // own staging area.
    void NoteUnstickTeleport(Player* bot);

    // `.playerbots hlbg`
    bool HandleStatusCommand(ChatHandler* handler, char const* args);
}

#endif
