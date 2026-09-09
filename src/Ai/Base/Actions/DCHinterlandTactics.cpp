/*
 * DarkChaos addition to mod-playerbots. See DCHinterlandTactics.h.
 */

#include "DCHinterlandTactics.h"

#include "Battleground.h"
#include "Config.h"
#include "Event.h"
#include "Player.h"
#include "Playerbots.h"
#include "PositionValue.h"

namespace
{
    struct HLBGPoint
    {
        float x;
        float y;
        float z;
    };

    // Geography read off the live map-1411 spawn table. The battleground runs
    // in an instance, so only the coordinates are reused -- the map id comes
    // from Battleground::GetMapId() at the call site.
    //
    // Alliance holds the Wildhammer camp in the east, Horde the Revantusk /
    // Kor'kron camp in the west, and both sides garrison the two mixed outposts
    // in the middle where their infantry lines meet.
    constexpr HLBGPoint HLBG_ALLIANCE_CAMP     = { 17.0f, -4662.0f, 10.5f };
    constexpr HLBGPoint HLBG_ALLIANCE_LINE     = { -110.0f, -4617.0f, 11.0f };
    constexpr HLBGPoint HLBG_ALLIANCE_BOSS     = { 175.9f, -4736.8f, 14.7f };  // King Varian Wrynn, 810003
    // The battleground's own team start (game_graveyard 1721). Using the spot the
    // BG teleports arrivals to is guaranteed walkable and inside StartMaxDist.
    constexpr HLBGPoint HLBG_ALLIANCE_STAGING  = { 197.165f, -4808.54f, 7.848f };

    constexpr HLBGPoint HLBG_HORDE_CAMP        = { -546.0f, -4560.0f, 12.0f };
    constexpr HLBGPoint HLBG_HORDE_LINE        = { -500.0f, -4560.0f, 10.0f };
    constexpr HLBGPoint HLBG_HORDE_BOSS        = { -623.7f, -4581.6f, 11.7f };  // Thrall Warchief, 810002
    // game_graveyard 1722. The previous value was averaged off the Revantusk
    // Drummer spawns, which sit on a platform at z 30.5 - about 20 yards above
    // the surrounding ground, so Horde bots were being sent somewhere they could
    // not path to during the prep phase.
    constexpr HLBGPoint HLBG_HORDE_STAGING     = { -628.484f, -4684.51f, 5.144f };

    constexpr HLBGPoint HLBG_MID_NORTH         = { -370.0f, -4428.0f, 12.5f };
    constexpr HLBGPoint HLBG_MID_CENTER        = { -315.0f, -4510.0f, 12.5f };

    struct HLBGSideView
    {
        HLBGPoint ownCamp;
        HLBGPoint ownLine;
        HLBGPoint ownStaging;
        HLBGPoint enemyCamp;
        HLBGPoint enemyLine;
        HLBGPoint enemyBoss;
    };

    HLBGSideView GetSideView(TeamId teamId)
    {
        if (teamId == TEAM_ALLIANCE)
            return { HLBG_ALLIANCE_CAMP, HLBG_ALLIANCE_LINE, HLBG_ALLIANCE_STAGING,
                     HLBG_HORDE_CAMP, HLBG_HORDE_LINE, HLBG_HORDE_BOSS };

        return { HLBG_HORDE_CAMP, HLBG_HORDE_LINE, HLBG_HORDE_STAGING,
                 HLBG_ALLIANCE_CAMP, HLBG_ALLIANCE_LINE, HLBG_ALLIANCE_BOSS };
    }

    struct HLBGConfig
    {
        bool enabled = true;
        uint32 repickSecondsMin = 45;
        uint32 repickSecondsMax = 90;
        float arrivalDistance = 18.0f;
        float engageDistance = 18.0f;
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
    HLBGSideView const side = GetSideView(bot->GetBgTeamId());
    return MoveTo(bg->GetMapId(), side.ownStaging.x + frand(-6.0f, 6.0f), side.ownStaging.y + frand(-6.0f, 6.0f),
                  side.ownStaging.z);
}

bool DCHinterlandTacticsAction::SelectObjective()
{
    HLBGSideView const side = GetSideView(bot->GetBgTeamId());

    // "bg role" is rolled 0-9 when the bot accepts the battleground port. It is
    // set for every battleground type, HLBG included, so it is free to reuse as
    // a stable per-match disposition instead of re-rolling one here.
    uint32 const role = AI_VALUE(uint32, "bg role");

    auto rollTarget = [&side, role]() -> HLBGPoint
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
            return urand(0, 1) ? HLBG_MID_CENTER : HLBG_MID_NORTH;
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
    HLBGPoint target = rollTarget();
    for (uint32 attempt = 0; attempt < 3; ++attempt)
    {
        if (!bot->IsWithinDist3d(target.x, target.y, target.z, rerollRadius))
            break;

        target = rollTarget();
    }

    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return false;

    PositionMap& posMap = AI_VALUE(PositionMap&, "position");
    PositionInfo objective;
    objective.Set(target.x + frand(-8.0f, 8.0f), target.y + frand(-8.0f, 8.0f), target.z, bg->GetMapId());
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
        return false;

    return MoveTo(objective.mapId, objective.x, objective.y, objective.z);
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
