/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_POSSIBLERPGTARGETSVALUE_H
#define PLAYERBOTS_POSSIBLERPGTARGETSVALUE_H

#include "NearestGameObjects.h"
#include "NearestUnitsValue.h"
#include "PlayerbotAIConfig.h"
#include "SharedDefines.h"

class PlayerbotAI;

class PossibleRpgTargetsValue : public NearestUnitsValue
{
public:
    PossibleRpgTargetsValue(PlayerbotAI* botAI, float range = 70.0f);

    static const std::vector<uint32> allowedNpcFlags;

protected:
    void FindUnits(std::list<Unit*>& targets) override;
    bool AcceptUnit(Unit* unit) override;
};

// New-rpg scans are read every tick, but NPCs and quest objects do not move and every consumer re-checks the GUID.
// Adapted from SquidBots (Zyth45/mod-playerbots f40d14fd, after jeddhor ef0362bd).
constexpr uint32 NEW_RPG_SCAN_INTERVAL_SECONDS = 2;

class PossibleNewRpgTargetsValue : public NearestUnitsValue
{
public:
    PossibleNewRpgTargetsValue(PlayerbotAI* botAI);

    // Candidate-cache radius: Playerbots.RpgTargetSearchDistance above 0, otherwise the map visibility range.
    static float GetSearchRange(Player* bot);

    static const std::vector<uint32> allowedNpcFlags;
    GuidVector Calculate() override;
protected:
    void FindUnits(std::list<Unit*>& targets) override;
    bool AcceptUnit(Unit* unit) override;
};

class PossibleNewRpgGameObjectsValue : public ObjectGuidListCalculatedValue
{
public:
    PossibleNewRpgGameObjectsValue(PlayerbotAI* botAI, bool ignoreLos = true)
        : ObjectGuidListCalculatedValue(botAI, "possible new rpg game objects", NEW_RPG_SCAN_INTERVAL_SECONDS),
          ignoreLos(ignoreLos)
    {
    }

    static const std::vector<GameobjectTypes> allowedGOFlags;
    GuidVector Calculate() override;

private:
    bool ignoreLos;
};

#endif
