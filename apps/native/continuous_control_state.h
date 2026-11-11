#pragma once
#include "combat.h"
#include <QStringList>

// One actor's explicit control ledger. Durations use [time, end), so a control
// ending at t no longer blocks an action at t. No proximity or skill-name
// heuristic invents control events. source/target identify actors, not slots.
class ContinuousControlState {
public:
    // Required: event_id/source/target/type/time/duration. slow additionally
    // requires slow_fraction in [0,1]. Optional duration_floor_seconds is an
    // explicit scenario assumption (default 0, not an inferred game rule).
    // charm/fear disable autonomous actions; their forced movement is not
    // simulated. taunt forces attacks onto its source; callers must use the
    // target-aware gate and separately verify that actor is alive/attackable.
    bool apply(const QJsonObject &event, const CombatStats &targetStats, QString *error = nullptr);
    bool advanceTo(double time, QString *error = nullptr);
    bool canCast(double time) const;
    bool canAttack(double time) const;
    bool canAttackTarget(double time, const QString &target) const;
    QString forcedAttackTarget(double time) const;
    bool canMove(double time) const;
    bool interruptsCast(double time) const;
    bool nearsighted(double time) const;
    // An observed death terminates this target's existing controls and taunts
    // originating from that actor, including pre-registered future entries.
    // Revival and eligibility of newly supplied events belong to the caller.
    bool notifyActorDeath(const QString &actor, double time, bool verified,
                          QString *error = nullptr);
    double movementMultiplier(double time) const;
    double nextBoundary(double at, double fallback) const;
    QJsonObject snapshot(double at) const;
    QJsonObject checkpoint() const;
    bool restore(const QJsonObject &, QString *error = nullptr);
    // Spelling aliases for timeline schedulers; queries do not mutate time.
    bool allowsCast(double time) const { return canCast(time); }
    bool allowsAttack(double time) const { return canAttack(time); }
    double nextExpiry(double at, double fallback) const;
private:
    struct Entry {
        QJsonObject event;
        QString id, source, target, type;
        double start = 0, end = 0, originalDuration = 0;
        double tenacity = 0, slowResist = 0, slowFraction = 0;
        double deathAt = -1;
        QString deadActor;
        bool tenacityApplied = false;
    };
    QVector<Entry> entries_;
    QSet<QString> eventIds_;
    QString target_;
    double time_ = 0;
    bool active(const Entry &, double time) const;
    double effectiveEnd(const Entry &) const;
};
