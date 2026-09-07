/*
 * Dark Chaos - random challenge modes for playerbots.
 *
 * The shrine gameobject lets a player take on a challenge mode (Hardcore, Slow
 * XP, Item Quality Restricted, ...). This file gives a share of the random bot
 * population the same thing, so the bots you meet in the world carry the same
 * challenge auras real players do.
 *
 * Taking a mode is NOT mandatory and NOT universal:
 * AiPlayerbot.DCChallengeModes.Chance decides what fraction of bots ever get
 * one. The roll is derived from the bot's character GUID (StableRoll), so a
 * bot's fate is fixed for its whole life - a bot that rolls "no challenge"
 * never re-rolls on a later login, and a bot that rolls a mode gets exactly
 * that mode, once. There is no per-bot table and nothing to invalidate.
 *
 * The challenge system itself lives in scripts.lib
 * (src/server/scripts/DC/Progression/ChallengeMode), which modules.lib does not
 * link. It exposes the three functions mirrored below; both libs land in
 * worldserver, so the symbols resolve at the final link. Going through that API
 * rather than writing player settings here means bots get the auras, the Iron
 * Man restrictions and the tracking rows on exactly the same code path players
 * do.
 *
 * Two guards matter:
 *   - Only *random* bots are eligible. `.bot add <alt>` drives a real player
 *     character; branding it with a permanent, un-removable challenge mode
 *     would be a disaster.
 *   - The default mode list excludes the destructive ones. See the conf block
 *     for why each is off.
 *
 * Hardcore needs one extra piece to work at all on a bot. The challenge system
 * enforces its permadeath lockout by kicking the session at login, and that is a
 * no-op here: a bot session has no socket and is not owned by WorldSessionMgr,
 * so KickPlayer() only sets a flag nothing reads. Without help, a hardcore bot
 * that died would be flagged deceased, announce its own death, and then keep
 * playing forever. RetireDeceasedBot() below closes that gap by taking the
 * character out of the random bot rotation instead.
 */

#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"

#include "Config.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

// ----------------------------------------------------------------------------
// Mirror of the challenge mode cross-library API. Declared rather than included:
// scripts.lib does not export its include directory to modules.lib. Keep in sync
// with src/server/scripts/DC/Progression/ChallengeMode/dc_challenge_modes.h.
// ----------------------------------------------------------------------------
namespace DCChallengeModes
{
    bool HasAnyActiveMode(Player* player);
    void GetEligibleModes(Player* player, std::vector<uint8>& out);
    bool ActivateMode(Player* player, uint8 mode);
}

namespace
{
    // Must match ChallengeModeSettings in dc_challenge_modes.h. Value 8 is
    // HARDCORE_DEAD (a player state, not a selectable mode) and is absent here
    // on purpose.
    struct ModeName
    {
        char const* name;
        uint8       setting;
    };

    constexpr ModeName kModeNames[] =
    {
        { "hardcore",     0 },
        { "semihardcore", 1 },
        { "selfcrafted",  2 },
        { "itemquality",  3 },
        { "slowxp",       4 },
        { "veryslowxp",   5 },
        { "questxponly",  6 },
        { "ironman",      7 },
        { "ironmanplus",  9 }
    };

    // ------------------------------------------------------------------------
    // Config
    // ------------------------------------------------------------------------
    struct ChallengeConfig
    {
        bool               enabled          = true;
        uint32             chance           = 15;
        uint32             secondModeChance = 0;
        uint32             minLevel         = 10;
        bool               retireDeceased   = true;
        std::vector<uint8> allowed;
    };

    ChallengeConfig s_cfg;

    // Separators and cosmetic characters are both tolerated so the key reads
    // naturally whether it is written "SlowXp, VerySlowXp" or "slow_xp|very-slow-xp".
    std::string NormalizeToken(std::string const& raw)
    {
        std::string out;
        out.reserve(raw.size());

        for (char c : raw)
        {
            if (c == ' ' || c == '\t' || c == '_' || c == '-')
                continue;

            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }

        return out;
    }

    void ParseAllowedModes(std::string const& value)
    {
        s_cfg.allowed.clear();

        size_t start = 0;
        while (start <= value.size())
        {
            size_t end = value.find_first_of(",;|", start);
            if (end == std::string::npos)
                end = value.size();

            std::string token = NormalizeToken(value.substr(start, end - start));
            start = end + 1;

            if (token.empty())
                continue;

            auto it = std::find_if(std::begin(kModeNames), std::end(kModeNames),
                                   [&token](ModeName const& m) { return token == m.name; });

            if (it == std::end(kModeNames))
            {
                LOG_WARN("playerbots",
                         "DCChallengeModes: unknown mode '{}' in AiPlayerbot.DCChallengeModes.Modes - ignored", token);
                continue;
            }

            if (std::find(s_cfg.allowed.begin(), s_cfg.allowed.end(), it->setting) == s_cfg.allowed.end())
                s_cfg.allowed.push_back(it->setting);
        }

        // A stable order keeps the GUID-derived pick reproducible no matter how
        // the admin wrote the list.
        std::sort(s_cfg.allowed.begin(), s_cfg.allowed.end());
    }

    void LoadConfig()
    {
        s_cfg.enabled          = sConfigMgr->GetOption<bool>("AiPlayerbot.DCChallengeModes.Enable", true);
        s_cfg.chance           = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCChallengeModes.Chance", 15);
        s_cfg.secondModeChance = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCChallengeModes.SecondModeChance", 0);
        s_cfg.minLevel         = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCChallengeModes.MinLevel", 10);
        s_cfg.retireDeceased   = sConfigMgr->GetOption<bool>("AiPlayerbot.DCChallengeModes.RetireDeceased", true);

        s_cfg.chance           = std::min<uint32>(s_cfg.chance, 100);
        s_cfg.secondModeChance = std::min<uint32>(s_cfg.secondModeChance, 100);

        ParseAllowedModes(sConfigMgr->GetOption<std::string>("AiPlayerbot.DCChallengeModes.Modes",
                                                             "ItemQuality,SlowXp,VerySlowXp,QuestXpOnly"));

        if (s_cfg.enabled && s_cfg.allowed.empty())
            LOG_WARN("playerbots", "DCChallengeModes: no usable modes configured - bots stay unchallenged");
        else if (s_cfg.enabled)
            LOG_INFO("playerbots", "DCChallengeModes: {}% of random bots roll one of {} challenge modes",
                     s_cfg.chance, s_cfg.allowed.size());
    }

    // ------------------------------------------------------------------------
    // Selection
    // ------------------------------------------------------------------------

    // Same splitmix64 finalizer DCBotCosmetics uses: bots are created in
    // contiguous GUID blocks, and this spreads them across the output range.
    uint32 StableRoll(uint32 guid, uint32 salt)
    {
        uint64 h = uint64(guid) * 0x9E3779B97F4A7C15ull;
        h ^= uint64(salt) * 0xBF58476D1CE4E5B9ull;
        h ^= h >> 30;
        h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 27;
        h *= 0x94D049BB133111EBull;
        h ^= h >> 31;
        return uint32(h & 0xFFFFFFFFull);
    }

    constexpr uint32 CHALLENGE_SALT = 0x43484C47u; // "CHLG"

    // ChallengeModeSettings::HARDCORE_DEAD - the challenge system's "this
    // character is finished" flag, persisted in character_settings. It is a
    // player state rather than a selectable mode, which is why it is not in
    // kModeNames above.
    constexpr uint32 SETTING_HARDCORE_DEAD = 8;

    // How long a retired character stays out of the rotation. A lease, not a
    // tombstone: if it ever lapses the bot logs in once, trips the check below
    // and is retired again. Ten years is "never" for a game server, and stays
    // well inside the signed int the module's event table stores it in.
    constexpr uint32 RETIRE_SECONDS = 10u * 365u * 24u * 60u * 60u;

    // `.bot add <alt>` puts a real player's character under bot control. A
    // challenge mode cannot be switched off again and retirement would pull a
    // real character out of someone's roster, so only the throwaway random bot
    // population is fair game for either.
    bool IsRandomBotPlayer(Player* player)
    {
        return player && sPlayerbotsMgr.GetPlayerbotAI(player) && sRandomPlayerbotMgr.IsRandomBot(player);
    }

    // Returns true when the bot was retired and the caller should do nothing
    // else with it. No level gate here - a hardcore bot that died at level 3 is
    // just as finished as one that died at 80.
    bool RetireDeceasedBot(Player* bot)
    {
        if (!s_cfg.retireDeceased || !IsRandomBotPlayer(bot))
            return false;

        if (bot->GetPlayerSetting("mod-challenge-modes", SETTING_HARDCORE_DEAD).value != 1)
            return false;

        // Deliberately no LogoutPlayerBot() here. This runs inside the login
        // hook, and tearing the player down mid-login would leave
        // HandlePlayerBotLoginCallback holding a dangling session. RetireBot
        // only sets the events; ProcessBot() performs the actual logout on its
        // next tick, outside the login path.
        sRandomPlayerbotMgr.RetireBot(bot->GetGUID(), RETIRE_SECONDS);

        LOG_INFO("playerbots", "DCChallengeModes: retiring deceased hardcore bot {} (guid {})",
                 bot->GetName(), bot->GetGUID().GetCounter());

        return true;
    }

    bool IsEligibleBot(Player* player)
    {
        return IsRandomBotPlayer(player) && player->GetLevel() >= s_cfg.minLevel;
    }

    void RollChallengeMode(Player* bot)
    {
        if (!s_cfg.enabled || s_cfg.allowed.empty() || !s_cfg.chance)
            return;

        if (!IsEligibleBot(bot))
            return;

        // Cheap in-memory check, and it also makes this whole hook idempotent:
        // once a bot has a mode it is skipped on every later login.
        if (DCChallengeModes::HasAnyActiveMode(bot))
            return;

        uint32 guid = bot->GetGUID().GetCounter();
        if (StableRoll(guid, CHALLENGE_SALT) % 100 >= s_cfg.chance)
            return;

        // Whether a mode is offered at all is the challenge system's call:
        // config switches, level gates and the "only one mode at a time" rule
        // all live there. Intersect its answer with the admin's allowlist.
        std::vector<uint8> eligible;
        DCChallengeModes::GetEligibleModes(bot, eligible);

        std::vector<uint8> candidates;
        for (uint8 mode : eligible)
        {
            if (std::find(s_cfg.allowed.begin(), s_cfg.allowed.end(), mode) != s_cfg.allowed.end())
                candidates.push_back(mode);
        }

        if (candidates.empty())
            return;

        uint8 picked = candidates[StableRoll(guid, CHALLENGE_SALT + 1) % candidates.size()];
        if (!DCChallengeModes::ActivateMode(bot, picked))
            return;

        LOG_DEBUG("playerbots", "DCChallengeModes: bot {} (guid {}) took challenge mode {}",
                  bot->GetName(), guid, uint32(picked));

        if (!s_cfg.secondModeChance || candidates.size() < 2)
            return;

        if (StableRoll(guid, CHALLENGE_SALT + 2) % 100 >= s_cfg.secondModeChance)
            return;

        // GetEligibleModes refuses once a mode is active - by design the shrine
        // hands out exactly one - so the second pick comes from the list rolled
        // above, minus the mode already taken.
        candidates.erase(std::remove(candidates.begin(), candidates.end(), picked), candidates.end());

        uint8 second = candidates[StableRoll(guid, CHALLENGE_SALT + 3) % candidates.size()];
        if (DCChallengeModes::ActivateMode(bot, second))
            LOG_DEBUG("playerbots", "DCChallengeModes: bot {} (guid {}) also took challenge mode {}",
                      bot->GetName(), guid, uint32(second));
    }

    // ------------------------------------------------------------------------
    // Scripts
    // ------------------------------------------------------------------------
    class DCBotChallengeModesWorldScript : public WorldScript
    {
    public:
        DCBotChallengeModesWorldScript()
            : WorldScript("DCBotChallengeModesWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) {}

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            LoadConfig();
        }
    };

    class DCBotChallengeModesPlayerScript : public PlayerScript
    {
    public:
        DCBotChallengeModesPlayerScript()
            : PlayerScript("DCBotChallengeModesPlayerScript",
                           { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LEVEL_CHANGED,
                             PLAYERHOOK_ON_PLAYER_JUST_DIED }) {}

        void OnPlayerLogin(Player* player) override
        {
            if (RetireDeceasedBot(player))
                return;

            RollChallengeMode(player);
        }

        void OnPlayerJustDied(Player* player) override
        {
            // Whether the challenge system has already run its own hardcore
            // handler for this death depends on script registration order, so
            // this only reacts to the flag. If it is not set yet, the login
            // path above catches the bot next time it comes online.
            RetireDeceasedBot(player);
        }

        void OnPlayerLevelChanged(Player* player, uint8 /*oldlevel*/) override
        {
            // A bot below MinLevel at login becomes eligible as it levels. The
            // roll is GUID-derived, so this can never flip a bot that already
            // rolled "no challenge" into taking one.
            RollChallengeMode(player);
        }
    };
}

void AddSC_dc_bot_challenge_modes()
{
    new DCBotChallengeModesWorldScript();
    new DCBotChallengeModesPlayerScript();
}
