/*
 * Dark Chaos - Hinterland BG bot monitoring. See DCHinterlandMonitor.h.
 *
 * SAMPLING
 *
 * One pass per battleground every SampleSeconds, from OnBattlegroundUpdate.
 * Everything it needs is on the Battleground itself - the roster, each player's
 * position and state, and both resource pools through GetTeamScore, which HLBG
 * keeps in the base class's m_TeamScores. No bot AI state is touched from here:
 * that lives behind the bot's own map thread, so the one thing only the bot can
 * know - whether it is stuck rather than merely standing on its objective - is
 * pushed in by the roaming action through NoteStall instead of pulled out.
 *
 * Positions are read cross-thread, which for a diagnostic is fine: the worst a
 * torn float can do is misreport one bot's distance in one sample.
 *
 * THE NUMBER TO WATCH
 *
 * "away" - the share of a team's bots further than AwayDistance from their own
 * staging area. A team whose bots are fighting sits near 100%; a team wedged at
 * its base sits near 0%. That one column separates "we are losing the fight"
 * from "our bots never left", which is not otherwise distinguishable from the
 * scoreboard.
 */

#include "DCHinterlandMonitor.h"

#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include "DCHinterlandGeography.h"
#include "DCHinterlandTactics.h"

#include "Battleground.h"
#include "Chat.h"
#include "Common.h"
#include "Config.h"
#include "Log.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "Timer.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    struct MonitorConfig
    {
        bool enabled = true;
        uint32 sampleMs = 3 * IN_MILLISECONDS;
        uint32 reportMs = 60 * IN_MILLISECONDS;
        float movedDistance = 3.0f;
        float awayDistance = 60.0f;
    };

    MonitorConfig s_cfg;

    void LoadConfig()
    {
        MonitorConfig cfg;

        cfg.enabled = sConfigMgr->GetOption<bool>("AiPlayerbot.DCHinterland.Monitor.Enable", true);
        cfg.sampleMs =
            sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.Monitor.SampleSeconds", 3) * IN_MILLISECONDS;
        cfg.reportMs =
            sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.Monitor.ReportSeconds", 60) * IN_MILLISECONDS;
        cfg.movedDistance = sConfigMgr->GetOption<float>("AiPlayerbot.DCHinterland.Monitor.MovedDistance", 3.0f);
        cfg.awayDistance = sConfigMgr->GetOption<float>("AiPlayerbot.DCHinterland.Monitor.AwayDistance", 60.0f);

        if (!cfg.sampleMs)
            cfg.sampleMs = IN_MILLISECONDS;

        s_cfg = cfg;
    }

    /* ------------------------------------------------------------------ */
    /* Snapshot                                                           */
    /* ------------------------------------------------------------------ */

    // Plain data, rebuilt every sample. The command prints this rather than
    // walking live objects, so it can answer from any thread without holding a
    // Battleground* or a Player* for longer than the sample that built it.
    struct StalledBot
    {
        std::string name;
        TeamId teamId = TEAM_NEUTRAL;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        char const* place = "";
        uint32 strikes = 0;
        uint32 stalledSeconds = 0;
    };

    struct TeamSample
    {
        uint32 resources = 0;
        int32 resourceDelta = 0;  // since the previous report
        uint32 bots = 0;
        uint32 realPlayers = 0;
        uint32 alive = 0;
        uint32 inCombat = 0;
        uint32 moving = 0;
        uint32 away = 0;  // further than AwayDistance from own staging
        float avgDistFromBase = 0.0f;
        // Counters accumulated between reports.
        uint32 kills = 0;
        uint32 deaths = 0;
        uint32 stalls = 0;
        uint32 teleports = 0;
    };

    struct MatchSnapshot
    {
        uint32 instanceId = 0;
        uint32 elapsedSeconds = 0;
        uint8 status = 0;
        TeamSample team[PVP_TEAMS_COUNT];
        std::vector<StalledBot> stalled;
        uint32 sampledMsAgo = 0;
        uint32 sampledAtMs = 0;
    };

    struct PlayerTrack
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        bool valid = false;
        // Filled by NoteStall from the bot's own thread.
        uint32 strikes = 0;
        uint32 lastStallMs = 0;
    };

    struct MatchMonitor
    {
        std::unordered_map<uint64, PlayerTrack> tracks;
        uint32 lastSampleMs = 0;
        uint32 lastReportMs = 0;
        uint32 startMs = 0;
        uint32 lastReportScore[PVP_TEAMS_COUNT] = { 0, 0 };
        // Accumulated between reports and zeroed by each one.
        uint32 kills[PVP_TEAMS_COUNT] = { 0, 0 };
        uint32 deaths[PVP_TEAMS_COUNT] = { 0, 0 };
        uint32 stalls[PVP_TEAMS_COUNT] = { 0, 0 };
        uint32 teleports[PVP_TEAMS_COUNT] = { 0, 0 };
        // Match totals, for the closing report.
        uint32 totalStalls[PVP_TEAMS_COUNT] = { 0, 0 };
        uint32 totalTeleports[PVP_TEAMS_COUNT] = { 0, 0 };
        uint32 totalKills[PVP_TEAMS_COUNT] = { 0, 0 };
        MatchSnapshot snapshot;
    };

    std::mutex s_mutex;
    std::unordered_map<uint32, MatchMonitor> s_matches;

    bool IsHinterlandBG(Battleground const* bg)
    {
        return bg && bg->GetBgTypeID(true) == BattlegroundTypeId(DC_BATTLEGROUND_HLBG_TYPE_ID);
    }

    bool IsBot(Player* player) { return player && GET_PLAYERBOT_AI(player) != nullptr; }

    char const* TeamName(TeamId teamId) { return teamId == TEAM_ALLIANCE ? "Alliance" : "Horde"; }

    /* ------------------------------------------------------------------ */
    /* Sampling                                                           */
    /* ------------------------------------------------------------------ */

    void Sample(Battleground* bg, MatchMonitor& monitor, uint32 now)
    {
        MatchSnapshot snapshot;
        snapshot.instanceId = bg->GetInstanceID();
        snapshot.status = uint8(bg->GetStatus());
        snapshot.elapsedSeconds = monitor.startMs ? (now - monitor.startMs) / IN_MILLISECONDS : 0;
        snapshot.sampledAtMs = now;

        float distSum[PVP_TEAMS_COUNT] = { 0.0f, 0.0f };

        for (auto const& itr : bg->GetPlayers())
        {
            Player* player = itr.second;
            if (!player)
                continue;

            TeamId const teamId = player->GetBgTeamId();
            if (teamId != TEAM_ALLIANCE && teamId != TEAM_HORDE)
                continue;

            TeamSample& sample = snapshot.team[teamId];

            if (!IsBot(player))
            {
                ++sample.realPlayers;
                continue;
            }

            ++sample.bots;

            if (player->IsAlive())
                ++sample.alive;
            if (player->IsInCombat())
                ++sample.inCombat;

            uint64 const guid = player->GetGUID().GetRawValue();
            PlayerTrack& track = monitor.tracks[guid];

            float const x = player->GetPositionX();
            float const y = player->GetPositionY();
            float const z = player->GetPositionZ();

            if (track.valid && player->GetExactDist(track.x, track.y, track.z) > s_cfg.movedDistance)
                ++sample.moving;
            else if (!track.valid)
                ++sample.moving;  // first sample: no baseline to call it stationary

            DCHinterland::Point const staging = DCHinterland::GetSideView(teamId).ownStaging;
            float const fromBase = player->GetExactDist(staging.x, staging.y, staging.z);
            distSum[teamId] += fromBase;
            if (fromBase > s_cfg.awayDistance)
                ++sample.away;

            // A stall reported in the last two sample windows is still current.
            if (track.strikes && (now - track.lastStallMs) < (2 * s_cfg.sampleMs + IN_MILLISECONDS))
            {
                StalledBot stalled;
                stalled.name = player->GetName();
                stalled.teamId = teamId;
                stalled.x = x;
                stalled.y = y;
                stalled.z = z;
                stalled.place = DCHinterland::NearestLandmarkName(x, y);
                stalled.strikes = track.strikes;
                stalled.stalledSeconds = (now - track.lastStallMs) / IN_MILLISECONDS;
                snapshot.stalled.push_back(std::move(stalled));
            }

            track.x = x;
            track.y = y;
            track.z = z;
            track.valid = true;
        }

        for (uint8 teamId = 0; teamId < PVP_TEAMS_COUNT; ++teamId)
        {
            TeamSample& sample = snapshot.team[teamId];

            sample.resources = bg->GetTeamScore(TeamId(teamId));
            sample.resourceDelta = monitor.lastReportScore[teamId]
                                       ? int32(sample.resources) - int32(monitor.lastReportScore[teamId])
                                       : 0;
            sample.avgDistFromBase = sample.bots ? distSum[teamId] / float(sample.bots) : 0.0f;
            sample.kills = monitor.kills[teamId];
            sample.deaths = monitor.deaths[teamId];
            sample.stalls = monitor.stalls[teamId];
            sample.teleports = monitor.teleports[teamId];
        }

        monitor.snapshot = std::move(snapshot);
    }

    /* ------------------------------------------------------------------ */
    /* Reporting                                                          */
    /* ------------------------------------------------------------------ */

    void LogTeamLine(MatchSnapshot const& snapshot, TeamId teamId, char const* prefix)
    {
        TeamSample const& sample = snapshot.team[teamId];

        uint32 const awayPct = sample.bots ? (sample.away * 100) / sample.bots : 0;

        LOG_INFO("playerbots.hlbg",
                 "{} {:<8} res {:>5} ({:+5}) | bots {:>2} ({} alive, {} fighting, {} moving) | away {}% avg {:.0f}y "
                 "| kills {} deaths {} | stalls {} teleports {} | players {}",
                 prefix, TeamName(teamId), sample.resources, sample.resourceDelta, sample.bots, sample.alive,
                 sample.inCombat, sample.moving, awayPct, sample.avgDistFromBase, sample.kills, sample.deaths,
                 sample.stalls, sample.teleports, sample.realPlayers);
    }

    void Report(MatchMonitor& monitor, char const* prefix)
    {
        MatchSnapshot const& snapshot = monitor.snapshot;

        LOG_INFO("playerbots.hlbg", "{} HLBG instance {} at t+{}s:", prefix, snapshot.instanceId,
                 snapshot.elapsedSeconds);

        LogTeamLine(snapshot, TEAM_ALLIANCE, prefix);
        LogTeamLine(snapshot, TEAM_HORDE, prefix);

        for (StalledBot const& stalled : snapshot.stalled)
        {
            LOG_WARN("playerbots.hlbg", "{}   stuck: {} ({}) at {} ({:.0f}, {:.0f}, {:.0f}), {} strike(s)", prefix,
                     stalled.name, TeamName(stalled.teamId), stalled.place, stalled.x, stalled.y, stalled.z,
                     stalled.strikes);
        }
    }

    // Zeroes the between-report counters and rolls the resource baseline
    // forward, after folding this window into the match totals.
    void RollWindow(MatchMonitor& monitor)
    {
        for (uint8 teamId = 0; teamId < PVP_TEAMS_COUNT; ++teamId)
        {
            monitor.totalStalls[teamId] += monitor.stalls[teamId];
            monitor.totalTeleports[teamId] += monitor.teleports[teamId];
            monitor.totalKills[teamId] += monitor.kills[teamId];

            monitor.lastReportScore[teamId] = monitor.snapshot.team[teamId].resources;
            monitor.kills[teamId] = 0;
            monitor.deaths[teamId] = 0;
            monitor.stalls[teamId] = 0;
            monitor.teleports[teamId] = 0;
        }
    }

    /* ------------------------------------------------------------------ */
    /* Hooks                                                              */
    /* ------------------------------------------------------------------ */

    class DCHinterlandMonitorWorldScript : public WorldScript
    {
    public:
        DCHinterlandMonitorWorldScript()
            : WorldScript("DCHinterlandMonitorWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD })
        {
        }

        void OnAfterConfigLoad(bool /*reload*/) override { LoadConfig(); }
    };

    class DCHinterlandMonitorPlayerScript : public PlayerScript
    {
    public:
        DCHinterlandMonitorPlayerScript()
            : PlayerScript("DCHinterlandMonitorPlayerScript", { PLAYERHOOK_ON_PVP_KILL })
        {
        }

        void OnPlayerPVPKill(Player* killer, Player* killed) override
        {
            if (!s_cfg.enabled || !killer || !killed || killer == killed)
                return;

            Battleground* bg = killer->GetBattleground();
            if (!IsHinterlandBG(bg) || bg != killed->GetBattleground())
                return;

            TeamId const killerTeam = killer->GetBgTeamId();
            TeamId const killedTeam = killed->GetBgTeamId();
            if (killerTeam == killedTeam)
                return;

            std::lock_guard<std::mutex> guard(s_mutex);
            MatchMonitor& monitor = s_matches[bg->GetInstanceID()];

            if (killerTeam == TEAM_ALLIANCE || killerTeam == TEAM_HORDE)
                ++monitor.kills[killerTeam];
            if (killedTeam == TEAM_ALLIANCE || killedTeam == TEAM_HORDE)
                ++monitor.deaths[killedTeam];
        }
    };

    class DCHinterlandMonitorBGScript : public AllBattlegroundScript
    {
    public:
        DCHinterlandMonitorBGScript()
            : AllBattlegroundScript("DCHinterlandMonitorBGScript",
                                    { ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_START,
                                      ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_UPDATE,
                                      ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_END,
                                      ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_DESTROY })
        {
        }

        void OnBattlegroundStart(Battleground* bg) override
        {
            if (!s_cfg.enabled || !IsHinterlandBG(bg))
                return;

            uint32 const now = getMSTime();

            std::lock_guard<std::mutex> guard(s_mutex);
            MatchMonitor& monitor = s_matches[bg->GetInstanceID()];
            monitor.startMs = now;
            monitor.lastReportMs = now;

            LOG_INFO("playerbots.hlbg", "[hlbg] instance {} started: {} vs {} on the roster",
                     bg->GetInstanceID(), bg->GetPlayersCountByTeam(TEAM_ALLIANCE),
                     bg->GetPlayersCountByTeam(TEAM_HORDE));
        }

        void OnBattlegroundUpdate(Battleground* bg, uint32 /*diff*/) override
        {
            if (!s_cfg.enabled || !IsHinterlandBG(bg))
                return;

            BattlegroundStatus const status = bg->GetStatus();
            if (status != STATUS_WAIT_JOIN && status != STATUS_IN_PROGRESS)
                return;

            uint32 const now = getMSTime();

            std::lock_guard<std::mutex> guard(s_mutex);
            MatchMonitor& monitor = s_matches[bg->GetInstanceID()];

            if (monitor.lastSampleMs && (now - monitor.lastSampleMs) < s_cfg.sampleMs)
                return;

            monitor.lastSampleMs = now;
            Sample(bg, monitor, now);

            if (status != STATUS_IN_PROGRESS || !s_cfg.reportMs)
                return;

            if (!monitor.lastReportMs)
                monitor.lastReportMs = now;

            if ((now - monitor.lastReportMs) < s_cfg.reportMs)
                return;

            monitor.lastReportMs = now;
            Report(monitor, "[hlbg]");
            RollWindow(monitor);
        }

        void OnBattlegroundEnd(Battleground* bg, TeamId winnerTeamId) override
        {
            if (!s_cfg.enabled || !IsHinterlandBG(bg))
                return;

            uint32 const now = getMSTime();

            std::lock_guard<std::mutex> guard(s_mutex);
            auto itr = s_matches.find(bg->GetInstanceID());
            if (itr == s_matches.end())
                return;

            MatchMonitor& monitor = itr->second;
            Sample(bg, monitor, now);
            Report(monitor, "[hlbg:final]");
            RollWindow(monitor);

            LOG_INFO("playerbots.hlbg",
                     "[hlbg:final] instance {} won by {} after {}s | unstick repicks A {} / H {} | unstick teleports "
                     "A {} / H {} | player kills A {} / H {}",
                     bg->GetInstanceID(), TeamName(winnerTeamId),
                     monitor.startMs ? (now - monitor.startMs) / IN_MILLISECONDS : 0,
                     monitor.totalStalls[TEAM_ALLIANCE], monitor.totalStalls[TEAM_HORDE],
                     monitor.totalTeleports[TEAM_ALLIANCE], monitor.totalTeleports[TEAM_HORDE],
                     monitor.totalKills[TEAM_ALLIANCE], monitor.totalKills[TEAM_HORDE]);
        }

        void OnBattlegroundDestroy(Battleground* bg) override
        {
            if (!bg)
                return;

            std::lock_guard<std::mutex> guard(s_mutex);
            s_matches.erase(bg->GetInstanceID());
        }
    };
}

namespace DCHinterlandMonitor
{
    void NoteStall(Player* bot, uint32 strikes)
    {
        if (!s_cfg.enabled || !bot)
            return;

        Battleground* bg = bot->GetBattleground();
        if (!IsHinterlandBG(bg))
            return;

        TeamId const teamId = bot->GetBgTeamId();
        if (teamId != TEAM_ALLIANCE && teamId != TEAM_HORDE)
            return;

        std::lock_guard<std::mutex> guard(s_mutex);
        MatchMonitor& monitor = s_matches[bg->GetInstanceID()];

        ++monitor.stalls[teamId];

        PlayerTrack& track = monitor.tracks[bot->GetGUID().GetRawValue()];
        track.strikes = strikes;
        track.lastStallMs = getMSTime();
    }

    void NoteUnstickTeleport(Player* bot)
    {
        if (!s_cfg.enabled || !bot)
            return;

        Battleground* bg = bot->GetBattleground();
        if (!IsHinterlandBG(bg))
            return;

        TeamId const teamId = bot->GetBgTeamId();
        if (teamId != TEAM_ALLIANCE && teamId != TEAM_HORDE)
            return;

        LOG_WARN("playerbots.hlbg",
                 "[hlbg] {} ({}) failed to path off {} after repeated repicks; teleported to its staging area",
                 bot->GetName(), TeamName(teamId),
                 DCHinterland::NearestLandmarkName(bot->GetPositionX(), bot->GetPositionY()));

        std::lock_guard<std::mutex> guard(s_mutex);
        ++s_matches[bg->GetInstanceID()].teleports[teamId];
    }

    bool HandleStatusCommand(ChatHandler* handler, char const* /*args*/)
    {
        if (!handler)
            return false;

        if (!s_cfg.enabled)
        {
            handler->PSendSysMessage("HLBG bot monitoring is off (AiPlayerbot.DCHinterland.Monitor.Enable = 0).");
            return true;
        }

        std::vector<MatchSnapshot> snapshots;
        {
            std::lock_guard<std::mutex> guard(s_mutex);
            uint32 const now = getMSTime();
            for (auto const& itr : s_matches)
            {
                MatchSnapshot snapshot = itr.second.snapshot;
                if (!snapshot.instanceId)
                    continue;

                snapshot.sampledMsAgo = now - snapshot.sampledAtMs;
                snapshots.push_back(std::move(snapshot));
            }
        }

        if (snapshots.empty())
        {
            handler->PSendSysMessage("No Hinterland BG is running.");
            return true;
        }

        for (MatchSnapshot const& snapshot : snapshots)
        {
            handler->PSendSysMessage("HLBG instance {} - t+{}s, status {}, sampled {}ms ago", snapshot.instanceId,
                                     snapshot.elapsedSeconds, uint32(snapshot.status), snapshot.sampledMsAgo);

            for (uint8 teamId = 0; teamId < PVP_TEAMS_COUNT; ++teamId)
            {
                TeamSample const& sample = snapshot.team[teamId];
                uint32 const awayPct = sample.bots ? (sample.away * 100) / sample.bots : 0;

                handler->PSendSysMessage(
                    "  {}: {} res ({:+}), {} bots ({} alive, {} fighting, {} moving), {}% away, avg {:.0f}y from base, "
                    "{} kills / {} deaths, {} stalls, {} teleports, {} real players",
                    TeamName(TeamId(teamId)), sample.resources, sample.resourceDelta, sample.bots, sample.alive,
                    sample.inCombat, sample.moving, awayPct, sample.avgDistFromBase, sample.kills, sample.deaths,
                    sample.stalls, sample.teleports, sample.realPlayers);
            }

            if (snapshot.stalled.empty())
            {
                handler->PSendSysMessage("  no bots reported stuck");
                continue;
            }

            for (StalledBot const& stalled : snapshot.stalled)
            {
                handler->PSendSysMessage("  stuck: {} ({}) at {} ({:.0f}, {:.0f}, {:.0f}) - {} strike(s), {}s ago",
                                         stalled.name, TeamName(stalled.teamId), stalled.place, stalled.x, stalled.y,
                                         stalled.z, stalled.strikes, stalled.stalledSeconds);
            }
        }

        return true;
    }
}

void AddSC_dc_hinterland_monitor()
{
    LoadConfig();

    new DCHinterlandMonitorWorldScript();
    new DCHinterlandMonitorPlayerScript();
    new DCHinterlandMonitorBGScript();
}
