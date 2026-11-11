#pragma once
#include "combat.h"
#include <QStringList>

// Explicit, conditional ally support actions. The sandbox never invents a cast
// from a role label, proximity, a cooldown, or an observed match summary.
struct SandboxSupportActor {
    QString key, team;
    int champion = 0;
    CombatContext context;
    CombatStats stats;
    double maximumHp = 0, hp = 0, factor = 1, threat = 0;
    QList<int> augments;
};

class SandboxSupportTimeline {
public:
    bool load(const CombatDatabase &, const QJsonValue &events,
              const QVector<SandboxSupportActor> &, double horizon, QString *error);
    // At a boundary, expire shields first, then apply explicitly scheduled
    // support in source order. Damage for the following interval settles later.
    void apply(double time, QVector<SandboxSupportActor> &);
    void expire(double time);
    double nextBoundary(double time, double fallback) const;
    double absorb(const QString &target, double incoming);
    QJsonObject summary() const;
    QSet<QString> unknowns() const;
    double healthCost(const QString &actor) const;
    double healingReceived(const QString &actor) const;
    double absorbed(const QString &actor) const;
    double shieldRemaining(const QString &actor) const;
    bool hasEvents() const { return !events_.isEmpty(); }
private:
    struct Shield { QString source, slot, target; double amount = 0, expires = 0; };
    const CombatDatabase *db_ = nullptr;
    QVector<QJsonObject> events_;
    QVector<Shield> shields_;
    QMap<QString, double> castAt_, costs_, healing_, absorbed_;
    QJsonArray results_;
    QSet<QString> unknowns_;
    int cursor_ = 0;
};
