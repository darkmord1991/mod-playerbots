/*
 * DarkChaos addition to mod-playerbots. See DCMythicRunActions.h.
 */

#include "DCMythicRunActions.h"

#include "Creature.h"
#include "Event.h"
#include "Group.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Playerbots.h"
#include "Timer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr uint32 BOSS_RESCAN_MS = 5000;
    // Hostiles this far above or below the tank are on another floor.
    constexpr float FLOOR_HEIGHT = 8.0f;
    // A pulled target still out of combat once the tank stands this close to it
    // refused the pull; so did one the tank has not engaged after the timeout.
    constexpr float REFUSED_PULL_CONTACT = 8.0f;
    constexpr uint32 REFUSED_PULL_TIMEOUT_MS = 20 * IN_MILLISECONDS;
    constexpr uint32 REFUSED_PULL_SKIP_MS = 60 * IN_MILLISECONDS;
    // Pull line of sight as a spell sees it: the map and WMO objects block it,
    // M2 decorations do not. Utgarde Keep's closed forge fires are M2 walls
    // cutting the Furnace of Hate into three; with every check the tank never
    // saw the Forge Masters whose pulls open them.
    constexpr LineOfSightChecks PULL_LOS_CHECKS =
        LineOfSightChecks(LINEOFSIGHT_CHECK_VMAP | LINEOFSIGHT_CHECK_GOBJECT_WMO);

    struct Spawn
    {
        uint32 entry;
        float x;
        float y;
        float z;
    };

    // Creature spawns of one map and spawn mode, read once from ObjectMgr. A
    // boss in a grid nobody has reached yet is not in the map's creature store,
    // so walking toward its spawn point is what loads it. The first call per map
    // walks every creature spawn once.
    std::vector<Spawn> const& GetSpawns(uint32 mapId, uint8 spawnMode)
    {
        static std::mutex mutex;
        static std::unordered_map<uint64, std::vector<Spawn>> cache;

        uint64 const key = (uint64(mapId) << 8) | spawnMode;

        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(key);
        if (it != cache.end())
            return it->second;

        std::vector<Spawn>& spawns = cache[key];
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            (void)spawnId;
            if (data.mapid != mapId || !(data.spawnMask & (1 << spawnMode)))
                continue;

            spawns.push_back({ data.id, data.posX, data.posY, data.posZ });
        }

        // unordered_map keeps references to its values valid across rehashes.
        return spawns;
    }
}

bool DCMythicAdvanceAction::isUseful()
{
    return bot->IsAlive() && !bot->IsInCombat() && !bot->IsBeingTeleported()
        && DCBotMythicRun::IsLeadingRunningRun(bot->GetGUID());
}

bool DCMythicAdvanceAction::Execute(Event /*event*/)
{
    DCBotMythicRun::Config const cfg = DCBotMythicRun::GetConfig();

    CheckLastPull();

    // Nothing moves on until the whole group can take the next fight.
    if (!IsGroupReady(cfg))
        return false;

    if (Unit* target = FindPullTarget(cfg.pullRange))
    {
        if (target->GetGUID() != _lastPull)
        {
            _lastPull = target->GetGUID();
            _lastPullMs = getMSTime();
        }
        return Attack(target);
    }

    Position destination;
    if (!FindNextBoss(destination))
        return false;

    return MoveTo(bot->GetMapId(), destination.GetPositionX(), destination.GetPositionY(),
        destination.GetPositionZ());
}

bool DCMythicAdvanceAction::IsGroupReady(DCBotMythicRun::Config const& cfg)
{
    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member)
            continue;

        if (!member->IsAlive() || member->IsBeingTeleported())
            return false;

        if (member->GetMapId() != bot->GetMapId() || member->GetInstanceId() != bot->GetInstanceId())
            return false;

        if (member != bot && member->GetExactDist(bot) > cfg.regroupDistance)
            return false;

        if (member->GetHealthPct() < float(cfg.restHealthPct))
            return false;

        if (member->getPowerType() == POWER_MANA && member->GetMaxPower(POWER_MANA) > 0
            && member->GetPowerPct(POWER_MANA) < float(cfg.restManaPct))
            return false;
    }

    return true;
}

void DCMythicAdvanceAction::CheckLastPull()
{
    if (_lastPull.IsEmpty())
        return;

    // Only called while the tank is out of combat (isUseful).
    Unit* unit = botAI->GetUnit(_lastPull);
    if (!unit || !unit->IsAlive() || unit->IsInCombat())
    {
        _lastPull.Clear();
        return;
    }

    uint32 const now = getMSTime();
    if (bot->GetExactDist(unit) > REFUSED_PULL_CONTACT && getMSTimeDiff(_lastPullMs, now) < REFUSED_PULL_TIMEOUT_MS)
        return;  // still on the way to it

    _refusedUntilMs[_lastPull] = now + REFUSED_PULL_SKIP_MS;
    _lastPull.Clear();
}

Unit* DCMythicAdvanceAction::FindPullTarget(float range)
{
    Unit* best = nullptr;
    float bestDistance = range;
    uint32 const now = getMSTime();

    for (ObjectGuid const guid : AI_VALUE(GuidVector, "possible targets"))
    {
        auto refused = _refusedUntilMs.find(guid);
        if (refused != _refusedUntilMs.end())
        {
            if (now < refused->second)
                continue;
            _refusedUntilMs.erase(refused);
        }

        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || !unit->IsCreature())
            continue;

        Creature* creature = unit->ToCreature();
        if (creature->IsCritter() || creature->IsInEvadeMode() || !bot->IsValidAttackTarget(creature))
            continue;

        if (std::fabs(creature->GetPositionZ() - bot->GetPositionZ()) > FLOOR_HEIGHT)
            continue;

        float const distance = bot->GetExactDist(creature);
        if (distance >= bestDistance
            || !bot->IsWithinLOSInMap(creature, VMAP::ModelIgnoreFlags::M2, PULL_LOS_CHECKS))
            continue;

        best = creature;
        bestDistance = distance;
    }

    return best;
}

bool DCMythicAdvanceAction::FindNextBoss(Position& out)
{
    uint32 const now = getMSTime();
    if (_hasBossPosition && now < _nextBossScanMs)
    {
        out = _bossPosition;
        return true;
    }

    _nextBossScanMs = now + BOSS_RESCAN_MS;
    _hasBossPosition = false;

    Map* map = bot->GetMap();
    if (!map)
        return false;

    // The keystone run's boss order, or without a keystone the dungeon's own
    // encounter order (DCBotMythicRun picks by run).
    std::vector<uint32> remaining;
    DCBotMythicRun::GetRemainingBossEntries(bot->GetGUID(), map, remaining);
    if (remaining.empty())
        return false;

    // The run's own boss order, never the nearest boss: from Utgarde Keep's
    // entrance the nearest one by straight-line distance is Skarvald & Dalronn,
    // 106 yards straight up on the floor above (2026-09-15).
    uint32 const next = remaining.front();

    // A loaded boss is wherever it is now (it may path or be moved by an event).
    for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
    {
        (void)spawnId;
        if (!creature || !creature->IsAlive() || creature->GetEntry() != next)
            continue;

        _bossPosition.Relocate(creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ());
        _hasBossPosition = true;
        break;
    }

    // Otherwise head for its spawn point (the nearest one, should the entry be
    // spawned more than once).
    if (!_hasBossPosition)
    {
        float bestDistance = std::numeric_limits<float>::max();
        for (Spawn const& spawn : GetSpawns(map->GetId(), map->GetSpawnMode()))
        {
            if (spawn.entry != next)
                continue;

            float const distance = bot->GetExactDist(spawn.x, spawn.y, spawn.z);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                _bossPosition.Relocate(spawn.x, spawn.y, spawn.z);
                _hasBossPosition = true;
            }
        }
    }

    if (!_hasBossPosition)
        return false;

    // The navmesh only covers loaded grids, and PathGenerator answers a
    // destination on an unloaded tile with a straight line - which is how the
    // tank walked through the ceiling onto Utgarde Keep's roof. Loading the
    // grid also brings the boss itself into the creature store.
    map->LoadGrid(_bossPosition.GetPositionX(), _bossPosition.GetPositionY());

    out = _bossPosition;
    return true;
}

bool DCMythicLeadTrigger::IsActive()
{
    return DCBotMythicRun::IsLeadingRunningRun(bot->GetGUID());
}
