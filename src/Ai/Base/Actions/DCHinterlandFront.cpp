/*
 * DarkChaos addition to mod-playerbots. See DCHinterlandFront.h.
 */

#include "DCHinterlandFront.h"

#include "Common.h"

#include <algorithm>
#include <deque>
#include <iterator>
#include <mutex>
#include <unordered_map>

namespace
{
    using DCHinterland::Point;

    // Both lanes sweep both midfield outposts, in opposite order, so the two
    // teams meet in the middle rather than sliding past each other on parallel
    // tracks - which is exactly what the random roaming produced.
    constexpr Point const ALLIANCE_LANE[] = {
        DCHinterland::ALLIANCE_LINE, DCHinterland::MID_CENTER, DCHinterland::MID_NORTH,
        DCHinterland::HORDE_LINE,    DCHinterland::HORDE_CAMP, DCHinterland::HORDE_BOSS,
    };

    constexpr Point const HORDE_LANE[] = {
        DCHinterland::HORDE_LINE,    DCHinterland::MID_NORTH,     DCHinterland::MID_CENTER,
        DCHinterland::ALLIANCE_LINE, DCHinterland::ALLIANCE_CAMP, DCHinterland::ALLIANCE_BOSS,
    };

    static_assert(std::size(ALLIANCE_LANE) == std::size(HORDE_LANE), "lanes must be the same length");

    constexpr uint32 LANE_LENGTH = uint32(std::size(ALLIANCE_LANE));

    struct TeamFront
    {
        uint32 index = 0;
        uint32 revision = 0;
        uint32 changedMs = 0;
        // Distinct reporters, with the time of their last clear report.
        std::unordered_map<uint64, uint32> clearReports;
        uint32 firstClearMs = 0;
        // Last time the node counted as held (MinPresence living teammates
        // near it), and what the last presence report said.
        uint32 lastHeldMs = 0;
        uint32 lastAliveNear = 0;
        uint32 lastAliveTeam = 0;
        // Timestamps of player kills scored / deaths taken near the node.
        std::deque<uint32> killsNear;
        std::deque<uint32> deathsNear;
    };

    struct InstanceFronts
    {
        TeamFront team[PVP_TEAMS_COUNT];
    };

    std::mutex s_mutex;
    std::unordered_map<uint32, InstanceFronts> s_fronts;
    DCHinterlandFront::Config s_cfg;

    TeamFront& Locate(uint32 instanceId, TeamId teamId, uint32 nowMs)
    {
        TeamFront& front = s_fronts[instanceId].team[teamId];
        if (!front.changedMs)
        {
            front.changedMs = nowMs;
            front.lastHeldMs = nowMs;
        }

        return front;
    }

    uint32 CountClearVotes(TeamFront& front, uint32 nowMs)
    {
        uint32 const windowMs = s_cfg.voteWindowSeconds * IN_MILLISECONDS;

        for (auto itr = front.clearReports.begin(); itr != front.clearReports.end();)
        {
            if ((nowMs - itr->second) > windowMs)
                itr = front.clearReports.erase(itr);
            else
                ++itr;
        }

        return uint32(front.clearReports.size());
    }

    void Prune(std::deque<uint32>& stamps, uint32 nowMs)
    {
        uint32 const windowMs = s_cfg.losingWindowSeconds * IN_MILLISECONDS;
        while (!stamps.empty() && (nowMs - stamps.front()) > windowMs)
            stamps.pop_front();
    }

    // Feeding: over the window, deaths near the node outnumber kills two to
    // one and there are at least a handful of them.
    bool IsLosing(TeamFront& front, uint32 nowMs)
    {
        Prune(front.killsNear, nowMs);
        Prune(front.deathsNear, nowMs);

        uint32 const deaths = uint32(front.deathsNear.size());
        uint32 const kills = uint32(front.killsNear.size());
        return deaths >= s_cfg.losingMinDeaths && deaths >= 2 * kills;
    }

    // Living attackers the lane wants near the node before it moves on. The
    // roster count includes defenders, who are never on the lane, so their
    // share is taken off first.
    uint32 Quorum(TeamFront const& front)
    {
        uint32 const attackerRoles = s_cfg.defenderRoles >= 10 ? 1 : 10 - s_cfg.defenderRoles;
        uint32 const attackers = (front.lastAliveTeam * attackerRoles + 9) / 10;
        uint32 const quorum = (attackers * s_cfg.advanceQuorumPct + 99) / 100;
        return std::max(quorum, s_cfg.minPresence);
    }

    void Move(TeamFront& front, uint32 newIndex, uint32 nowMs)
    {
        front.index = newIndex;
        ++front.revision;
        front.changedMs = nowMs;
        front.clearReports.clear();
        front.firstClearMs = 0;
        front.killsNear.clear();
        front.deathsNear.clear();
        // Grace period: the group needs time to walk up to the new node
        // before its absence there can mean anything.
        front.lastHeldMs = nowMs;
    }
}

namespace DCHinterlandFront
{
    uint32 LaneLength() { return LANE_LENGTH; }

    Point const& LaneNode(TeamId teamId, uint32 index)
    {
        if (index >= LANE_LENGTH)
            index = LANE_LENGTH - 1;

        return teamId == TEAM_ALLIANCE ? ALLIANCE_LANE[index] : HORDE_LANE[index];
    }

    void SetConfig(Config const& config)
    {
        std::lock_guard<std::mutex> guard(s_mutex);
        s_cfg = config;

        if (!s_cfg.advanceVotes)
            s_cfg.advanceVotes = 1;
        if (!s_cfg.voteWindowSeconds)
            s_cfg.voteWindowSeconds = 1;
        if (!s_cfg.minPresence)
            s_cfg.minPresence = 1;
        s_cfg.advanceQuorumPct = std::min<uint32>(s_cfg.advanceQuorumPct, 100);
    }

    View Get(uint32 instanceId, TeamId teamId, uint32 nowMs)
    {
        View view;
        if (teamId != TEAM_ALLIANCE && teamId != TEAM_HORDE)
            return view;

        std::lock_guard<std::mutex> guard(s_mutex);
        TeamFront& front = Locate(instanceId, teamId, nowMs);

        view.index = front.index;
        view.name = LaneNode(teamId, front.index).name;
        view.heldSeconds = (nowMs - front.changedMs) / IN_MILLISECONDS;
        view.clearVotes = CountClearVotes(front, nowMs);
        view.presentNear = front.lastAliveNear;
        view.quorum = Quorum(front);
        Prune(front.killsNear, nowMs);
        Prune(front.deathsNear, nowMs);
        view.recentKillsNear = uint32(front.killsNear.size());
        view.recentDeathsNear = uint32(front.deathsNear.size());
        view.revision = front.revision;
        return view;
    }

    void ReportAtNode(uint32 instanceId, TeamId teamId, uint64 botGuid, bool nodeClear, uint32 nowMs)
    {
        if (teamId != TEAM_ALLIANCE && teamId != TEAM_HORDE)
            return;

        std::lock_guard<std::mutex> guard(s_mutex);
        TeamFront& front = Locate(instanceId, teamId, nowMs);

        if (!nodeClear)
        {
            // A hostile back on the node resets the hold: the node has to be
            // clear for the whole window, not just clear at some point.
            front.clearReports.erase(botGuid);
            if (front.clearReports.empty())
                front.firstClearMs = 0;
            return;
        }

        front.clearReports[botGuid] = nowMs;
        if (!front.firstClearMs)
            front.firstClearMs = nowMs;

        if (front.index + 1 >= LANE_LENGTH)
            return;  // already at the enemy boss: hold there

        if (CountClearVotes(front, nowMs) < s_cfg.advanceVotes)
            return;

        uint32 const clearForMs = nowMs - front.firstClearMs;
        uint32 const holdMs = s_cfg.holdSeconds * IN_MILLISECONDS;
        if (clearForMs < holdMs)
            return;

        // Wait for the group, but not forever: if the quorum has not shown up
        // after three hold windows, whoever is here goes. Otherwise a team
        // that lost too many bots to something else would sit on a clear node
        // for the rest of the match.
        if (front.lastAliveNear < Quorum(front) && clearForMs < holdMs * 3)
            return;

        Move(front, front.index + 1, nowMs);
    }

    void ReportPresence(uint32 instanceId, TeamId teamId, uint32 aliveNear, uint32 aliveTeam, uint32 nowMs)
    {
        if (teamId != TEAM_ALLIANCE && teamId != TEAM_HORDE)
            return;

        std::lock_guard<std::mutex> guard(s_mutex);
        TeamFront& front = Locate(instanceId, teamId, nowMs);

        front.lastAliveNear = aliveNear;
        front.lastAliveTeam = aliveTeam;

        // Held: enough of us here, and not visibly feeding while short of the
        // numbers to turn it around.
        bool const feeding = IsLosing(front, nowMs) && aliveNear < Quorum(front);
        if (aliveNear >= s_cfg.minPresence && !feeding)
        {
            front.lastHeldMs = nowMs;
            return;
        }

        if (front.index == 0)
            return;  // nothing to fall back to

        if ((nowMs - front.lastHeldMs) < s_cfg.regroupSeconds * IN_MILLISECONDS)
            return;

        Move(front, front.index - 1, nowMs);
    }

    void NoteKill(uint32 instanceId, TeamId killerTeam, TeamId victimTeam, float x, float y, uint32 nowMs)
    {
        std::lock_guard<std::mutex> guard(s_mutex);
        float const radiusSq = s_cfg.presenceRadius * s_cfg.presenceRadius;

        if (victimTeam == TEAM_ALLIANCE || victimTeam == TEAM_HORDE)
        {
            TeamFront& front = Locate(instanceId, victimTeam, nowMs);
            Point const& node = LaneNode(victimTeam, front.index);
            float const dx = node.x - x;
            float const dy = node.y - y;
            if (dx * dx + dy * dy <= radiusSq)
                front.deathsNear.push_back(nowMs);
        }

        if (killerTeam == TEAM_ALLIANCE || killerTeam == TEAM_HORDE)
        {
            TeamFront& front = Locate(instanceId, killerTeam, nowMs);
            Point const& node = LaneNode(killerTeam, front.index);
            float const dx = node.x - x;
            float const dy = node.y - y;
            if (dx * dx + dy * dy <= radiusSq)
                front.killsNear.push_back(nowMs);
        }
    }

    void Forget(uint32 instanceId)
    {
        std::lock_guard<std::mutex> guard(s_mutex);
        s_fronts.erase(instanceId);
    }
}
