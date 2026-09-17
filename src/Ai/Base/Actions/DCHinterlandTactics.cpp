/*
 * DarkChaos addition to mod-playerbots. See DCHinterlandTactics.h.
 */

#include "DCHinterlandTactics.h"

#include "DCHinterlandFront.h"
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
        float engageDistance = 60.0f;
        uint32 stallSeconds = 20;
        float stallDistance = 3.0f;
        uint32 unstickTeleportStrikes = 3;
        uint32 defenderRoles = 2;
        float nodeRadius = 45.0f;
        float presenceRadius = 60.0f;
        uint32 defenderDwellMin = 20;
        uint32 defenderDwellMax = 40;
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
            c.engageDistance = sConfigMgr->GetOption<float>("AiPlayerbot.DCHinterland.EngageDistance", 60.0f);
            c.stallSeconds = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.StallSeconds", 20);
            c.stallDistance = sConfigMgr->GetOption<float>("AiPlayerbot.DCHinterland.StallDistance", 3.0f);
            c.unstickTeleportStrikes =
                sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.UnstickTeleportStrikes", 3);
            c.defenderRoles = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.Front.DefenderRoles", 2);
            c.nodeRadius = sConfigMgr->GetOption<float>("AiPlayerbot.DCHinterland.Front.NodeRadius", 45.0f);
            c.presenceRadius = sConfigMgr->GetOption<float>("AiPlayerbot.DCHinterland.Front.PresenceRadius", 60.0f);
            c.defenderDwellMin = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.Front.DefenderDwellMin", 20);
            c.defenderDwellMax = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.Front.DefenderDwellMax", 40);

            if (c.repickSecondsMax < c.repickSecondsMin)
                c.repickSecondsMax = c.repickSecondsMin;
            if (c.defenderDwellMax < c.defenderDwellMin)
                c.defenderDwellMax = c.defenderDwellMin;

            DCHinterlandFront::Config front;
            front.advanceVotes = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.Front.AdvanceVotes", 3);
            front.holdSeconds = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.Front.HoldSeconds", 10);
            front.regroupSeconds = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.Front.RegroupSeconds", 45);
            front.voteWindowSeconds =
                sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.Front.VoteWindowSeconds", 6);
            front.advanceQuorumPct =
                sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.Front.AdvanceQuorumPct", 50);
            front.minPresence = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.Front.MinPresence", 3);
            front.defenderRoles = c.defenderRoles;
            front.presenceRadius = c.presenceRadius;
            front.losingWindowSeconds =
                sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.Front.LosingWindowSeconds", 60);
            front.losingMinDeaths = sConfigMgr->GetOption<uint32>("AiPlayerbot.DCHinterland.Front.LosingMinDeaths", 6);
            DCHinterlandFront::SetConfig(front);

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

float DCHinterlandTacticsAction::GetEngageDistance() { return GetConfig().engageDistance; }

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

bool DCHinterlandTacticsAction::IsDefender()
{
    // "bg role" is rolled 0-9 when the bot accepts the battleground port. It is
    // set for every battleground type, HLBG included, so it is free to reuse as
    // a stable per-match disposition instead of re-rolling one here. The lowest
    // DefenderRoles values stay home: HLBG drains resources when a team's own
    // guards and boss die, so somebody has to, or the camp is free to farm.
    return AI_VALUE(uint32, "bg role") < GetConfig().defenderRoles;
}

bool DCHinterlandTacticsAction::SelectObjective()
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;

    HLBGConfig const& cfg = GetConfig();
    TeamId const teamId = bot->GetBgTeamId();
    DCHinterland::Point target = DCHinterland::MID_CENTER;
    float jitter = 8.0f;
    uint32 dwellMin = cfg.repickSecondsMin;
    uint32 dwellMax = cfg.repickSecondsMax;

    if (IsDefender())
    {
        DCHinterland::SideView const side = DCHinterland::GetSideView(teamId);

        // Patrol the three home points rather than park on one. Eight
        // defenders standing in a sixteen-yard square at the camp for a minute
        // at a time was what the "stuck at the Horde base" screenshot showed,
        // and it was them doing their job. Wider spread, shorter dwell, and
        // never the point the bot is already standing on.
        DCHinterland::Point const home[] = { side.ownLine, side.ownCamp, side.ownBoss };
        uint32 pick = urand(0, 2);
        for (uint32 attempt = 0; attempt < 3; ++attempt)
        {
            if (!bot->IsWithinDist3d(home[pick].x, home[pick].y, home[pick].z, cfg.arrivalDistance * 2.0f))
                break;

            pick = (pick + 1) % 3;
        }

        target = home[pick];
        jitter = 14.0f;
        dwellMin = cfg.defenderDwellMin;
        dwellMax = cfg.defenderDwellMax;
        _objectiveIsFront = false;
    }
    else
    {
        DCHinterlandFront::View const front = DCHinterlandFront::Get(bg->GetInstanceID(), teamId, getMSTime());
        target = DCHinterlandFront::LaneNode(teamId, front.index);
        _frontRevision = front.revision;
        _objectiveIsFront = true;
    }

    // The jitter spreads bots out so a node does not become a stack of bodies
    // on one coordinate, but it moves x and y only, and the table's z belongs to
    // the unjittered point. On the Alliance side, which is flat ground, that is
    // harmless. On the Horde side it is not: the Revantusk village sits on a
    // wooden platform about 7 yards above the beach the team spawns on, so a
    // jittered target could end up hanging in the air off the platform edge,
    // where the path search returns INVALID_HEIGHT and MoveTo quietly does
    // nothing - for the whole 45-90s the bot had committed to that target.
    // Snapping z to whatever is actually walkable under the jittered x/y is what
    // makes the target reachable in the first place.
    float targetX = target.x + frand(-jitter, jitter);
    float targetY = target.y + frand(-jitter, jitter);
    float targetZ = target.z;
    bot->UpdateAllowedPositionZ(targetX, targetY, targetZ);

    PositionMap& posMap = AI_VALUE(PositionMap&, "position");
    PositionInfo objective;
    objective.Set(targetX, targetY, targetZ, bg->GetMapId());
    posMap[DC_HLBG_OBJECTIVE_KEY] = objective;

    _lastPickMs = getMSTime();
    _repickIntervalMs = urand(dwellMin, dwellMax) * IN_MILLISECONDS;
    return true;
}

void DCHinterlandTacticsAction::ReportNodeState(DCHinterland::Point const& node)
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return;

    HLBGConfig const& cfg = GetConfig();

    // "possible targets" is everything hostile the bot could attack within
    // sight distance, players and creatures alike, LOS ignored - the enemy
    // guards at an outpost and any enemy player sitting on it both count, own
    // guards do not. Filtered by distance to the node rather than to the bot,
    // so a bot standing on the far edge of the node still speaks for it.
    bool nodeClear = true;
    GuidVector const hostiles = AI_VALUE(GuidVector, "possible targets");
    for (ObjectGuid const& guid : hostiles)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive())
            continue;

        if (unit->GetExactDist2d(node.x, node.y) <= cfg.nodeRadius)
        {
            nodeClear = false;
            break;
        }
    }

    DCHinterlandFront::ReportAtNode(bg->GetInstanceID(), bot->GetBgTeamId(), bot->GetGUID().GetRawValue(),
                                    nodeClear, getMSTime());
}

void DCHinterlandTacticsAction::ReportPresence(DCHinterland::Point const& node)
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return;

    uint32 const now = getMSTime();
    if (_lastPresenceMs && (now - _lastPresenceMs) < 2 * IN_MILLISECONDS)
        return;

    _lastPresenceMs = now;

    // Living teammates - bots and real players - near the node, including the
    // ones mid-fight on it, who cannot report for themselves because this
    // action does not run in combat. The front gets a grace period after every
    // move for the group to walk up, so the radius does not have to cover the
    // approach as well.
    HLBGConfig const& cfg = GetConfig();
    TeamId const teamId = bot->GetBgTeamId();
    uint32 aliveNear = 0;
    uint32 aliveTeam = 0;

    for (auto const& itr : bg->GetPlayers())
    {
        Player* player = itr.second;
        if (!player || !player->IsAlive() || player->GetBgTeamId() != teamId)
            continue;

        ++aliveTeam;
        if (player->GetExactDist2d(node.x, node.y) <= cfg.presenceRadius)
            ++aliveNear;
    }

    DCHinterlandFront::ReportPresence(bg->GetInstanceID(), teamId, aliveNear, aliveTeam, now);
}

bool DCHinterlandTacticsAction::MoveToObjective()
{
    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;

    HLBGConfig const& cfg = GetConfig();

    TeamId const teamId = bot->GetBgTeamId();
    uint32 const now = getMSTime();
    DCHinterlandFront::View const front = DCHinterlandFront::Get(bg->GetInstanceID(), teamId, now);
    DCHinterland::Point const node = DCHinterlandFront::LaneNode(teamId, front.index);

    // Before anything that can return early. The retreat decision lives in
    // this report, and during a losing fight nearly every attacker out of
    // combat has an enemy in sight - putting the report after the enemy
    // check below starved the front of reports exactly when it needed them,
    // and a wiped team sat on a contested node for minutes.
    if (!IsDefender())
        ReportPresence(node);

    // An enemy player in sight outranks any waypoint. "attack enemy player"
    // fires ahead of this action whenever this value resolves; the only way to
    // get here with it set is that the attack could not start - no line of
    // sight, usually - so close the distance instead of walking past.
    if (Unit* enemy = AI_VALUE(Unit*, "enemy player target"))
    {
        if (enemy->IsAlive())
            return MoveTo(bg->GetMapId(), enemy->GetPositionX(), enemy->GetPositionY(), enemy->GetPositionZ());
    }

    PositionMap& posMap = AI_VALUE(PositionMap&, "position");
    PositionInfo objective = posMap[DC_HLBG_OBJECTIVE_KEY];

    // Unsigned arithmetic, so a getMSTime() wrap resolves the same way it does
    // everywhere else in the module.
    bool const expired = _repickIntervalMs && (now - _lastPickMs) >= _repickIntervalMs;
    bool const wrongMap = objective.isSet() && objective.mapId != bg->GetMapId();
    bool const frontMoved = _objectiveIsFront && front.revision != _frontRevision;

    if (!objective.isSet() || wrongMap || expired || frontMoved)
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
    // leaving. The objective only changes when the dwell timer expires or the
    // front moves.
    if (bot->IsWithinDist3d(objective.x, objective.y, objective.z, cfg.arrivalDistance))
    {
        // Standing on the objective is the job, not a stall.
        NoteProgress();

        if (_objectiveIsFront)
            ReportNodeState(node);

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

bool DCHinterlandEnemyNearTrigger::IsActive()
{
    if (!DCHinterlandTacticsAction::IsInHinterlandBG(bot))
        return false;

    return AI_VALUE(Unit*, "enemy player target") != nullptr;
}
