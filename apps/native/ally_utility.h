#pragma once
#include "combat.h"

// A common, explicit injured-ally / incoming-pressure reference. Reports
// health preserved through capped healing, consumed shields and action gates.
// It never claims observed casts, hit rates, positioning or win probability.
QJsonObject evaluateAllyUtility(const CombatDatabase &,const CombatContext &,
    const CombatStats &,const QList<int> &augments);
