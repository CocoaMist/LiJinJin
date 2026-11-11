#pragma once
#include "combat.h"
#include <QStringList>

// Declarative, source-bound skill state. This ledger never invents a hit,
// recipient, cast admission, actual damage receipt, resource payment or path.
// apply() is transactional; effect requests are returned only after all state,
// target, lifetime and formula checks pass. The continuous engine owns their
// mitigation, delivery, resource and control ledgers.
class StatefulSkillLedgerV7 {
public:
    bool load(const QJsonArray &programs, const QString &ownerKey, const QString &team,
              int championId, const QString &revision, double start,
              QString *error = nullptr);
    // event: event_id, binding_id, transition_id, time, target_key?, entity_id?,
    // facts?. runtime is an engine-produced envelope, never caller facts:
    // self_current_hp/self_current_mana/target_current_hp/target_max_hp,
    // target_team/target_key, cast_admitted/attack_admitted, actual_damage,
    // damage_receipt_id/damage_source_key/damage_target_key, owner_alive.
    // owner-commanded entities additionally require an admitted native attack:
    // owner_attack_ready_at, owner_attack_receipt_id, owner_attack_owner_key,
    // owner_attack_receipt_time. Facts cannot grant that admission or clock.
    // v8 entity bridges are exact source declarations on a transition, not
    // event-selected foreign IDs. Unknown pet clocks accept a one-time engine
    // observation only when the transition declares verified_observation.
    // Observed timer initialization and cast payments likewise enter through
    // engine receipts and remain binding-owned in checkpoint state.
    QJsonObject apply(const QJsonObject &event, const CombatContext &context,
                      const CombatStats &stats, const QJsonObject &runtime,
                      QString *error = nullptr);
    bool advance(double time, QString *error = nullptr);
    bool onDeath(double time, QString *error = nullptr);
    bool onInterrupt(double time, QString *error = nullptr);
    bool onResourceExhausted(double time, const QString &resourceKind = QStringLiteral("mana"),
                             QString *error = nullptr);
    // Only the native damage pipeline may call this with post-mitigation
    // damage. The resulting envelope may be consumed by apply() exactly once
    // per reaction; it is never reconstructed from an event/facts payload.
    QJsonObject receiveEntityDamage(const QString &bindingId, const QString &entityKey,
        const QString &receiptId, const QString &sourceKey, double time,
        double postMitigationDamage, QString *error = nullptr);
    // Conditional scenario observation, not a replacement for source data.
    // An unmodelled entity may be seeded once; source-known HP is immutable.
    bool seedObservedEntityStats(const QString &bindingId, const QString &entityKey,
        const QString &observationId, double time, const QJsonObject &observed,
        QString *error = nullptr);
    double nextBoundary(double at, double fallback) const;
    QJsonArray activeEffects(double time) const;
    QJsonObject snapshot() const;
    // Restore must use the already loaded source programs/identity. Source
    // drift, unknown bindings, forged ownership and invalid clocks fail closed.
    bool restore(const QJsonObject &checkpoint, QString *error = nullptr);
private:
    QMap<QString,QJsonObject> programs_, instances_;
    QJsonObject sharedCounters_, sharedCounterExpiries_, sharedClocks_;
    QSet<QString> eventIds_;
    QString owner_, team_, revision_, programHash_;
    int champion_ = 0;
    double time_ = 0;
    bool loaded_ = false;
    QJsonObject applyInPlace(const QJsonObject &, const CombatContext &,
                             const CombatStats &, const QJsonObject &, QString *);
    bool advanceInPlace(double time, QString *error);
};
