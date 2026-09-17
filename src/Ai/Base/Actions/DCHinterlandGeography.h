/*
 * DarkChaos addition to mod-playerbots.
 *
 * Shared map-1411 geography for the Hinterland BG bot code. This was local to
 * DCHinterlandTactics.cpp until the battleground chatter script needed to name
 * the same places the roaming action walks to: two copies of the table would
 * eventually drift and have bots calling out a spot they never actually go to.
 *
 * Coordinates were read off the live map-1411 spawn table. The battleground
 * runs in an instance, so only the coordinates are reused -- the map id comes
 * from Battleground::GetMapId() at the call site.
 *
 * The `modules` and `scripts` libraries are siblings and do not link each
 * other, so nothing here may reach into BattlegroundHLBG. The battleground is
 * addressed by the numeric type id in DCHinterlandTactics.h and its geography
 * is this local table.
 */

#ifndef PLAYERBOTS_DCHINTERLANDGEOGRAPHY_H
#define PLAYERBOTS_DCHINTERLANDGEOGRAPHY_H

#include "SharedDefines.h"

namespace DCHinterland
{
    struct Point
    {
        float x;
        float y;
        float z;
        // Spoken form, used verbatim by the chatter lines. Written to read
        // naturally after "at", "to" and "near".
        char const* name;
    };

    // Alliance holds the Wildhammer camp in the east, Horde the Revantusk /
    // Kor'kron camp in the west, and both sides garrison the two mixed outposts
    // in the middle where their infantry lines meet.
    inline constexpr Point ALLIANCE_CAMP = { 17.0f, -4662.0f, 10.5f, "the Wildhammer camp" };
    inline constexpr Point ALLIANCE_LINE = { -110.0f, -4617.0f, 11.0f, "the Alliance line" };
    inline constexpr Point ALLIANCE_BOSS = { 175.9f, -4736.8f, 14.7f, "Varian's camp" };  // King Varian Wrynn, 810003
    // The battleground's own team start (game_graveyard 1721). Using the spot the
    // BG teleports arrivals to is guaranteed walkable and inside StartMaxDist.
    inline constexpr Point ALLIANCE_STAGING = { 197.165f, -4808.54f, 7.848f, "the Alliance base" };

    inline constexpr Point HORDE_CAMP = { -546.0f, -4560.0f, 12.0f, "the Revantusk camp" };
    inline constexpr Point HORDE_LINE = { -500.0f, -4560.0f, 10.0f, "the Horde line" };
    inline constexpr Point HORDE_BOSS = { -623.7f, -4581.6f, 11.7f, "Thrall's camp" };  // Thrall Warchief, 810002
    // game_graveyard 1722. The previous value was averaged off the Revantusk
    // Drummer spawns, which sit on a platform at z 30.5 - about 20 yards above
    // the surrounding ground, so Horde bots were being sent somewhere they could
    // not path to during the prep phase.
    inline constexpr Point HORDE_STAGING = { -628.484f, -4684.51f, 5.144f, "the Horde base" };

    inline constexpr Point MID_NORTH = { -370.0f, -4428.0f, 12.5f, "the north outpost" };
    inline constexpr Point MID_CENTER = { -315.0f, -4510.0f, 12.5f, "the middle" };

    struct SideView
    {
        Point ownCamp;
        Point ownLine;
        Point ownStaging;
        Point ownBoss;
        Point enemyCamp;
        Point enemyLine;
        Point enemyBoss;
    };

    inline SideView GetSideView(TeamId teamId)
    {
        if (teamId == TEAM_ALLIANCE)
            return { ALLIANCE_CAMP, ALLIANCE_LINE, ALLIANCE_STAGING, ALLIANCE_BOSS,
                     HORDE_CAMP,    HORDE_LINE,    HORDE_BOSS };

        return { HORDE_CAMP,    HORDE_LINE,    HORDE_STAGING, HORDE_BOSS,
                 ALLIANCE_CAMP, ALLIANCE_LINE, ALLIANCE_BOSS };
    }

    // Everything a bot can sensibly name out loud. The two staging areas are in
    // here because a bot that just released at its graveyard is standing there.
    inline constexpr Point const LANDMARKS[] = {
        ALLIANCE_CAMP, ALLIANCE_LINE, ALLIANCE_BOSS, ALLIANCE_STAGING,
        HORDE_CAMP,    HORDE_LINE,    HORDE_BOSS,    HORDE_STAGING,
        MID_NORTH,     MID_CENTER
    };

    // Nearest named landmark to a position, for chatter that calls out where
    // something happened. Squared 2D distance is enough: the landmarks are far
    // apart horizontally and the z spread across the valley is small next to
    // the gaps between them, so the extra axis never changes the winner.
    inline char const* NearestLandmarkName(float x, float y)
    {
        char const* best = LANDMARKS[0].name;
        float bestDistSq = -1.0f;

        for (Point const& point : LANDMARKS)
        {
            float const dx = point.x - x;
            float const dy = point.y - y;
            float const distSq = dx * dx + dy * dy;

            if (bestDistSq < 0.0f || distSq < bestDistSq)
            {
                bestDistSq = distSq;
                best = point.name;
            }
        }

        return best;
    }
}

#endif
