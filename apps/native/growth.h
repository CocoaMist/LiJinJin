#pragma once
#include "combat.h"

// Fixed-loadout, level-based comparison. Confirmed stacks can be held constant;
// future stacks, kills, hits, purchases and elapsed time are never inferred.
QJsonObject predictCombatGrowth(const CombatDatabase &,const CombatContext &current,
    const QList<int> &items,const QList<int> &augments,int endLevel=18);

// Read-only growth evidence. Counters alone remain caller-supplied hypotheses;
// only matching accepted ingestion records attest a visible observation.
QJsonObject equipmentGrowthSelectionState(const CombatDatabase &,const CombatContext &,
    const QList<int> &items,const QList<int> &augments,const QJsonObject &observationReport={});
// No sale, reset or upgrade inheritance is inferred from an inventory diff.
QJsonObject equipmentGrowthInventoryTransition(const CombatDatabase &,const CombatContext &,
    const QList<int> &before,const QList<int> &after,const QList<int> &augments,
    const QJsonObject &observationReport={});
