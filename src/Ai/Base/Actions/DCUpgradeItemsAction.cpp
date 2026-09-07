/*
 * DarkChaos addition to mod-playerbots. See DCUpgradeItemsAction.h.
 */

#include "DCUpgradeItemsAction.h"

#include "Config.h"
#include "DCItemUpgradeApi.h"
#include "Event.h"
#include "Player.h"
#include "Playerbots.h"
#include "ScriptMgr.h"

#include <algorithm>
#include <vector>

namespace
{
    namespace Api = DarkChaos::ItemUpgradeApi;

    // Bot-side policy. Kept out of PlayerbotAIConfig so the DC options stay out
    // of an upstream file, but cached rather than read per call: isUseful() runs
    // for every bot on every AI tick, and a string-keyed config lookup per knob
    // per tick across a full roster is not free. DCUpgradeItemsWorldScript
    // refreshes this below, so ".reload config" still takes without a restart.
    // The initialisers are the defaults, so the cache is already correct if
    // anything manages to run before the first config load.
    struct BotUpgradeConfig
    {
        bool   enabled         = true;
        uint32 maxStepsPerRun  = 3;
        uint32 minLevel        = 10;
        // Percentage of each tier's max upgrade level a bot will climb to. Lets
        // an admin keep bots a step behind players without switching the system
        // off. Clamped to 100 on load.
        uint32 maxLevelPercent = 100;
        // Seconds a bot must wait between runs, on top of the trigger's own rarity.
        uint32 cooldownSeconds = 3600;
        uint32 currencyReserve = 0;
    };

    BotUpgradeConfig s_cfg;

    void LoadConfig()
    {
        s_cfg.enabled         = sConfigMgr->GetOption<bool>("AiPlayerbot.DCItemUpgrade.Enable", true);
        s_cfg.maxStepsPerRun  = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCItemUpgrade.StepsPerRun", 3);
        s_cfg.minLevel        = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCItemUpgrade.MinLevel", 10);
        s_cfg.cooldownSeconds = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCItemUpgrade.CooldownSeconds", 3600);
        s_cfg.currencyReserve = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCItemUpgrade.CurrencyReserve", 0);

        s_cfg.maxLevelPercent =
            std::min<uint32>(sConfigMgr->GetOption<uint32>("AiPlayerbot.DCItemUpgrade.MaxLevelPercent", 100), 100);
    }

    uint8 EffectiveMaxLevel(uint8 tierMaxLevel)
    {
        uint32 const percent = s_cfg.maxLevelPercent;
        if (!tierMaxLevel || percent >= 100)
            return tierMaxLevel;

        uint32 const capped = (uint32(tierMaxLevel) * percent) / 100;
        return static_cast<uint8>(std::max<uint32>(capped, 1));
    }

    // What is left of a balance once the configured reserve is set aside.
    uint32 Spendable(Api::Provider* provider, Player* bot, Api::Currency currency)
    {
        uint32 const reserve = s_cfg.currencyReserve;
        uint32 const balance = provider->GetCurrencyAmount(bot, currency);
        return balance > reserve ? balance - reserve : 0;
    }
}

bool DCUpgradeItemsAction::isUseful()
{
    if (!s_cfg.enabled || !Api::GetProvider())
        return false;

    if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat())
        return false;

    if (bot->GetLevel() < s_cfg.minLevel)
        return false;

    // Unsigned arithmetic, so a getMSTime() wrap resolves the same way it does
    // for RandomTrigger::IsActive. _lastRunMs == 0 means "never ran".
    if (_lastRunMs && (getMSTime() - _lastRunMs) < s_cfg.cooldownSeconds * IN_MILLISECONDS)
        return false;

    // Cheap gate: no currency at all means nothing to do this pass. The per-tier
    // affordability check still happens in Execute.
    Api::Provider* provider = Api::GetProvider();
    return provider->GetCurrencyAmount(bot, Api::CURRENCY_UPGRADE_TOKEN) > 0 ||
           provider->GetCurrencyAmount(bot, Api::CURRENCY_ARTIFACT_ESSENCE) > 0 ||
           provider->GetCurrencyAmount(bot, Api::CURRENCY_FRONTIER_SAP) > 0;
}

bool DCUpgradeItemsAction::Execute(Event /*event*/)
{
    Api::Provider* provider = Api::GetProvider();
    if (!provider || !bot)
        return false;

    std::vector<Api::SlotUpgradeInfo> candidates;
    std::vector<uint32> cold;

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Api::SlotUpgradeInfo info;
        if (!provider->DescribeEquippedSlot(bot, slot, info))
            continue;

        // The item's upgrade state is not cached yet. Reading it now would cost a
        // blocking query on the world thread, so ask for an off-thread warm and
        // let a later run of this action pick it up.
        if (!info.stateKnown)
        {
            cold.push_back(info.itemGuid);
            continue;
        }

        info.tierMaxLevel = EffectiveMaxLevel(info.tierMaxLevel);
        if (info.level >= info.tierMaxLevel)
            continue;

        // A zero cost means there is no step to buy -- including tiers priced from
        // a different cost table, which must never be bought through this path.
        if (!info.nextCost)
            continue;

        candidates.push_back(info);
    }

    // Counts as a run either way: a pass that only warmed cold items has still
    // done its work for this cooldown, and the next one reads them from cache.
    _lastRunMs = getMSTime();

    if (!cold.empty())
        provider->WarmItemStates(bot, cold);

    if (candidates.empty())
        return !cold.empty();

    uint32 const maxSteps = s_cfg.maxStepsPerRun;
    uint32 bought = 0;

    // Cheapest step first, so the spend spreads across slots instead of sinking
    // into one piece: within a tier the cost climbs with the level, so the
    // lowest-level slot always wins the next step.
    while (bought < maxSteps && !candidates.empty())
    {
        auto best = std::min_element(candidates.begin(), candidates.end(),
            [](Api::SlotUpgradeInfo const& lhs, Api::SlotUpgradeInfo const& rhs)
            { return lhs.nextCost < rhs.nextCost; });

        if (Spendable(provider, bot, best->currency) < best->nextCost)
        {
            // `best` is the global cheapest, so it is also the cheapest in its own
            // currency -- nothing else paid in that currency is affordable either.
            Api::Currency const broke = best->currency;
            candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                [broke](Api::SlotUpgradeInfo const& info) { return info.currency == broke; }),
                candidates.end());
            continue;
        }

        if (!provider->UpgradeOnce(bot, best->itemGuid))
        {
            candidates.erase(best);
            continue;
        }

        ++bought;
        LOG_DEBUG("playerbots", "DCUpgradeItems: {} upgraded item {} (tier {}) to level {} for {} of currency {}",
            bot->GetName(), best->itemGuid, uint32(best->tier), uint32(best->level + 1), best->nextCost,
            uint32(best->currency));

        // Re-read the slot from the DC side rather than guessing the next price.
        Api::SlotUpgradeInfo refreshed;
        bool stillUpgradable = provider->DescribeEquippedSlot(bot, best->slot, refreshed) &&
            refreshed.itemGuid == best->itemGuid && refreshed.stateKnown;

        if (stillUpgradable)
        {
            refreshed.tierMaxLevel = EffectiveMaxLevel(refreshed.tierMaxLevel);
            stillUpgradable = refreshed.level < refreshed.tierMaxLevel && refreshed.nextCost > 0;
        }

        if (stillUpgradable)
            *best = refreshed;
        else
            candidates.erase(best);
    }

    return bought > 0;
}

namespace
{
    // Refreshes the cache above on startup and on ".reload config".
    class DCUpgradeItemsWorldScript : public WorldScript
    {
    public:
        DCUpgradeItemsWorldScript()
            : WorldScript("DCUpgradeItemsWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) {}

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            LoadConfig();
        }
    };
}

void AddSC_dc_upgrade_items()
{
    new DCUpgradeItemsWorldScript();
}
