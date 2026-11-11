#pragma once
#include "combat.h"
#include <QStringList>

// The caller supplies a fresh, untransformed context and the completed stats
// of that SAME baseline equipment/augment state. Never supply a context that
// already contains a prior form delta. All form expressions read the frozen
// baseline, then their bonuses enter eventStats before item conversions and
// amplifiers. Base replacements (range/AS growth) change context.base only.
struct FormStatsV4Evaluation {
    bool valid = false, applied = false, ranged = false;
    CombatContext context;
    CombatStats delta, baseDelta;
    QJsonArray transforms;
    QStringList unknowns;
    QString healthTransitionPolicy = QStringLiteral("preserve_absolute_current_health_clamp");
    QJsonObject json() const;
};

FormStatsV4Evaluation evaluateFormStatsV4(const QJsonObject &hero, const QString &form,
                                        const CombatContext &baselineContext,
                                        const CombatStats &baselineStats);
