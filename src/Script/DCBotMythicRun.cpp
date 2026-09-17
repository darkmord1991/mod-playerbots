/*
 * DarkChaos addition to mod-playerbots. See DCBotMythicRun.h.
 */

#include "DCBotMythicRun.h"

#include "Chat.h"
#include "Common.h"
#include "Config.h"
#include "DBCStores.h"
#include "Group.h"
#include "GroupMgr.h"
#include "InstanceSaveMgr.h"
#include "InstanceScript.h"
#include "LFGMgr.h"
#include "Log.h"
#include "LootMgr.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"
#include "Position.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "WorldSession.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

// Mirror of the DC Mythic+ bot API. Declared rather than included: scripts.lib
// does not export its include directory to modules.lib. Keep in sync with
// src/server/scripts/DC/MythicPlus/dc_mythicplus_run_manager.h.
namespace DCMythicPlusBots
{
    bool StartBotRun(Player* activator, uint8 keystoneLevel, std::string& outError);
    bool AbortRun(Map* map, std::string const& reason);
    uint8 GetRunPhase(Map* map);
    void GetRemainingBossEntries(Map* map, std::vector<uint32>& out);
    // dc_mythicplus_spectator.cpp: the Spectate tab entry of a run without a keystone.
    void RegisterSpectatableDungeonRun(Map* map, std::string const& leaderName);
    void UnregisterSpectatableDungeonRun(uint32 instanceId);
}

namespace
{
    using DCBotMythicRun::Config;

    constexpr uint32 TICK_MS = 1000;
    constexpr uint32 COUNTDOWN_TIMEOUT_MS = 60 * IN_MILLISECONDS;
    // Tank displacement between samples that counts as moving on.
    constexpr float PROGRESS_MOVE_DISTANCE = 5.0f;
    // Runs without a keystone. The group must have been out of combat this
    // long before a dead bot is revived (bot healers get to resurrect first),
    // and a cleared dungeon is kept this long for looting before teardown.
    constexpr uint32 REVIVE_CALM_MS = 10 * IN_MILLISECONDS;
    constexpr uint32 CLEARED_GRACE_MS = 30 * IN_MILLISECONDS;
    // Without progress for this long (and nobody fighting), members that fell
    // behind are brought to the tank, again every interval, well before the
    // StuckSeconds watchdog gives up on the run.
    constexpr uint32 REGROUP_AFTER_MS = 30 * IN_MILLISECONDS;

    enum class Phase : uint8
    {
        EnteringLeader,  // tank teleporting into a fresh instance
        EnteringGroup,   // the other four following into the tank's instance
        Starting,        // everyone inside, settle delay before the tank moves off
        Running,         // keystone accepted (countdown, then the timer), or under way without one
    };

    struct Member
    {
        ObjectGuid guid;
        std::string name;
        // Where the bot stood when it was recruited; teardown sends it back.
        WorldLocation origin;
    };

    struct Run
    {
        uint32 id = 0;
        uint32 mapId = 0;
        uint32 instanceId = 0;
        // 0 for a Normal or Heroic run without a keystone.
        uint8 keystoneLevel = 0;
        Difficulty difficulty = DUNGEON_DIFFICULTY_NORMAL;
        ObjectGuid requester;
        ObjectGuid tank;
        std::vector<Member> members;  // tank first
        Phase phase = Phase::EnteringLeader;
        uint32 phaseMs = 0;
        uint32 timerMs = 0;
        bool timerSeen = false;
        uint32 noProgressMs = 0;
        size_t bossesRemaining = std::numeric_limits<size_t>::max();
        float tankX = 0.0f;
        float tankY = 0.0f;
        float tankZ = 0.0f;
        uint32 nextRegroupMs = REGROUP_AFTER_MS;
        // What the last regroup found, for `status` and the log.
        std::string stuckNote;
        // Runs without a keystone.
        uint32 calmMs = 0;
        uint32 clearedMs = 0;
        bool cleared = false;
    };

    std::string DescribeMode(Run const& run)
    {
        if (run.keystoneLevel)
            return Acore::StringFormat("+{}", uint32(run.keystoneLevel));

        return run.difficulty == DUNGEON_DIFFICULTY_HEROIC ? "heroic" : "normal";
    }

    char const* PhaseName(Run const& run)
    {
        switch (run.phase)
        {
            case Phase::EnteringLeader: return "tank entering";
            case Phase::EnteringGroup: return "group entering";
            case Phase::Starting: return "starting";
            case Phase::Running:
                if (run.keystoneLevel)
                    return "keystone active";
                return run.cleared ? "cleared, looting" : "running";
        }
        return "unknown";
    }

    // Recursive: the teardown and start paths call into bot AI, whose strategy
    // rebuild asks IsManaged on the same thread.
    std::recursive_mutex sMutex;
    std::unordered_map<uint32, Run> sRuns;
    std::unordered_map<ObjectGuid, uint32> sMemberRun;
    uint32 sNextRunId = 1;
    Config sConfig;

    void LoadConfig()
    {
        Config cfg;
        cfg.enabled = sConfigMgr->GetOption<bool>("AiPlayerbot.DCMythicBots.Enable", true);

        std::string const dungeons = sConfigMgr->GetOption<std::string>("AiPlayerbot.DCMythicBots.Dungeons", "574");
        std::istringstream dungeonList(dungeons);
        std::string token;
        while (std::getline(dungeonList, token, ','))
        {
            token.erase(std::remove(token.begin(), token.end(), ' '), token.end());
            if (Optional<uint32> mapId = Acore::StringTo<uint32>(token))
                cfg.dungeons.push_back(*mapId);
        }

        cfg.maxConcurrentRuns = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCMythicBots.MaxConcurrentRuns", 1);
        cfg.botLevel = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCMythicBots.BotLevel", 80);
        cfg.maxKeystoneLevel = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCMythicBots.MaxKeystoneLevel", 10);
        cfg.enterTimeoutSeconds = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCMythicBots.EnterTimeoutSeconds", 90);
        cfg.startDelaySeconds = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCMythicBots.StartDelaySeconds", 3);
        cfg.stuckSeconds = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCMythicBots.StuckSeconds", 180);
        cfg.maxRunMinutes = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCMythicBots.MaxRunMinutes", 45);
        cfg.pullRange = sConfigMgr->GetOption<float>("AiPlayerbot.DCMythicBots.PullRange", 30.0f);
        cfg.regroupDistance = sConfigMgr->GetOption<float>("AiPlayerbot.DCMythicBots.RegroupDistance", 40.0f);
        cfg.restHealthPct = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCMythicBots.RestHealthPct", 60);
        cfg.restManaPct = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCMythicBots.RestManaPct", 50);
        cfg.reviveDeadBots = sConfigMgr->GetOption<bool>("AiPlayerbot.DCMythicBots.ReviveDeadBots", true);

        std::lock_guard<std::recursive_mutex> lock(sMutex);
        sConfig = cfg;
    }

    void Notify(ObjectGuid requester, std::string const& message)
    {
        LOG_INFO("playerbots.mplus", "{}", message);

        if (Player* player = ObjectAccessor::FindConnectedPlayer(requester))
            if (WorldSession* session = player->GetSession())
                ChatHandler(session).PSendSysMessage("|cffff8000[Bot Run]|r {}", message);
    }

    PlayerbotAI* GetBotAI(Player* bot)
    {
        return bot ? sPlayerbotsMgr.GetPlayerbotAI(bot) : nullptr;
    }

    // A run without a keystone takes its bosses from the dungeon itself: the
    // encounters instance_encounters lists for the map and difficulty, in their
    // DungeonEncounter.dbc order (a signed field - negative values come first),
    // each done once its bit is in the instance's completed-encounter mask.
    // `creatures` gets the kill-credit creature of every open encounter, in
    // order; `open` also counts encounters credited by a spell cast, which have
    // no creature to walk to. False when the map has no encounter list.
    bool GetEncounterProgress(Map* map, std::vector<uint32>& creatures, size_t& open)
    {
        creatures.clear();
        open = 0;

        if (!map)
            return false;

        DungeonEncounterList const* encounters = sObjectMgr->GetDungeonEncounterList(map->GetId(), map->GetDifficulty());
        if (!encounters || encounters->empty())
            return false;

        uint32 completed = 0;
        if (InstanceMap* instance = map->ToInstanceMap())
            if (InstanceScript* script = instance->GetInstanceScript())
                completed = script->GetCompletedEncounterMask();

        std::vector<DungeonEncounter const*> ordered;
        ordered.reserve(encounters->size());
        for (DungeonEncounter const* encounter : *encounters)
            if (encounter && encounter->dbcEntry && encounter->dbcEntry->encounterIndex < 32)
                ordered.push_back(encounter);

        std::sort(ordered.begin(), ordered.end(), [](DungeonEncounter const* a, DungeonEncounter const* b)
        {
            if (a->dbcEntry->orderIndex != b->dbcEntry->orderIndex)
                return a->dbcEntry->orderIndex < b->dbcEntry->orderIndex;
            return a->dbcEntry->encounterIndex < b->dbcEntry->encounterIndex;
        });

        for (DungeonEncounter const* encounter : ordered)
        {
            if (completed & (1u << encounter->dbcEntry->encounterIndex))
                continue;

            ++open;
            if (encounter->creditType == ENCOUNTER_CREDIT_KILL_CREATURE)
                creatures.push_back(encounter->creditEntry);
        }

        return !ordered.empty();
    }

    bool IsRecruitable(Player* bot, uint32 minLevel, uint32 mapId, Difficulty difficulty)
    {
        if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->GetLevel() < minLevel)
            return false;

        if (!sRandomPlayerbotMgr.IsRandomBot(bot) || !GetBotAI(bot))
            return false;

        if (!bot->GetSession() || bot->GetSession()->PlayerLogout())
            return false;

        if (bot->IsInCombat() || bot->GetGroup() || bot->IsBeingTeleported() || bot->IsInFlight())
            return false;

        if (bot->InBattleground() || bot->InArena() || bot->InBattlegroundQueue())
            return false;

        if (sLFGMgr->GetState(bot->GetGUID()) != lfg::LFG_STATE_NONE)
            return false;

        Map* map = bot->FindMap();
        if (!map || map->Instanceable())
            return false;

        // A lockout on this dungeon would send the bot into its own saved copy
        // rather than the tank's (PlayerGetDestinationInstanceId prefers a
        // permanent bind): Heroic binds for the day, Mythic for the week.
        if (sInstanceSaveMgr->PlayerIsPermBoundToInstance(bot->GetGUID(), mapId, difficulty))
            return false;

        std::lock_guard<std::recursive_mutex> lock(sMutex);
        return sMemberRun.find(bot->GetGUID()) == sMemberRun.end();
    }

    // One tank, one healer, three damage dealers, all of the tank's faction.
    bool Recruit(Config const& cfg, uint32 mapId, Difficulty difficulty, std::vector<Player*>& out, std::string& error)
    {
        std::vector<Player*> tanks;
        std::vector<Player*> healers;
        std::vector<Player*> damage;

        for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
        {
            (void)guid;
            if (!IsRecruitable(bot, cfg.botLevel, mapId, difficulty))
                continue;

            if (PlayerbotAI::IsTank(bot))
                tanks.push_back(bot);
            else if (PlayerbotAI::IsHeal(bot))
                healers.push_back(bot);
            else if (PlayerbotAI::IsDps(bot))
                damage.push_back(bot);
        }

        for (Player* tank : tanks)
        {
            TeamId const team = tank->GetTeamId();

            auto healer = std::find_if(healers.begin(), healers.end(),
                [team](Player* bot) { return bot->GetTeamId() == team; });
            if (healer == healers.end())
                continue;

            std::vector<Player*> picked;
            for (Player* bot : damage)
            {
                if (bot->GetTeamId() == team)
                    picked.push_back(bot);
                if (picked.size() == 3)
                    break;
            }

            if (picked.size() < 3)
                continue;

            out = { tank, *healer, picked[0], picked[1], picked[2] };
            return true;
        }

        error = Acore::StringFormat("Not enough idle level {}+ random bots of one faction without a lockout on this "
            "dungeon: found {} tanks, {} healers, {} damage dealers (need 1 / 1 / 3).", cfg.botLevel, tanks.size(),
            healers.size(), damage.size());
        return false;
    }

    // Removes the run from the registry, disbands its group and sends every bot
    // back to where it was recruited. A keystone run must already be over.
    void TearDown(uint32 runId, std::string const& reason)
    {
        Run run;
        {
            std::lock_guard<std::recursive_mutex> lock(sMutex);
            auto it = sRuns.find(runId);
            if (it == sRuns.end())
                return;

            run = it->second;
            sRuns.erase(it);
            for (Member const& member : run.members)
                sMemberRun.erase(member.guid);
        }

        // Spectators of a run without a keystone are sent home with it; a
        // keystone run's spectator entry belongs to the run manager.
        if (!run.keystoneLevel && run.instanceId)
            DCMythicPlusBots::UnregisterSpectatableDungeonRun(run.instanceId);

        // Unregistered first: the strategy rebuilds below, and the one the group
        // change triggers, must not hand "dc mythic" back to these bots.
        for (Member const& member : run.members)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(member.guid);
            if (!bot)
                continue;

            if (Group* group = bot->GetGroup())
                if (group->GetLeaderGUID() == run.tank)
                    group->Disband();
        }

        for (Member const& member : run.members)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(member.guid);
            if (!bot)
                continue;

            if (PlayerbotAI* botAI = GetBotAI(bot))
            {
                botAI->SetMaster(nullptr);
                botAI->ResetStrategies();
            }

            if (!bot->IsAlive())
            {
                bot->ResurrectPlayer(1.0f);
                bot->SpawnCorpseBones();
            }

            if (!bot->IsBeingTeleported() && (bot->GetMapId() != member.origin.GetMapId()
                || (bot->FindMap() && bot->FindMap()->Instanceable())))
            {
                bot->TeleportTo(member.origin);
            }
        }

        Notify(run.requester, Acore::StringFormat("Bot run #{} ended: {}", run.id, reason));
    }

    // Ends a run for any reason: a keystone still running is failed first.
    void FinishRun(Run const& run, std::string const& reason)
    {
        if (run.phase == Phase::Running && run.keystoneLevel)
        {
            if (Map* map = sMapMgr->FindMap(run.mapId, run.instanceId))
                DCMythicPlusBots::AbortRun(map, "Bot run ended: " + reason);
        }

        TearDown(run.id, reason);
    }

    void SetPhase(Run& run, Phase phase)
    {
        run.phase = phase;
        run.phaseMs = 0;
    }

    // Where revived and stray members rejoin the group: the tank, else any
    // living member standing in the instance, else the dungeon's entrance.
    bool FindRegroupPoint(Run const& run, std::vector<Player*> const& bots, WorldLocation& out)
    {
        auto inRun = [&run](Player* bot)
        {
            return bot->IsAlive() && !bot->IsBeingTeleported() && bot->GetMapId() == run.mapId
                && bot->GetInstanceId() == run.instanceId;
        };

        Player* anchor = inRun(bots.front()) ? bots.front() : nullptr;
        if (!anchor)
        {
            auto living = std::find_if(bots.begin(), bots.end(), inRun);
            if (living != bots.end())
                anchor = *living;
        }

        if (anchor)
        {
            out = WorldLocation(run.mapId, anchor->GetPositionX(), anchor->GetPositionY(), anchor->GetPositionZ(),
                anchor->GetOrientation());
            return true;
        }

        AreaTriggerTeleport const* entrance = sObjectMgr->GetMapEntranceTrigger(run.mapId);
        if (!entrance)
            return false;

        out = WorldLocation(entrance->target_mapId, entrance->target_X, entrance->target_Y, entrance->target_Z,
            entrance->target_Orientation);
        return true;
    }

    // The tank's lead action waits until every member is alive and within
    // RegroupDistance, so one member held up anywhere stalls the whole run - a
    // follower whose chase path stops at an M2 collision wall the tank walked
    // through (Utgarde Keep's closed forge fires), for instance. Brings such
    // members to a living tank and notes where the tank stood and what it was
    // heading for, so a stall that survives this can be traced.
    void RegroupAtTank(Run& run, std::vector<Player*> const& bots, Config const& cfg, uint32 nextBoss)
    {
        Player* tank = bots.front();
        bool const tankInRun = tank->IsAlive() && !tank->IsBeingTeleported() && tank->GetMapId() == run.mapId
            && tank->GetInstanceId() == run.instanceId;

        uint32 moved = 0;
        if (tankInRun)
        {
            for (size_t i = 1; i < bots.size(); ++i)
            {
                Player* bot = bots[i];
                if (!bot->IsAlive() || bot->IsBeingTeleported() || bot->IsInCombat())
                    continue;

                if (bot->GetMapId() == run.mapId && bot->GetInstanceId() == run.instanceId
                    && bot->GetExactDist(tank) <= cfg.regroupDistance)
                    continue;

                bot->TeleportTo(run.mapId, tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ(),
                    tank->GetOrientation());
                ++moved;
            }
        }

        run.stuckNote = Acore::StringFormat("{}s without progress: tank {} at ({:.1f}, {:.1f}, {:.1f}) heading for "
            "boss {}, {} member(s) brought to it", run.noProgressMs / IN_MILLISECONDS,
            tankInRun ? "in the dungeon" : "not usable", tank->GetPositionX(), tank->GetPositionY(),
            tank->GetPositionZ(), nextBoss, moved);
        LOG_INFO("playerbots.mplus", "Bot run #{}: {}", run.id, run.stuckNote);
    }

    // Each returns an end reason, or an empty string to carry on.
    std::string AdvanceEnteringLeader(Run& run, std::vector<Player*> const& bots, Config const& cfg)
    {
        Player* tank = bots.front();
        if (!tank->IsBeingTeleported())
        {
            Map* map = tank->FindMap();
            if (map && map->GetId() == run.mapId && map->IsDungeon())
            {
                run.instanceId = map->GetInstanceId();

                // The leader is now bound to this instance, so the others follow
                // him into it rather than each opening a copy of their own.
                AreaTriggerTeleport const* entrance = sObjectMgr->GetMapEntranceTrigger(run.mapId);
                if (!entrance)
                    return "the dungeon has no entrance location";

                for (size_t i = 1; i < bots.size(); ++i)
                {
                    if (!bots[i]->TeleportTo(entrance->target_mapId, entrance->target_X, entrance->target_Y,
                            entrance->target_Z, entrance->target_Orientation))
                    {
                        return Acore::StringFormat("{} could not enter the dungeon (entry requirement, instance "
                            "limit or lockout?)", bots[i]->GetName());
                    }
                }

                SetPhase(run, Phase::EnteringGroup);
                return "";
            }
        }

        if (run.phaseMs >= cfg.enterTimeoutSeconds * IN_MILLISECONDS)
            return "the tank did not reach the dungeon in time";

        return "";
    }

    std::string AdvanceEnteringGroup(Run& run, std::vector<Player*> const& bots, Config const& cfg)
    {
        bool allInside = true;
        for (Player* bot : bots)
        {
            Map* map = bot->IsBeingTeleported() ? nullptr : bot->FindMap();
            if (!map || map->GetId() != run.mapId)
            {
                allInside = false;
                continue;
            }

            if (map->GetInstanceId() != run.instanceId)
                return Acore::StringFormat("{} landed in a different instance", bot->GetName());
        }

        if (allInside)
        {
            SetPhase(run, Phase::Starting);
            return "";
        }

        if (run.phaseMs >= cfg.enterTimeoutSeconds * IN_MILLISECONDS)
            return "not every bot reached the dungeon in time";

        return "";
    }

    std::string AdvanceStarting(Run& run, std::vector<Player*> const& bots, Config const& cfg)
    {
        if (run.phaseMs < cfg.startDelaySeconds * IN_MILLISECONDS)
            return "";

        // The navmesh only covers loaded grids, and PathGenerator answers a
        // destination on an unloaded tile with a straight line through the
        // walls. Load the dungeon around the group before anyone moves;
        // DCMythicAdvanceAction also loads each destination's grid.
        if (Map* map = bots.front()->FindMap())
            map->LoadGridsInRange(*bots.front(), SIZE_OF_GRIDS);

        if (!run.keystoneLevel)
        {
            SetPhase(run, Phase::Running);

            // A keystone run is listed by the run manager; this one lists
            // itself, under the same "BOT <leader>" label.
            if (Map* map = sMapMgr->FindMap(run.mapId, run.instanceId))
                DCMythicPlusBots::RegisterSpectatableDungeonRun(map, "BOT " + run.members.front().name);

            Notify(run.requester, Acore::StringFormat("Bot run #{}: {} run under way.", run.id, DescribeMode(run)));
            return "";
        }

        std::string error;
        if (!DCMythicPlusBots::StartBotRun(bots.front(), run.keystoneLevel, error))
            return "the keystone could not be started: " + error;

        SetPhase(run, Phase::Running);
        Notify(run.requester, Acore::StringFormat("Bot run #{}: keystone +{} activated.", run.id,
            uint32(run.keystoneLevel)));
        return "";
    }

    // Without a keystone nothing else looks after the group. A cleared dungeon
    // is left after a looting break; once the fighting has stopped for a
    // while, dead bots are revived and members outside the instance (released
    // to the graveyard, revived at a spirit healer) are brought back.
    std::string AdvanceUnkeyed(Run& run, std::vector<Player*> const& bots, Config const& cfg, uint32 elapsedMs,
        bool anyoneFighting)
    {
        if (run.bossesRemaining == 0)
        {
            if (!run.cleared)
            {
                run.cleared = true;
                Notify(run.requester, Acore::StringFormat("Bot run #{}: dungeon cleared in {}s.", run.id,
                    run.timerMs / IN_MILLISECONDS));
            }

            run.clearedMs += elapsedMs;
            if (run.clearedMs >= CLEARED_GRACE_MS)
                return "the dungeon is cleared";
        }

        run.calmMs = anyoneFighting ? 0 : run.calmMs + elapsedMs;
        if (!cfg.reviveDeadBots || run.calmMs < REVIVE_CALM_MS)
            return "";

        WorldLocation regroup;
        if (!FindRegroupPoint(run, bots, regroup))
            return "";

        for (Player* bot : bots)
        {
            if (bot->IsBeingTeleported())
                continue;

            bool const inRun = bot->GetMapId() == run.mapId && bot->GetInstanceId() == run.instanceId;
            if (bot->IsAlive() && inRun)
                continue;

            // Alive first: a ghost cannot enter an instance its corpse is not in.
            if (!bot->IsAlive())
            {
                bot->ResurrectPlayer(0.5f);
                bot->SpawnCorpseBones();
            }

            bot->TeleportTo(regroup);
        }

        return "";
    }

    std::string AdvanceRunning(Run& run, std::vector<Player*> const& bots, Config const& cfg, uint32 elapsedMs)
    {
        Player* tank = bots.front();
        Map* map = sMapMgr->FindMap(run.mapId, run.instanceId);

        // A keystone run moves from countdown (1) to timer (2) and back to 0 when
        // it completes or fails; a run without one is under way at once.
        uint8 keyPhase = 2;
        if (run.keystoneLevel)
        {
            keyPhase = map ? DCMythicPlusBots::GetRunPhase(map) : 0;
            if (keyPhase == 0)
            {
                return run.timerSeen ? "the keystone run is over (completed or failed, see the mythic.run log)"
                                     : "the keystone run ended before its timer started";
            }
        }
        else if (!map)
            return "the dungeon instance is gone";

        // Bots count their own deaths and leave a group once the count runs
        // high; a bot run revives them, so the count is meaningless here.
        // Followers also need the tank as their master, or "follow" has nobody
        // to follow.
        for (Player* bot : bots)
        {
            PlayerbotAI* botAI = GetBotAI(bot);
            if (!botAI)
                continue;

            botAI->GetAiObjectContext()->GetValue<uint32>("death count")->Set(0);
            if (bot != tank && botAI->GetMaster() != tank)
                botAI->SetMaster(tank);
        }

        if (keyPhase == 1)
        {
            if (!run.timerSeen && run.phaseMs >= COUNTDOWN_TIMEOUT_MS)
                return "the keystone countdown never finished";
            return "";
        }

        if (!run.timerSeen)
        {
            run.timerSeen = true;
            run.timerMs = 0;
            run.noProgressMs = 0;
            run.tankX = tank->GetPositionX();
            run.tankY = tank->GetPositionY();
            run.tankZ = tank->GetPositionZ();
        }

        run.timerMs += elapsedMs;

        // Progress: a boss died, anyone is fighting, or the tank moved on.
        bool progressed = false;

        size_t open = run.bossesRemaining;
        uint32 nextBoss = 0;
        if (run.keystoneLevel)
        {
            std::vector<uint32> remaining;
            DCMythicPlusBots::GetRemainingBossEntries(map, remaining);
            open = remaining.size();
            nextBoss = remaining.empty() ? 0 : remaining.front();
        }
        else
        {
            std::vector<uint32> creatures;
            size_t encountersOpen = 0;
            if (GetEncounterProgress(map, creatures, encountersOpen))
                open = encountersOpen;
            nextBoss = creatures.empty() ? 0 : creatures.front();
        }

        if (open < run.bossesRemaining)
            progressed = true;
        run.bossesRemaining = open;

        bool anyoneFighting = false;
        for (Player* bot : bots)
            if (bot->IsInCombat())
                anyoneFighting = true;

        if (anyoneFighting)
            progressed = true;

        if (tank->GetMapId() == run.mapId
            && tank->GetExactDist(run.tankX, run.tankY, run.tankZ) > PROGRESS_MOVE_DISTANCE)
        {
            progressed = true;
            run.tankX = tank->GetPositionX();
            run.tankY = tank->GetPositionY();
            run.tankZ = tank->GetPositionZ();
        }

        run.noProgressMs = progressed ? 0 : run.noProgressMs + elapsedMs;

        if (progressed)
            run.nextRegroupMs = REGROUP_AFTER_MS;
        else if (!anyoneFighting && run.noProgressMs >= run.nextRegroupMs)
        {
            run.nextRegroupMs = run.noProgressMs + REGROUP_AFTER_MS;
            RegroupAtTank(run, bots, cfg, nextBoss);
        }

        if (!run.keystoneLevel)
        {
            std::string const reason = AdvanceUnkeyed(run, bots, cfg, elapsedMs, anyoneFighting);
            if (!reason.empty())
                return reason;
        }

        if (cfg.stuckSeconds && run.noProgressMs >= cfg.stuckSeconds * IN_MILLISECONDS)
            return Acore::StringFormat("no progress for {} seconds", cfg.stuckSeconds);

        if (cfg.maxRunMinutes && run.timerMs >= cfg.maxRunMinutes * MINUTE * IN_MILLISECONDS)
            return Acore::StringFormat("the run passed {} minutes", cfg.maxRunMinutes);

        return "";
    }

    void AdvanceRun(uint32 runId, uint32 elapsedMs)
    {
        Config const cfg = DCBotMythicRun::GetConfig();

        Run run;
        {
            std::lock_guard<std::recursive_mutex> lock(sMutex);
            auto it = sRuns.find(runId);
            if (it == sRuns.end())
                return;
            run = it->second;
        }

        run.phaseMs += elapsedMs;

        std::vector<Player*> bots;
        bots.reserve(run.members.size());
        for (Member const& member : run.members)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(member.guid);
            if (!bot || !bot->IsInWorld())
            {
                // Mid-teleport a bot is briefly out of the world; only a missing
                // session means it is really gone.
                if (bot && bot->IsBeingTeleported())
                {
                    bots.push_back(bot);
                    continue;
                }

                FinishRun(run, Acore::StringFormat("{} logged out", member.name));
                return;
            }

            bots.push_back(bot);
        }

        std::string endReason;
        switch (run.phase)
        {
            case Phase::EnteringLeader:
                endReason = AdvanceEnteringLeader(run, bots, cfg);
                break;
            case Phase::EnteringGroup:
                endReason = AdvanceEnteringGroup(run, bots, cfg);
                break;
            case Phase::Starting:
                endReason = AdvanceStarting(run, bots, cfg);
                break;
            case Phase::Running:
                endReason = AdvanceRunning(run, bots, cfg, elapsedMs);
                break;
        }

        if (!endReason.empty())
        {
            FinishRun(run, endReason);
            return;
        }

        std::lock_guard<std::recursive_mutex> lock(sMutex);
        auto it = sRuns.find(runId);
        if (it != sRuns.end())
            it->second = run;
    }

    // Recruits the five bots, groups them and walks the tank in; the world
    // tick does the rest. keystoneLevel 0 runs the dungeon at `difficulty`
    // without a keystone.
    void StartRun(ChatHandler* handler, Config const& cfg, uint32 mapId, uint8 keystoneLevel, Difficulty difficulty)
    {
        AreaTriggerTeleport const* entrance = sObjectMgr->GetMapEntranceTrigger(mapId);
        if (!entrance)
        {
            handler->PSendSysMessage("Map {} has no entrance location.", mapId);
            return;
        }

        {
            std::lock_guard<std::recursive_mutex> lock(sMutex);
            if (sRuns.size() >= cfg.maxConcurrentRuns)
            {
                handler->PSendSysMessage("{} bot run(s) already active (AiPlayerbot.DCMythicBots.MaxConcurrentRuns).",
                    sRuns.size());
                return;
            }
        }

        std::vector<Player*> bots;
        std::string error;
        if (!Recruit(cfg, mapId, difficulty, bots, error))
        {
            handler->SendSysMessage(error);
            return;
        }

        Player* tank = bots.front();

        Run run;
        run.mapId = mapId;
        run.keystoneLevel = keystoneLevel;
        run.difficulty = difficulty;
        run.tank = tank->GetGUID();
        if (WorldSession* session = handler->GetSession())
            if (Player* requester = session->GetPlayer())
                run.requester = requester->GetGUID();

        for (Player* bot : bots)
        {
            run.members.push_back({ bot->GetGUID(), bot->GetName(),
                WorldLocation(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                    bot->GetOrientation()) });
        }

        // Registered before any strategy is rebuilt, so AiFactory already sees
        // these bots as managed when the group change resets them.
        {
            std::lock_guard<std::recursive_mutex> lock(sMutex);
            run.id = sNextRunId++;
            sRuns[run.id] = run;
            for (Member const& member : run.members)
                sMemberRun[member.guid] = run.id;
        }

        Group* group = new Group();
        if (!group->Create(tank))
        {
            delete group;
            TearDown(run.id, "the group could not be created");
            return;
        }

        sGroupMgr->AddGroup(group);
        for (size_t i = 1; i < bots.size(); ++i)
            group->AddMember(bots[i]);

        group->SetLootMethod(NEED_BEFORE_GREED);
        group->SetLootThreshold(ITEM_QUALITY_UNCOMMON);
        group->ResetInstances(INSTANCE_RESET_CHANGE_DIFFICULTY, false, tank);
        group->SetDungeonDifficulty(difficulty);

        for (Player* bot : bots)
        {
            if (PlayerbotAI* botAI = GetBotAI(bot))
            {
                botAI->ResetStrategies();
                botAI->SetMaster(bot == tank ? nullptr : tank);
            }
        }

        handler->PSendSysMessage("Bot run #{} forming for map {} ({}): {} (tank), {}, {}, {}, {}.", run.id, mapId,
            DescribeMode(run), run.members[0].name, run.members[1].name, run.members[2].name, run.members[3].name,
            run.members[4].name);

        if (!tank->TeleportTo(entrance->target_mapId, entrance->target_X, entrance->target_Y, entrance->target_Z,
                entrance->target_Orientation))
        {
            TearDown(run.id, Acore::StringFormat("{} could not enter the dungeon (entry requirement, instance limit "
                "or lockout?)", run.members[0].name));
        }
    }

    class DCBotMythicRunWorldScript : public WorldScript
    {
    public:
        DCBotMythicRunWorldScript()
            : WorldScript("DCBotMythicRunWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE })
        {
        }

        void OnAfterConfigLoad(bool /*reload*/) override { LoadConfig(); }

        void OnUpdate(uint32 diff) override
        {
            _timer += diff;
            if (_timer < TICK_MS)
                return;

            uint32 const elapsed = _timer;
            _timer = 0;

            std::vector<uint32> runIds;
            {
                std::lock_guard<std::recursive_mutex> lock(sMutex);
                for (auto const& [id, run] : sRuns)
                {
                    (void)run;
                    runIds.push_back(id);
                }
            }

            for (uint32 id : runIds)
                AdvanceRun(id, elapsed);
        }

    private:
        uint32 _timer = 0;
    };
}

namespace DCBotMythicRun
{
    Config GetConfig()
    {
        std::lock_guard<std::recursive_mutex> lock(sMutex);
        return sConfig;
    }

    bool IsManaged(ObjectGuid guid)
    {
        std::lock_guard<std::recursive_mutex> lock(sMutex);
        return sMemberRun.find(guid) != sMemberRun.end();
    }

    bool IsLeadingRunningRun(ObjectGuid guid)
    {
        std::lock_guard<std::recursive_mutex> lock(sMutex);

        auto member = sMemberRun.find(guid);
        if (member == sMemberRun.end())
            return false;

        auto run = sRuns.find(member->second);
        return run != sRuns.end() && run->second.tank == guid && run->second.phase == Phase::Running
            && run->second.timerSeen && !run->second.cleared;
    }

    void GetRemainingBossEntries(ObjectGuid member, Map* map, std::vector<uint32>& out)
    {
        out.clear();

        uint8 keystoneLevel = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(sMutex);
            auto memberRun = sMemberRun.find(member);
            if (memberRun == sMemberRun.end())
                return;

            auto run = sRuns.find(memberRun->second);
            if (run == sRuns.end())
                return;

            keystoneLevel = run->second.keystoneLevel;
        }

        if (keystoneLevel)
        {
            DCMythicPlusBots::GetRemainingBossEntries(map, out);
            return;
        }

        size_t open = 0;
        GetEncounterProgress(map, out, open);
    }

    bool HandleStartCommand(ChatHandler* handler, char const* args)
    {
        Config const cfg = GetConfig();
        if (!cfg.enabled)
        {
            handler->SendSysMessage("Bot runs are disabled (AiPlayerbot.DCMythicBots.Enable).");
            return true;
        }

        uint32 mapId = cfg.dungeons.empty() ? 574 : cfg.dungeons.front();
        uint32 keystoneLevel = 2;

        std::istringstream input(args ? args : "");
        std::string token;
        if (input >> token)
        {
            Optional<uint32> value = Acore::StringTo<uint32>(token);
            if (!value)
            {
                handler->SendSysMessage("Usage: .playerbots mplus start [mapId] [keystoneLevel]");
                return true;
            }
            mapId = *value;
        }
        if (input >> token)
        {
            Optional<uint32> value = Acore::StringTo<uint32>(token);
            if (!value)
            {
                handler->SendSysMessage("Usage: .playerbots mplus start [mapId] [keystoneLevel]");
                return true;
            }
            keystoneLevel = *value;
        }

        if (std::find(cfg.dungeons.begin(), cfg.dungeons.end(), mapId) == cfg.dungeons.end())
        {
            handler->PSendSysMessage("Map {} is not in AiPlayerbot.DCMythicBots.Dungeons.", mapId);
            return true;
        }

        if (keystoneLevel < 2 || keystoneLevel > cfg.maxKeystoneLevel)
        {
            handler->PSendSysMessage("Keystone level must be between 2 and {}.", cfg.maxKeystoneLevel);
            return true;
        }

        StartRun(handler, cfg, mapId, static_cast<uint8>(keystoneLevel), DUNGEON_DIFFICULTY_EPIC);
        return true;
    }

    bool HandleDungeonStartCommand(ChatHandler* handler, char const* args)
    {
        static char const* const usage = "Usage: .playerbots dungeon start [mapId] [normal|heroic]";

        Config const cfg = GetConfig();
        if (!cfg.enabled)
        {
            handler->SendSysMessage("Bot runs are disabled (AiPlayerbot.DCMythicBots.Enable).");
            return true;
        }

        uint32 mapId = cfg.dungeons.empty() ? 574 : cfg.dungeons.front();
        Difficulty difficulty = DUNGEON_DIFFICULTY_NORMAL;

        // Either order: `start 574 heroic`, `start heroic 574` or `start heroic`.
        std::istringstream input(args ? args : "");
        std::string token;
        while (input >> token)
        {
            std::transform(token.begin(), token.end(), token.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            if (token == "normal" || token == "n")
                difficulty = DUNGEON_DIFFICULTY_NORMAL;
            else if (token == "heroic" || token == "h")
                difficulty = DUNGEON_DIFFICULTY_HEROIC;
            else if (Optional<uint32> value = Acore::StringTo<uint32>(token))
                mapId = *value;
            else
            {
                handler->SendSysMessage(usage);
                return true;
            }
        }

        if (std::find(cfg.dungeons.begin(), cfg.dungeons.end(), mapId) == cfg.dungeons.end())
        {
            handler->PSendSysMessage("Map {} is not in AiPlayerbot.DCMythicBots.Dungeons.", mapId);
            return true;
        }

        MapEntry const* mapEntry = sMapStore.LookupEntry(mapId);
        if (!mapEntry || !mapEntry->IsNonRaidDungeon())
        {
            handler->PSendSysMessage("Map {} is not a five-player dungeon.", mapId);
            return true;
        }

        // Entering downscales a missing Heroic to Normal without a word.
        if (difficulty == DUNGEON_DIFFICULTY_HEROIC && !GetMapDifficultyData(mapId, DUNGEON_DIFFICULTY_HEROIC))
        {
            handler->PSendSysMessage("Map {} has no heroic mode.", mapId);
            return true;
        }

        // The tank walks from boss to boss; with no creature to walk to it would
        // stand at the entrance until the watchdog gives up.
        DungeonEncounterList const* encounters = sObjectMgr->GetDungeonEncounterList(mapId, difficulty);
        bool const hasBoss = encounters && std::any_of(encounters->begin(), encounters->end(),
            [](DungeonEncounter const* encounter)
            {
                return encounter && encounter->dbcEntry && encounter->creditType == ENCOUNTER_CREDIT_KILL_CREATURE;
            });

        if (!hasBoss)
        {
            handler->PSendSysMessage("Map {} has no creature boss encounters for {} in instance_encounters.", mapId,
                difficulty == DUNGEON_DIFFICULTY_HEROIC ? "heroic" : "normal");
            return true;
        }

        StartRun(handler, cfg, mapId, 0, difficulty);
        return true;
    }

    bool HandleStopCommand(ChatHandler* handler, char const* /*args*/)
    {
        std::vector<Run> runs;
        {
            std::lock_guard<std::recursive_mutex> lock(sMutex);
            for (auto const& [id, run] : sRuns)
            {
                (void)id;
                runs.push_back(run);
            }
        }

        if (runs.empty())
        {
            handler->SendSysMessage("No bot run is active.");
            return true;
        }

        for (Run const& run : runs)
            FinishRun(run, "stopped by a GM");

        handler->PSendSysMessage("Stopped {} bot run(s).", runs.size());
        return true;
    }

    bool HandleStatusCommand(ChatHandler* handler, char const* /*args*/)
    {
        std::vector<Run> runs;
        {
            std::lock_guard<std::recursive_mutex> lock(sMutex);
            for (auto const& [id, run] : sRuns)
            {
                (void)id;
                runs.push_back(run);
            }
        }

        if (runs.empty())
        {
            handler->SendSysMessage("No bot run is active.");
            return true;
        }

        for (Run const& run : runs)
        {
            handler->PSendSysMessage("Bot run #{}: map {} instance {} {} - {}, {}s in phase.", run.id, run.mapId,
                run.instanceId, DescribeMode(run), PhaseName(run), run.phaseMs / IN_MILLISECONDS);

            if (run.timerSeen)
            {
                handler->PSendSysMessage("|- Time {}s, bosses left {}, {}s without progress.",
                    run.timerMs / IN_MILLISECONDS, run.bossesRemaining, run.noProgressMs / IN_MILLISECONDS);
            }

            if (!run.stuckNote.empty())
                handler->PSendSysMessage("|- Last regroup: {}", run.stuckNote);

            for (Member const& member : run.members)
            {
                Player* bot = ObjectAccessor::FindConnectedPlayer(member.guid);
                if (!bot)
                {
                    handler->PSendSysMessage("|- {}: offline", member.name);
                    continue;
                }

                handler->PSendSysMessage("|- {}{}: map {} {}, {:.0f}% health{}", member.name,
                    member.guid == run.tank ? " (tank)" : "", bot->GetMapId(), bot->IsAlive() ? "alive" : "dead",
                    bot->GetHealthPct(), bot->IsInCombat() ? ", in combat" : "");
            }

            // The tank's combat strategies, so the dungeon strategy (for example
            // "wotlk-uk") can be checked in game: ApplyInstanceStrategies adds it
            // on the worldport ack, and the mythic map's difficulty resolves to the
            // heroic branch of every dungeon strategy.
            if (Player* tank = ObjectAccessor::FindConnectedPlayer(run.tank))
            {
                if (PlayerbotAI* botAI = GetBotAI(tank))
                {
                    std::ostringstream strategies;
                    for (std::string const& name : botAI->GetStrategies(BOT_STATE_COMBAT))
                        strategies << (strategies.tellp() > 0 ? ", " : "") << name;

                    handler->PSendSysMessage("|- Tank combat strategies: {}", strategies.str());
                }
            }
        }

        return true;
    }
}

void AddSC_dc_bot_mythic_run()
{
    LoadConfig();
    new DCBotMythicRunWorldScript();
}
