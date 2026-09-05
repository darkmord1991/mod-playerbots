/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DESTROYITEMACTION_H
#define PLAYERBOTS_DESTROYITEMACTION_H

#include "InventoryAction.h"

class FindItemVisitor;
class PlayerbotAI;

class DestroyItemAction : public InventoryAction
{
public:
    DestroyItemAction(PlayerbotAI* botAI, std::string const name = "destroy") : InventoryAction(botAI, name) {}

    bool Execute(Event event) override;

protected:
    // silent: skip the "<item> destroyed" whisper. Housekeeping destroys run unattended and would otherwise
    // whisper the master once per item; an explicit "destroy" chat command still reports what it did.
    void DestroyItem(FindItemVisitor* visitor, bool silent = false);
};

class SmartDestroyItemAction : public DestroyItemAction
{
public:
    SmartDestroyItemAction(PlayerbotAI* botAI) : DestroyItemAction(botAI, "smart destroy") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
