/*
 * DarkChaos addition to mod-playerbots. See DCHeirloomUpgradeAction.h.
 */

#include "DCHeirloomUpgradeAction.h"

#include "Config.h"
#include "DCItemUpgradeApi.h"
#include "Event.h"
#include "Player.h"
#include "Playerbots.h"
#include "ScriptMgr.h"

#include <vector>

namespace
{
    namespace Api = DarkChaos::ItemUpgradeApi;

    // The twelve stat packages, from HandleGetPackages in dc_addon_upgrade.cpp.
    // Kept here as named constants so the role mapping below reads as intent
    // rather than as magic numbers.
    enum HeirloomPackage : uint8
    {
        PACKAGE_FURY        = 1,   // Crit, Haste
        PACKAGE_PRECISION   = 2,   // Hit, Expertise
        PACKAGE_DEVASTATION = 3,   // Crit, ArmorPen
        PACKAGE_SWIFTBLADE  = 4,   // Haste, ArmorPen
        PACKAGE_SPELLFIRE   = 5,   // Crit, Haste, SpellPower
        PACKAGE_ARCANE      = 6,   // Hit, Haste, SpellPower
        PACKAGE_BULWARK     = 7,   // Dodge, Parry, Block
        PACKAGE_FORTRESS    = 8,   // Defense, Block, Stamina
        PACKAGE_SURVIVOR    = 9,   // Dodge, Stamina
        PACKAGE_GLADIATOR   = 10,  // Resilience, Crit
        PACKAGE_WARLORD     = 11,  // Resilience, Stamina
        PACKAGE_BALANCED    = 12   // Crit, Hit, Haste
    };

    // Cached like DCUpgradeItemsAction's config, and for the same reason:
    // isUseful() runs per bot per trigger fire and a string-keyed lookup per knob
    // is not free across a full roster. Refreshed on ".reload config" below.
    struct HeirloomConfig
    {
        bool   enabled          = true;
        bool   grantStarter     = true;
        uint32 stepsPerRun      = 2;
        uint32 minLevel         = 10;
        uint32 cooldownSeconds  = 3600;
        // 0 = pick from the bot's role; 1..12 forces that package on every bot.
        uint32 forcedPackage    = 0;
    };

    HeirloomConfig s_cfg;

    void LoadConfig()
    {
        s_cfg.enabled         = sConfigMgr->GetOption<bool>("AiPlayerbot.DCHeirloom.Enable", true);
        s_cfg.grantStarter    = sConfigMgr->GetOption<bool>("AiPlayerbot.DCHeirloom.GrantStarter", true);
        s_cfg.stepsPerRun     = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHeirloom.StepsPerRun", 2);
        s_cfg.minLevel        = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHeirloom.MinLevel", 10);
        s_cfg.cooldownSeconds = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHeirloom.CooldownSeconds", 3600);
        s_cfg.forcedPackage   = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHeirloom.ForcePackage", 0);
    }

    // Which stat package suits this bot.
    //
    // Tank first, then healer, then caster, because the checks are not mutually
    // exclusive -- a holy paladin answers true to more than one of them, and the
    // most specific answer is the one worth keeping.
    uint8 PackageForBot(Player* bot)
    {
        if (s_cfg.forcedPackage >= 1 && s_cfg.forcedPackage <= PACKAGE_BALANCED)
            return static_cast<uint8>(s_cfg.forcedPackage);

        if (!bot)
            return PACKAGE_BALANCED;

        // Defense is what keeps a tank crit-immune, so Fortress over Bulwark.
        if (PlayerbotAI::IsTank(bot, true))
            return PACKAGE_FORTRESS;

        // No package carries spirit or mp5, so healers take the spell-power one;
        // Spellfire's crit and haste are the next best thing for them.
        if (PlayerbotAI::IsHeal(bot, true))
            return PACKAGE_SPELLFIRE;

        if (PlayerbotAI::IsCaster(bot, true))
            return PACKAGE_SPELLFIRE;

        // Physical damage: armour penetration beats raw haste for them.
        if (PlayerbotAI::IsMelee(bot, true) || PlayerbotAI::IsRangedDps(bot, true))
            return PACKAGE_DEVASTATION;

        return PACKAGE_BALANCED;
    }
}

bool DCHeirloomUpgradeAction::isUseful()
{
    if (!s_cfg.enabled)
        return false;

    Api::Provider* provider = Api::GetProvider();
    if (!provider)
        return false;

    if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat())
        return false;

    if (bot->GetLevel() < s_cfg.minLevel)
        return false;

    // Unsigned arithmetic, so a getMSTime() wrap resolves the same way it does
    // for RandomTrigger::IsActive. _lastRunMs == 0 means "never ran".
    if (_lastRunMs && (getMSTime() - _lastRunMs) < s_cfg.cooldownSeconds * IN_MILLISECONDS)
        return false;

    // Heirloom levels are paid almost entirely in essence (50 at level 1 rising
    // to 2050 at 15); the token side is a trickle and only from level 5. Holding
    // no essence at all means there is nothing to do -- unless the bot has yet to
    // be given its heirloom, which is worth a run on its own.
    if (provider->GetCurrencyAmount(bot, Api::CURRENCY_ARTIFACT_ESSENCE) > 0)
        return true;

    return s_cfg.grantStarter && provider->GetStarterHeirloomItemId() != 0;
}

bool DCHeirloomUpgradeAction::Execute(Event /*event*/)
{
    Api::Provider* provider = Api::GetProvider();
    if (!provider || !bot)
        return false;

    _lastRunMs = getMSTime();

    std::vector<Api::HeirloomSlotInfo> heirlooms;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Api::HeirloomSlotInfo info;
        if (provider->DescribeEquippedHeirloom(bot, slot, info))
            heirlooms.push_back(info);
    }

    // Nothing to upgrade yet. The starter heirloom only drops out of the
    // onboarding quest chain, which bots never run, so hand it over and let the
    // next run start levelling it.
    if (heirlooms.empty())
    {
        if (!s_cfg.grantStarter)
            return false;

        if (!provider->GrantStarterHeirloom(bot))
            return false;

        LOG_DEBUG("playerbots", "DCHeirloomUpgrade: granted starter heirloom to {}", bot->GetName());
        return true;
    }

    uint8 const packageId = PackageForBot(bot);
    uint32 started = 0;

    for (auto const& heirloom : heirlooms)
    {
        if (started >= s_cfg.stepsPerRun)
            break;

        // Fire-and-forget: the current level, the affordability check and the
        // payment all happen in the DC side's async continuation, because that
        // state is only in the DB and reading it here would block the world
        // thread. A refusal there is silent by design.
        if (!provider->RequestHeirloomStep(bot, heirloom.itemGuid, packageId))
            continue;

        ++started;
        LOG_DEBUG("playerbots", "DCHeirloomUpgrade: {} requested a step on heirloom {} with package {}",
            bot->GetName(), heirloom.itemGuid, uint32(packageId));
    }

    return started > 0;
}

namespace
{
    // Refreshes the cache above on startup and on ".reload config".
    class DCHeirloomUpgradeWorldScript : public WorldScript
    {
    public:
        DCHeirloomUpgradeWorldScript()
            : WorldScript("DCHeirloomUpgradeWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) {}

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            LoadConfig();
        }
    };
}

void AddSC_dc_heirloom_upgrade()
{
    new DCHeirloomUpgradeWorldScript();
}
