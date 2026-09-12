/*
 * DarkChaos addition to mod-playerbots. See DCHinterlandTactics.h.
 */

#include "DCHinterlandTactics.h"

#include "DCHinterlandGeography.h"
#include "DCHinterlandMonitor.h"

#include "Battleground.h"
#include "Config.h"
#include "Event.h"
#include "Player.h"
#include "Playerbots.h"
#include "PositionValue.h"

namespace
{
    struct HLBGConfig
    {
        bool enabled = true;
        uint32 repickSecondsMin = 45;
        uint32 repickSecondsMax = 90;
        float arrivalDistance = 18.0f;
        float engageDistance = 18.0f;
        uint32 stallSeconds = 20;
        float stallDistance = 3.0f;
        uint32 unstickTeleportStrikes = 3;
    };

    HLBGConfig const& GetConfig()
    {
        // Read once. isUseful() runs for every bot on every tick, and a
        // string-keyed config lookup per knob per tick across a full roster is
        // not free. A restart picks up changes; nothing here is hot-tunable.
        static HLBGConfig const cfg = []
        {
            HLBGConfig c;
            c.enabled = sConfigMgr->GetOption<bool>("AiPlayerbot.DCHinterland.Enable", true);
            c.repickSecondsMin = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.RepickSecondsMin", 45);
            c.repickSecondsMax = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.RepickSecondsMax", 90);
            c.arrivalDistance = sConfigMgr->GetOption<float>("AiPlayerbot.DCHinterland.ArrivalDistance", 18.0f);
            c.engageDistance = sConfigMgr->GetOption<float>("AiPlayerbot.DCHinterland.EngageDistance", 18.0f);
            c.stallSeconds = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.StallSeconds", 20);
            c.stallDistance = sConfigMgr->GetOption<float>("AiPlayerbot.DCHinterland.StallDistance", 3.0f);
            c.unstickTeleportStrikes =
                sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.UnstickTeleportStrikes", 3);

            if (c.repickSecondsMax < c.repickSecondsMin)
                c.repickSecondsMax = c.repickSecondsMin;

            return c;
        }();

        return cfg;
    }
}

bool DCHinterlandTacticsAction::IsInHinterlandBG(Player* player)
{
    if (!player || !player->InBattleground())
        return false;

    Battleground* bg = player->GetBattleground();
    if (!bg)
        return false;

    return bg->GetBgTypeID(true) == BattlegroundTypeId(DC_BATTLEGROUND_HLBG_TYPE_ID);
}

bool DCHinterlandTacticsAction::isUseful()
{
    if (!GetConfig().enabled)
        return false;

    if (!IsInHinterlandBG(bot))
        return false;

    // Nothing to steer while the bot is dead, mid-teleport, or already busy
    // fighting -- the combat engine owns movement then.
    if (bot->isDead() || bot->IsBeingTeleported() || bot->IsInCombat())
        return false;

    return true;
}

bool DCHinterlandTacticsAction::Execute(Event /*event*/)
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;

    if (bg->GetStatus() == STATUS_WAIT_JOIN)
        return MoveToStaging();

    if (bg->GetStatus() != STATUS_IN_PROGRESS)
        return false;

    return MoveToObjective();
}

bool DCHinterlandTacticsAction::MoveToStaging()
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;

    // The battleground teleports arrivals onto the team start and enforces
    // StartMaxDist (25y) until the doors open, so this only spreads the bots
    // out inside that radius rather than moving them anywhere new.
    DCHinterland::SideView const side = DCHinterland::GetSideView(bot->GetBgTeamId());
    return MoveTo(bg->GetMapId(), side.ownStaging.x + frand(-6.0f, 6.0f), side.ownStaging.y + frand(-6.0f, 6.0f),
                  side.ownStaging.z);
}

bool DCHinterlandTacticsAction::SelectObjective()
{
    DCHinterland::SideView const side = DCHinterland::GetSideView(bot->GetBgTeamId());

    // "bg role" is rolled 0-9 when the bot accepts the battleground port. It is
    // set for every battleground type, HLBG included, so it is free to reuse as
    // a stable per-match disposition instead of re-rolling one here.
    uint32 const role = AI_VALUE(uint32, "bg role");

    auto rollTarget = [&side, role]() -> DCHinterland::Point
    {
        if (role <= 1)
        {
            // Home defence. HLBG drains resources when a team's own guards and
            // boss die, so somebody has to stay back or the camp is free to farm.
            return urand(0, 1) ? side.ownCamp : side.ownLine;
        }

        if (role <= 6)
        {
            // Midfield. The two mixed outposts are where both infantry lines
            // meet, which is where the player kills happen.
            return urand(0, 1) ? DCHinterland::MID_CENTER : DCHinterland::MID_NORTH;
        }

        // Offence: push the enemy camp, and occasionally go for the boss, which
        // is worth 200 resources against the 5 a normal guard is worth.
        uint32 const roll = urand(0, 2);
        return roll == 0 ? side.enemyBoss : (roll == 1 ? side.enemyCamp : side.enemyLine);
    };

    // A roll that lands where the bot already stands is a wasted cycle: it would
    // hold the same spot until the next expiry, and a bot that never moves is
    // what the battleground's AFK sweep exists to remove. A bounded retry is
    // enough - the buckets a role can draw from are far enough apart that one or
    // two rolls almost always move it.
    float const rerollRadius = GetConfig().arrivalDistance * 2.0f;
    DCHinterland::Point target = rollTarget();
    for (uint32 attempt = 0; attempt < 3; ++attempt)
    {
        if (!bot->IsWithinDist3d(target.x, target.y, target.z, rerollRadius))
            break;

        target = rollTarget();
    }

    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;

    // The jitter spreads bots out so a camp does not become a stack of bodies
    // on one coordinate, but it moves x and y only, and the table's z belongs to
    // the unjittered point. On the Alliance side, which is flat ground, that is
    // harmless. On the Horde side it is not: the Revantusk village sits on a
    // wooden platform about 7 yards above the beach the team spawns on, so a
    // jittered target could end up hanging in the air off the platform edge,
    // where the path search returns INVALID_HEIGHT and MoveTo quietly does
    // nothing - for the whole 45-90s the bot had committed to that target.
    // Snapping z to whatever is actually walkable under the jittered x/y is what
    // makes the target reachable in the first place.
    float targetX = target.x + frand(-8.0f, 8.0f);
    float targetY = target.y + frand(-8.0f, 8.0f);
    float targetZ = target.z;
    bot->UpdateAllowedPositionZ(targetX, targetY, targetZ);

    PositionMap& posMap = AI_VALUE(PositionMap&, "position");
    PositionInfo objective;
    objective.Set(targetX, targetY, targetZ, bg->GetMapId());
    posMap[DC_HLBG_OBJECTIVE_KEY] = objective;

    HLBGConfig const& cfg = GetConfig();
    _lastPickMs = getMSTime();
    _repickIntervalMs = urand(cfg.repickSecondsMin, cfg.repickSecondsMax) * IN_MILLISECONDS;
    return true;
}

bool DCHinterlandTacticsAction::MoveToObjective()
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;

    HLBGConfig const& cfg = GetConfig();

    // An enemy player in reach outranks any waypoint: stop steering and let the
    // combat engine pick the target. Without this the bot walks past a fight to
    // reach a coordinate.
    GuidVector const enemies = AI_VALUE(GuidVector, "nearest enemy players");
    for (ObjectGuid const& guid : enemies)
    {
        Unit* enemy = botAI->GetUnit(guid);
        if (enemy && enemy->IsAlive() && bot->IsWithinDist(enemy, cfg.engageDistance, false))
            return false;
    }

    PositionMap& posMap = AI_VALUE(PositionMap&, "position");
    PositionInfo objective = posMap[DC_HLBG_OBJECTIVE_KEY];

    // Unsigned arithmetic, so a getMSTime() wrap resolves the same way it does
    // everywhere else in the module.
    bool const expired = _repickIntervalMs && (getMSTime() - _lastPickMs) >= _repickIntervalMs;
    bool const wrongMap = objective.isSet() && objective.mapId != bg->GetMapId();

    if (!objective.isSet() || wrongMap || expired)
    {
        if (!SelectObjective())
            return false;

        objective = posMap[DC_HLBG_OBJECTIVE_KEY];
    }

    // Arriving means hold the ground, not pick somewhere new. Rerolling the
    // moment the bot crossed into the arrival radius made it shuffle on the
    // spot: the replacement target carries up to 8 yards of jitter, well inside
    // the 18 yard arrival test, so the bot arrived again on the very next tick
    // and rerolled again. For the two home-defence points, which sit ~46 yards
    // apart in the Horde base, that read as bots pacing back and forth and never
    // leaving. The objective now only changes when the dwell timer expires.
    if (bot->IsWithinDist3d(objective.x, objective.y, objective.z, cfg.arrivalDistance))
    {
        // Standing on the objective is the job, not a stall.
        NoteProgress();
        return false;
    }

    // Still travelling. If no ground has been covered for the whole stall
    // window, the target is unreachable from here and waiting out the rest of
    // the dwell timer just burns the match - take a new one now.
    if (IsStalled())
    {
        if (!SelectObjective())
            return false;

        objective = posMap[DC_HLBG_OBJECTIVE_KEY];
    }

    return MoveTo(objective.mapId, objective.x, objective.y, objective.z);
}

void DCHinterlandTacticsAction::NoteProgress()
{
    _lastX = bot->GetPositionX();
    _lastY = bot->GetPositionY();
    _lastZ = bot->GetPositionZ();
    _hasLastPos = true;
    _lastProgressMs = getMSTime();
    _stallStrikes = 0;
}

bool DCHinterlandTacticsAction::IsStalled()
{
    HLBGConfig const& cfg = GetConfig();
    uint32 const now = getMSTime();

    // MoveTo() returning false is not the signal: it says so for three
    // different reasons - the path search failed, the bot is already heading
    // there, or the previous move's delay has not run out - and only the first
    // is a problem. Actual displacement is unambiguous.
    uint32 const sinceTick = now - _lastTickMs;
    _lastTickMs = now;

    // isUseful() blocks this action while the bot is in combat, dead or being
    // teleported, so a gap between ticks means the bot was busy elsewhere, not
    // that it stood still. Rebase instead of counting it.
    if (!_hasLastPos || !_lastProgressMs || sinceTick > 5 * IN_MILLISECONDS)
    {
        NoteProgress();
        return false;
    }

    if (bot->GetExactDist(_lastX, _lastY, _lastZ) > cfg.stallDistance)
    {
        NoteProgress();
        return false;
    }

    if (!cfg.stallSeconds || (now - _lastProgressMs) < cfg.stallSeconds * IN_MILLISECONDS)
        return false;

    ++_stallStrikes;
    DCHinterlandMonitor::NoteStall(bot, _stallStrikes);

    // Repeated strikes mean rerolling the target is not helping, so the bot is
    // not standing next to an unreachable point - it is standing somewhere
    // nothing is reachable from. The staging area is the one spot on the map the
    // battleground itself guarantees is walkable, and its own AFK sweep already
    // moves players there, so this borrows the same remedy.
    if (cfg.unstickTeleportStrikes && _stallStrikes >= cfg.unstickTeleportStrikes)
    {
        DCHinterland::Point const staging = DCHinterland::GetSideView(bot->GetBgTeamId()).ownStaging;
        float const x = staging.x + frand(-5.0f, 5.0f);
        float const y = staging.y + frand(-5.0f, 5.0f);
        float z = staging.z;
        bot->UpdateAllowedPositionZ(x, y, z);

        DCHinterlandMonitor::NoteUnstickTeleport(bot);
        bot->NearTeleportTo(x, y, z, bot->GetOrientation());
        _stallStrikes = 0;
    }

    // Whatever happened, this counts as a fresh start for the watchdog: give the
    // replacement target a full window before deciding it failed too.
    _lastX = bot->GetPositionX();
    _lastY = bot->GetPositionY();
    _lastZ = bot->GetPositionZ();
    _lastProgressMs = now;

    return true;
}

bool DCHinterlandResetObjectiveAction::isUseful()
{
    return DCHinterlandTacticsAction::IsInHinterlandBG(bot);
}

bool DCHinterlandResetObjectiveAction::Execute(Event /*event*/)
{
    // Fired on death. The bot releases to a graveyard well away from wherever
    // it was heading, so the stored waypoint is stale by definition.
    PositionMap& posMap = AI_VALUE(PositionMap&, "position");
    PositionInfo objective = posMap[DC_HLBG_OBJECTIVE_KEY];
    objective.Reset();
    posMap[DC_HLBG_OBJECTIVE_KEY] = objective;
    return true;
}
