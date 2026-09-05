/*
 * Dark Chaos - playerbot backfill for the DC Group Finder matchmaking queue.
 *
 * The queue lives in scripts.lib (src/server/scripts/DC/AddonExtension) and this
 * module lives in modules.lib. Neither target links the other, so the queue
 * cannot call sPlayerbotsMgr directly. It instead exposes a small provider
 * struct of function pointers that this file fills in at startup; both libs are
 * linked into worldserver, so the symbol resolves at the final link and no
 * header is shared beyond the two declarations below.
 *
 * Bots are backfill only. They never enter the queue on their own -- the queue
 * calls Recruit() when a group of real players is short a role and has waited
 * out DC.GroupFinder.Queue.Bots.BackfillAfterSec.
 */

#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"

#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "ScriptMgr.h"

#include <algorithm>
#include <vector>

// ----------------------------------------------------------------------------
// Mirror of DCAddon::Matchmaking's provider hook. Declared rather than included:
// scripts.lib does not export its include directory to modules.lib. Keep in
// sync with src/server/scripts/DC/AddonExtension/dc_addon_matchmaking.h.
// ----------------------------------------------------------------------------
namespace DCAddon
{
namespace Matchmaking
{
    struct BotProvider
    {
        bool (*IsBot)(Player* player) = nullptr;
        uint32 (*Recruit)(uint8 roleMask, uint8 minLevel, uint8 maxLevel,
                          uint32 count, std::vector<Player*>& out) = nullptr;
    };

    void RegisterBotProvider(BotProvider const& provider);
}
}

namespace
{
    // Must match DCAddon::Matchmaking::QueueRoleFlag.
    enum DCQueueRole : uint8
    {
        DC_ROLE_TANK   = 1,
        DC_ROLE_HEALER = 2,
        DC_ROLE_DPS    = 4
    };

    bool DCIsBot(Player* player)
    {
        return player && sPlayerbotsMgr.GetPlayerbotAI(player) != nullptr;
    }

    // A bot is recruitable when nothing else has a claim on it: it is alive, out
    // of combat, ungrouped, and standing in the open world rather than already
    // inside an instance or battleground.
    bool IsIdleForBackfill(Player* bot)
    {
        if (!bot || !bot->IsInWorld() || !bot->IsAlive())
            return false;
        if (bot->IsInCombat() || bot->GetGroup())
            return false;
        if (bot->IsBeingTeleported())
            return false;
        if (bot->InBattleground() || bot->InArena() || bot->InBattlegroundQueue())
            return false;

        Map* map = bot->FindMap();
        if (map && (map->IsDungeon() || map->IsRaid() || map->IsBattlegroundOrArena()))
            return false;

        return true;
    }

    bool CanFillRole(Player* bot, uint8 roleMask)
    {
        if (roleMask & DC_ROLE_TANK)
            return PlayerbotAI::IsTank(bot);
        if (roleMask & DC_ROLE_HEALER)
            return PlayerbotAI::IsHeal(bot);
        return PlayerbotAI::IsDps(bot);
    }

    uint32 DCRecruit(uint8 roleMask, uint8 minLevel, uint8 maxLevel, uint32 count,
                     std::vector<Player*>& out)
    {
        if (count == 0)
            return 0;

        // Only random bots: a player's own alt-bots belong to that player and
        // must not be dragged into a stranger's dungeon group.
        std::vector<Player*> candidates;
        for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
        {
            if (!bot || !sRandomPlayerbotMgr.IsRandomBot(bot))
                continue;
            if (bot->GetLevel() < minLevel)
                continue;
            if (maxLevel != 0 && bot->GetLevel() > maxLevel)
                continue;
            if (!IsIdleForBackfill(bot) || !CanFillRole(bot, roleMask))
                continue;

            candidates.push_back(bot);
        }

        if (candidates.empty())
            return 0;

        // Prefer the bots closest to the bottom of the requested band, so a
        // levelling group is not handed the highest bot on the realm.
        std::sort(candidates.begin(), candidates.end(),
            [minLevel](Player const* a, Player const* b)
            {
                uint32 da = a->GetLevel() > minLevel ? a->GetLevel() - minLevel : 0u;
                uint32 db = b->GetLevel() > minLevel ? b->GetLevel() - minLevel : 0u;
                if (da != db)
                    return da < db;
                return a->GetGUID() < b->GetGUID();
            });

        uint32 added = 0;
        for (Player* bot : candidates)
        {
            if (added >= count)
                break;
            out.push_back(bot);
            ++added;
        }

        return added;
    }

    class DCGroupFinderBotProviderScript : public WorldScript
    {
    public:
        // The hook list is mandatory -- an empty one registers the script but
        // never dispatches it (see ScriptMgrMacros.h CALL_ENABLED_HOOKS).
        DCGroupFinderBotProviderScript()
            : WorldScript("DCGroupFinderBotProviderScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) {}

        // Registered after config load so the queue's own LoadConfig has already
        // read DC.GroupFinder.Queue.Bots.* by the time any match tick runs.
        void OnAfterConfigLoad(bool reload) override
        {
            if (reload)
                return;

            DCAddon::Matchmaking::BotProvider provider;
            provider.IsBot = &DCIsBot;
            provider.Recruit = &DCRecruit;
            DCAddon::Matchmaking::RegisterBotProvider(provider);

            LOG_INFO("dc.groupfinder",
                "Playerbots registered as the Group Finder queue bot backfill provider");
        }
    };
}

void AddSC_dc_groupfinder_bots()
{
    new DCGroupFinderBotProviderScript();
}
