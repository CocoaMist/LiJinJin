#pragma once
#include "combat.h"

// Historical direction arguments are accepted for call compatibility only.
QString growthCombatPreference();
// Bind after observed-state calibration, before comparing any equipment or
// offered augment. All candidates must reuse this same baseline and metric set.
void bindGrowthObjective(const CombatDatabase &, CombatContext &,
    const QList<int> &baselineItems, const QList<int> &selectedAugments);
void applyGrowthObjective(const CombatDatabase &, const CombatContext &,
    const QList<int> &items, const QList<int> &augments, CombatEvaluation &, bool details=true);
QJsonObject describeGrowthObjective(const CombatDatabase &, const CombatContext &,
    const QList<int> &items, const QList<int> &augments);
double growthObjectiveMetricWeight(const CombatEvaluation &, const QString &metric);
int growthRequiredItemCoverage(const CombatContext &, const QList<int> &items);
double growthEffectiveSkillCooldown(const CombatDatabase &, const CombatContext &,
    const QList<int> &augments, const CombatStats &, const QString &slot,
    double baseCooldown, bool hasteApplies);
