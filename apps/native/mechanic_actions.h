#pragma once
#include "mechanic_triggers.h"

struct MechanicLedger {
    QJsonObject values;
    QJsonArray effects,decisions;
    QSet<QString> consumed;
    QMap<QString,double> clocks;
    QSet<int> inventory,augments;
    double lastTime=-1;
    CombatStats statsAt(double time,bool growthBaselineOnly=false) const;
    QMap<int,CombatStats> itemStatsAt(double time) const;
    QJsonArray itemEffectsAt(double time) const;
    QJsonObject json(double time) const;
};
void applyMechanicLedgerContext(const MechanicLedger &,double time,CombatContext &,bool replace=false);
// Transactional: missing/invalid facts, unknown references and a failed action
// leave state and cooldowns untouched. Observations must be in time order.
bool executeMechanicActions(const CombatEntry &,const CombatContext &,const CombatStats &,
    const QJsonObject &observation,MechanicLedger &,QStringList *missing=nullptr,
    const CombatDatabase *database=nullptr,bool native3116Receipt=false);
// Rebuild only reviewed persistent modifiers from committed reward receipts or
// an explicit completed baseline. Observed stack totals never imply completion.
void restoreMechanicGrowthModifiers(const CombatDatabase &,const CombatContext &,MechanicLedger &);
MechanicLedger replayCombatMechanics(const CombatDatabase &,const CombatContext &,
    const QList<int> &items,const QList<int> &augments,const QJsonArray &observations);
