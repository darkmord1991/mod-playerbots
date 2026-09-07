/*
 * Dark Chaos - playerbot cosmetics.
 *
 * Gives bots the visual variety a real collector has. Nothing here changes bot
 * stats, spells or behaviour:
 *
 *   - Mounts ............ every bot learns a stable handful of mounts drawn
 *                         from the DC catalog (acore_world.dc_mount_definitions),
 *                         bucketed into the same four tiers CheckMountStateAction
 *                         already understands, so the existing mount AI picks
 *                         among them unchanged.
 *   - Companions ........ a share of bots carry a random non-combat pet from
 *                         acore_world.dc_pet_definitions.
 *   - Warlock minions ... imp/voidwalker/succubus/felhunter/felguard/doomguard
 *                         get a random display from the module's own
 *                         acore_playerbots.playerbots_demon_skins.
 *   - Hunter pets ....... re-skinned to a random model of the same beast family
 *                         from the curated acore_world.dc_beastmaster_pets.
 *
 * Shapeshift forms are NOT here: that catalog (dc_shapeshift_form_skins) and the
 * ObjectMgr display provider live in scripts.lib, which modules.lib cannot see,
 * so the bot roll for forms sits in the file that owns the data
 * (src/server/scripts/DC/AddonExtension/dc_addon_forms.cpp).
 *
 * Picks are deterministic: StableRoll() hashes the character GUID, so a bot
 * keeps the same mounts, the same companion and the same demon skin for its
 * whole life, across restarts, with no per-bot table and no cache to invalidate.
 * The only DB write is the mount spells themselves - a bot has to actually know
 * a mount for CollectMountData to see it.
 */

#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"

#include "Config.h"
#include "CreatureData.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <algorithm>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // ------------------------------------------------------------------------
    // Config
    // ------------------------------------------------------------------------
    struct CosmeticsConfig
    {
        bool   enabled           = true;
        bool   mounts            = true;
        uint32 mountsPerTier     = 3;
        uint32 mountMaxSpellId   = 0;
        bool   companions        = true;
        uint32 companionChance   = 35;
        bool   petSkins          = true;
        uint32 hunterSkinChance  = 70;
        float  maxPetScale       = 1.6f;
    };

    CosmeticsConfig s_cfg;
    bool s_poolsLoaded = false;

    void LoadConfig()
    {
        s_cfg.enabled          = sConfigMgr->GetOption<bool>("AiPlayerbot.DCCosmetics.Enable", true);
        s_cfg.mounts           = sConfigMgr->GetOption<bool>("AiPlayerbot.DCCosmetics.Mounts.Enable", true);
        s_cfg.mountsPerTier    = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCCosmetics.Mounts.PerTier", 3);
        s_cfg.mountMaxSpellId  = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCCosmetics.Mounts.MaxSpellId", 0);
        s_cfg.companions       = sConfigMgr->GetOption<bool>("AiPlayerbot.DCCosmetics.Companions.Enable", true);
        s_cfg.companionChance  = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCCosmetics.Companions.Chance", 35);
        s_cfg.petSkins         = sConfigMgr->GetOption<bool>("AiPlayerbot.DCCosmetics.PetSkins.Enable", true);
        s_cfg.hunterSkinChance = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCCosmetics.PetSkins.HunterChance", 70);
        s_cfg.maxPetScale      = sConfigMgr->GetOption<float>("AiPlayerbot.DCCosmetics.PetSkins.MaxScale", 1.6f);

        s_cfg.mountsPerTier    = std::min<uint32>(s_cfg.mountsPerTier, 10);
        s_cfg.companionChance  = std::min<uint32>(s_cfg.companionChance, 100);
        s_cfg.hunterSkinChance = std::min<uint32>(s_cfg.hunterSkinChance, 100);
        s_cfg.maxPetScale      = std::max(s_cfg.maxPetScale, 0.1f);
    }

    // ------------------------------------------------------------------------
    // Helpers
    // ------------------------------------------------------------------------
    bool IsBot(Player* player)
    {
        return player && sPlayerbotsMgr.GetPlayerbotAI(player) != nullptr;
    }

    // splitmix64 finalizer - cheap, and it spreads adjacent GUIDs (bots are
    // created in contiguous blocks) across the whole output range.
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

    // ------------------------------------------------------------------------
    // Mount pool
    //
    // The tiers mirror CheckMountStateAction's four mount slots, so a bot that
    // learns from this pool is handled by the existing mount AI unchanged.
    // ------------------------------------------------------------------------
    enum MountTier : uint8
    {
        MOUNT_TIER_GROUND_SLOW = 0,
        MOUNT_TIER_GROUND_FAST = 1,
        MOUNT_TIER_FLY_SLOW    = 2,
        MOUNT_TIER_FLY_FAST    = 3,
        MOUNT_TIER_MAX
    };

    std::array<std::vector<uint32>, MOUNT_TIER_MAX> s_mountPool;

    // dc_mount_definitions.class_mask is 0 on every row, so the handful of
    // genuinely class-locked mounts are gated here instead. A bot of the wrong
    // class simply skips them.
    std::unordered_map<uint32, uint8> const s_classLockedMounts =
    {
        { 48778, CLASS_DEATH_KNIGHT }, // Acherus Deathcharger
        { 54729, CLASS_DEATH_KNIGHT }, // Winged Steed of the Ebon Blade
        { 66906, CLASS_PALADIN      }, // Argent Charger
        { 13819, CLASS_PALADIN      }, // Warhorse
        { 23214, CLASS_PALADIN      }, // Charger
        { 34767, CLASS_PALADIN      }, // Summon Charger
        { 34769, CLASS_PALADIN      }, // Summon Warhorse
        {  5784, CLASS_WARLOCK      }, // Felsteed
        { 23161, CLASS_WARLOCK      }, // Dreadsteed
    };

    std::vector<uint32> s_companionPool;                            // summon spells
    std::unordered_map<uint32, std::vector<uint32>> s_demonSkins;    // family -> displays
    std::unordered_map<uint32, std::vector<uint32>> s_hunterSkins;   // family -> displays

    void LoadMountPool()
    {
        for (auto& tier : s_mountPool)
            tier.clear();

        QueryResult result = WorldDatabase.Query("SELECT spell_id FROM dc_mount_definitions");
        if (!result)
        {
            LOG_WARN("playerbots", "DCCosmetics: dc_mount_definitions is empty - bots keep their racial mounts");
            return;
        }

        uint32 skipped = 0;
        do
        {
            uint32 spellId = result->Fetch()[0].Get<uint32>();

            // Optional ceiling: DC retroported mounts live in the 300000+ band,
            // so 299999 keeps bots on stock 3.3.5 mounts.
            if (s_cfg.mountMaxSpellId && spellId > s_cfg.mountMaxSpellId)
                continue;

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            if (!spellInfo || spellInfo->IsPassive())
            {
                ++skipped;
                continue;
            }

            // A usable mount applies SPELL_AURA_MOUNTED on effect 0; the speed
            // lives on one of the two remaining effects. This is the same shape
            // CollectMountData reads, so both agree on every spell.
            if (spellInfo->Effects[EFFECT_0].ApplyAuraName != SPELL_AURA_MOUNTED)
            {
                ++skipped;
                continue;
            }

            int32 speed = std::max(spellInfo->Effects[EFFECT_1].BasePoints,
                                   spellInfo->Effects[EFFECT_2].BasePoints);

            // 54729 (Winged Steed of the Ebon Blade) scales with riding skill
            // and is mis-detected as a ground mount - the same exception
            // CollectMountData carries.
            bool flying =
                spellInfo->Effects[EFFECT_1].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED ||
                spellInfo->Effects[EFFECT_2].ApplyAuraName == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED ||
                spellId == 54729;

            MountTier tier;
            if (flying)
                tier = speed >= 279 ? MOUNT_TIER_FLY_FAST : MOUNT_TIER_FLY_SLOW;
            else
                tier = speed >= 99 ? MOUNT_TIER_GROUND_FAST : MOUNT_TIER_GROUND_SLOW;

            s_mountPool[tier].push_back(spellId);
        } while (result->NextRow());

        LOG_INFO("playerbots",
                 "DCCosmetics: mount pool {} slow ground / {} fast ground / {} slow fly / {} fast fly "
                 "({} catalog rows unusable)",
                 s_mountPool[MOUNT_TIER_GROUND_SLOW].size(), s_mountPool[MOUNT_TIER_GROUND_FAST].size(),
                 s_mountPool[MOUNT_TIER_FLY_SLOW].size(), s_mountPool[MOUNT_TIER_FLY_FAST].size(), skipped);
    }

    void LoadCompanionPool()
    {
        s_companionPool.clear();

        QueryResult result = WorldDatabase.Query(
            "SELECT DISTINCT pet_spell_id FROM dc_pet_definitions WHERE pet_spell_id > 0");
        if (!result)
        {
            LOG_WARN("playerbots", "DCCosmetics: dc_pet_definitions is empty - bots get no companions");
            return;
        }

        uint32 skipped = 0;
        do
        {
            uint32 spellId = result->Fetch()[0].Get<uint32>();

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            if (!spellInfo)
            {
                ++skipped;
                continue;
            }

            bool summons = false;
            for (uint8 i = EFFECT_0; i < MAX_SPELL_EFFECTS; ++i)
            {
                if (spellInfo->Effects[i].Effect == SPELL_EFFECT_SUMMON)
                {
                    summons = true;
                    break;
                }
            }

            if (!summons)
            {
                ++skipped;
                continue;
            }

            s_companionPool.push_back(spellId);
        } while (result->NextRow());

        LOG_INFO("playerbots", "DCCosmetics: {} companion pets available ({} catalog rows unusable)",
                 s_companionPool.size(), skipped);
    }

    void LoadDemonSkins()
    {
        s_demonSkins.clear();

        QueryResult result = PlayerbotsDatabase.Query(
            "SELECT family, display_id FROM playerbots_demon_skins WHERE enabled = 1");
        if (!result)
        {
            LOG_WARN("playerbots",
                     "DCCosmetics: playerbots_demon_skins is empty - warlock minions keep their stock look");
            return;
        }

        uint32 count = 0;
        do
        {
            Field* fields = result->Fetch();
            uint32 family = fields[0].Get<uint32>();
            uint32 displayId = fields[1].Get<uint32>();

            // A display without a creature_model_info row has no bounding radius
            // or combat reach, and the core warns on every use.
            if (!displayId || !sObjectMgr->GetCreatureModelInfo(displayId))
                continue;

            s_demonSkins[family].push_back(displayId);
            ++count;
        } while (result->NextRow());

        LOG_INFO("playerbots", "DCCosmetics: {} warlock minion skins across {} families",
                 count, s_demonSkins.size());
    }

    void LoadHunterSkins()
    {
        s_hunterSkins.clear();

        // The curated beastmaster roster is the source of truth for "a model a
        // hunter pet is allowed to wear" - every entry there already renders on
        // the client.
        QueryResult result = WorldDatabase.Query(
            "SELECT DISTINCT ct.family, ctm.CreatureDisplayID "
            "FROM dc_beastmaster_pets bp "
            "JOIN creature_template ct ON ct.entry = bp.creature_id "
            "JOIN creature_template_model ctm ON ctm.CreatureID = bp.creature_id "
            "WHERE bp.enabled = 1 AND ct.family > 0 AND ctm.CreatureDisplayID > 0");
        if (!result)
        {
            LOG_WARN("playerbots", "DCCosmetics: no beastmaster pets - hunter bots keep their tamed look");
            return;
        }

        uint32 count = 0;
        do
        {
            Field* fields = result->Fetch();
            uint32 family = fields[0].Get<uint32>();
            uint32 displayId = fields[1].Get<uint32>();

            if (!sObjectMgr->GetCreatureModelInfo(displayId))
                continue;

            s_hunterSkins[family].push_back(displayId);
            ++count;
        } while (result->NextRow());

        LOG_INFO("playerbots", "DCCosmetics: {} hunter pet skins across {} families",
                 count, s_hunterSkins.size());
    }

    void LoadPools()
    {
        if (s_poolsLoaded || !s_cfg.enabled)
            return;

        LoadMountPool();
        LoadCompanionPool();
        LoadDemonSkins();
        LoadHunterSkins();
        s_poolsLoaded = true;
    }

    // ------------------------------------------------------------------------
    // Selection
    // ------------------------------------------------------------------------
    constexpr uint32 MOUNT_SALT     = 0x4D4F554Eu; // "MOUN"
    constexpr uint32 COMPANION_SALT = 0x50455453u; // "PETS"
    constexpr uint32 DEMON_SALT     = 0x44454D4Fu; // "DEMO"
    constexpr uint32 HUNTER_SALT    = 0x48554E54u; // "HUNT"

    uint32 PickFrom(std::vector<uint32> const& pool, uint32 guid, uint32 salt)
    {
        if (pool.empty())
            return 0;

        return pool[StableRoll(guid, salt) % pool.size()];
    }

    uint32 MinLevelForTier(MountTier tier)
    {
        // Reuse the module's own thresholds so the grant and the mount AI never
        // disagree about when a bot may ride a given tier.
        switch (tier)
        {
            case MOUNT_TIER_GROUND_SLOW: return sPlayerbotAIConfig.useGroundMountAtMinLevel;
            case MOUNT_TIER_GROUND_FAST: return sPlayerbotAIConfig.useFastGroundMountAtMinLevel;
            case MOUNT_TIER_FLY_SLOW:    return sPlayerbotAIConfig.useFlyMountAtMinLevel;
            case MOUNT_TIER_FLY_FAST:
            default:                     return sPlayerbotAIConfig.useFastFlyMountAtMinLevel;
        }
    }

    bool MountAllowedForClass(uint32 spellId, uint8 playerClass)
    {
        auto it = s_classLockedMounts.find(spellId);
        return it == s_classLockedMounts.end() || it->second == playerClass;
    }

    void GrantMounts(Player* bot)
    {
        if (!s_cfg.mounts || !s_cfg.mountsPerTier)
            return;

        uint32 guid = bot->GetGUID().GetCounter();
        uint8 playerClass = bot->getClass();
        uint8 level = bot->GetLevel();

        for (uint8 tier = MOUNT_TIER_GROUND_SLOW; tier < MOUNT_TIER_MAX; ++tier)
        {
            std::vector<uint32> const& pool = s_mountPool[tier];
            if (pool.empty() || level < MinLevelForTier(MountTier(tier)))
                continue;

            // Walk a deterministic sequence of candidates and keep the first
            // mountsPerTier this class may use. The extra attempts absorb
            // duplicate rolls and class-locked entries without ever making the
            // result depend on how many times the bot has logged in.
            std::unordered_set<uint32> chosen;
            uint32 const attempts = s_cfg.mountsPerTier * 4;

            for (uint32 i = 0; i < attempts && chosen.size() < size_t(s_cfg.mountsPerTier); ++i)
            {
                uint32 spellId = PickFrom(pool, guid, MOUNT_SALT + uint32(tier) * 64u + i);
                if (!spellId || !chosen.insert(spellId).second)
                    continue;

                if (!MountAllowedForClass(spellId, playerClass))
                {
                    chosen.erase(spellId);
                    continue;
                }

                if (!bot->HasSpell(spellId))
                    bot->learnSpell(spellId);
            }
        }
    }

    // ------------------------------------------------------------------------
    // Companion pets
    // ------------------------------------------------------------------------
    uint32 CompanionSpellFor(Player* bot)
    {
        if (!s_cfg.companions || !s_cfg.companionChance || s_companionPool.empty())
            return 0;

        uint32 guid = bot->GetGUID().GetCounter();
        if (StableRoll(guid, COMPANION_SALT) % 100 >= s_cfg.companionChance)
            return 0;

        return PickFrom(s_companionPool, guid, COMPANION_SALT + 1);
    }

    void SummonCompanion(Player* bot)
    {
        // OnPlayerUpdateZone also fires while a teleport is still settling, so
        // do not push a summon into a half-applied map change.
        if (!bot->IsInWorld() || bot->IsBeingTeleported() || !bot->IsAlive() || bot->IsInCombat())
            return;

        if (!bot->GetCritterGUID().IsEmpty())
            return;

        if (uint32 spellId = CompanionSpellFor(bot))
            bot->CastSpell(bot, spellId, true);
    }

    // ------------------------------------------------------------------------
    // Pet skins
    // ------------------------------------------------------------------------
    bool IsWarlockMinionFamily(uint32 family)
    {
        switch (family)
        {
            case CREATURE_FAMILY_IMP:
            case CREATURE_FAMILY_VOIDWALKER:
            case CREATURE_FAMILY_SUCCUBUS:
            case CREATURE_FAMILY_FELHUNTER:
            case CREATURE_FAMILY_FELGUARD:
            case CREATURE_FAMILY_DOOMGUARD:
                return true;
            default:
                return false;
        }
    }

    void ApplyPetDisplay(Pet* pet, uint32 displayId)
    {
        if (!displayId || pet->GetDisplayId() == displayId)
            return;

        // Models carry their own scale from CreatureDisplayInfo.dbc; counter-
        // scale so a downported, boss-sized demon does not tower over its owner.
        // SetDisplayId recomputes bounding radius and combat reach against the
        // scale we pass, so both stay consistent.
        float scale = 1.0f;
        if (CreatureDisplayInfoEntry const* info = sCreatureDisplayInfoStore.LookupEntry(displayId))
            if (info->scale > s_cfg.maxPetScale)
                scale = s_cfg.maxPetScale / info->scale;

        pet->SetDisplayId(displayId, scale);
        pet->SetNativeDisplayId(displayId);
    }

    void ReskinPet(Pet* pet)
    {
        if (!s_cfg.petSkins)
            return;

        Unit* owner = pet->GetOwner();
        if (!owner || !owner->IsPlayer())
            return;

        Player* bot = owner->ToPlayer();
        if (!IsBot(bot))
            return;

        CreatureTemplate const* creatureTemplate = pet->GetCreatureTemplate();
        if (!creatureTemplate || !creatureTemplate->family)
            return;

        uint32 family = creatureTemplate->family;
        uint32 guid = bot->GetGUID().GetCounter();

        if (pet->getPetType() == HUNTER_PET)
        {
            if (StableRoll(guid, HUNTER_SALT) % 100 >= s_cfg.hunterSkinChance)
                return;

            auto it = s_hunterSkins.find(family);
            if (it == s_hunterSkins.end())
                return;

            // Salt with the tamed entry so a bot that re-tames gets a new look,
            // while a given pet keeps its own across summons.
            ApplyPetDisplay(pet, PickFrom(it->second, guid, HUNTER_SALT + pet->GetEntry()));
            return;
        }

        if (!IsWarlockMinionFamily(family))
            return;

        auto it = s_demonSkins.find(family);
        if (it == s_demonSkins.end())
            return;

        ApplyPetDisplay(pet, PickFrom(it->second, guid, DEMON_SALT + family));
    }

    // ------------------------------------------------------------------------
    // Scripts
    // ------------------------------------------------------------------------
    class DCBotCosmeticsWorldScript : public WorldScript
    {
    public:
        DCBotCosmeticsWorldScript()
            : WorldScript("DCBotCosmeticsWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) {}

        void OnAfterConfigLoad(bool reload) override
        {
            LoadConfig();

            // A `.reload config` is also how an admin picks up catalog edits. At
            // startup the DB pools are not open yet, so OnStartup loads first.
            if (reload)
            {
                s_poolsLoaded = false;
                LoadPools();
            }
        }

        void OnStartup() override
        {
            LoadPools();
        }
    };

    class DCBotCosmeticsPlayerScript : public PlayerScript
    {
    public:
        DCBotCosmeticsPlayerScript()
            : PlayerScript("DCBotCosmeticsPlayerScript",
                           { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_UPDATE_ZONE }) {}

        void OnPlayerLogin(Player* player) override
        {
            if (!s_cfg.enabled || !IsBot(player))
                return;

            LoadPools();
            GrantMounts(player);
            SummonCompanion(player);
        }

        void OnPlayerLevelChanged(Player* player, uint8 /*oldlevel*/) override
        {
            if (!s_cfg.enabled || !IsBot(player))
                return;

            // A new level can unlock a mount tier the bot did not qualify for at
            // login. GrantMounts is idempotent, so this stays cheap.
            GrantMounts(player);
        }

        void OnPlayerUpdateZone(Player* player, uint32 /*newZone*/, uint32 /*newArea*/) override
        {
            if (!s_cfg.enabled || !IsBot(player))
                return;

            // Critters do not survive every teleport; re-summon on the next zone
            // change if the slot went empty.
            SummonCompanion(player);
        }
    };

    class DCBotCosmeticsPetScript : public PetScript
    {
    public:
        DCBotCosmeticsPetScript()
            : PetScript("DCBotCosmeticsPetScript", { PETHOOK_ON_PET_ADD_TO_WORLD }) {}

        void OnPetAddToWorld(Pet* pet) override
        {
            if (!s_cfg.enabled || !pet)
                return;

            ReskinPet(pet);
        }
    };
}

void AddSC_dc_bot_cosmetics()
{
    new DCBotCosmeticsWorldScript();
    new DCBotCosmeticsPlayerScript();
    new DCBotCosmeticsPetScript();
}
