/*
 * DarkChaos addition to mod-playerbots.
 *
 * Great Vault for random bots that ran Mythic+. A bot has no vault panel, so
 * pool generation and the reward choice happen in one step: every few minutes
 * the sweep asks the DC Mythic+ side which characters had a successful bot run
 * in the claim week (last week, the vault's grace window) and have not claimed
 * yet, builds the pool for each online random bot among them, scores every
 * slot for the bot's spec and claims the best one. The reward arrives through
 * SendNewItem, whose item push result already makes the bot equip upgrades.
 *
 * The vault lives in scripts.lib, which modules.lib does not link, so the calls
 * go through the DCMythicPlusBots functions it exports (see
 * src/server/scripts/DC/MythicPlus/dc_mythicplus_run_manager.h).
 *
 * Threads: the sweep runs in a WorldScript OnUpdate on the world thread, while
 * no map is updating. Pool generation queries the database synchronously,
 * which is why the sweep is spread out and capped per pass.
 */

#include "Common.h"
#include "Config.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "StatsWeightCalculator.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

// Mirror of the DC Mythic+ bot API. Declared rather than included: scripts.lib
// does not export its include directory to modules.lib. Keep in sync with
// src/server/scripts/DC/MythicPlus/dc_mythicplus_run_manager.h.
namespace DCMythicPlusBots
{
    void GetBotVaultCandidates(std::vector<uint32>& outGuids);
    void GetBotVaultChoices(Player* bot, std::vector<std::pair<uint8, uint32>>& out);
    bool ClaimBotVaultReward(Player* bot, uint8 slot, uint32 itemId);
}

namespace
{
    // Claims per sweep. Each one runs a handful of synchronous queries.
    constexpr uint32 MAX_CLAIMS_PER_SWEEP = 10;

    struct VaultConfig
    {
        bool enabled = true;
        uint32 sweepMs = 10 * MINUTE * IN_MILLISECONDS;
    };

    VaultConfig sConfig;

    void LoadConfig()
    {
        VaultConfig cfg;
        cfg.enabled = sConfigMgr->GetOption<bool>("AiPlayerbot.DCMythicBots.VaultAutoClaim", true);
        uint32 const minutes = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCMythicBots.VaultSweepMinutes", 10);
        cfg.sweepMs = std::max<uint32>(minutes, 1) * MINUTE * IN_MILLISECONDS;
        sConfig = cfg;
    }

    // How much the item improves on what the bot wears in the slot it would go
    // to. Lowest for anything the bot cannot wear (upgrade tokens), so gear
    // always wins over a token, and a piece that is worse than the equipped one
    // still beats a token.
    float ScoreReward(Player* bot, StatsWeightCalculator& calculator, uint32 itemId)
    {
        constexpr float UNUSABLE = std::numeric_limits<float>::lowest();

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto || proto->InventoryType == INVTYPE_NON_EQUIP || bot->CanUseItem(proto) != EQUIP_ERR_OK)
            return UNUSABLE;

        float const score = calculator.CalculateItem(itemId);

        uint8 const slot = bot->FindEquipSlot(proto, NULL_SLOT, true);
        if (slot == NULL_SLOT)
            return score;

        Item* equipped = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!equipped || !equipped->GetTemplate())
            return score;

        return score - calculator.CalculateItem(equipped->GetTemplate()->ItemId, equipped->GetItemRandomPropertyId());
    }

    bool ClaimFor(Player* bot)
    {
        std::vector<std::pair<uint8, uint32>> choices;
        DCMythicPlusBots::GetBotVaultChoices(bot, choices);
        if (choices.empty())
            return false;

        StatsWeightCalculator calculator(bot);

        std::pair<uint8, uint32> const* best = nullptr;
        float bestScore = 0.0f;
        for (auto const& choice : choices)
        {
            float const score = ScoreReward(bot, calculator, choice.second);
            if (!best || score > bestScore)
            {
                best = &choice;
                bestScore = score;
            }
        }

        if (!DCMythicPlusBots::ClaimBotVaultReward(bot, best->first, best->second))
            return false;

        LOG_INFO("playerbots.mplus", "Great Vault: bot {} claimed item {} from slot {} ({} choices)",
            bot->GetName(), best->second, uint32(best->first), choices.size());
        return true;
    }

    void Sweep()
    {
        std::vector<uint32> candidates;
        DCMythicPlusBots::GetBotVaultCandidates(candidates);

        uint32 claimed = 0;
        for (uint32 guidLow : candidates)
        {
            if (claimed >= MAX_CLAIMS_PER_SWEEP)
                break;

            // Offline bots wait for a sweep that finds them logged in: the pool
            // is rolled for the bot's spec, which needs the Player.
            Player* bot = ObjectAccessor::FindPlayerByLowGUID(guidLow);
            if (!bot || !bot->IsInWorld() || !sPlayerbotsMgr.GetPlayerbotAI(bot) || !sRandomPlayerbotMgr.IsRandomBot(bot))
                continue;

            if (ClaimFor(bot))
                ++claimed;
        }
    }

    class DCBotGreatVaultWorldScript : public WorldScript
    {
    public:
        DCBotGreatVaultWorldScript()
            : WorldScript("DCBotGreatVaultWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE })
        {
        }

        void OnAfterConfigLoad(bool /*reload*/) override { LoadConfig(); }

        void OnUpdate(uint32 diff) override
        {
            if (!sConfig.enabled)
                return;

            _timer += diff;
            if (_timer < sConfig.sweepMs)
                return;

            _timer = 0;
            Sweep();
        }

    private:
        uint32 _timer = 0;
    };
}

void AddSC_dc_bot_great_vault()
{
    LoadConfig();
    new DCBotGreatVaultWorldScript();
}
