#pragma once
#include "mechanic_actions.h"

// Conditional source-pinned consumers. Facts are explicit scenario observations,
// not server measurements; all failed transitions leave the entire ledger intact.
bool executeHextech40State(const CombatDatabase *, const CombatEntry &,
    const CombatContext &, const CombatStats &, const QJsonObject &,
    MechanicLedger &, QStringList *missing);
int hextech40LockedItem(const MechanicLedger &);
