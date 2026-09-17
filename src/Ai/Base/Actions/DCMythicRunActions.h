/*
 * DarkChaos addition to mod-playerbots.
 *
 * The tank of a bot-only dungeon run (see DCBotMythicRun.h), keyed or not, leads
 * the group through the dungeon. It waits until every member is alive, close and
 * rested; then pulls the nearest hostile it can see within range, or otherwise
 * walks toward the next boss the run still needs. Everyone else follows the tank
 * (their master) and joins its fights through the ordinary tank/dps assist and
 * heal strategies, which pick targets from the group's threat lists.
 */

#ifndef PLAYERBOTS_DCMYTHICRUNACTIONS_H
#define PLAYERBOTS_DCMYTHICRUNACTIONS_H

#include "AttackAction.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "Trigger.h"

#include "DCBotMythicRun.h"

#include <unordered_map>

class PlayerbotAI;
class Unit;

class DCMythicAdvanceAction : public AttackAction
{
public:
    DCMythicAdvanceAction(PlayerbotAI* botAI) : AttackAction(botAI, "dc mythic advance") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    bool IsGroupReady(DCBotMythicRun::Config const& cfg);
    Unit* FindPullTarget(float range);
    bool FindNextBoss(Position& out);
    // A pull that never turned into a fight - the target is still alive, out
    // of combat and within reach, or the tank gave up walking to it - is
    // skipped for a while so the next candidate gets its turn. Gated pulls do
    // this: a Utgarde Keep Forge Master engaged out of order evades on the spot.
    void CheckLastPull();

    // Per bot: the action object exists once per bot AI context, used only on
    // that bot's map thread.
    Position _bossPosition;
    bool _hasBossPosition = false;
    uint32 _nextBossScanMs = 0;
    ObjectGuid _lastPull;
    uint32 _lastPullMs = 0;
    std::unordered_map<ObjectGuid, uint32> _refusedUntilMs;
};

// Active for the run's tank while the run is under way. Checked every tick.
class DCMythicLeadTrigger : public Trigger
{
public:
    DCMythicLeadTrigger(PlayerbotAI* botAI) : Trigger(botAI, "dc mythic lead", 1) {}

    bool IsActive() override;
};

#endif
