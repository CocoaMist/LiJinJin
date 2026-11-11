#pragma once
#include "combat.h"

// Received modifiers belong to the recipient. raw amounts passed to these
// helpers have already applied the producer's heal/shield power, if any.
// They must not be used to amplify another actor merely because the producer
// owns an item such as Spirit Visage.
struct CombatSustainModifiers {
    double healingReceived = 1, shieldingReceived = 1;
    QJsonArray sources;
    QStringList unknowns;
};
CombatSustainModifiers combatSelfSustainModifiers(const CombatDatabase &,
    const CombatContext &, const QList<int> &items, const QList<int> &augments,
    const CombatStats &);

struct CombatHealSettlement {
    bool valid = false;
    double raw = 0, amplified = 0, effective = 0, overheal = 0;
};
CombatHealSettlement settleCombatSelfHeal(double raw, double missingHp,
    const CombatSustainModifiers &);
std::optional<double> amplifyCombatSelfShield(double raw,
    const CombatSustainModifiers &);

// Resolves one source-bound damage->self-heal proc. It neither admits an
// activation nor infers targets, distance, combat time or damage actually dealt.
struct CombatDrainProfile {
    bool valid = false;
    double rawDamage = 0, healingFromDamage = 0, cooldown = 0, range = 0;
    QString damageType;
    QStringList unknowns;
};
CombatDrainProfile combatDrainProfile(const CombatDatabase &,
    const CombatContext &, const CombatStats &, int itemId = 2502);

// One available shield pool, not the sum of repeatedly refreshed pools and not
// observed absorption. Only components explicitly admitted for this objective
// participate; continuous state programs remain their delivery authority.
struct CombatSelfShieldCapacity {
    double raw = 0, amplified = 0;
    QJsonArray components;
    QStringList unknowns;
};
CombatSelfShieldCapacity combatSelfShieldCapacity(const CombatDatabase &,
    const CombatContext &, const CombatStats &, const CombatSustainModifiers &);
