#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QString>
#include <QVector>

// Explicit synthetic NPC observations, separate from the player roster. This
// ledger receives only native evaluated/mitigated damage; event facts cannot
// claim a death. It never evaluates attacks, grants gold/XP, moves units, or
// invents shield/regeneration behavior. Each accepted damage event is unique.
class CombatUnitsV8 {
public:
    // Required observation fields: unit_id, unit_class (minion|large),
    // maximum_health, current_health, armor, mr,
    // verified_unit_observation=true, synthetic_observation=true.
    // Optional current_shield requires verified_shield_observation=true.
    // Optional movement_speed is a verified observation, never defaulted.
    bool loadObserved(const QJsonArray &observations, const QSet<QString> &playerKeys,
                      double start, QString *error = nullptr);
    bool contains(const QString &unitId) const { return units_.contains(unitId); }
    QJsonObject unit(const QString &unitId) const;
    bool advanceTo(double at, QString *error = nullptr);
    QJsonObject receiveDamage(const QString &unitId, const QString &attackerPlayerKey,
                              const QString &eventId, double at, double mitigatedAmount,
                              QString *error = nullptr);
    // Returns a native damage or death receipt already committed by this
    // ledger. Arbitrary caller-provided receipts are never imported.
    QJsonObject receipt(const QString &receiptId) const { return receipts_.value(receiptId); }
    QJsonObject snapshot() const;
    // Replays original observations and damage events, then requires exact
    // equality with all saved balances/receipts. The caller additionally
    // requires time() <= its resume time before accepting this checkpoint.
    bool restore(const QJsonObject &checkpoint, const QSet<QString> &playerKeys,
                 QString *error = nullptr);
    double time() const { return time_; }
private:
    struct Unit {
        QJsonObject observation;
        QString id, unitClass, deathReceiptId;
        double maximumHealth = 0, health = 0, armor = 0, mr = 0, shield = 0;
        double initialHealth = 0, initialShield = 0, diedAt = -1;
    };
    QMap<QString, Unit> units_;
    QSet<QString> playerKeys_, eventIds_;
    QMap<QString, QJsonObject> receipts_;
    QVector<QJsonObject> damageEvents_;
    double start_ = 0, time_ = 0;
    bool loaded_ = false;
};
