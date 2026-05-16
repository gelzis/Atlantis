#pragma once
#ifndef BATTLE_H
#define BATTLE_H


class Battle;

#include "army.h"
#include "items.h"
#include "events.h"
#include <map>
#include <vector>

class Location;

#include "external/nlohmann/json.hpp"
using json = nlohmann::json;

enum {
    ASS_NONE,
    ASS_SUCC,
    ASS_FAIL
};

enum {
    BATTLE_IMPOSSIBLE,
    BATTLE_LOST,
    BATTLE_WON,
    BATTLE_DRAW
};

struct BattleDetails
{
    int attackerLost = 0;
    int defenderLost = 0;
    std::map<int, int> spoils;

    void AddSpoils(ItemList& items);
    void AddAttackerArmy(Army *army);
    void AddDefenderArmy(Army *army);
};

class Battle
{
    public:
        void build_json_report(json &j, Faction *fac);
        void AddLine(const std::string& line);

        int Run(
            Events* events, ARegion *region, Unit *att, std::list<Location *>& atts,
            Unit *tar, std::list<Location *>& defs, int ass, BattleDetails *details = nullptr
        );
        void FreeRound(Army *,Army *, int ass = 0);
        void NormalRound(int,Army *,Army *);
        void DoAttack(int round, Soldier *a, Army *attackers, Army *def,
                int behind, int ass = 0, bool canAttackBehind = false, bool canAttackFromBehind = false);

        void GetSpoils(std::list<Location *>& losers, ItemList& spoils, int ass);

        //
        // These functions should be implemented in specials.cpp
        //
        void UpdateShields(Army *);
        void DoSpecialAttack(Soldier *a, Army *attackers,
                Army *def, int canattackback);

        void WriteSides(
            ARegion * r, Unit * att, Unit * tar, std::list<Location *>& atts, std::list<Location *>& defs, int ass
        );

        // void WriteBattleStats(ArmyStats *);

        int assassination;
        Faction * attacker; /* Only matters in the case of an assassination */
        std::string asstext;
        std::vector<std::string> text;
};

#endif // BATTLE_H
