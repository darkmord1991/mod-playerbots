/*
 * DarkChaos addition to mod-playerbots.
 *
 * The team front line for Hinterland BG bots.
 *
 * Roaming - every bot rolling its own destination from a bucket of landmarks -
 * kept the bots busy but made them useless as a team: forty bots arrived at
 * five places in ones and twos, each group got eaten by the guards it found
 * there, and from the outside it read as bots wandering off in all directions
 * and never actually taking anything. The monitor confirmed it: both sides at
 * 100% away from base, kill counts swinging wildly, and the resource pools
 * draining from guard deaths on both ends of the map at once.
 *
 * This replaces the per-bot roll with one shared objective per team: a lane of
 * nodes running from the team's own line, through the two midfield outposts,
 * to the enemy line, the enemy camp and finally the enemy faction boss. Every
 * attacker on the team heads for the same node, so they arrive as a blob, clear
 * it together, and only then move on.
 *
 * Two things gate the lane, and both were learned the hard way from the first
 * monitored match on the lane:
 *
 *   - It advances only when enough bots on the node have reported it clear of
 *     hostiles for a few seconds AND a quorum of the team's attackers is
 *     actually there. Without the quorum the first three arrivals at the own
 *     line voted it clear at once, the lane ran three nodes ahead in under a
 *     minute, and the "blob" was a string of bots spread along the whole lane.
 *
 *   - It falls back one node when fewer than a handful of living teammates
 *     have been near the node for a while. Without the threshold a single
 *     respawner walking up every twenty seconds counted as presence, so a team
 *     wiped at the middle never regrouped: it fed itself back into the enemy
 *     blob one bot at a time, for the rest of the match.
 *
 * Defenders (a fixed share of the roster, by "bg role") are not on the lane;
 * they patrol the team's own line, camp and boss.
 *
 * State is keyed by battleground instance and team. Every bot in one instance
 * runs on that instance's map thread, but instances run in parallel and the
 * monitor reads from the world thread, so it is mutex guarded.
 */

#ifndef PLAYERBOTS_DCHINTERLANDFRONT_H
#define PLAYERBOTS_DCHINTERLANDFRONT_H

#include "DCHinterlandGeography.h"

namespace DCHinterlandFront
{
    // Nodes in a team's lane, first to last. Index 0 is the team's own line,
    // the last index is the enemy faction boss.
    uint32 LaneLength();
    DCHinterland::Point const& LaneNode(TeamId teamId, uint32 index);

    struct View
    {
        uint32 index = 0;
        char const* name = "";
        // How long the lane has pointed at this node.
        uint32 heldSeconds = 0;
        // Distinct bots that have reported the node clear inside the vote window.
        uint32 clearVotes = 0;
        // Living teammates near the node at the last presence report, and how
        // many the lane wants there before it advances.
        uint32 presentNear = 0;
        uint32 quorum = 0;
        // Player kills scored and deaths taken near the node in the last
        // minute - the losing signal.
        uint32 recentKillsNear = 0;
        uint32 recentDeathsNear = 0;
        // Bumped on every advance or retreat, so a bot can notice a change with
        // one integer compare instead of re-deriving the node.
        uint32 revision = 0;
    };

    View Get(uint32 instanceId, TeamId teamId, uint32 nowMs);

    // A bot standing on the front node says whether any hostile is still
    // around it. Clear reports are votes to advance.
    void ReportAtNode(uint32 instanceId, TeamId teamId, uint64 botGuid, bool nodeClear, uint32 nowMs);

    // Any attacker may report how many living teammates are near the front
    // node right now, and how many the team has alive in total. The node
    // counts as held while at least MinPresence are near it; not held for
    // RegroupSeconds retreats the lane by one node. This is what catches a
    // wipe, because dead bots and bots mid-fight do not report through
    // ReportAtNode.
    void ReportPresence(uint32 instanceId, TeamId teamId, uint32 aliveNear, uint32 aliveTeam, uint32 nowMs);

    // Every player kill in the battleground, from the monitor's PvP-kill hook.
    // A death near a team's front node counts against that team; a kill near
    // the killer's front node counts for it. Over the last minute, deaths
    // outnumbering kills two to one while the node is below quorum means the
    // team is feeding into a fight it cannot win, and the node stops counting
    // as held - the respawn waves the battleground releases every 30 s were
    // otherwise enough to keep resetting the retreat timer, three or five bots
    // at a time, for the rest of the match.
    void NoteKill(uint32 instanceId, TeamId killerTeam, TeamId victimTeam, float x, float y, uint32 nowMs);

    // Drops everything for one instance. Called at the horn (fresh lane) and
    // when the battleground is destroyed.
    void Forget(uint32 instanceId);

    struct Config
    {
        uint32 advanceVotes = 3;
        uint32 holdSeconds = 10;
        uint32 regroupSeconds = 45;
        uint32 voteWindowSeconds = 6;
        // Share of the team's living attackers that must be near a node before
        // the lane leaves it, so the group moves as one. Attackers are
        // estimated from the living roster and the defender share.
        uint32 advanceQuorumPct = 50;
        uint32 minPresence = 3;
        uint32 defenderRoles = 2;
        // Yards from the node within which kills and deaths are attributed to it.
        float presenceRadius = 60.0f;
        uint32 losingWindowSeconds = 60;
        uint32 losingMinDeaths = 6;
    };

    void SetConfig(Config const& config);
}

#endif
