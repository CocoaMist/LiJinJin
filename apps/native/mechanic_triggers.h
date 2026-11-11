#pragma once
#include "combat.h"

// Observations come from a replay or an explicit state source. A tooltip never
// manufactures a hit, a quest completion, an item purchase or a random roll.
struct TriggerDecision {
    bool matched=false;
    QString reason;
    QStringList missing;
    QJsonObject parameters;
    QJsonObject json() const;
};
TriggerDecision evaluateMechanicTrigger(const CombatEntry &,const CombatContext &,
    const CombatStats &,const QJsonObject &observation,QMap<QString,double> &lastTriggers);
QJsonArray describeCombatMechanics(const CombatDatabase &,const CombatContext &,
    const QList<int> &items,const QList<int> &augments);
