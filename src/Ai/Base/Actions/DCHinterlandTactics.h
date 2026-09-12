/*
 * DarkChaos addition to mod-playerbots.
 *
 * Hinterland BG (BattlegroundTypeId 20, map 1411) is a DC custom battleground.
 * The queue side already works unmodified -- the core registers type 20 in
 * BattlegroundMgr::queueToBg/bgToQueue, so BGJoinAction's scan over the queue
 * types picks it up like any other bracket. What was missing is what happens
 * after the port: AiFactory only hands out the generic "battleground" strategy
 * for bgType <= BATTLEGROUND_EY or IC, and BGTactics dispatches on hardcoded
 * per-BG waypoint tables. HLBG matched neither, so bots landed on their start
 * point and stood there until the battleground's own AFK sweep removed them.
 *
 * HLBG is not an objective battleground: there are no flags, no capturable
 * nodes and no graveyard ownership. Both sides start with a resource pool and
 * drain the enemy's by killing their players, their guard camps and finally
 * their faction boss. So this is deliberately NOT another case in BGTactics --
 * it needs roaming, not objective capture. All this action does is keep the bot
 * walking towards somewhere worth fighting; the ordinary combat engine and the
 * "pvp" / "dps assist" / "attack tagged" strategies that AiFactory already adds
 * inside a battleground handle the fighting.
 *
 * The `modules` and `scripts` libraries are siblings and do not link each
 * other, so nothing here may reach into BattlegroundHLBG. The battleground is
 * addressed by its numeric type id and its geography is a local table.
 */

#ifndef PLAYERBOTS_DCHINTERLANDTACTICS_H
#define PLAYERBOTS_DCHINTERLANDTACTICS_H

#include "MovementActions.h"

class PlayerbotAI;

// BattlegroundTypeId(20) / BattlegroundQueueTypeId(14), declared in the DC
// scripts tree as BATTLEGROUND_HLBG. Repeated as a literal because that header
// lives behind the scripts/modules library boundary.
#define DC_BATTLEGROUND_HLBG_TYPE_ID 20
#define DC_BATTLEGROUND_HLBG_MAP_ID 1411

// Slot in the bot's PositionMap holding the current roaming target.
// Deliberately distinct from "bg objective", which BGTactics owns -- the two
// must never fight over the same slot if a bot somehow carries both strategies.
// BGStatusAction::LeaveBG clears it alongside the BGTactics one.
#define DC_HLBG_OBJECTIVE_KEY "dc hlbg objective"

class DCHinterlandTacticsAction : public MovementAction
{
public:
    DCHinterlandTacticsAction(PlayerbotAI* botAI, std::string const name = "dc hinterland tactics")
        : MovementAction(botAI, name)
    {
    }

    bool Execute(Event event) override;
    bool isUseful() override;

    // True when the bot is inside a Hinterland BG instance. Used by isUseful
    // and by the strategy wiring in AiFactory.
    static bool IsInHinterlandBG(Player* player);

private:
    bool MoveToStaging();
    bool MoveToObjective();
    // Chooses the next roaming target and stores it in the bot's PositionMap
    // under "dc hlbg objective". Returns false when no target could be picked.
    bool SelectObjective();

    // Progress watchdog. True when the bot has been outside its objective's
    // arrival radius without covering any ground for the configured window,
    // i.e. it is wedged rather than holding position. Escalates to a teleport
    // back to the team's staging area after repeated strikes, and reports both
    // to DCHinterlandMonitor.
    bool IsStalled();
    void NoteProgress();

    // Per-bot, because the action object is created once per bot AI context.
    uint32 _lastPickMs = 0;
    uint32 _repickIntervalMs = 0;

    // Watchdog state. _lastTickMs separates "has not moved" from "has not been
    // asked to move": the action does not run while the bot is in combat, dead
    // or mid-teleport, and coming back from any of those must not read as a
    // stall.
    float _lastX = 0.0f;
    float _lastY = 0.0f;
    float _lastZ = 0.0f;
    bool _hasLastPos = false;
    uint32 _lastProgressMs = 0;
    uint32 _lastTickMs = 0;
    uint32 _stallStrikes = 0;
};

class DCHinterlandResetObjectiveAction : public Action
{
public:
    DCHinterlandResetObjectiveAction(PlayerbotAI* botAI, std::string const name = "dc hinterland reset objective")
        : Action(botAI, name)
    {
    }

    bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
