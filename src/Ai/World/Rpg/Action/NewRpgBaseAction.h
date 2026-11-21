/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGBASEACTION_H
#define PLAYERBOTS_NEWRPGBASEACTION_H

#include "LastMovementValue.h"
#include "MovementActions.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "TravelMgr.h"
#include <optional>

struct POIInfo
{
    G3D::Vector2 pos;
    int32 objectiveIdx;
    // Known height, for a position taken from a spawn. Without it the caller looks the ground up.
    std::optional<float> z{};
};

/// A base (composition) class for all new rpg actions
/// All functions that may be shared by multiple actions should be declared here
/// And we should make all actions composable instead of inheritable
class NewRpgBaseAction : public MovementAction
{
public:
    NewRpgBaseAction(PlayerbotAI* botAI, std::string name) : MovementAction(botAI, name) {}

protected:
    /* MOVEMENT RELATED */
    bool MoveFarTo(WorldPosition dest);
    bool MoveWorldObjectTo(ObjectGuid guid, float distance = INTERACTION_DISTANCE);
    bool MoveRandomNear(float moveStep = 50.0f, MovementPriority priority = MovementPriority::MOVEMENT_NORMAL, WorldObject* center = nullptr);
    bool ForceToWait(uint32 duration, MovementPriority priority = MovementPriority::MOVEMENT_NORMAL);

    /* QUEST RELATED CHECK */
    bool InteractWithNpcOrGameObjectForQuest(ObjectGuid guid);
    bool IsWithinInteractionDist(Object* object);
    uint32 BestRewardIndex(Quest const* quest);

    /* QUEST RELATED ACTION */
    bool SearchQuestGiverAndAcceptOrReward();
    bool AcceptQuest(Quest const* quest, ObjectGuid guid);
    bool TurnInQuest(Quest const* quest, ObjectGuid guid);
    bool OrganizeQuestLog();

protected:
    bool GetQuestPOIPosAndObjectiveIdx(uint32 questId, std::vector<POIInfo>& poiInfo, bool toComplete = false);
    static WorldPosition SelectRandomGrindPos(Player* bot);
    static WorldPosition SelectRandomCampPos(Player* bot);
    bool SelectRandomFlightTaxiNode(uint32& flightMasterEntry, WorldPosition& flightMasterPos, std::vector<uint32>& path);
    bool RandomChangeStatus(std::vector<NewRpgStatus> candidateStatus);
    bool CheckRpgStatusAvailable(NewRpgStatus status);
    bool IsSelectionFailureRecent(NewRpgInfo::SelectionFailure const& failure) const;
    void RememberSelectionFailure(NewRpgInfo::SelectionFailure& failure) const;

    /* DARKCHAOS: AZSHARA CRATER QUESTLINE, see DCCraterQuestline */
    bool FollowCraterQuestline();
    bool GetCraterQuestlinePos(Quest const* quest, std::vector<POIInfo>& poiInfo, bool toComplete);
    bool CanPickUpCraterQuest(Quest const* quest);

    /* DARKCHAOS: LEVELING CONTINENTS, see DCQuestSpawnIndex */
    // POI positions from the map's own spawns, for a quest with no usable POI on the bot's map.
    bool GetDCQuestSpawnPos(Quest const* quest, std::vector<POIInfo>& poiInfo, bool toComplete);
    // A POI stored for the stock map counts on its DarkChaos copy (Playerbots.DCLevelAreas.QuestPoiMapAlias).
    bool IsPoiOnBotMap(uint32 poiMapId) const;

    // Destinations found by CheckRpgStatusAvailable, reused by RandomChangeStatus for the status it picks
    // so the search does not run twice in one status change.
    WorldPosition availableGrindPos;
    WorldPosition availableCampPos;

protected:
    /* FOR MOVE FAR */
    const float pathFinderDis = 70.0f;
    // Time without real progress toward dest before MoveFarTo
    // falls back to teleport recovery. Kept short enough that a
    // bot truly oscillating around an unreachable destination
    // (mmap returning non-progressing partial paths, or NOPATH +
    // cone fallback wandering) doesn't spin for 5 minutes before
    // the teleport fires, but long enough that a genuine long
    // walk that is slowly making progress never triggers it.
    const uint32 stuckTime = 90 * IN_MILLISECONDS;

    /* FOR DESTINATION SELECTION */
    // A GO_GRIND / GO_CAMP search that found nothing is retried after this long, or sooner once the bot
    // has moved this far, changed map or zone, or levelled up.
    const uint32 selectionFailureRetryTime = 5 * MINUTE * IN_MILLISECONDS;
    const float selectionFailureRetryDistance = 100.0f;
    /* FOR QUEST GIVER */
    // Time on one giver, walking and interacting, after which it is skipped until the next IDLE.
    const uint32 questGiverGiveUpTime = 60 * IN_MILLISECONDS;
    // A longer pause (combat, eating) starts the timer over.
    const uint32 questGiverRetryGap = 5 * IN_MILLISECONDS;
    // DarkChaos: a log that stayed full after a clean-up is not walked again sooner than this.
    const uint32 questLogOrganizeGap = 30 * IN_MILLISECONDS;
    // A turn-in spawn this close (2D) to a turn-in point is where the bot goes, at the spawn's height.
    const float turnInSpawnRadius = 60.0f;
    // An objective target spawn this close (2D) to an objective point, standing more than objectiveHeightGap off
    // the ground height there (in a cave or on a platform), is where the bot goes instead.
    const float objectiveSpawnRadius = 80.0f;
    const float objectiveHeightGap = 8.0f;
};

#endif
