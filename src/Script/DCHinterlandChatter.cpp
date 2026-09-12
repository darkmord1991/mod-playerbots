/*
 * Dark Chaos - Hinterland BG bot chatter.
 *
 * Bots fought in HLBG in complete silence: the battleground chat window stayed
 * empty for the whole match no matter how many bots were in it, which is the
 * one thing that makes a bot-filled battleground read as empty even when it is
 * full. This gives them something to say.
 *
 * WHY IT IS NOT PlayerbotAI::TellMaster / SayToRaid
 *
 * A random bot in a battleground has no master, so the module's ordinary
 * chatter path is dead there by construction. And SayToRaid is the wrong
 * channel anyway: a 3.3.5 client inside a battleground types into
 * CHAT_MSG_BATTLEGROUND (the client rewrites /raid to /bg while you are in
 * one), so that is what a bot has to send for the line to land in the same
 * window, with the same colour, as everything a real player says. It is sent to
 * the bot's own team only, exactly like the core's CHAT_MSG_BATTLEGROUND
 * handler does.
 *
 * THROTTLING IS THE WHOLE DESIGN
 *
 * This fork routes bot chatter to the group rather than whispering the master,
 * so every line is seen by everybody and a chatty bot multiplies by roster
 * size. Four independent limits apply to every single line, and a line is only
 * built when at least one real player is on that team to read it:
 *
 *   - a minimum gap between any two lines on a team,
 *   - a rolling per-minute budget per team,
 *   - a per-bot cooldown, so one bot cannot carry the conversation,
 *   - a short recent-lines memory, so the same sentence is not repeated.
 *
 * All of it is off with AiPlayerbot.DCHinterland.Chatter.Enable = 0.
 *
 * STATE
 *
 * Per battleground instance, keyed by GetInstanceID() and dropped in
 * OnBattlegroundDestroy. Nothing here holds a Player* or a Battleground*
 * between calls - bots leave, log out and get removed mid-match, so speakers
 * are resolved fresh out of Battleground::GetPlayers() every time and bot
 * cooldowns are keyed by raw GUID value.
 *
 * OnBattlegroundUpdate runs on the world thread but OnPlayerPVPKill fires from
 * Unit::Kill on a map update thread, so the state map is mutex guarded.
 */

#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

// For DC_BATTLEGROUND_HLBG_TYPE_ID; the geography header carries the landmark
// names the lines are built from.
#include "DCHinterlandGeography.h"
#include "DCHinterlandTactics.h"

#include "Battleground.h"
#include "Chat.h"
#include "Common.h"
#include "Config.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    /* ------------------------------------------------------------------ */
    /* Lines                                                              */
    /* ------------------------------------------------------------------ */

    // %place  - a landmark name from DCHinterlandGeography
    // %target - the enemy player that was killed
    // %killer - the enemy player that landed the killing blow
    // %res    - a resource count
    //
    // Deliberately plain battleground talk: no lore voice, no emotes spelled
    // out, nothing that reads as a scripted NPC. Keep new lines short - long
    // sentences are what make a chat log look generated.

    constexpr char const* LINES_PREP[] = {
        "ready when you are",
        "anyone healing?",
        "stick together at the start, we lose these 1v1",
        "gogo",
        "who's going to %place?",
        "im going to %place",
        "invite for heals pls",
        "lets not feed this time",
        "buffs?",
    };

    constexpr char const* LINES_START[] = {
        "go go go",
        "moving to %place",
        "im taking %place",
        "group up, dont run in solo",
        "lets go",
    };

    constexpr char const* LINES_KILL[] = {
        "got %target",
        "%target down at %place",
        "%target down",
        "killed %target near %place",
        "ez",
        "one down at %place",
    };

    constexpr char const* LINES_DEATH[] = {
        "%killer got me at %place",
        "im down at %place",
        "dead at %place, they have numbers there",
        "%killer is hitting hard, careful",
        "rip",
        "need help at %place",
    };

    constexpr char const* LINES_HELP[] = {
        "low at %place, help",
        "heal pls",
        "getting focused at %place",
        "im low at %place",
        "help at %place",
    };

    constexpr char const* LINES_ENEMY_LOW[] = {
        "they're down to %res, keep pushing",
        "%res left on them, dont stop now",
        "they're bleeding out, push %place",
        "almost, %res left",
    };

    constexpr char const* LINES_TEAM_LOW[] = {
        "we're at %res, stop feeding",
        "%res left, someone defend %place",
        "we're losing resources fast, back to %place",
        "we need bodies at %place",
    };

    // Every idle line has to read correctly for a bot standing AT %place, since
    // that is what the sweep fills in - "heading %place" would have a bot
    // announce a trip to where it already is.
    constexpr char const* LINES_IDLE[] = {
        "anyone at %place?",
        "%place is clear",
        "they're stacking %place",
        "need one more at %place",
        "watch the flank",
        "im holding %place",
        "someone take the boss?",
        "nobody at %place, im moving",
        "regrouping",
    };

    constexpr char const* LINES_WIN[] = {
        "gg",
        "gg wp",
        "easy",
        "good fight",
    };

    constexpr char const* LINES_LOSE[] = {
        "gg",
        "next one",
        "we needed defence",
        "gf",
    };

    enum LineId : uint8
    {
        LINE_PREP,
        LINE_START,
        LINE_KILL,
        LINE_DEATH,
        LINE_HELP,
        LINE_ENEMY_LOW,
        LINE_TEAM_LOW,
        LINE_IDLE,
        LINE_WIN,
        LINE_LOSE,
        LINE_MAX
    };

    struct LinePool
    {
        char const* const* lines;
        uint32 count;
    };

    constexpr LinePool POOLS[LINE_MAX] = {
        { LINES_PREP, uint32(std::size(LINES_PREP)) },
        { LINES_START, uint32(std::size(LINES_START)) },
        { LINES_KILL, uint32(std::size(LINES_KILL)) },
        { LINES_DEATH, uint32(std::size(LINES_DEATH)) },
        { LINES_HELP, uint32(std::size(LINES_HELP)) },
        { LINES_ENEMY_LOW, uint32(std::size(LINES_ENEMY_LOW)) },
        { LINES_TEAM_LOW, uint32(std::size(LINES_TEAM_LOW)) },
        { LINES_IDLE, uint32(std::size(LINES_IDLE)) },
        { LINES_WIN, uint32(std::size(LINES_WIN)) },
        { LINES_LOSE, uint32(std::size(LINES_LOSE)) },
    };

    /* ------------------------------------------------------------------ */
    /* Config                                                             */
    /* ------------------------------------------------------------------ */

    struct ChatterConfig
    {
        bool enabled = true;
        uint32 teamGapMs = 12 * IN_MILLISECONDS;
        uint32 linesPerMinute = 5;
        uint32 botCooldownMs = 60 * IN_MILLISECONDS;
        uint32 recentMemory = 6;
        uint32 idleMinMs = 70 * IN_MILLISECONDS;
        uint32 idleMaxMs = 160 * IN_MILLISECONDS;
        uint32 killChance = 25;
        uint32 deathChance = 18;
        uint32 helpChance = 30;
        uint32 helpHealthPct = 30;
        uint32 sayChance = 15;
        uint32 emoteChance = 35;
        bool resourceCalls = true;
    };

    ChatterConfig s_cfg;

    void LoadConfig()
    {
        ChatterConfig cfg;

        cfg.enabled = sConfigMgr->GetOption<bool>("AiPlayerbot.DCHinterland.Chatter.Enable", true);
        cfg.teamGapMs = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.Chatter.MinSecondsBetweenLines", 12) *
                        IN_MILLISECONDS;
        cfg.linesPerMinute = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.Chatter.LinesPerMinute", 5);
        cfg.botCooldownMs =
            sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.Chatter.BotCooldownSeconds", 60) * IN_MILLISECONDS;
        cfg.recentMemory = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.Chatter.RecentMemory", 6);
        cfg.idleMinMs =
            sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.Chatter.IdleSecondsMin", 70) * IN_MILLISECONDS;
        cfg.idleMaxMs =
            sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.Chatter.IdleSecondsMax", 160) * IN_MILLISECONDS;
        cfg.killChance = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.Chatter.KillChance", 25);
        cfg.deathChance = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.Chatter.DeathChance", 18);
        cfg.helpChance = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.Chatter.HelpChance", 30);
        cfg.helpHealthPct = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.Chatter.HelpHealthPct", 30);
        cfg.sayChance = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.Chatter.SayChance", 15);
        cfg.emoteChance = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.Chatter.EmoteChance", 35);
        cfg.resourceCalls = sConfigMgr->GetOption<bool>("AiPlayerbot.DCHinterland.Chatter.ResourceCalls", true);

        if (cfg.idleMaxMs < cfg.idleMinMs)
            cfg.idleMaxMs = cfg.idleMinMs;

        // A zero recent-lines memory would let the same sentence repeat back to
        // back, which is the single most obvious tell that chat is generated.
        if (!cfg.recentMemory)
            cfg.recentMemory = 1;

        s_cfg = cfg;
    }

    /* ------------------------------------------------------------------ */
    /* Per match state                                                    */
    /* ------------------------------------------------------------------ */

    // Fractions of a team's starting resources at which the other side is told
    // to keep pushing and that side is told to defend. Announced once each.
    constexpr float RESOURCE_MILESTONES[] = { 0.5f, 0.25f, 0.1f };

    constexpr uint32 SWEEP_INTERVAL_MS = 2 * IN_MILLISECONDS;

    struct TeamChatter
    {
        uint32 lastLineMs = 0;
        uint32 windowStartMs = 0;
        uint32 linesInWindow = 0;
        uint32 lastIdleMs = 0;
        uint32 idleIntervalMs = 0;
        // Starting resource pool, captured on the first sweep that sees a
        // score. HLBG sizes the two pools differently and does not fill them
        // until the match actually starts, so this cannot be read at doors-open.
        uint32 baseline = 0;
        // How many resource thresholds this team has already called out: pushCall
        // about the enemy's pool, defendCall about its own. Two counters, both
        // owned by the speaking team, because one crossing has to be able to
        // produce both a "keep pushing" on one side and a "get back" on the
        // other - a single shared counter let whichever team swept first eat the
        // milestone and silence the other.
        uint8 pushCall = 0;
        uint8 defendCall = 0;
        std::deque<std::string> recent;
    };

    struct MatchChatter
    {
        TeamChatter team[PVP_TEAMS_COUNT];
        std::unordered_map<uint64, uint32> botLastSpokeMs;
        uint32 lastSweepMs = 0;
    };

    std::mutex s_mutex;
    std::unordered_map<uint32, MatchChatter> s_matches;

    bool IsHinterlandBG(Battleground const* bg)
    {
        return bg && bg->GetBgTypeID(true) == BattlegroundTypeId(DC_BATTLEGROUND_HLBG_TYPE_ID);
    }

    bool IsBot(Player* player) { return player && GET_PLAYERBOT_AI(player) != nullptr; }

    /* ------------------------------------------------------------------ */
    /* Line building                                                      */
    /* ------------------------------------------------------------------ */

    using Substitutions = std::vector<std::pair<char const*, std::string>>;

    std::string BuildLine(LineId line, Substitutions const& subs)
    {
        LinePool const& pool = POOLS[line];
        std::string text = pool.lines[urand(0, pool.count - 1)];

        for (auto const& sub : subs)
        {
            char const* token = sub.first;
            std::string const& value = sub.second;

            std::size_t pos = 0;
            while ((pos = text.find(token, pos)) != std::string::npos)
            {
                text.replace(pos, std::strlen(token), value);
                pos += value.length();
            }
        }

        return text;
    }

    // True when somebody on this team would actually read the line. An all-bot
    // team talking to itself is pure overhead, and the packets would go nowhere.
    bool HasRealAudience(Battleground* bg, TeamId teamId)
    {
        for (auto const& itr : bg->GetPlayers())
        {
            Player* player = itr.second;
            if (player && player->GetBgTeamId() == teamId && !IsBot(player))
                return true;
        }

        return false;
    }

    // Somewhere worth naming before the gates open. The prep and start lines
    // talk about where a bot is going, and the nearest landmark at that point is
    // always its own staging area - every bot would announce a plan to stand
    // still.
    char const* RandomObjectiveName(TeamId teamId)
    {
        DCHinterland::SideView const side = DCHinterland::GetSideView(teamId);

        switch (urand(0, 3))
        {
            case 0:
                return DCHinterland::MID_CENTER.name;
            case 1:
                return DCHinterland::MID_NORTH.name;
            case 2:
                return side.enemyCamp.name;
            default:
                return side.ownCamp.name;
        }
    }

    // Random bot on a team, optionally restricted to the living. Reservoir
    // sampled so the roster is walked once - GetPlayers() is a map, so there is
    // no cheap indexed pick.
    Player* PickBot(Battleground* bg, TeamId teamId, bool aliveOnly)
    {
        Player* chosen = nullptr;
        uint32 seen = 0;

        for (auto const& itr : bg->GetPlayers())
        {
            Player* player = itr.second;
            if (!player || player->GetBgTeamId() != teamId || !IsBot(player))
                continue;

            if (aliveOnly && !player->IsAlive())
                continue;

            if (urand(0, seen++) == 0)
                chosen = player;
        }

        return chosen;
    }

    /* ------------------------------------------------------------------ */
    /* Throttling                                                         */
    /* ------------------------------------------------------------------ */

    // Stamps every throttle and returns false when any of them refuses. `force`
    // is for the handful of lines that are tied to a moment rather than to a
    // rhythm - the start of the match and its result - which would otherwise be
    // swallowed by a gap that happened to still be running. Even those still
    // respect the per-bot cooldown and the repeat memory.
    bool Reserve(MatchChatter& match, TeamId teamId, uint64 speakerGuid, uint32 now, std::string const& text,
                 bool force)
    {
        TeamChatter& tc = match.team[teamId];

        if (!force)
        {
            if (tc.lastLineMs && (now - tc.lastLineMs) < s_cfg.teamGapMs)
                return false;

            if ((now - tc.windowStartMs) >= MINUTE * IN_MILLISECONDS)
            {
                tc.windowStartMs = now;
                tc.linesInWindow = 0;
            }

            if (tc.linesInWindow >= s_cfg.linesPerMinute)
                return false;
        }

        auto itr = match.botLastSpokeMs.find(speakerGuid);
        if (itr != match.botLastSpokeMs.end() && (now - itr->second) < s_cfg.botCooldownMs)
            return false;

        if (std::find(tc.recent.begin(), tc.recent.end(), text) != tc.recent.end())
            return false;

        tc.lastLineMs = now;
        ++tc.linesInWindow;
        match.botLastSpokeMs[speakerGuid] = now;

        tc.recent.push_back(text);
        while (tc.recent.size() > s_cfg.recentMemory)
            tc.recent.pop_front();

        return true;
    }

    /* ------------------------------------------------------------------ */
    /* Delivery                                                           */
    /* ------------------------------------------------------------------ */

    void SendBattlegroundChat(Battleground* bg, Player* speaker, std::string const& text)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_BATTLEGROUND, text, LANG_UNIVERSAL, CHAT_TAG_NONE,
                                     speaker->GetGUID(), speaker->GetName());

        TeamId const teamId = speaker->GetBgTeamId();

        for (auto const& itr : bg->GetPlayers())
        {
            Player* player = itr.second;
            if (!player || player->GetBgTeamId() != teamId || IsBot(player))
                continue;

            if (WorldSession* session = player->GetSession())
                session->SendPacket(&data);
        }
    }

    // The one call site for everything a bot says. Returns true when the line
    // actually went out; `spoken` receives the text that did, for the callers
    // that also want to repeat it in local /say.
    bool Speak(Battleground* bg, Player* speaker, LineId line, Substitutions const& subs, bool force = false,
               std::string* spoken = nullptr)
    {
        if (!s_cfg.enabled || !bg || !speaker)
            return false;

        TeamId const teamId = speaker->GetBgTeamId();
        if (teamId != TEAM_ALLIANCE && teamId != TEAM_HORDE)
            return false;

        if (!HasRealAudience(bg, teamId))
            return false;

        std::string const text = BuildLine(line, subs);

        {
            std::lock_guard<std::mutex> guard(s_mutex);
            MatchChatter& match = s_matches[bg->GetInstanceID()];
            if (!Reserve(match, teamId, speaker->GetGUID().GetRawValue(), getMSTime(), text, force))
                return false;
        }

        SendBattlegroundChat(bg, speaker, text);

        if (spoken)
            *spoken = text;

        return true;
    }

    /* ------------------------------------------------------------------ */
    /* Hooks                                                              */
    /* ------------------------------------------------------------------ */

    // The battleground's own chat is team-only, so an enemy standing over the
    // corpse sees nothing. A share of kill lines also go out in local /say -
    // in the bot's racial language, so the other side gets the same garbled
    // text a real player's taunt would produce - plus a one-shot emote.
    void TauntLocally(Player* bot, std::string const& text)
    {
        if (urand(1, 100) <= s_cfg.sayChance)
            bot->Say(text, bot->GetTeamId() == TEAM_ALLIANCE ? LANG_COMMON : LANG_ORCISH);

        if (urand(1, 100) <= s_cfg.emoteChance)
        {
            constexpr uint32 emotes[] = { EMOTE_ONESHOT_RUDE, EMOTE_ONESHOT_LAUGH, EMOTE_ONESHOT_POINT,
                                          EMOTE_ONESHOT_ROAR, EMOTE_ONESHOT_FLEX };
            bot->HandleEmoteCommand(emotes[urand(0, uint32(std::size(emotes)) - 1)]);
        }
    }

    class DCHinterlandChatterWorldScript : public WorldScript
    {
    public:
        DCHinterlandChatterWorldScript()
            : WorldScript("DCHinterlandChatterWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD })
        {
        }

        void OnAfterConfigLoad(bool /*reload*/) override { LoadConfig(); }
    };

    class DCHinterlandChatterPlayerScript : public PlayerScript
    {
    public:
        DCHinterlandChatterPlayerScript()
            : PlayerScript("DCHinterlandChatterPlayerScript", { PLAYERHOOK_ON_PVP_KILL })
        {
        }

        void OnPlayerPVPKill(Player* killer, Player* killed) override
        {
            if (!s_cfg.enabled || !killer || !killed || killer == killed)
                return;

            Battleground* bg = killer->GetBattleground();
            if (!IsHinterlandBG(bg) || bg != killed->GetBattleground())
                return;

            // The killer talks about where the kill happened, the victim about
            // where it died - the same spot, but they are two different calls
            // to two different teams and either can be throttled away alone.
            if (IsBot(killer) && urand(1, 100) <= s_cfg.killChance)
            {
                std::string const place =
                    DCHinterland::NearestLandmarkName(killed->GetPositionX(), killed->GetPositionY());

                std::string spoken;
                if (Speak(bg, killer, LINE_KILL, { { "%target", killed->GetName() }, { "%place", place } }, false,
                          &spoken))
                {
                    TauntLocally(killer, spoken);
                }
            }

            if (IsBot(killed) && urand(1, 100) <= s_cfg.deathChance)
            {
                std::string const place = DCHinterland::NearestLandmarkName(killed->GetPositionX(),
                                                                           killed->GetPositionY());
                Speak(bg, killed, LINE_DEATH, { { "%killer", killer->GetName() }, { "%place", place } });
            }
        }
    };

    class DCHinterlandChatterBGScript : public AllBattlegroundScript
    {
    public:
        DCHinterlandChatterBGScript()
            : AllBattlegroundScript("DCHinterlandChatterBGScript",
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

            // Push the first idle line out past the opening rush, otherwise a
            // bot chats while everyone is still running out of the gate.
            {
                std::lock_guard<std::mutex> guard(s_mutex);
                MatchChatter& match = s_matches[bg->GetInstanceID()];
                for (TeamChatter& tc : match.team)
                {
                    tc.lastIdleMs = getMSTime();
                    tc.idleIntervalMs = urand(s_cfg.idleMinMs, s_cfg.idleMaxMs);
                }
            }

            ForEachTeam([&](TeamId teamId)
            {
                if (Player* bot = PickBot(bg, teamId, true))
                    Speak(bg, bot, LINE_START, { { "%place", RandomObjectiveName(teamId) } }, true);
            });
        }

        void OnBattlegroundUpdate(Battleground* bg, uint32 /*diff*/) override
        {
            if (!s_cfg.enabled || !IsHinterlandBG(bg))
                return;

            uint32 const now = getMSTime();

            {
                std::lock_guard<std::mutex> guard(s_mutex);
                MatchChatter& match = s_matches[bg->GetInstanceID()];
                if (match.lastSweepMs && (now - match.lastSweepMs) < SWEEP_INTERVAL_MS)
                    return;

                match.lastSweepMs = now;
            }

            BattlegroundStatus const status = bg->GetStatus();
            if (status != STATUS_WAIT_JOIN && status != STATUS_IN_PROGRESS)
                return;

            ForEachTeam([&](TeamId teamId) { SweepTeam(bg, teamId, status, now); });
        }

        void OnBattlegroundEnd(Battleground* bg, TeamId winnerTeamId) override
        {
            if (!s_cfg.enabled || !IsHinterlandBG(bg))
                return;

            ForEachTeam([&](TeamId teamId)
            {
                // Dead bots still talk after the horn, so the living-only
                // filter is dropped here on purpose.
                if (Player* bot = PickBot(bg, teamId, false))
                    Speak(bg, bot, teamId == winnerTeamId ? LINE_WIN : LINE_LOSE, {}, true);
            });
        }

        void OnBattlegroundDestroy(Battleground* bg) override
        {
            if (!bg)
                return;

            std::lock_guard<std::mutex> guard(s_mutex);
            s_matches.erase(bg->GetInstanceID());
        }

    private:
        template <typename Fn>
        static void ForEachTeam(Fn&& fn)
        {
            fn(TEAM_ALLIANCE);
            fn(TEAM_HORDE);
        }

        // At most one line per team per sweep, in priority order: the score
        // swinging is worth more than somebody being low, which is worth more
        // than filler.
        void SweepTeam(Battleground* bg, TeamId teamId, BattlegroundStatus status, uint32 now)
        {
            if (!HasRealAudience(bg, teamId))
                return;

            if (status == STATUS_IN_PROGRESS && s_cfg.resourceCalls && ResourceCall(bg, teamId))
                return;

            if (status == STATUS_IN_PROGRESS && HelpCall(bg, teamId))
                return;

            IdleCall(bg, teamId, status, now);
        }

        // HLBG drains a resource pool rather than capturing anything, so the
        // score is the only running commentary the match offers. Milestones are
        // measured against what the pool held when the doors opened - the two
        // sides do not start equal (HinterlandBG.Resources.*), so a fixed
        // number would fire at different points for Alliance and Horde.
        //
        // A crossing is only marked as called once the line survived the
        // throttles; a milestone that got squeezed out by a gap is retried on
        // the next sweep rather than lost.
        bool ResourceCall(Battleground* bg, TeamId teamId)
        {
            TeamId const enemyTeamId = teamId == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
            uint32 const enemyScore = bg->GetTeamScore(enemyTeamId);
            uint32 const ownScore = bg->GetTeamScore(teamId);

            uint8 pushStep = 0;
            uint8 defendStep = 0;

            {
                std::lock_guard<std::mutex> guard(s_mutex);
                MatchChatter& match = s_matches[bg->GetInstanceID()];
                TeamChatter& own = match.team[teamId];
                TeamChatter& enemy = match.team[enemyTeamId];

                if (!own.baseline)
                    own.baseline = ownScore;
                if (!enemy.baseline)
                    enemy.baseline = enemyScore;

                uint8 const enemyReached = MilestoneStep(enemy.baseline, enemyScore);
                uint8 const ownReached = MilestoneStep(own.baseline, ownScore);

                if (enemyReached > own.pushCall)
                    pushStep = enemyReached;
                else if (ownReached > own.defendCall)
                    defendStep = ownReached;
            }

            if (!pushStep && !defendStep)
                return false;

            Player* bot = PickBot(bg, teamId, true);
            if (!bot)
                return false;

            DCHinterland::SideView const side = DCHinterland::GetSideView(teamId);

            bool const said =
                pushStep ? Speak(bg, bot, LINE_ENEMY_LOW,
                                 { { "%res", std::to_string(enemyScore) }, { "%place", side.enemyCamp.name } })
                         : Speak(bg, bot, LINE_TEAM_LOW,
                                 { { "%res", std::to_string(ownScore) }, { "%place", side.ownCamp.name } });

            if (said)
            {
                std::lock_guard<std::mutex> guard(s_mutex);
                TeamChatter& own = s_matches[bg->GetInstanceID()].team[teamId];
                if (pushStep)
                    own.pushCall = pushStep;
                else
                    own.defendCall = defendStep;
            }

            return said;
        }

        // How many thresholds a pool has dropped through. A pool can fall past
        // two between sweeps - a faction boss is worth 200 resources against the
        // 5 an ordinary guard gives - so this counts rather than steps.
        static uint8 MilestoneStep(uint32 baseline, uint32 score)
        {
            if (!baseline)
                return 0;

            float const fraction = float(score) / float(baseline);

            uint8 step = 0;
            for (float const threshold : RESOURCE_MILESTONES)
            {
                if (fraction <= threshold)
                    ++step;
            }

            return step;
        }

        // One wounded bot per sweep, picked at random among those actually
        // taking damage, so a fight produces a call for help rather than a
        // chorus of them.
        bool HelpCall(Battleground* bg, TeamId teamId)
        {
            if (urand(1, 100) > s_cfg.helpChance)
                return false;

            Player* wounded = nullptr;
            uint32 seen = 0;

            for (auto const& itr : bg->GetPlayers())
            {
                Player* player = itr.second;
                if (!player || player->GetBgTeamId() != teamId || !IsBot(player))
                    continue;

                if (!player->IsAlive() || !player->IsInCombat())
                    continue;

                if (player->GetHealthPct() > float(s_cfg.helpHealthPct))
                    continue;

                if (urand(0, seen++) == 0)
                    wounded = player;
            }

            if (!wounded)
                return false;

            std::string const place =
                DCHinterland::NearestLandmarkName(wounded->GetPositionX(), wounded->GetPositionY());

            return Speak(bg, wounded, LINE_HELP, { { "%place", place } });
        }

        bool IdleCall(Battleground* bg, TeamId teamId, BattlegroundStatus status, uint32 now)
        {
            {
                std::lock_guard<std::mutex> guard(s_mutex);
                MatchChatter& match = s_matches[bg->GetInstanceID()];
                TeamChatter& tc = match.team[teamId];

                if (!tc.idleIntervalMs)
                {
                    tc.lastIdleMs = now;
                    // The gates stay shut for a short countdown, so the prep
                    // phase gets its own much shorter rhythm - on the match
                    // interval nobody would ever say anything before the start.
                    tc.idleIntervalMs = status == STATUS_WAIT_JOIN ? urand(8 * IN_MILLISECONDS, 20 * IN_MILLISECONDS)
                                                                   : urand(s_cfg.idleMinMs, s_cfg.idleMaxMs);
                    return false;
                }

                if ((now - tc.lastIdleMs) < tc.idleIntervalMs)
                    return false;

                tc.lastIdleMs = now;
                tc.idleIntervalMs = status == STATUS_WAIT_JOIN ? urand(8 * IN_MILLISECONDS, 20 * IN_MILLISECONDS)
                                                               : urand(s_cfg.idleMinMs, s_cfg.idleMaxMs);
            }

            Player* bot = PickBot(bg, teamId, status == STATUS_IN_PROGRESS);
            if (!bot)
                return false;

            if (status == STATUS_WAIT_JOIN)
                return Speak(bg, bot, LINE_PREP, { { "%place", RandomObjectiveName(teamId) } });

            std::string const place = DCHinterland::NearestLandmarkName(bot->GetPositionX(), bot->GetPositionY());

            return Speak(bg, bot, LINE_IDLE, { { "%place", place } });
        }
    };
}

void AddSC_dc_hinterland_chatter()
{
    LoadConfig();

    new DCHinterlandChatterWorldScript();
    new DCHinterlandChatterPlayerScript();
    new DCHinterlandChatterBGScript();
}
