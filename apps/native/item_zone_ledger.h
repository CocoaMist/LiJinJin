#pragma once
#include "combat.h"

// Conditional, explicit-contact model for the reviewed 16.21 KIWI item 3118.
// Only native settled damage may construct an UltimateDamageReceipt. Neither
// this typed interface nor a checkpoint authenticates caller observations.
class ItemZoneLedger {
public:
    struct Participant { QString key, team; };
    enum class Origin { ChampionUltimate, ItemEffect, Other };
    struct UltimateDamageReceipt {
        QString receiptId, ownerKey, targetKey, castInstanceId, sourceSlot;
        double time = 0, actualDamage = 0;
        Origin origin = Origin::Other;
        bool ownerCurrentlyHasItem = false;
    };
    struct TriggerResult {
        bool accepted = false, created = false, refreshed = false, opportunityOnly = false;
        QString reason, zoneId;
        double expiresAt = 0;
    };
    struct Contact {
        QString eventId, ownerKey, zoneId, targetKey;
        double time = 0;
        bool inside = false;
    };
    struct DamageInterval {
        QString ownerKey, targetKey;
        QStringList zoneIds;
        double from = 0, until = 0, rawDamage = 0, flatMrReduction = 0;
    };
    bool load(const CombatDatabase &, const QVector<Participant> &, double start,
              QString *error = nullptr);
    static QString zoneIdForReceipt(const QString &owner, const QString &receiptId);
    // Must follow advance(time). A qualifying hit refreshes one active own zone
    // only if this target already has an explicit active contact with it.
    TriggerResult trigger(const UltimateDamageReceipt &);
    // Event time must equal the ledger's current time. Enter never backdates
    // contact; exit at t is after the integral up to t, with no instant tick.
    bool setContact(const Contact &, QString *error = nullptr);
    bool clearTargetContacts(const QString &target, double time, QString *error = nullptr);
    // AP is native current AP, constant over this explicit event-to-event span.
    // Output is a rate integral, not a certified 0.25s server tick schedule.
    // flatMrReduction applies within the interval before the returned item
    // damage is mitigated. The earlier triggering R hit is never recalculated.
    bool advance(double until, const QMap<QString, double> &apByOwner,
                 QVector<DamageInterval> *intervals, QString *error = nullptr);
    double nextBoundary(double at, double fallback) const;
    double flatMrReduction(const QString &target) const;
    double time() const { return time_; }
    QJsonObject snapshot() const;
    // Restores by replaying the journal against already loaded source/roster,
    // then exact-compares all derived state. The native bridge must additionally
    // bind trigger receipts to its own stored damage-receipt provenance.
    bool restore(const QJsonObject &, QString *error = nullptr);
private:
    struct Zone {
        QString id, owner, triggerTarget, triggerReceipt, cast;
        double start = 0, expires = 0;
        bool activeItem = false;
        QStringList refreshReceipts;
        QSet<QString> contacts;
    };
    QMap<QString, Participant> participants_;
    QMap<QString, Zone> zones_;
    QMap<QString, double> creationReady_;
    QSet<QString> damageReceipts_, contactEvents_;
    QJsonArray journal_;
    QString revision_, patch_, sourceHash_, rosterHash_;
    double start_ = 0, time_ = 0, baseDps_ = 0, apRatio_ = 0, duration_ = 0, mrReduction_ = 0;
    bool loaded_ = false;
    TriggerResult triggerInPlace(const UltimateDamageReceipt &, bool record);
    bool contactInPlace(const Contact &, bool record, QString *);
    bool clearInPlace(const QString &, double, bool record, QString *);
    bool advanceInPlace(double, const QMap<QString, double> &, QVector<DamageInterval> *, bool record, QString *);
};
