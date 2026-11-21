/*
 * DarkChaos addition to mod-playerbots. See DCBotMythicRun.h.
 */

#include "DCBotMythicRun.h"

#include "DCBotRunEngine.h"

#include "Chat.h"
#include "Common.h"
#include "Config.h"
#include "Containers.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
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
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"
#include "Position.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

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

namespace DCBotRunEngine
{
    namespace
    {
        Hooks sHooks;
        bool sRegistered = false;
    }

    void Register(Hooks const& hooks)
    {
        sHooks = hooks;
        sRegistered = true;
    }

    Hooks const* Get()
    {
        if (!sRegistered || (sHooks.available && !sHooks.available()))
            return nullptr;

        return &sHooks;
    }
}

namespace
{
    using DCBotMythicRun::Config;

    constexpr uint32 TICK_MS = 1000;
    constexpr uint32 COUNTDOWN_TIMEOUT_MS = 60 * IN_MILLISECONDS;
    // Tank displacement between samples that counts as moving on.
    constexpr float PROGRESS_MOVE_DISTANCE = 5.0f;
    // The group must have been out of combat this long before a dead bot is revived (bot healers
    // get to resurrect first), and a cleared dungeon is kept this long for looting before teardown.
    constexpr uint32 REVIVE_CALM_MS = 10 * IN_MILLISECONDS;
    constexpr uint32 CLEARED_GRACE_MS = 30 * IN_MILLISECONDS;
    // Without progress for this long (and nobody fighting), members that fell behind are brought
    // to the tank, again every interval. mod-dungeon-clear recovers stragglers itself after 60 s.
    constexpr uint32 REGROUP_AFTER_MS = 30 * IN_MILLISECONDS;
    constexpr uint32 CLEAR_REGROUP_AFTER_MS = 120 * IN_MILLISECONDS;
    constexpr uint32 CLEAR_START_TIMEOUT_MS = 30 * IN_MILLISECONDS;
    // A clear still on the target the watchdog skipped this much later ends the run.
    constexpr uint32 CLEAR_TARGET_SKIP_GRACE_MS = 60 * IN_MILLISECONDS;
    // The target watchdog counts a tank reaching a 40-yard square (20-yard height band) it has not stood in
    // since the target began as progress, like a new target.
    constexpr float CLEAR_TARGET_CELL_YARDS = 40.0f;
    constexpr float CLEAR_TARGET_CELL_HEIGHT = 20.0f;
    constexpr uint32 PARTY_SIZE = 5;
    constexpr uint32 LEVEL_SLOTS = 256;
    constexpr uint32 MIN_AUTO_INTERVAL_SECONDS = 10;

    enum class Phase : uint8
    {
        EnteringLeader,  // tank teleporting into a fresh instance
        EnteringGroup,   // the others following into the tank's instance
        Starting,        // everyone inside, settle delay before the run starts
        Running,         // keystone accepted (countdown, then the timer), or under way without one
    };

    enum Role : uint8
    {
        ROLE_TANK,
        ROLE_HEALER,
        ROLE_DAMAGE,
        ROLE_COUNT
    };

    // What a run enters and which bots fit it.
    struct RunSpec
    {
        uint32 mapId = 0;
        // Catalogue token, or the map id for a map outside the catalogue.
        std::string token;
        std::string name;
        std::string wing;
        WorldLocation entrance;
        Difficulty difficulty = DUNGEON_DIFFICULTY_NORMAL;
        uint8 keystoneLevel = 0;
        bool raid = false;
        uint32 minLevel = 1;
        uint32 maxLevel = LEVEL_SLOTS - 1;
        std::array<uint32, ROLE_COUNT> roles = { 1, 1, 3 };
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
        RunSpec spec;
        uint32 instanceId = 0;
        ObjectGuid requester;
        ObjectGuid tank;
        std::vector<Member> members;  // tank first
        bool automatic = false;
        // mod-dungeon-clear leads; otherwise the tank leads through "dc mythic".
        bool clear = false;
        Phase phase = Phase::EnteringLeader;
        uint32 phaseMs = 0;
        uint32 lifeMs = 0;
        uint32 timerMs = 0;
        bool timerSeen = false;
        bool gridsLoaded = false;
        uint32 noProgressMs = 0;
        size_t bossesRemaining = std::numeric_limits<size_t>::max();
        float tankX = 0.0f;
        float tankY = 0.0f;
        float tankZ = 0.0f;
        uint32 nextRegroupMs = REGROUP_AFTER_MS;
        // What the last regroup found, for `status` and the log.
        std::string stuckNote;
        uint32 calmMs = 0;
        uint32 clearedMs = 0;
        bool cleared = false;
        // mod-dungeon-clear runs.
        bool clearStarted = false;
        bool clearFinished = false;
        uint32 clearStartMs = 0;
        uint32 restarts = 0;
        uint32 skips = 0;
        uint32 pausedMs = 0;
        uint32 stalledMs = 0;
        std::string clearNote;
        // Every boss or objective the clear has headed for, and how long since it last had a new one.
        std::vector<std::string> clearTargets;
        uint32 clearTargetMs = 0;
        bool clearTargetSkipped = false;
        // Ground the tank has covered since the current target began (TargetCell keys).
        std::unordered_set<uint64> clearTargetCells;
    };

    struct Totals
    {
        uint32 started = 0;
        uint32 cleared = 0;
        uint32 ended = 0;
    };

    // An idle random bot the scheduler or a start command may take.
    struct Candidate
    {
        Player* bot = nullptr;
        Role role = ROLE_DAMAGE;
        uint8 team = TEAM_ALLIANCE;
        uint32 level = 0;
    };

    std::string DescribeMode(RunSpec const& spec)
    {
        if (spec.keystoneLevel)
            return Acore::StringFormat("+{}", uint32(spec.keystoneLevel));

        if (spec.raid)
            return "raid";

        switch (spec.difficulty)
        {
            case DUNGEON_DIFFICULTY_HEROIC: return "heroic";
            case DUNGEON_DIFFICULTY_EPIC: return "mythic";
            default: return "normal";
        }
    }

    char const* PhaseName(Run const& run)
    {
        switch (run.phase)
        {
            case Phase::EnteringLeader: return "tank entering";
            case Phase::EnteringGroup: return "group entering";
            case Phase::Starting: return "starting";
            case Phase::Running:
                if (run.spec.keystoneLevel)
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
    Totals sTotals;

    // World thread only.
    std::vector<RunSpec> sTemplates;
    bool sTemplatesBuilt = false;
    std::optional<bool> sAutoOverride;
    std::string sLastAutoNote;
    uint32 sAutoIntervalMs = 120 * IN_MILLISECONDS;

    std::string Lowered(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    std::vector<std::string> ParseTokens(std::string const& list)
    {
        std::vector<std::string> out;
        std::istringstream input(list);
        std::string token;
        while (std::getline(input, token, ','))
        {
            token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char c) { return std::isspace(c); }),
                token.end());
            if (!token.empty())
                out.push_back(Lowered(token));
        }
        return out;
    }

    std::vector<uint32> ParseMapIds(std::string const& list)
    {
        std::vector<uint32> out;
        for (std::string const& token : ParseTokens(list))
            if (Optional<uint32> mapId = Acore::StringTo<uint32>(token))
                out.push_back(*mapId);
        return out;
    }

    // "349:120, 230:90" -> {349: 120, 230: 90}; malformed entries are dropped.
    std::unordered_map<uint32, uint32> ParseMapMinutes(std::string const& list)
    {
        std::unordered_map<uint32, uint32> out;
        for (std::string const& token : ParseTokens(list))
        {
            size_t const colon = token.find(':');
            if (colon == std::string::npos)
                continue;

            Optional<uint32> const mapId = Acore::StringTo<uint32>(token.substr(0, colon));
            Optional<uint32> const minutes = Acore::StringTo<uint32>(token.substr(colon + 1));
            if (mapId && minutes)
                out[*mapId] = *minutes;
        }
        return out;
    }

    void LoadConfig()
    {
        Config cfg;
        cfg.enabled = sConfigMgr->GetOption<bool>("Playerbots.DCMythicBots.Enable", true);
        cfg.dungeons = ParseMapIds(sConfigMgr->GetOption<std::string>("Playerbots.DCMythicBots.Dungeons", "574"));
        cfg.maxConcurrentRuns = sConfigMgr->GetOption<uint32>("Playerbots.DCMythicBots.MaxConcurrentRuns", 2);
        cfg.botLevel = sConfigMgr->GetOption<uint32>("Playerbots.DCMythicBots.BotLevel", 80);
        cfg.maxKeystoneLevel = sConfigMgr->GetOption<uint32>("Playerbots.DCMythicBots.MaxKeystoneLevel", 10);
        cfg.enterTimeoutSeconds = sConfigMgr->GetOption<uint32>("Playerbots.DCMythicBots.EnterTimeoutSeconds", 90);
        cfg.startDelaySeconds = sConfigMgr->GetOption<uint32>("Playerbots.DCMythicBots.StartDelaySeconds", 3);
        cfg.stuckSeconds = sConfigMgr->GetOption<uint32>("Playerbots.DCMythicBots.StuckSeconds", 180);
        cfg.maxRunMinutes = sConfigMgr->GetOption<uint32>("Playerbots.DCMythicBots.MaxRunMinutes", 45);
        cfg.pullRange = sConfigMgr->GetOption<float>("Playerbots.DCMythicBots.PullRange", 30.0f);
        cfg.regroupDistance = sConfigMgr->GetOption<float>("Playerbots.DCMythicBots.RegroupDistance", 40.0f);
        cfg.restHealthPct = sConfigMgr->GetOption<uint32>("Playerbots.DCMythicBots.RestHealthPct", 60);
        cfg.restManaPct = sConfigMgr->GetOption<uint32>("Playerbots.DCMythicBots.RestManaPct", 50);
        cfg.reviveDeadBots = sConfigMgr->GetOption<bool>("Playerbots.DCMythicBots.ReviveDeadBots", true);

        cfg.useDungeonClear = sConfigMgr->GetOption<bool>("Playerbots.DCMythicBots.UseDungeonClear", true);
        cfg.clearNoProgressSeconds =
            sConfigMgr->GetOption<uint32>("Playerbots.DCMythicBots.ClearNoProgressSeconds", 600);
        cfg.clearMaxRunMinutes = sConfigMgr->GetOption<uint32>("Playerbots.DCMythicBots.ClearMaxRunMinutes", 120);
        cfg.clearMaxRunMinutesByMap =
            ParseMapMinutes(sConfigMgr->GetOption<std::string>("Playerbots.DCMythicBots.ClearMaxRunMinutesByMap", ""));
        cfg.clearPauseGraceSeconds =
            sConfigMgr->GetOption<uint32>("Playerbots.DCMythicBots.ClearPauseGraceSeconds", 60);
        cfg.clearStallGraceSeconds =
            sConfigMgr->GetOption<uint32>("Playerbots.DCMythicBots.ClearStallGraceSeconds", 120);
        cfg.clearMaxSkips = sConfigMgr->GetOption<uint32>("Playerbots.DCMythicBots.ClearMaxSkips", 3);
        cfg.clearTargetMinutes = sConfigMgr->GetOption<uint32>("Playerbots.DCMythicBots.ClearTargetMinutes", 20);
        cfg.clearMaxRestarts = sConfigMgr->GetOption<uint32>("Playerbots.DCMythicBots.ClearMaxRestarts", 3);

        cfg.autoRuns = sConfigMgr->GetOption<bool>("Playerbots.DCMythicBots.AutoRuns", true);
        cfg.autoIntervalSeconds = std::max(MIN_AUTO_INTERVAL_SECONDS,
            sConfigMgr->GetOption<uint32>("Playerbots.DCMythicBots.AutoIntervalSeconds", 120));
        cfg.autoDungeons = ParseTokens(sConfigMgr->GetOption<std::string>("Playerbots.DCMythicBots.AutoDungeons", ""));
        cfg.autoExcludeDungeons =
            ParseTokens(sConfigMgr->GetOption<std::string>("Playerbots.DCMythicBots.AutoExcludeDungeons", ""));
        cfg.autoExtraDungeons =
            ParseMapIds(sConfigMgr->GetOption<std::string>("Playerbots.DCMythicBots.AutoExtraDungeons", "820,823"));
        cfg.levelBelow = sConfigMgr->GetOption<uint32>("Playerbots.DCMythicBots.AutoLevelBelow", 2);
        cfg.levelAbove = sConfigMgr->GetOption<uint32>("Playerbots.DCMythicBots.AutoLevelAbove", 10);
        cfg.normalWeight = sConfigMgr->GetOption<uint32>("Playerbots.DCMythicBots.AutoNormalWeight", 100);
        cfg.heroicWeight = sConfigMgr->GetOption<uint32>("Playerbots.DCMythicBots.AutoHeroicWeight", 60);
        cfg.mythicWeight = sConfigMgr->GetOption<uint32>("Playerbots.DCMythicBots.AutoMythicWeight", 40);
        cfg.keystoneWeight = sConfigMgr->GetOption<uint32>("Playerbots.DCMythicBots.AutoKeystoneWeight", 0);
        cfg.autoKeystoneLevel = std::max<uint32>(2,
            sConfigMgr->GetOption<uint32>("Playerbots.DCMythicBots.AutoKeystoneLevel", 2));
        cfg.autoRaids = sConfigMgr->GetOption<bool>("Playerbots.DCMythicBots.AutoRaids", false);
        cfg.raidWeight = sConfigMgr->GetOption<uint32>("Playerbots.DCMythicBots.AutoRaidWeight", 20);
        cfg.raidSize = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Playerbots.DCMythicBots.AutoRaidSize", 10),
            PARTY_SIZE + 1, 40);

        sTemplatesBuilt = false;
        sAutoIntervalMs = cfg.autoIntervalSeconds * IN_MILLISECONDS;

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

    DCBotRunEngine::Hooks const* ClearEngine(Config const& cfg)
    {
        return cfg.useDungeonClear ? DCBotRunEngine::Get() : nullptr;
    }

    // Mythic five-player dungeons borrow the heroic list, then the normal one, exactly like
    // Map::UpdateEncounterState.
    DungeonEncounterList const* GetEncounters(uint32 mapId, Difficulty difficulty, bool raid)
    {
        // As Map::UpdateEncounterState: Mythic borrows Heroic then Normal, a classic Heroic borrows Normal.
        DungeonEncounterList const* encounters = sObjectMgr->GetDungeonEncounterList(mapId, difficulty);
        if (!encounters && !raid && difficulty != DUNGEON_DIFFICULTY_NORMAL)
        {
            if (difficulty == DUNGEON_DIFFICULTY_EPIC)
                encounters = sObjectMgr->GetDungeonEncounterList(mapId, DUNGEON_DIFFICULTY_HEROIC);
            if (!encounters)
                encounters = sObjectMgr->GetDungeonEncounterList(mapId, DUNGEON_DIFFICULTY_NORMAL);
        }
        return encounters;
    }

    // The bosses are the dungeon's own encounters (instance_encounters, DungeonEncounter.dbc), in
    // their DBC order (a signed field - negative values come first), each done once its bit is in
    // the instance's completed-encounter mask. `creatures` gets the kill-credit creature of every
    // open encounter, in order; `open` also counts encounters credited by a spell cast. False when
    // the map has no encounter list.
    bool GetEncounterProgress(Map* map, std::vector<uint32>& creatures, size_t& open)
    {
        creatures.clear();
        open = 0;

        if (!map)
            return false;

        DungeonEncounterList const* encounters = GetEncounters(map->GetId(), map->GetDifficulty(), map->IsRaid());
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

    // ---- Templates: every dungeon at every difficulty a bot run may take ------------------------

    struct MythicProfile
    {
        bool heroic = false;
        bool mythic = false;
        uint32 heroicLevel = 0;
        uint32 mythicLevel = 0;
    };

    // Loaded on the world thread when the templates are (re)built, after a config load.
    std::unordered_map<uint32, MythicProfile> LoadMythicProfiles()
    {
        std::unordered_map<uint32, MythicProfile> out;
        QueryResult result = WorldDatabase.Query("SELECT map_id, heroic_enabled, mythic_enabled, heroic_level_normal, "
            "mythic_level_normal FROM dc_dungeon_mythic_profile");
        if (!result)
            return out;

        do
        {
            Field* fields = result->Fetch();
            MythicProfile& profile = out[fields[0].Get<uint32>()];
            profile.heroic = fields[1].Get<uint32>() != 0;
            profile.mythic = fields[2].Get<uint32>() != 0;
            profile.heroicLevel = fields[3].Get<uint32>();
            profile.mythicLevel = fields[4].Get<uint32>();
        } while (result->NextRow());

        return out;
    }

    // dungeon_access_template's level floor per map and difficulty.
    std::map<std::pair<uint32, uint32>, uint32> LoadAccessLevels()
    {
        std::map<std::pair<uint32, uint32>, uint32> out;
        QueryResult result = WorldDatabase.Query("SELECT map_id, difficulty, min_level FROM dungeon_access_template");
        if (!result)
            return out;

        do
        {
            Field* fields = result->Fetch();
            out[{ fields[0].Get<uint32>(), fields[1].Get<uint32>() }] = fields[2].Get<uint32>();
        } while (result->NextRow());

        return out;
    }

    std::unordered_set<uint32> LoadKeystoneMaps()
    {
        std::unordered_set<uint32> out;
        QueryResult result = WorldDatabase.Query("SELECT dungeon_id FROM dc_mplus_dungeons");
        if (!result)
            return out;

        do
        {
            out.insert(result->Fetch()[0].Get<uint32>());
        } while (result->NextRow());

        return out;
    }

    std::array<uint32, ROLE_COUNT> RaidRoles(uint32 size)
    {
        uint32 const tanks = size <= 10 ? 2 : size <= 25 ? 3 : 4;
        uint32 const healers = size <= 10 ? 3 : size <= 25 ? 6 : 10;
        return { tanks, healers, size - tanks - healers };
    }

    uint32 MaxPlayers(uint32 mapId, Difficulty difficulty)
    {
        if (MapDifficulty const* data = GetMapDifficultyData(mapId, difficulty))
            if (data->maxPlayers)
                return data->maxPlayers;

        MapEntry const* entry = sMapStore.LookupEntry(mapId);
        return entry ? entry->maxPlayers : 0;
    }

    struct TemplateSources
    {
        std::unordered_map<uint32, MythicProfile> profiles;
        std::map<std::pair<uint32, uint32>, uint32> accessLevels;
        std::unordered_set<uint32> keystoneMaps;
    };

    uint32 AccessLevel(TemplateSources const& sources, uint32 mapId, Difficulty difficulty)
    {
        auto it = sources.accessLevels.find({ mapId, uint32(difficulty) });
        return it != sources.accessLevels.end() ? it->second : 0;
    }

    void AddTemplate(std::vector<RunSpec>& out, Config const& cfg, TemplateSources const& sources,
        DCBotRunEngine::Dungeon const& dungeon, Difficulty difficulty, uint8 keystoneLevel, uint32 contentLevel)
    {
        if (!contentLevel)
            return;

        if (difficulty != DUNGEON_DIFFICULTY_NORMAL && !GetMapDifficultyData(dungeon.mapId, difficulty))
            return;

        RunSpec spec;
        spec.mapId = dungeon.mapId;
        spec.token = dungeon.token;
        spec.name = dungeon.name;
        spec.wing = dungeon.wing;
        spec.entrance = WorldLocation(dungeon.mapId, dungeon.x, dungeon.y, dungeon.z, dungeon.o);
        spec.difficulty = difficulty;
        spec.keystoneLevel = keystoneLevel;
        spec.raid = dungeon.raid;

        uint32 const floor = AccessLevel(sources, dungeon.mapId, difficulty);
        uint32 const below = contentLevel > cfg.levelBelow ? contentLevel - cfg.levelBelow : 1;
        spec.minLevel = std::max(below, floor);
        spec.maxLevel = std::min(std::max(contentLevel, floor) + cfg.levelAbove, LEVEL_SLOTS - 1);
        if (spec.minLevel > spec.maxLevel)
            return;

        if (dungeon.raid)
        {
            uint32 const cap = MaxPlayers(dungeon.mapId, difficulty);
            uint32 const size = cap ? std::min(cfg.raidSize, cap) : cfg.raidSize;
            if (size <= PARTY_SIZE)
                return;

            spec.roles = RaidRoles(size);
        }

        out.push_back(std::move(spec));
    }

    std::vector<DCBotRunEngine::Dungeon> CollectDungeons(Config const& cfg, DCBotRunEngine::Hooks const* engine,
        TemplateSources const& sources)
    {
        std::vector<DCBotRunEngine::Dungeon> dungeons = engine->dungeons();

        // Maps outside the catalogue (DarkChaos dungeons) enter at their area trigger and reach
        // their level floors from dungeon_access_template.
        for (uint32 mapId : cfg.autoExtraDungeons)
        {
            bool const known = std::any_of(dungeons.begin(), dungeons.end(),
                [mapId](DCBotRunEngine::Dungeon const& dungeon) { return dungeon.mapId == mapId; });
            if (known)
                continue;

            MapEntry const* entry = sMapStore.LookupEntry(mapId);
            AreaTriggerTeleport const* trigger = sObjectMgr->GetMapEntranceTrigger(mapId);
            if (!entry || !entry->IsDungeon() || !trigger)
            {
                LOG_WARN("playerbots.mplus", "Bot runs: map {} in AutoExtraDungeons is not a dungeon with an entrance "
                    "area trigger, skipped.", mapId);
                continue;
            }

            DCBotRunEngine::Dungeon dungeon;
            dungeon.token = std::to_string(mapId);
            dungeon.name = entry->name[0] && *entry->name[0] ? entry->name[0] : dungeon.token;
            dungeon.mapId = mapId;
            dungeon.x = trigger->target_X;
            dungeon.y = trigger->target_Y;
            dungeon.z = trigger->target_Z;
            dungeon.o = trigger->target_Orientation;
            dungeon.normalLevel = AccessLevel(sources, mapId, DUNGEON_DIFFICULTY_NORMAL);
            dungeon.raid = entry->IsRaid();
            dungeons.push_back(std::move(dungeon));
        }

        return dungeons;
    }

    void BuildTemplates(Config const& cfg)
    {
        sTemplates.clear();

        // mod-dungeon-clear switches on at the first world tick; until then try again next time.
        DCBotRunEngine::Hooks const* engine = ClearEngine(cfg);
        if (!engine)
            return;

        sTemplatesBuilt = true;

        TemplateSources sources;
        sources.profiles = LoadMythicProfiles();
        sources.accessLevels = LoadAccessLevels();
        sources.keystoneMaps = LoadKeystoneMaps();

        for (DCBotRunEngine::Dungeon const& dungeon : CollectDungeons(cfg, engine, sources))
        {
            if (dungeon.raid)
            {
                // The raid strategies fight the bosses; they attach by map only with this on.
                if (sPlayerbotAIConfig.ApplyInstanceStrategies)
                    AddTemplate(sTemplates, cfg, sources, dungeon, RAID_DIFFICULTY_10MAN_NORMAL, 0,
                        dungeon.normalLevel);
                continue;
            }

            AddTemplate(sTemplates, cfg, sources, dungeon, DUNGEON_DIFFICULTY_NORMAL, 0, dungeon.normalLevel);

            auto const profileIt = sources.profiles.find(dungeon.mapId);
            MythicProfile const* profile = profileIt != sources.profiles.end() ? &profileIt->second : nullptr;

            // DarkChaos raises classic heroics to 80-82 and its own dungeons further; the profile
            // says where, the catalogue covers the native TBC and WotLK heroics.
            if (!profile || profile->heroic)
            {
                uint32 heroicLevel = profile && profile->heroicLevel ? profile->heroicLevel : dungeon.heroicLevel;
                if (!heroicLevel && profile)
                    heroicLevel = AccessLevel(sources, dungeon.mapId, DUNGEON_DIFFICULTY_HEROIC);

                AddTemplate(sTemplates, cfg, sources, dungeon, DUNGEON_DIFFICULTY_HEROIC, 0, heroicLevel);
            }

            if (profile && profile->mythic)
            {
                uint32 mythicLevel = profile->mythicLevel ? profile->mythicLevel : dungeon.heroicLevel;
                if (!mythicLevel)
                    mythicLevel = AccessLevel(sources, dungeon.mapId, DUNGEON_DIFFICULTY_EPIC);

                AddTemplate(sTemplates, cfg, sources, dungeon, DUNGEON_DIFFICULTY_EPIC, 0, mythicLevel);
                if (sources.keystoneMaps.count(dungeon.mapId))
                    AddTemplate(sTemplates, cfg, sources, dungeon, DUNGEON_DIFFICULTY_EPIC,
                        static_cast<uint8>(std::min<uint32>(cfg.autoKeystoneLevel, cfg.maxKeystoneLevel)),
                        mythicLevel);
            }
        }

        LOG_INFO("playerbots.mplus", "Bot runs: {} dungeon templates from mod-dungeon-clear's catalogue.",
            sTemplates.size());
    }

    std::vector<RunSpec> const& Templates(Config const& cfg)
    {
        if (!sTemplatesBuilt)
            BuildTemplates(cfg);

        return sTemplates;
    }

    uint32 WeightOf(Config const& cfg, RunSpec const& spec)
    {
        if (spec.keystoneLevel)
            return cfg.keystoneWeight;
        if (spec.raid)
            return cfg.autoRaids ? cfg.raidWeight : 0;

        switch (spec.difficulty)
        {
            case DUNGEON_DIFFICULTY_HEROIC: return cfg.heroicWeight;
            case DUNGEON_DIFFICULTY_EPIC: return cfg.mythicWeight;
            default: return cfg.normalWeight;
        }
    }

    bool Listed(std::vector<std::string> const& list, RunSpec const& spec)
    {
        std::string const token = Lowered(spec.token);
        std::string const mapId = std::to_string(spec.mapId);
        return std::any_of(list.begin(), list.end(),
            [&](std::string const& entry) { return entry == token || entry == mapId; });
    }

    bool WantedAutomatically(Config const& cfg, RunSpec const& spec)
    {
        if (!cfg.autoDungeons.empty() && !Listed(cfg.autoDungeons, spec))
            return false;

        return !Listed(cfg.autoExcludeDungeons, spec);
    }

    // How long a run may take before the run-time watchdog ends it, in minutes (0 = no limit).
    uint32 RunLimitMinutes(Config const& cfg, uint32 mapId, bool clear)
    {
        if (!clear)
            return cfg.maxRunMinutes;

        auto const byMap = cfg.clearMaxRunMinutesByMap.find(mapId);
        return byMap != cfg.clearMaxRunMinutesByMap.end() ? byMap->second : cfg.clearMaxRunMinutes;
    }

    // Would the instance's daily (heroic) or weekly (raid) reset come before an automatic run could end? The reset
    // unbinds the instance and unloads it with the bots inside: minipc bot run #18 (Black Morass heroic, 11-21)
    // formed at 03:55:30 and ended at 04:00:02 with "the dungeon instance is gone", two seconds after
    // Instance.ResetTimeHour 4. Normal dungeons have no reset time and always pass.
    bool ResetsDuringRun(Config const& cfg, RunSpec const& spec)
    {
        time_t const reset = sInstanceSaveMgr->GetResetTimeFor(spec.mapId, spec.difficulty);
        if (!reset)
            return false;

        uint32 limitMinutes = RunLimitMinutes(cfg, spec.mapId, true);
        if (!limitMinutes)
            limitMinutes = 60;

        return reset - GameTime::GetGameTime().count() < time_t(limitMinutes) * MINUTE;
    }

    // A start command's dungeon: a catalogue token or a map id, at a difficulty.
    RunSpec const* FindTemplate(Config const& cfg, std::string const& which, Difficulty difficulty, bool keystone)
    {
        std::string const key = Lowered(which);
        for (RunSpec const& spec : Templates(cfg))
        {
            if ((spec.keystoneLevel != 0) != keystone || spec.difficulty != difficulty)
                continue;

            if (Lowered(spec.token) == key || std::to_string(spec.mapId) == key)
                return &spec;
        }
        return nullptr;
    }

    // A run outside mod-dungeon-clear: the tank leads through "dc mythic".
    bool LegacySpec(Config const& cfg, uint32 mapId, Difficulty difficulty, uint8 keystoneLevel, RunSpec& out,
        std::string& error)
    {
        AreaTriggerTeleport const* entrance = sObjectMgr->GetMapEntranceTrigger(mapId);
        MapEntry const* entry = sMapStore.LookupEntry(mapId);
        if (!entrance || !entry)
        {
            error = Acore::StringFormat("Map {} has no entrance location.", mapId);
            return false;
        }

        out = RunSpec();
        out.mapId = mapId;
        out.token = std::to_string(mapId);
        out.name = entry->name[0] && *entry->name[0] ? entry->name[0] : out.token;
        out.entrance = WorldLocation(entrance->target_mapId, entrance->target_X, entrance->target_Y,
            entrance->target_Z, entrance->target_Orientation);
        out.difficulty = difficulty;
        out.keystoneLevel = keystoneLevel;
        out.minLevel = cfg.botLevel;
        out.maxLevel = LEVEL_SLOTS - 1;
        return true;
    }

    // ---- Recruiting ---------------------------------------------------------------------------

    std::optional<Role> RoleOf(Player* bot)
    {
        if (PlayerbotAI::IsTank(bot))
            return ROLE_TANK;
        if (PlayerbotAI::IsHeal(bot))
            return ROLE_HEALER;
        if (PlayerbotAI::IsDps(bot))
            return ROLE_DAMAGE;
        return std::nullopt;
    }

    // Free to be taken, wherever the run goes.
    bool IsIdle(Player* bot)
    {
        if (!bot || !bot->IsInWorld() || !bot->IsAlive())
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

        std::lock_guard<std::recursive_mutex> lock(sMutex);
        return sMemberRun.find(bot->GetGUID()) == sMemberRun.end();
    }

    // Everything that would keep the bot out of this run's instance.
    bool CanEnter(Player* bot, RunSpec const& spec)
    {
        // A lockout would send the bot into its own saved copy rather than the tank's
        // (PlayerGetDestinationInstanceId prefers a permanent bind).
        if (sInstanceSaveMgr->PlayerIsPermBoundToInstance(bot->GetGUID(), spec.mapId, spec.difficulty))
            return false;

        // The instances-per-hour limit refuses a new instance.
        if (!bot->CheckInstanceCount(0))
            return false;

        return bot->Satisfy(sObjectMgr->GetAccessRequirement(spec.mapId, spec.difficulty), spec.mapId, false);
    }

    std::vector<Candidate> CollectIdle()
    {
        std::vector<Candidate> out;
        for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
        {
            (void)guid;
            if (!IsIdle(bot))
                continue;

            std::optional<Role> role = RoleOf(bot);
            TeamId const team = bot->GetTeamId();
            if (!role || (team != TEAM_ALLIANCE && team != TEAM_HORDE))
                continue;

            out.push_back({ bot, *role, static_cast<uint8>(team), std::min<uint32>(bot->GetLevel(), LEVEL_SLOTS - 1) });
        }
        return out;
    }

    using IdleCounts = std::array<std::array<std::array<uint32, LEVEL_SLOTS>, ROLE_COUNT>, 2>;

    // Whether one faction has the spec's roles in its level range, lockouts and entry requirements
    // aside (Recruit checks those on the few bots it takes).
    bool Fits(IdleCounts const& counts, RunSpec const& spec)
    {
        for (uint8 team = 0; team < 2; ++team)
        {
            bool enough = true;
            for (uint8 role = 0; role < ROLE_COUNT && enough; ++role)
            {
                uint32 found = 0;
                for (uint32 level = spec.minLevel; level <= spec.maxLevel && found < spec.roles[role]; ++level)
                    found += counts[team][role][level];

                enough = found >= spec.roles[role];
            }

            if (enough)
                return true;
        }
        return false;
    }

    // Tanks first (the first is the main tank), then healers, then damage dealers, all of one faction.
    bool Recruit(RunSpec const& spec, std::vector<Candidate> const& idle, std::vector<Player*>& out, std::string& error)
    {
        std::array<std::array<std::vector<Player*>, ROLE_COUNT>, 2> found;
        for (Candidate const& candidate : idle)
        {
            if (candidate.level < spec.minLevel || candidate.level > spec.maxLevel)
                continue;

            if (!CanEnter(candidate.bot, spec))
                continue;

            found[candidate.team][candidate.role].push_back(candidate.bot);
        }

        std::array<uint8, 2> teams = { TEAM_ALLIANCE, TEAM_HORDE };
        if (urand(0, 1))
            std::swap(teams[0], teams[1]);

        for (uint8 team : teams)
        {
            bool enough = true;
            for (uint8 role = 0; role < ROLE_COUNT; ++role)
                enough = enough && found[team][role].size() >= spec.roles[role];

            if (!enough)
                continue;

            out.clear();
            for (uint8 role = 0; role < ROLE_COUNT; ++role)
            {
                std::vector<Player*>& bots = found[team][role];
                Acore::Containers::RandomShuffle(bots);
                out.insert(out.end(), bots.begin(), bots.begin() + spec.roles[role]);
            }
            return true;
        }

        error = Acore::StringFormat("Not enough idle level {}-{} random bots of one faction that can enter {} ({}): "
            "Alliance {}/{}/{}, Horde {}/{}/{} tanks/healers/damage dealers, {}/{}/{} needed.", spec.minLevel,
            spec.maxLevel, spec.name, DescribeMode(spec), found[TEAM_ALLIANCE][ROLE_TANK].size(),
            found[TEAM_ALLIANCE][ROLE_HEALER].size(), found[TEAM_ALLIANCE][ROLE_DAMAGE].size(),
            found[TEAM_HORDE][ROLE_TANK].size(), found[TEAM_HORDE][ROLE_HEALER].size(),
            found[TEAM_HORDE][ROLE_DAMAGE].size(), spec.roles[ROLE_TANK], spec.roles[ROLE_HEALER],
            spec.roles[ROLE_DAMAGE]);
        return false;
    }

    // ---- Run lifecycle ------------------------------------------------------------------------

    // Removes the run from the registry, stops its clear, disbands its group and sends every bot
    // back to where it was recruited. A keystone run must already be over.
    void TearDown(uint32 runId, std::string const& reason, bool cleared)
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

            if (cleared)
                ++sTotals.cleared;
            ++sTotals.ended;
        }

        // Spectators of a run without a keystone are sent home with it; a keystone run's
        // spectator entry belongs to the run manager.
        if (!run.spec.keystoneLevel && !run.spec.raid && run.instanceId)
            DCMythicPlusBots::UnregisterSpectatableDungeonRun(run.instanceId);

        if (run.clear)
        {
            if (DCBotRunEngine::Hooks const* engine = DCBotRunEngine::Get())
            {
                if (Player* tank = ObjectAccessor::FindConnectedPlayer(run.tank))
                    engine->stop(tank, "Bot run ended: " + reason);
                engine->disarmRelay(run.id);
            }
        }

        // Unregistered first: the strategy rebuilds below, and the one the group change
        // triggers, must not hand "dc mythic" back to these bots. The run's group is the one a
        // member leads - not necessarily the tank, which hands the lead on if it leaves.
        auto const isMember = [&run](ObjectGuid guid)
        {
            return std::any_of(run.members.begin(), run.members.end(),
                [guid](Member const& member) { return member.guid == guid; });
        };

        for (Member const& member : run.members)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(member.guid);
            if (!bot)
                continue;

            if (Group* group = bot->GetGroup())
                if (isMember(group->GetLeaderGUID()))
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

        std::string const bossesLeft =
            run.bossesRemaining == std::numeric_limits<size_t>::max() ? "?" : std::to_string(run.bossesRemaining);
        Notify(run.requester, Acore::StringFormat("Bot run #{} ({} {}) ended after {}s: {} (bosses left {}, skipped "
            "{}, restarts {}).", run.id, run.spec.name, DescribeMode(run.spec), run.lifeMs / IN_MILLISECONDS,
            reason, bossesLeft, run.skips, run.restarts));
    }

    // Ends a run for any reason: a keystone still running is failed first.
    void FinishRun(Run const& run, std::string const& reason)
    {
        if (run.phase == Phase::Running && run.spec.keystoneLevel)
        {
            if (Map* map = sMapMgr->FindMap(run.spec.mapId, run.instanceId))
                DCMythicPlusBots::AbortRun(map, "Bot run ended: " + reason);
        }

        // The caller's copy is this tick's; TearDown reads the registry's for its summary.
        {
            std::lock_guard<std::recursive_mutex> lock(sMutex);
            auto it = sRuns.find(run.id);
            if (it != sRuns.end())
                it->second = run;
        }

        TearDown(run.id, reason, run.cleared);
    }

    void SetPhase(Run& run, Phase phase)
    {
        run.phase = phase;
        run.phaseMs = 0;
    }

    bool InRunInstance(Run const& run, Player* bot)
    {
        return !bot->IsBeingTeleported() && bot->GetMapId() == run.spec.mapId
            && bot->GetInstanceId() == run.instanceId;
    }

    // Where revived and stray members rejoin the group: the tank, else any living member
    // standing in the instance, else the dungeon's entrance.
    bool FindRegroupPoint(Run const& run, std::vector<Player*> const& bots, WorldLocation& out)
    {
        auto inRun = [&run](Player* bot) { return bot->IsAlive() && InRunInstance(run, bot); };

        Player* anchor = inRun(bots.front()) ? bots.front() : nullptr;
        if (!anchor)
        {
            auto living = std::find_if(bots.begin(), bots.end(), inRun);
            if (living != bots.end())
                anchor = *living;
        }

        if (anchor)
        {
            out = WorldLocation(run.spec.mapId, anchor->GetPositionX(), anchor->GetPositionY(),
                anchor->GetPositionZ(), anchor->GetOrientation());
            return true;
        }

        out = run.spec.entrance;
        return out.GetMapId() == run.spec.mapId;
    }

    // One member held up anywhere stalls the whole run - a follower whose chase path stops at
    // an M2 collision wall the tank walked through (Utgarde Keep's closed forge fires), for
    // instance. Brings such members to a living tank and notes where the tank stood and what it
    // was heading for, so a stall that survives this can be traced.
    void RegroupAtTank(Run& run, std::vector<Player*> const& bots, Config const& cfg, uint32 nextBoss)
    {
        Player* tank = bots.front();
        bool const tankInRun = tank->IsAlive() && InRunInstance(run, tank);

        uint32 moved = 0;
        if (tankInRun)
        {
            for (size_t i = 1; i < bots.size(); ++i)
            {
                Player* bot = bots[i];
                if (!bot->IsAlive() || bot->IsBeingTeleported() || bot->IsInCombat())
                    continue;

                if (InRunInstance(run, bot) && bot->GetExactDist(tank) <= cfg.regroupDistance)
                    continue;

                bot->TeleportTo(run.spec.mapId, tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ(),
                    tank->GetOrientation());
                ++moved;
            }
        }

        run.stuckNote = Acore::StringFormat("{}s without progress: tank {} at ({:.1f}, {:.1f}, {:.1f}) heading for "
            "boss {}, {} member(s) brought to it{}", run.noProgressMs / IN_MILLISECONDS,
            tankInRun ? "in the dungeon" : "not usable", tank->GetPositionX(), tank->GetPositionY(),
            tank->GetPositionZ(), nextBoss, moved, run.clearNote.empty() ? "" : "; clear: " + run.clearNote);
        LOG_INFO("playerbots.mplus", "Bot run #{}: {}", run.id, run.stuckNote);
    }

    // The clear's owner as every member's master: it puts the bots on playerbots' fast reaction
    // path and enables the master-gated "avoid aoe". The owner is parked outside the instance,
    // so stock follow-master is taken out.
    void AssertOwner(std::vector<Player*> const& bots, Player* owner)
    {
        for (Player* bot : bots)
        {
            PlayerbotAI* botAI = GetBotAI(bot);
            if (!botAI)
                continue;

            if (botAI->GetMaster() != owner)
            {
                botAI->SetMaster(owner);
                if (sPlayerbotAIConfig.AutoAvoidAoe && !botAI->HasStrategy("avoid aoe", BOT_STATE_COMBAT))
                    botAI->ChangeStrategy("+avoid aoe", BOT_STATE_COMBAT);
            }

            if (botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT))
                botAI->ChangeStrategy("-follow", BOT_STATE_NON_COMBAT);
            if (botAI->HasStrategy("follow", BOT_STATE_COMBAT))
                botAI->ChangeStrategy("-follow", BOT_STATE_COMBAT);
        }
    }

    // Each returns an end reason, or an empty string to carry on.
    std::string AdvanceEnteringLeader(Run& run, std::vector<Player*> const& bots, Config const& cfg)
    {
        Player* tank = bots.front();
        if (!tank->IsBeingTeleported())
        {
            Map* map = tank->FindMap();
            if (map && map->GetId() == run.spec.mapId && map->IsDungeon())
            {
                run.instanceId = map->GetInstanceId();

                // The leader is now bound to this instance, so the others follow him into it
                // rather than each opening a copy of their own.
                for (size_t i = 1; i < bots.size(); ++i)
                {
                    if (!bots[i]->TeleportTo(run.spec.entrance))
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
            if (!map || map->GetId() != run.spec.mapId)
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

        // The navmesh only covers loaded grids, and PathGenerator answers a destination on an
        // unloaded tile with a straight line through the walls. Load the dungeon around the
        // group before anyone moves; DCMythicAdvanceAction also loads each destination's grid.
        if (!run.gridsLoaded)
        {
            if (Map* map = bots.front()->FindMap())
                map->LoadGridsInRange(*bots.front(), SIZE_OF_GRIDS);
            run.gridsLoaded = true;
        }

        if (run.clear)
        {
            DCBotRunEngine::Hooks const* engine = DCBotRunEngine::Get();
            if (!engine)
                return "mod-dungeon-clear is no longer available";

            std::string why;
            Player* owner = engine->owner(why);
            if (!owner)
            {
                if (run.phaseMs >= cfg.enterTimeoutSeconds * IN_MILLISECONDS)
                    return "the clear's owner did not come online: " + why;
                return "";
            }

            AssertOwner(bots, owner);
        }

        if (!run.spec.keystoneLevel)
        {
            SetPhase(run, Phase::Running);

            // A keystone run is listed by the run manager; this one lists itself, under the same
            // "BOT <leader>" label.
            if (!run.spec.raid)
                if (Map* map = sMapMgr->FindMap(run.spec.mapId, run.instanceId))
                    DCMythicPlusBots::RegisterSpectatableDungeonRun(map, "BOT " + run.members.front().name);

            Notify(run.requester, Acore::StringFormat("Bot run #{}: {} {} under way.", run.id, run.spec.name,
                DescribeMode(run.spec)));
            return "";
        }

        std::string error;
        if (!DCMythicPlusBots::StartBotRun(bots.front(), run.spec.keystoneLevel, error))
            return "the keystone could not be started: " + error;

        SetPhase(run, Phase::Running);
        Notify(run.requester, Acore::StringFormat("Bot run #{}: {} keystone +{} activated.", run.id, run.spec.name,
            uint32(run.spec.keystoneLevel)));
        return "";
    }

    uint64 TargetCell(Player const* tank)
    {
        auto const cell = [](float value, float size)
        { return static_cast<uint64>(static_cast<int64>(std::floor(value / size)) & 0xFFFFF); };
        return (cell(tank->GetPositionX(), CLEAR_TARGET_CELL_YARDS) << 40) |
               (cell(tank->GetPositionY(), CLEAR_TARGET_CELL_YARDS) << 20) |
               cell(tank->GetPositionZ(), CLEAR_TARGET_CELL_HEIGHT);
    }

    // Fights and tank movement keep the progress watchdog quiet even while the clear walks the same
    // dead-end route again and again without reporting a stall; here only a new target counts, or the tank
    // reaching ground it has not stood on since the target began. A long single-boss wing (the Armory has
    // only Herod) keeps one target for the whole run while the tank clears its way down the halls; a dead-end
    // route walked again, or a fight in one place (Warp Splinter's saplings), covers no new ground.
    std::string WatchClearTarget(Run& run, DCBotRunEngine::Hooks const& engine,
        DCBotRunEngine::ClearStatus const& status, Config const& cfg, uint32 elapsedMs, Player* tank, Player* owner)
    {
        bool const tankOnGround = tank && tank->IsAlive() && tank->IsInWorld() &&
                                  tank->GetMapId() == run.spec.mapId && tank->GetInstanceId() == run.instanceId;
        if (std::find(run.clearTargets.begin(), run.clearTargets.end(), status.next) == run.clearTargets.end())
        {
            run.clearTargets.push_back(status.next);
            run.clearTargetMs = 0;
            run.clearTargetSkipped = false;
            run.clearTargetCells.clear();
            if (tankOnGround)
                run.clearTargetCells.insert(TargetCell(tank));
            return "";
        }

        if (tankOnGround && run.clearTargetCells.insert(TargetCell(tank)).second)
        {
            run.clearTargetMs = 0;
            return "";
        }

        run.clearTargetMs += elapsedMs;
        uint32 const limitMs = cfg.clearTargetMinutes * MINUTE * IN_MILLISECONDS;
        if (!limitMs || !owner || run.clearTargetMs < limitMs)
            return "";

        std::string const target = status.next.empty() ? "its current objective" : status.next;
        if (run.clearTargetSkipped)
        {
            if (run.clearTargetMs < limitMs + CLEAR_TARGET_SKIP_GRACE_MS)
                return "";

            return Acore::StringFormat("the clear spent {} minutes on {} and skipping it did not move it on",
                cfg.clearTargetMinutes, target);
        }

        if (run.skips >= cfg.clearMaxSkips)
            return Acore::StringFormat("the clear spent {} minutes on {} with no skips left", cfg.clearTargetMinutes,
                target);

        engine.skip(tank, owner);
        ++run.skips;
        run.clearTargetSkipped = true;
        LOG_INFO("playerbots.mplus", "Bot run #{}: skipped {} after {} minutes without reaching a new target.",
            run.id, target, cfg.clearTargetMinutes);
        return "";
    }

    // mod-dungeon-clear's side of a running run: start it, nudge it out of pauses and stalls,
    // and restart it when it stopped before the dungeon was done.
    std::string AdvanceClear(Run& run, std::vector<Player*> const& bots, Config const& cfg, uint32 elapsedMs,
        bool anyoneFighting, Player* owner)
    {
        DCBotRunEngine::Hooks const* engine = DCBotRunEngine::Get();
        if (!engine)
            return "mod-dungeon-clear is no longer available";

        Player* tank = bots.front();

        if (!run.clearStarted)
        {
            std::string error = "the clear's owner is offline";
            bool const ready = owner && tank->IsAlive() && InRunInstance(run, tank);
            if (ready && engine->start(tank, owner, run.spec.wing, error))
            {
                run.clearStarted = true;
                run.clearStartMs = 0;
                run.pausedMs = 0;
                run.stalledMs = 0;
                engine->armRelay(run.id, run.spec.mapId);
                run.clearNote = "started";
                LOG_INFO("playerbots.mplus", "Bot run #{}: mod-dungeon-clear started{}.", run.id,
                    run.restarts ? Acore::StringFormat(" again (restart {})", run.restarts) : "");
                return "";
            }

            run.clearStartMs += elapsedMs;
            if (run.clearStartMs >= CLEAR_START_TIMEOUT_MS)
                return "the clear did not start: " + error;
            return "";
        }

        DCBotRunEngine::ClearStatus const status = engine->status(tank);
        if (status.enabled)
        {
            if (status.paused)
                run.clearNote = "paused: " + status.pauseReason;
            else if (!status.stallReason.empty())
                run.clearNote = "stalled: " + status.stallReason;
            else
                run.clearNote = "heading for " + (status.next.empty() ? "the next objective" : status.next);

            // A paused clear waits for a human; nobody is coming. One paused at a door that will
            // not open skips the boss behind it.
            bool skipped = false;
            if (status.paused)
            {
                run.pausedMs += elapsedMs;
                if (owner && run.pausedMs >= cfg.clearPauseGraceSeconds * IN_MILLISECONDS)
                {
                    run.pausedMs = 0;
                    if (status.pausedAtDoor)
                    {
                        if (run.skips >= cfg.clearMaxSkips)
                            return "the clear waited at a door that never opened";

                        engine->skip(tank, owner);
                        ++run.skips;
                        skipped = true;
                    }

                    engine->resume(tank, owner);
                    LOG_INFO("playerbots.mplus", "Bot run #{}: resumed the clear ({}).", run.id, status.pauseReason);
                }
            }
            else
                run.pausedMs = 0;

            if (!status.paused && !status.stallReason.empty())
            {
                run.stalledMs += elapsedMs;
                if (owner && run.stalledMs >= cfg.clearStallGraceSeconds * IN_MILLISECONDS)
                {
                    run.stalledMs = 0;
                    if (run.skips >= cfg.clearMaxSkips)
                        return "the clear stalled: " + status.stallReason;

                    engine->skip(tank, owner);
                    ++run.skips;
                    skipped = true;
                    LOG_INFO("playerbots.mplus", "Bot run #{}: skipped an objective after a stall ({}).", run.id,
                        status.stallReason);
                }
            }
            else
                run.stalledMs = 0;

            // One skip per tick: the next status shows whether it moved the clear on.
            if (skipped)
                return "";

            return WatchClearTarget(run, *engine, status, cfg, elapsedMs, tank, owner);
        }

        if (status.finished)
        {
            run.clearFinished = true;
            run.clearNote = "finished";
            return "";
        }

        // It stopped early: a wipe without a resurrector, or a member that left. The revival
        // brings everyone back; once they are all up, inside and calm, it starts again.
        run.clearNote = "stopped early";
        if (anyoneFighting || run.calmMs < REVIVE_CALM_MS)
            return "";

        for (Player* bot : bots)
            if (!bot->IsAlive() || !InRunInstance(run, bot))
                return "";

        if (run.restarts >= cfg.clearMaxRestarts)
            return Acore::StringFormat("the clear stopped early {} times", run.restarts + 1);

        ++run.restarts;
        run.clearStarted = false;
        engine->disarmRelay(run.id);
        return "";
    }

    // Nothing else looks after the group: a cleared dungeon is left after a looting break; once
    // the fighting has stopped for a while, dead bots are revived and members outside the
    // instance (released to the graveyard, revived at a spirit healer) are brought back.
    std::string AdvanceRecovery(Run& run, std::vector<Player*> const& bots, Config const& cfg, uint32 elapsedMs)
    {
        if (!run.spec.keystoneLevel && (run.bossesRemaining == 0 || run.clearFinished))
        {
            if (!run.cleared)
            {
                run.cleared = true;
                Notify(run.requester, Acore::StringFormat("Bot run #{}: {} {} cleared in {}s.", run.id,
                    run.spec.name, DescribeMode(run.spec), run.timerMs / IN_MILLISECONDS));
            }

            run.clearedMs += elapsedMs;
            if (run.clearedMs >= CLEARED_GRACE_MS)
                return "the dungeon is cleared";
        }

        // A keystone run led by "dc mythic" keeps its own death rules.
        bool const revive = cfg.reviveDeadBots && (!run.spec.keystoneLevel || run.clear);
        if (!revive || run.calmMs < REVIVE_CALM_MS)
            return "";

        WorldLocation regroup;
        if (!FindRegroupPoint(run, bots, regroup))
            return "";

        for (Player* bot : bots)
        {
            if (bot->IsBeingTeleported())
                continue;

            if (bot->IsAlive() && InRunInstance(run, bot))
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
        Map* map = sMapMgr->FindMap(run.spec.mapId, run.instanceId);

        // A keystone run moves from countdown (1) to timer (2) and back to 0 when it completes or
        // fails; a run without one is under way at once.
        uint8 keyPhase = 2;
        if (run.spec.keystoneLevel)
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

        DCBotRunEngine::Hooks const* engine = run.clear ? DCBotRunEngine::Get() : nullptr;
        Player* owner = nullptr;
        if (engine)
        {
            std::string why;
            owner = engine->owner(why);
        }

        // Bots count their own deaths and leave a group once the count runs high; a bot run
        // revives them, so the count is meaningless here.
        for (Player* bot : bots)
            if (PlayerbotAI* botAI = GetBotAI(bot))
                botAI->GetAiObjectContext()->GetValue<uint32>("death count")->Set(0);

        if (run.clear)
        {
            if (owner)
                AssertOwner(bots, owner);
        }
        else
        {
            // "dc mythic" followers need the tank as their master, or "follow" has nobody to follow.
            for (Player* bot : bots)
                if (PlayerbotAI* botAI = GetBotAI(bot))
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
        if (run.spec.keystoneLevel)
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

        run.calmMs = anyoneFighting ? 0 : run.calmMs + elapsedMs;

        if (tank->GetMapId() == run.spec.mapId
            && tank->GetExactDist(run.tankX, run.tankY, run.tankZ) > PROGRESS_MOVE_DISTANCE)
        {
            progressed = true;
            run.tankX = tank->GetPositionX();
            run.tankY = tank->GetPositionY();
            run.tankZ = tank->GetPositionZ();
        }

        run.noProgressMs = progressed ? 0 : run.noProgressMs + elapsedMs;

        uint32 const regroupAfterMs = run.clear ? CLEAR_REGROUP_AFTER_MS : REGROUP_AFTER_MS;
        if (progressed)
            run.nextRegroupMs = regroupAfterMs;
        else if (!anyoneFighting && run.noProgressMs >= run.nextRegroupMs)
        {
            run.nextRegroupMs = run.noProgressMs + regroupAfterMs;
            RegroupAtTank(run, bots, cfg, nextBoss);
        }

        if (run.clear)
        {
            std::string const reason = AdvanceClear(run, bots, cfg, elapsedMs, anyoneFighting, owner);
            if (!reason.empty())
                return reason;
        }

        std::string const reason = AdvanceRecovery(run, bots, cfg, elapsedMs);
        if (!reason.empty())
            return reason;

        uint32 const noProgressSeconds = run.clear ? cfg.clearNoProgressSeconds : cfg.stuckSeconds;
        if (noProgressSeconds && run.noProgressMs >= noProgressSeconds * IN_MILLISECONDS)
            return Acore::StringFormat("no progress for {} seconds", noProgressSeconds);

        uint32 const maxMinutes = RunLimitMinutes(cfg, run.spec.mapId, run.clear);
        if (maxMinutes && run.timerMs >= maxMinutes * MINUTE * IN_MILLISECONDS)
            return Acore::StringFormat("the run passed {} minutes", maxMinutes);

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
        run.lifeMs += elapsedMs;

        std::vector<Player*> bots;
        bots.reserve(run.members.size());
        for (Member const& member : run.members)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(member.guid);
            if (!bot || !bot->IsInWorld())
            {
                // Mid-teleport a bot is briefly out of the world; only a missing session means
                // it is really gone.
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

        // The group is formed under the tank before any phase runs, and the tank carries the
        // clear. A tank outside it (something outside the run took it out) leaves the others
        // nobody to follow, and a lone tank fighting whatever comes keeps the progress watchdog
        // from ever firing.
        if (!bots.empty() && !bots.front()->GetGroup())
        {
            FinishRun(run, Acore::StringFormat("{}, the tank, left the group", run.members.front().name));
            return;
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

    // Registers the run, groups the bots under the tank and walks the tank in; the world tick
    // does the rest. 0 when the run could not be formed, `message` says why.
    uint32 FormRun(RunSpec const& spec, std::vector<Player*> const& bots, ObjectGuid requester, bool automatic,
        bool clear, std::string& message)
    {
        Player* tank = bots.front();

        Run run;
        run.spec = spec;
        run.tank = tank->GetGUID();
        run.requester = requester;
        run.automatic = automatic;
        run.clear = clear;
        run.nextRegroupMs = clear ? CLEAR_REGROUP_AFTER_MS : REGROUP_AFTER_MS;

        for (Player* bot : bots)
        {
            run.members.push_back({ bot->GetGUID(), bot->GetName(),
                WorldLocation(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                    bot->GetOrientation()) });
        }

        // Registered before any strategy is rebuilt, so AiFactory already sees these bots as
        // managed when the group change resets them.
        {
            std::lock_guard<std::recursive_mutex> lock(sMutex);
            run.id = sNextRunId++;
            sRuns[run.id] = run;
            for (Member const& member : run.members)
                sMemberRun[member.guid] = run.id;
            ++sTotals.started;
        }

        Group* group = new Group();
        if (!group->Create(tank))
        {
            delete group;
            message = "the group could not be created";
            TearDown(run.id, message, false);
            return 0;
        }

        sGroupMgr->AddGroup(group);

        // A raid group before the sixth member (a full party refuses AddMember), and for any raid
        // map, whose entry requires one.
        bool const raidGroup = spec.raid || bots.size() > PARTY_SIZE;
        if (raidGroup)
            group->ConvertToRaid();

        for (size_t i = 1; i < bots.size(); ++i)
        {
            if (!group->AddMember(bots[i]))
            {
                message = Acore::StringFormat("{} could not join the group", bots[i]->GetName());
                TearDown(run.id, message, false);
                return 0;
            }
        }

        if (raidGroup)
        {
            uint8 subGroup = 0;
            for (Player* bot : bots)
            {
                group->ChangeMembersGroup(bot->GetGUID(), subGroup);
                subGroup = (subGroup + 1) % MAX_RAID_SUBGROUPS;
            }

            // mod-dungeon-clear elects the flagged main tank to lead a raid.
            group->SetGroupMemberFlag(tank->GetGUID(), true, MEMBER_FLAG_MAINTANK);
        }

        group->SetLootMethod(NEED_BEFORE_GREED);
        group->SetLootThreshold(ITEM_QUALITY_UNCOMMON);
        group->ResetInstances(INSTANCE_RESET_CHANGE_DIFFICULTY, spec.raid, tank);
        if (spec.raid)
            group->SetRaidDifficulty(spec.difficulty);
        else
            group->SetDungeonDifficulty(spec.difficulty);

        for (Player* bot : bots)
        {
            // A bot that ran this dungeon lately is still bound to that instance and would land there
            // instead of in the tank's. Lockouts stay: Recruit skips permanently bound bots.
            if (InstancePlayerBind* bind = sInstanceSaveMgr->PlayerGetBoundInstance(bot->GetGUID(), spec.mapId,
                    spec.difficulty))
                if (!bind->perm)
                    sInstanceSaveMgr->PlayerUnbindInstance(bot->GetGUID(), spec.mapId, spec.difficulty, true, bot);

            if (PlayerbotAI* botAI = GetBotAI(bot))
            {
                botAI->ResetStrategies();
                // A clear's members get its owner once inside; "dc mythic" followers follow the tank.
                botAI->SetMaster(clear || bot == tank ? nullptr : tank);
            }
        }

        message = Acore::StringFormat("Bot run #{}{} forming: {} {} (map {}, levels {}-{}), {} bots led by {}{}.",
            run.id, automatic ? " (automatic)" : "", spec.name, DescribeMode(spec), spec.mapId, spec.minLevel,
            spec.maxLevel, run.members.size(), run.members.front().name,
            clear ? ", cleared by mod-dungeon-clear" : "");
        LOG_INFO("playerbots.mplus", "{}", message);

        if (!tank->TeleportTo(spec.entrance))
        {
            message = Acore::StringFormat("{} could not enter the dungeon (entry requirement, instance limit or "
                "lockout?)", run.members.front().name);
            TearDown(run.id, message, false);
            return 0;
        }

        return run.id;
    }

    // ---- Automatic runs -----------------------------------------------------------------------

    bool AutoRunsOn(Config const& cfg)
    {
        return cfg.enabled && (sAutoOverride ? *sAutoOverride : cfg.autoRuns);
    }

    // Picks a dungeon that enough idle bots fit, at random by weight, and starts it.
    bool TryAutoRun(Config const& cfg, std::string& note)
    {
        DCBotRunEngine::Hooks const* engine = ClearEngine(cfg);
        if (!engine)
        {
            note = "automatic runs need mod-dungeon-clear (DungeonClear.Enable and "
                   "Playerbots.DCMythicBots.UseDungeonClear)";
            return false;
        }

        std::unordered_set<uint32> busyMaps;
        {
            std::lock_guard<std::recursive_mutex> lock(sMutex);
            if (sRuns.size() >= cfg.maxConcurrentRuns)
            {
                note = Acore::StringFormat("all {} run slots are taken", cfg.maxConcurrentRuns);
                return false;
            }

            for (auto const& [id, run] : sRuns)
            {
                (void)id;
                busyMaps.insert(run.spec.mapId);
            }
        }

        std::string why;
        if (!engine->owner(why))
        {
            note = "waiting for the clear's owner: " + why;
            return false;
        }

        std::vector<Candidate> const idle = CollectIdle();
        IdleCounts counts{};
        for (Candidate const& candidate : idle)
            ++counts[candidate.team][candidate.role][candidate.level];

        std::vector<std::pair<RunSpec const*, uint32>> eligible;
        uint32 totalWeight = 0;
        for (RunSpec const& spec : Templates(cfg))
        {
            uint32 const weight = WeightOf(cfg, spec);
            if (!weight || busyMaps.count(spec.mapId) || !WantedAutomatically(cfg, spec) || !Fits(counts, spec) ||
                ResetsDuringRun(cfg, spec))
                continue;

            eligible.emplace_back(&spec, weight);
            totalWeight += weight;
        }

        while (!eligible.empty())
        {
            uint32 roll = urand(1, totalWeight);
            size_t pick = 0;
            while (pick + 1 < eligible.size() && roll > eligible[pick].second)
                roll -= eligible[pick++].second;

            RunSpec const spec = *eligible[pick].first;
            totalWeight -= eligible[pick].second;
            eligible.erase(eligible.begin() + pick);

            // Lockouts and entry requirements (keys, attunements) can still rule the bots out.
            std::vector<Player*> bots;
            std::string error;
            if (!Recruit(spec, idle, bots, error))
                continue;

            return FormRun(spec, bots, ObjectGuid::Empty, true, true, note) != 0;
        }

        note = Acore::StringFormat("no dungeon fits the {} idle bots by level, faction and lockouts", idle.size());
        return false;
    }

    void StartFromCommand(ChatHandler* handler, Config const& cfg, RunSpec const& spec, bool clear)
    {
        {
            std::lock_guard<std::recursive_mutex> lock(sMutex);
            if (sRuns.size() >= cfg.maxConcurrentRuns)
            {
                handler->PSendSysMessage("{} bot run(s) already active (Playerbots.DCMythicBots.MaxConcurrentRuns).",
                    sRuns.size());
                return;
            }
        }

        if (clear)
        {
            std::string why;
            if (!DCBotRunEngine::Get()->owner(why))
            {
                handler->PSendSysMessage("mod-dungeon-clear's driver is not ready ({}); try again in a few seconds.",
                    why);
                return;
            }
        }

        std::vector<Player*> bots;
        std::string error;
        if (!Recruit(spec, CollectIdle(), bots, error))
        {
            handler->SendSysMessage(error);
            return;
        }

        ObjectGuid requester;
        if (WorldSession* session = handler->GetSession())
            if (Player* player = session->GetPlayer())
                requester = player->GetGUID();

        std::string message;
        FormRun(spec, bots, requester, false, clear, message);
        handler->SendSysMessage(message);
    }

    // A run's group is saved like any other, so a run that was under way at a shutdown or crash
    // comes back at the next start as a group holding its bots, with no run to lead or end it.
    // Random bots only group for a run or a battleground queue, neither of which outlives a
    // restart, so every group of random bots alone is disbanded once, before the first run.
    void ReleaseLeftoverGroups()
    {
        QueryResult result =
            CharacterDatabase.Query("SELECT gm.guid, c.account FROM group_member gm JOIN characters c ON "
                                    "c.guid = gm.memberGuid");
        if (!result)
            return;

        std::unordered_map<uint32, bool> randomBotsOnly;
        do
        {
            Field* fields = result->Fetch();
            bool const randomBot = sPlayerbotAIConfig.IsInRandomAccountList(fields[1].Get<uint32>());
            auto [it, inserted] = randomBotsOnly.emplace(fields[0].Get<uint32>(), randomBot);
            if (!inserted)
                it->second = it->second && randomBot;
        } while (result->NextRow());

        uint32 released = 0;
        for (auto const& [groupId, botsOnly] : randomBotsOnly)
        {
            if (!botsOnly)
                continue;

            Group* group = sGroupMgr->GetGroupByGUID(groupId);
            if (!group || group->isBGGroup() || group->isBFGroup() || group->isLFGGroup())
                continue;

            group->Disband();
            ++released;
        }

        if (released)
            LOG_INFO("playerbots.mplus", "Bot runs: disbanded {} group(s) of random bots left from before the start.",
                released);
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
            // The account list tells random bots apart; playerbots fills it while the server starts.
            if (!_leftoversReleased && !sPlayerbotAIConfig.RandomBotAccounts.empty())
            {
                _leftoversReleased = true;
                ReleaseLeftoverGroups();
            }

            TickRelays();

            _timer += diff;
            _autoTimer += diff;

            if (_timer >= TICK_MS)
            {
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

            if (_autoTimer >= sAutoIntervalMs)
            {
                _autoTimer = 0;
                Config const cfg = DCBotMythicRun::GetConfig();
                if (AutoRunsOn(cfg))
                {
                    std::string note;
                    TryAutoRun(cfg, note);
                    if (note != sLastAutoNote)
                        LOG_DEBUG("playerbots.mplus", "Bot runs (automatic): {}", note);
                    sLastAutoNote = note;
                }
            }
        }

    private:
        // A client sends an area trigger the moment its player walks in, so the relay runs every
        // world tick: a one-second sample can carry a running bot clean through a small volume.
        static void TickRelays()
        {
            DCBotRunEngine::Hooks const* engine = DCBotRunEngine::Get();
            if (!engine)
                return;

            std::vector<std::pair<uint32, ObjectGuid>> relays;
            {
                std::lock_guard<std::recursive_mutex> lock(sMutex);
                for (auto const& [id, run] : sRuns)
                    if (run.clear && run.clearStarted)
                        relays.emplace_back(id, run.tank);
            }

            for (auto const& [id, tankGuid] : relays)
                if (Player* tank = ObjectAccessor::FindConnectedPlayer(tankGuid))
                    if (tank->IsInWorld())
                        engine->tickRelay(id, tank);
        }

        uint32 _timer = 0;
        uint32 _autoTimer = 0;
        bool _leftoversReleased = false;
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

    bool UsesOwnLead(ObjectGuid guid)
    {
        std::lock_guard<std::recursive_mutex> lock(sMutex);

        auto member = sMemberRun.find(guid);
        if (member == sMemberRun.end())
            return false;

        auto run = sRuns.find(member->second);
        return run != sRuns.end() && !run->second.clear;
    }

    bool IsLeadingRunningRun(ObjectGuid guid)
    {
        std::lock_guard<std::recursive_mutex> lock(sMutex);

        auto member = sMemberRun.find(guid);
        if (member == sMemberRun.end())
            return false;

        auto run = sRuns.find(member->second);
        return run != sRuns.end() && !run->second.clear && run->second.tank == guid
            && run->second.phase == Phase::Running && run->second.timerSeen && !run->second.cleared;
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

            keystoneLevel = run->second.spec.keystoneLevel;
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
        static char const* const usage = "Usage: .playerbots mplus start [mapId] [keystoneLevel]";

        Config const cfg = GetConfig();
        if (!cfg.enabled)
        {
            handler->SendSysMessage("Bot runs are disabled (Playerbots.DCMythicBots.Enable).");
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
                handler->SendSysMessage(usage);
                return true;
            }
            mapId = *value;
        }
        if (input >> token)
        {
            Optional<uint32> value = Acore::StringTo<uint32>(token);
            if (!value)
            {
                handler->SendSysMessage(usage);
                return true;
            }
            keystoneLevel = *value;
        }

        if (keystoneLevel < 2 || keystoneLevel > cfg.maxKeystoneLevel)
        {
            handler->PSendSysMessage("Keystone level must be between 2 and {}.", cfg.maxKeystoneLevel);
            return true;
        }

        RunSpec spec;
        bool const clear = ClearEngine(cfg) != nullptr;
        if (clear)
        {
            RunSpec const* found = FindTemplate(cfg, std::to_string(mapId), DUNGEON_DIFFICULTY_EPIC, true);
            if (!found)
                found = FindTemplate(cfg, std::to_string(mapId), DUNGEON_DIFFICULTY_EPIC, false);
            if (!found)
            {
                handler->PSendSysMessage("Map {} has no Mythic mode a bot run can take (dc_dungeon_mythic_profile, "
                    "MapDifficulty).", mapId);
                return true;
            }

            spec = *found;
            spec.keystoneLevel = static_cast<uint8>(keystoneLevel);
        }
        else
        {
            if (std::find(cfg.dungeons.begin(), cfg.dungeons.end(), mapId) == cfg.dungeons.end())
            {
                handler->PSendSysMessage("Map {} is not in Playerbots.DCMythicBots.Dungeons.", mapId);
                return true;
            }

            std::string error;
            if (!LegacySpec(cfg, mapId, DUNGEON_DIFFICULTY_EPIC, static_cast<uint8>(keystoneLevel), spec, error))
            {
                handler->SendSysMessage(error);
                return true;
            }
        }

        StartFromCommand(handler, cfg, spec, clear);
        return true;
    }

    bool HandleDungeonStartCommand(ChatHandler* handler, char const* args)
    {
        static char const* const usage = "Usage: .playerbots dungeon start [token|mapId] [normal|heroic|mythic]";

        Config const cfg = GetConfig();
        if (!cfg.enabled)
        {
            handler->SendSysMessage("Bot runs are disabled (Playerbots.DCMythicBots.Enable).");
            return true;
        }

        std::string which;
        Difficulty difficulty = DUNGEON_DIFFICULTY_NORMAL;

        // Either order: `start uk heroic`, `start heroic 574` or `start heroic`.
        std::istringstream input(args ? args : "");
        std::string token;
        while (input >> token)
        {
            std::string const word = Lowered(token);
            if (word == "normal" || word == "n")
                difficulty = DUNGEON_DIFFICULTY_NORMAL;
            else if (word == "heroic" || word == "h")
                difficulty = DUNGEON_DIFFICULTY_HEROIC;
            else if (word == "mythic" || word == "m")
                difficulty = DUNGEON_DIFFICULTY_EPIC;
            else if (which.empty())
                which = word;
            else
            {
                handler->SendSysMessage(usage);
                return true;
            }
        }

        if (ClearEngine(cfg))
        {
            if (which.empty())
            {
                handler->SendSysMessage(usage);
                handler->SendSysMessage("Name a dungeon by its mod-dungeon-clear token (`.dc test list`) or map id.");
                return true;
            }

            RunSpec const* found = FindTemplate(cfg, which, difficulty, false);
            if (!found)
            {
                handler->PSendSysMessage("No {} bot run for '{}'. Tokens and map ids come from mod-dungeon-clear's "
                    "catalogue plus Playerbots.DCMythicBots.AutoExtraDungeons.", difficulty == DUNGEON_DIFFICULTY_EPIC
                    ? "mythic" : difficulty == DUNGEON_DIFFICULTY_HEROIC ? "heroic" : "normal", which);
                return true;
            }

            StartFromCommand(handler, cfg, *found, true);
            return true;
        }

        uint32 mapId = cfg.dungeons.empty() ? 574 : cfg.dungeons.front();
        if (!which.empty())
        {
            Optional<uint32> value = Acore::StringTo<uint32>(which);
            if (!value)
            {
                handler->SendSysMessage("Without mod-dungeon-clear a bot run takes a map id.");
                return true;
            }
            mapId = *value;
        }

        if (std::find(cfg.dungeons.begin(), cfg.dungeons.end(), mapId) == cfg.dungeons.end())
        {
            handler->PSendSysMessage("Map {} is not in Playerbots.DCMythicBots.Dungeons.", mapId);
            return true;
        }

        MapEntry const* mapEntry = sMapStore.LookupEntry(mapId);
        if (!mapEntry || !mapEntry->IsNonRaidDungeon())
        {
            handler->PSendSysMessage("Map {} is not a five-player dungeon.", mapId);
            return true;
        }

        // Entering downscales a missing difficulty to Normal without a word.
        if (difficulty != DUNGEON_DIFFICULTY_NORMAL && !GetMapDifficultyData(mapId, difficulty))
        {
            handler->PSendSysMessage("Map {} has no such difficulty.", mapId);
            return true;
        }

        // The tank walks from boss to boss; with no creature to walk to it would stand at the
        // entrance until the watchdog gives up.
        DungeonEncounterList const* encounters = GetEncounters(mapId, difficulty, false);
        bool const hasBoss = encounters && std::any_of(encounters->begin(), encounters->end(),
            [](DungeonEncounter const* encounter)
            {
                return encounter && encounter->dbcEntry && encounter->creditType == ENCOUNTER_CREDIT_KILL_CREATURE;
            });

        if (!hasBoss)
        {
            handler->PSendSysMessage("Map {} has no creature boss encounters at that difficulty in "
                "instance_encounters.", mapId);
            return true;
        }

        RunSpec spec;
        std::string error;
        if (!LegacySpec(cfg, mapId, difficulty, 0, spec, error))
        {
            handler->SendSysMessage(error);
            return true;
        }

        StartFromCommand(handler, cfg, spec, false);
        return true;
    }

    bool HandleAutoCommand(ChatHandler* handler, char const* args)
    {
        Config const cfg = GetConfig();
        std::vector<std::string> const words = ParseTokens(args ? args : "");
        std::string const word = words.empty() ? std::string() : words.front();

        if (word == "on" || word == "off")
        {
            sAutoOverride = word == "on";
            handler->PSendSysMessage("Automatic bot runs {} until the next config reload.",
                *sAutoOverride ? "on" : "off");
            return true;
        }

        if (word == "now")
        {
            std::string note;
            bool const started = TryAutoRun(cfg, note);
            sLastAutoNote = note;
            handler->PSendSysMessage("{}{}", started ? "" : "No run started: ", note);
            return true;
        }

        if (!word.empty())
        {
            handler->SendSysMessage("Usage: .playerbots dungeon auto [now|on|off]");
            return true;
        }

        std::vector<RunSpec> const& templates = Templates(cfg);
        std::array<uint32, 5> byMode{};
        for (RunSpec const& spec : templates)
        {
            if (spec.keystoneLevel)
                ++byMode[3];
            else if (spec.raid)
                ++byMode[4];
            else
                ++byMode[std::min<uint32>(spec.difficulty, 2)];
        }

        Totals totals;
        size_t active = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(sMutex);
            totals = sTotals;
            active = sRuns.size();
        }

        handler->PSendSysMessage("Automatic bot runs: {} (every {}s, {} of {} slots in use, mod-dungeon-clear {}).",
            AutoRunsOn(cfg) ? "on" : "off", cfg.autoIntervalSeconds, active, cfg.maxConcurrentRuns,
            ClearEngine(cfg) ? "leading" : "unavailable");
        handler->PSendSysMessage("Templates: {} normal, {} heroic, {} mythic, {} keystone, {} raid "
            "(weights {}/{}/{}/{}/{}).", byMode[0], byMode[1], byMode[2], byMode[3], byMode[4], cfg.normalWeight,
            cfg.heroicWeight, cfg.mythicWeight, cfg.keystoneWeight, cfg.autoRaids ? cfg.raidWeight : 0);
        handler->PSendSysMessage("Since start: {} runs started, {} cleared, {} ended.", totals.started, totals.cleared,
            totals.ended);
        if (!sLastAutoNote.empty())
            handler->PSendSysMessage("Last automatic attempt: {}", sLastAutoNote);
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
            handler->PSendSysMessage("Bot run #{}{}: {} {} (map {} instance {}) - {}, {}s in phase, led by {}.", run.id,
                run.automatic ? " (automatic)" : "", run.spec.name, DescribeMode(run.spec), run.spec.mapId,
                run.instanceId, PhaseName(run), run.phaseMs / IN_MILLISECONDS,
                run.clear ? "mod-dungeon-clear" : "dc mythic");

            if (run.timerSeen)
            {
                std::string const bossesLeft = run.bossesRemaining == std::numeric_limits<size_t>::max()
                    ? "?" : std::to_string(run.bossesRemaining);
                handler->PSendSysMessage("|- Time {}s, bosses left {}, {}s without progress.",
                    run.timerMs / IN_MILLISECONDS, bossesLeft, run.noProgressMs / IN_MILLISECONDS);
            }

            if (run.clear)
                handler->PSendSysMessage("|- Clear: {} (skipped {}, restarts {}, {}s since a new target).",
                    run.clearNote.empty() ? "not started" : run.clearNote, run.skips, run.restarts,
                    run.clearTargetMs / IN_MILLISECONDS);

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

                handler->PSendSysMessage("|- {}{}: level {}, map {} {}, {:.0f}% health{}", member.name,
                    member.guid == run.tank ? " (tank)" : "", bot->GetLevel(), bot->GetMapId(),
                    bot->IsAlive() ? "alive" : "dead", bot->GetHealthPct(), bot->IsInCombat() ? ", in combat" : "");
            }

            // The tank's combat strategies, so the dungeon strategy (for example "wotlk-uk") can be
            // checked in game: ApplyInstanceStrategies adds it on the worldport ack, and the mythic
            // map's difficulty resolves to the heroic branch of every dungeon strategy.
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
