/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PRIESTTRIGGERS_H
#define PLAYERBOTS_PRIESTTRIGGERS_H

#include "CureTriggers.h"
#include "SharedDefines.h"
#include "Trigger.h"
#include <set>

class PlayerbotAI;

DEBUFF_CHECKISOWNER_TRIGGER(HolyFireTrigger, "holy fire");

// The 8 s default rarely passes: the group dps estimate it divides by runs several times above a solo bot's damage.
inline constexpr float SHADOW_DOT_MIN_LIFETIME = 0.5f;

class ShadowWordPainTrigger : public DebuffTrigger
{
public:
    ShadowWordPainTrigger(PlayerbotAI* botAI)
        : DebuffTrigger(botAI, "shadow word: pain", 1, true, SHADOW_DOT_MIN_LIFETIME)
    {
    }
};

DEBUFF_ENEMY_TRIGGER(ShadowWordPainOnAttackerTrigger, "shadow word: pain");

class VampiricTouchTrigger : public DebuffTrigger
{
public:
    VampiricTouchTrigger(PlayerbotAI* botAI) : DebuffTrigger(botAI, "vampiric touch", 1, true, SHADOW_DOT_MIN_LIFETIME)
    {
    }
};

DEBUFF_ENEMY_TRIGGER(VampiricTouchOnAttackerTrigger, "vampiric touch on attacker");
BUFF_TRIGGER(VampiricEmbraceTrigger, "vampiric embrace");
CURE_TRIGGER(DispelMagicTrigger, "dispel magic", DISPEL_MAGIC);
CURE_PARTY_TRIGGER(DispelMagicPartyMemberTrigger, "dispel magic", DISPEL_MAGIC);
CURE_TRIGGER(CureDiseaseTrigger, "cure disease", DISPEL_DISEASE);
CURE_PARTY_TRIGGER(PartyMemberCureDiseaseTrigger, "cure disease", DISPEL_DISEASE);
BUFF_TRIGGER_A(InnerFireTrigger, "inner fire");
BUFF_TRIGGER_A(ShadowformTrigger, "shadowform");
BOOST_TRIGGER(PowerInfusionTrigger, "power infusion");
BUFF_TRIGGER(InnerFocusTrigger, "inner focus");
CC_TRIGGER(ShackleUndeadTrigger, "shackle undead");
INTERRUPT_TRIGGER(SilenceTrigger, "silence");
INTERRUPT_HEALER_TRIGGER(SilenceEnemyHealerTrigger, "silence");

class DevouringPlagueTrigger : public DebuffTrigger
{
public:
    DevouringPlagueTrigger(PlayerbotAI* botAI)
        : DebuffTrigger(botAI, "devouring plague", 1, true, SHADOW_DOT_MIN_LIFETIME)
    {
    }
};

BUFF_TRIGGER(TouchOfWeaknessTrigger, "touch of weakness");
DEBUFF_TRIGGER(HexOfWeaknessTrigger, "hex of weakness");
BUFF_TRIGGER(ShadowguardTrigger, "shadowguard");
DEFLECT_TRIGGER(FeedbackTrigger, "feedback");
SNARE_TRIGGER(ChastiseTrigger, "chastise");
BOOST_TRIGGER_A(ShadowfiendTrigger, "shadowfiend");

class ShadowProtectionTrigger : public BuffTrigger
{
public:
    ShadowProtectionTrigger(PlayerbotAI* botAI)
        : BuffTrigger(botAI, "shadow protection", 8 * IN_MILLISECONDS) {}

    bool IsActive() override;
};

class ShadowProtectionOnPartyTrigger : public BuffOnPartyTrigger
{
public:
    ShadowProtectionOnPartyTrigger(PlayerbotAI* botAI)
        : BuffOnPartyTrigger(botAI, "shadow protection", 8 * IN_MILLISECONDS) {}
};

class PowerWordFortitudeOnPartyTrigger : public BuffOnPartyTrigger
{
public:
    PowerWordFortitudeOnPartyTrigger(PlayerbotAI* botAI)
        : BuffOnPartyTrigger(botAI, "power word: fortitude", 8 * IN_MILLISECONDS) {}
};

class PowerWordFortitudeTrigger : public BuffTrigger
{
public:
    PowerWordFortitudeTrigger(PlayerbotAI* botAI)
        : BuffTrigger(botAI, "power word: fortitude", 8 * IN_MILLISECONDS) {}

    bool IsActive() override;
};

class DivineSpiritOnPartyTrigger : public BuffOnPartyTrigger
{
public:
    DivineSpiritOnPartyTrigger(PlayerbotAI* botAI)
        : BuffOnPartyTrigger(botAI, "divine spirit", 8 * IN_MILLISECONDS) {}
};

class DivineSpiritTrigger : public BuffTrigger
{
public:
    DivineSpiritTrigger(PlayerbotAI* botAI)
        : BuffTrigger(botAI, "divine spirit", 8 * IN_MILLISECONDS) {}

    bool IsActive() override;
};

class FearWardOnMainTankTrigger : public BuffOnMainTankTrigger
{
public:
    FearWardOnMainTankTrigger(PlayerbotAI* botAI)
        : BuffOnMainTankTrigger(botAI, "fear ward", false, 2 * IN_MILLISECONDS) {}

    bool IsActive() override;
};

class BindingHealTrigger : public PartyMemberLowHealthTrigger
{
public:
    BindingHealTrigger(PlayerbotAI* botAI);

    bool IsActive() override;
};

class MindSearChannelCheckTrigger : public Trigger
{
public:
    MindSearChannelCheckTrigger(PlayerbotAI* botAI, uint32 minEnemies = 2)
        : Trigger(botAI, "mind sear channel check"), minEnemies(minEnemies) {}

    bool IsActive() override;

protected:
    uint32 minEnemies;
    static const std::set<uint32> MIND_SEAR_SPELL_IDS;
};

#endif
