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
 * their faction boss. So this is deliberately NOT another case in BGTactics.
 *
 * What the action does is steer. A team's attackers all head for the same
 * node of a lane that runs from their own line, through the midfield
 * outposts, to the enemy line, camp and boss (see DCHinterlandFront.h); the
 * lane advances when the bots on the node report it clear and falls back when
 * they have been wiped off it. A fixed share of the roster stays home and
 * holds the team's own camp and line. Any enemy player the bot can see is
 * approached rather than walked past. The ordinary combat engine and the
 * "pvp" / "dps assist" / "attack tagged" strategies that AiFactory already adds
 * inside a battleground do the actual fighting.
 *
 * The `modules` and `scripts` libraries are siblings and do not link each
 * other, so nothing here may reach into BattlegroundHLBG. The battleground is
 * addressed by its numeric type id and its geography is a local table.
 */

#ifndef PLAYERBOTS_DCHINTERLANDTACTICS_H
#define PLAYERBOTS_DCHINTERLANDTACTICS_H

#include "DCHinterlandGeography.h"
#include "MovementActions.h"
#include "Trigger.h"

class PlayerbotAI;

// BattlegroundTypeId(20) / BattlegroundQueueTypeId(14), declared in the DC
// scripts tree as BATTLEGROUND_HLBG. Repeated as a literal because that header
// lives behind the scripts/modules library boundary.
#define DC_BATTLEGROUND_HLBG_TYPE_ID 20
#define DC_BATTLEGROUND_HLBG_MAP_ID 1411

// Slot in the bot's PositionMap holding the current movement target.
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

    // Yards at which a bot in HLBG starts on an enemy player it can see.
    // Read by EnemyPlayerValue, which otherwise uses the stock 40y battleground
    // figure - and halves it whenever the bot's raw health is below the
    // target's, which in a 40-a-side fight is most of the time.
    static float GetEngageDistance();

private:
    bool MoveToStaging();
    bool MoveToObjective();
    // Chooses the bot's movement target - the team's current front node for
    // attackers, the own camp or line for defenders - and stores it in the
    // bot's PositionMap under "dc hlbg objective". Returns false when no target
    // could be picked.
    bool SelectObjective();

    // True for the share of the roster that stays home. Derived from "bg role",
    // which is rolled 0-9 once per battleground, so it is stable for the match.
    bool IsDefender();

    // Attackers standing on the front node tell the front whether any hostile
    // is still around it; everyone tells it how many living teammates are near
    // it. See DCHinterlandFront.h for what the front does with both.
    void ReportNodeState(DCHinterland::Point const& node);
    void ReportPresence(DCHinterland::Point const& node);

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
    // The front revision the current objective was picked against, so a lane
    // advance or retreat is noticed on the next tick rather than at the next
    // dwell expiry.
    uint32 _frontRevision = 0;
    bool _objectiveIsFront = false;
    uint32 _lastPresenceMs = 0;

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

// The stock "enemy player near" trigger is only evaluated every 3 seconds. At
// battleground run speed that is over 20 yards between looks, so two bots
// crossing at 30-40 yards were routinely never inside each other's engage
// range at a moment either of them was looking - the "run past each other in
// parallel" the match report kept describing. Same value, same action, checked
// every tick instead. Only wired into the HLBG strategy.
class DCHinterlandEnemyNearTrigger : public Trigger
{
public:
    DCHinterlandEnemyNearTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dc hlbg enemy near", 1) {}

    bool IsActive() override;
};

#endif
