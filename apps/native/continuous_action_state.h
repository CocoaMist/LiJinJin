#pragma once
#include "combat.h"

// Absolute-time, copyable action ledger. Rejected actions are transactional:
// no resource, clock, cooldown, charge or attack timer changes are committed.
// advance() accounts for the interval using the previous alive state; the
// supplied alive state becomes effective at its right endpoint. A checkpoint
// restores timers/resources; callers pass the current context and computed
// stats to load(), so rank/gear changes do not roll back to old inputs.
class ContinuousActionState {
public:
    bool load(const QJsonObject &rules, const CombatContext &context,
              const CombatStats &stats, const QJsonObject &initial,
              double start, QString *error = nullptr);
    bool advance(double at, bool alive);
    bool updateStats(const CombatStats &stats, double at);
    // Growth and equipment changes apply at this instant. Existing absolute
    // timers remain committed; increased resource capacity never refills it.
    bool updateContext(const CombatContext &context, const CombatStats &stats,
                       double at, const QString &resourcePolicy = QStringLiteral("clamp_current"));
    bool interruptCast(double at);
    bool beginChannel(const QString &slot, double at, double until,
                      const QString &instanceId, bool verified);
    QJsonObject channelState() const;
    QJsonObject cast(const QString &slot, double at, const CombatStats &stats,
                     const QJsonObject &facts = {});
    // Internal state-program admission only. The compiled source contract must
    // match both program identifiers; caller facts cannot select this policy.
    // The program consumer must install the matched continuous upkeep effect.
    QJsonObject castSourceUpkeep(const QString &slot, double at, const CombatStats &stats,
                                const QJsonObject &facts, const QString &bindingId,
                                const QString &programHash);
    // Explicit documented model, selected from the loaded catalog, never from
    // facts. Health installments are paid by the owning combat HP ledger.
    QJsonObject castSourceHealthScheduleV9(const QString &slot, double at, const CombatStats &stats,
                                         const QJsonObject &facts, const QString &profileId,
                                         const QString &profileHash, const QString &bindingId,
                                         const QString &programHash);
    // A source-pinned per-second toggle is a state transition, never a cast
    // that pays its per-second rate once. Contacts generate damage separately.
    QJsonObject toggle(const QString &slot, bool enabled, double at,
                       const CombatStats &stats, const QJsonObject &facts = {});
    QJsonObject upkeepState() const;
    QJsonObject attack(double at, double attackSpeed, bool alive);
    QJsonObject transform(const QString &form, double at,
                          const QJsonObject &facts = {});
    bool reduceCooldown(const QString &slot, double seconds, double at);
    // Source-bound action consumers supply evaluated refund effects only after
    // event admission. This changes the actual clock transactionally, never a
    // parallel "ready:" bookkeeping counter. A configure marker does not grant
    // a minimum-cooldown policy, and charge recharges need a separate contract.
    QJsonObject applyCooldownRefunds(const QJsonArray &effects, double at);
    // Only an explicitly observed restoration changes resource on respawn.
    bool restoreResource(double amount, double at, bool verified);
    // Source-state consumers use these methods before advance()/updateContext()
    // at the right endpoint. Each integration advances the complete action
    // clock exactly once and replaces only that interval's resource evolution.
    QJsonObject payResource(double amount, double at, bool verified);
    QJsonObject integrateResourceUpkeep(double rate, const QString &basis,
                                       double from, double to, bool verified);
    // Combine simultaneous flat/maximum-based and current-fraction costs into
    // one affine ODE rather than advancing regeneration once per effect.
    QJsonObject integrateResourceUpkeep(double flatRate, double currentFractionRate,
                                       double from, double to, bool verified);
    QJsonObject snapshot() const;
    QSet<QString> formConditions() const;
    double nextBoundary(double at, double fallback) const;
    bool alive() const { return m_alive; }
    double time() const { return m_time; }
private:
    struct SkillState {
        double readyAt = 0;
        int charges = 0;
        bool chargesKnown = false;
        double rechargeAt = -1, rechargeDuration = 0;
        bool rechargeAwaitingObservation = false, lastAmmoCostObserved = false;
        int lastAmmoCost = 0;
        double tokenExpiresAt = -1;
        double lastCastAt = -1;
        QString tokenTarget, tokenPhase;
        int burstRemaining = 0, burstEmitted = 0, burstMaximum = 0;
        double burstExpiresAt = -1, burstReadyAt = -1;
        bool burstChannelled = false;
        QString burstInstanceId;
    };
    QJsonObject m_rules;
    CombatContext m_context;
    CombatStats m_stats;
    QMap<QString, SkillState> m_skills;
    QString m_form, m_resourceKind;
    double m_time = 0, m_resource = 0, m_resourceMaximum = 0, m_regen = 0;
    bool m_loaded = false, m_alive = true, m_resourceKnown = false, m_regenWhileDead = false;
    bool m_resourceMaximumObserved = false;
    double m_attackReadyAt = 0, m_attackReloadAt = -1, m_castReadyAt = 0;
    int m_attackAmmo = 0, m_attackAmmoMaximum = 0;
    double m_attackReloadSeconds = 0;
    QString m_channelSlot, m_channelInstanceId;
    double m_channelExpiresAt = -1;
    QString m_upkeepSlot, m_upkeepInstanceId, m_upkeepStopReason;
    bool m_upkeepActive = false;
    double m_upkeepRate = 0, m_upkeepStartedAt = -1, m_upkeepStoppedAt = -1;
    double m_toggleReadyAt = 0, m_upkeepSpent = 0;
    void stopUpkeep(double at, const QString &reason);
    QJsonObject slotRule(const QString &slot) const;
    QString skillKey(const QString &slot) const;
    bool advanceInPlace(double at, bool alive);
    QJsonObject castInContext(const QString &slot, double at, const CombatStats &stats,
                              const QJsonObject &facts, const QString &upkeepBindingId,
                              const QString &upkeepProgramHash, const QString &profileId = {},
                              const QString &profileHash = {}, const QString &bindingId = {},
                              const QString &programHash = {});
    bool restoreCheckpoint(const QJsonObject &checkpoint, double start, QString *error);
};
