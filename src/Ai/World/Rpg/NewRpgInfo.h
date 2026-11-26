/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGINFO_H
#define PLAYERBOTS_NEWRPGINFO_H

#include <unordered_set>

#include "Define.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "QuestDef.h"
#include "Strategy.h"
#include "Timer.h"
#include "TravelMgr.h"

#include <array>
#include <string>
#include <unordered_map>

using NewRpgStatusTransitionProb = std::vector<std::vector<int>>;

struct NewRpgInfo
{
    NewRpgInfo() : data(Idle{}) {}
    ~NewRpgInfo() = default;

    // RPG_GO_GRIND
    struct GoGrind
    {
        WorldPosition pos{};
    };
    // RPG_GO_CAMP
    struct GoCamp
    {
        WorldPosition pos{};
    };
    // RPG_WANDER_NPC
    struct WanderNpc
    {
        ObjectGuid npcOrGo{};
        uint32 lastReach{0};
    };
    // RPG_WANDER_RANDOM
    struct WanderRandom
    {
        WanderRandom() = default;
    };
    // RPG_DO_QUEST
    struct DoQuest
    {
        Quest const* quest{nullptr};
        uint32 questId{0};
        int32 objectiveIdx{0};
        WorldPosition pos{};
        uint32 lastReachPOI{0};
        // The quest is not in the log yet: pos is its quest giver, and the bot goes there to take it.
        bool pickUp{false};
        // Last time the bot reported standing in one of the quest's exploration triggers.
        uint32 exploreReportedAt{0};
    };
    // RPG_TRAVEL_FLIGHT
    struct TravelFlight
    {
        uint32 flightMasterEntry{0};
        WorldPosition flightMasterPos{};
        std::vector<uint32> path;
        bool inFlight{false};
    };
    // RPG_REST
    struct Rest
    {
        Rest() = default;
    };
    // RPG_OUTDOOR_PVP
    struct OutdoorPvP
    {
        ObjectGuid::LowType capturePointSpawnId{0};
    };
    // RPG_DO_GATHER
    struct DoGather
    {
        ObjectGuid::LowType nodeSpawnId{0};
        WorldPosition nodePos{};
        std::unordered_set<ObjectGuid::LowType> visited;
        uint32 lastReach{0};
        uint32 lastPassiveCheck{0};
        ObjectGuid::LowType lastSwitchedFrom{0};
    };
    struct Idle
    {
    };

    uint32 startT{0};  // start timestamp of the current status

    // A GO_GRIND or GO_CAMP destination search that found nothing, with where the bot stood when it ran.
    // While the bot stays there at the same level the search cannot succeed, so the status update skips
    // that status for a while instead of rescanning every destination on each status change.
    // flightSelectionFailure records a TRAVEL_FLIGHT whose flight master was missing at its spawn.
    struct SelectionFailure
    {
        uint32 failedAt{0};
        uint32 mapId{0};
        uint32 zoneId{0};
        uint8 level{0};
        float x{0.0f};
        float y{0.0f};
    };
    SelectionFailure grindSelectionFailure;
    SelectionFailure campSelectionFailure;
    SelectionFailure flightSelectionFailure;

    // MOVE_FAR
    float nearestMoveFarDis{FLT_MAX};
    uint32 stuckTs{0};
    uint32 stuckAttempts{0};
    // When MoveFarTo last ran (getMSTime): a long gap between two calls is no time stuck.
    uint32 moveFarLastMs{0};
    WorldPosition moveFarPos;
    // END MOVE_FAR

    // QUEST GIVER
    ObjectGuid questGiver;  // giver the bot is walking to or talking with
    uint32 questGiverSince{0};
    uint32 questGiverLastTry{0};
    uint32 questLogOrganizedAt{0};  // last clean-up of a full quest log, see NewRpgBaseAction::OrganizeQuestLog
    GuidSet ignoredQuestGivers;  // skipped until the next IDLE
    // END QUEST GIVER

    using RpgData = std::variant<
        Idle,
        GoGrind,
        GoCamp,
        WanderNpc,
        WanderRandom,
        DoQuest,
        Rest,
        TravelFlight,
        OutdoorPvP,
        DoGather
    >;
    RpgData data;

    NewRpgStatus GetStatus();
    static NewRpgStatus StatusFromString(std::string const& name);
    bool HasStatusPersisted(uint32 maxDuration) { return GetMSTimeDiffToNow(startT) > maxDuration; }
    void ChangeToGoGrind(WorldPosition pos);
    void ChangeToGoCamp(WorldPosition pos);
    void ChangeToWanderNpc();
    void ChangeToWanderRandom();
    void ChangeToDoQuest(uint32 questId, Quest const* quest);
    void ChangeToPickUpQuest(uint32 questId, Quest const* quest, WorldPosition giverPos);
    void ChangeToTravelFlight(uint32 flightMasterEntry, WorldPosition flightMasterPos, std::vector<uint32> path);
    void ChangeToOutdoorPvp(ObjectGuid::LowType capturePointSpawnId = 0);
    void ChangeToDoGather();
    void ChangeToRest();
    void ChangeToIdle();
    bool CanChangeTo(NewRpgStatus status);
    void Reset();
    void SetMoveFarTo(WorldPosition pos);
    std::string ToString();
};

enum class QuestDropReason : uint8
{
    Trivial,
    NotWorth,
    NotCapable,
    Failed,
    OtherZone,
    ClassOrHoliday,
    LogFull,
    Count
};

enum class QuestAbandonReason : uint8
{
    NoProgress,
    CannotTurnIn,
    Count
};

// DarkChaos: the bot holds the quest but cannot work on it. Counted once per bot and quest, and kept apart from
// the abandon counts so the totals keep meaning what upstream reports.
enum class QuestStuckReason : uint8
{
    NoPoi,         // no POI or spawn to go to for an unfinished objective
    NoTurnInPoi,   // finished, but no POI or spawn of the turn-in on this map
    GiverTimeout,  // stood at the giver and was never offered the quest
    Count
};

inline char const* QuestStuckReasonName(QuestStuckReason reason)
{
    switch (reason)
    {
        case QuestStuckReason::NoPoi:
            return "no_poi";
        case QuestStuckReason::NoTurnInPoi:
            return "no_turn_in_poi";
        case QuestStuckReason::GiverTimeout:
            return "giver_timeout";
        default:
            return "unknown";
    }
}

inline char const* QuestDropReasonName(QuestDropReason reason)
{
    switch (reason)
    {
        case QuestDropReason::Trivial:
            return "trivial";
        case QuestDropReason::NotWorth:
            return "not_worth";
        case QuestDropReason::NotCapable:
            return "not_capable";
        case QuestDropReason::Failed:
            return "failed";
        case QuestDropReason::OtherZone:
            return "other_zone";
        case QuestDropReason::ClassOrHoliday:
            return "class_or_holiday";
        case QuestDropReason::LogFull:
            return "log_full";
        default:
            return "unknown";
    }
}

inline char const* QuestAbandonReasonName(QuestAbandonReason reason)
{
    switch (reason)
    {
        case QuestAbandonReason::NoProgress:
            return "no_progress";
        case QuestAbandonReason::CannotTurnIn:
            return "cannot_turn_in";
        default:
            return "unknown";
    }
}

struct QuestCounters
{
    uint32 accepted{0};
    uint32 completed{0};
    uint32 rewarded{0};
    std::array<uint32, static_cast<size_t>(QuestDropReason::Count)> dropped{};
    std::array<uint32, static_cast<size_t>(QuestAbandonReason::Count)> abandoned{};
    std::array<uint32, static_cast<size_t>(QuestStuckReason::Count)> stuck{};
};

struct NewRpgStatistic
{
    uint32 questAccepted{0};
    uint32 questCompleted{0};
    uint32 questAbandoned{0};
    uint32 questRewarded{0};
    uint32 questDropped{0};
    // Filled only while CollectDetailedQuestStats is enabled.
    std::unordered_map<uint32, QuestCounters> byQuest;

    NewRpgStatistic& operator+=(NewRpgStatistic const& other)
    {
        questAccepted += other.questAccepted;
        questCompleted += other.questCompleted;
        questAbandoned += other.questAbandoned;
        questRewarded += other.questRewarded;
        questDropped += other.questDropped;

        for (auto const& [questId, counters] : other.byQuest)
        {
            QuestCounters& target = byQuest[questId];
            target.accepted += counters.accepted;
            target.completed += counters.completed;
            target.rewarded += counters.rewarded;
            for (size_t i = 0; i < target.dropped.size(); ++i)
                target.dropped[i] += counters.dropped[i];
            for (size_t i = 0; i < target.abandoned.size(); ++i)
                target.abandoned[i] += counters.abandoned[i];
            for (size_t i = 0; i < target.stuck.size(); ++i)
                target.stuck[i] += counters.stuck[i];
        }
        return *this;
    }
};

#endif
