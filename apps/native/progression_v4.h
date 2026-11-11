#pragma once
#include "mechanic_actions.h"

// A persistent ledger for explicitly observed growth. It never grants future
// hits, purchases, stacks, levels, random rewards or resource refills.
class ProgressionLedger {
public:
    bool load(const CombatDatabase &,const CombatContext &,const QList<int> &items,
              const QList<int> &augments,double time,QString *error=nullptr);
    // executionContext is an engine-produced fresh current-form context, used
    // only to evaluate this observation. Its form deltas are never persisted.
    bool apply(const CombatDatabase &,const QJsonObject &event,QString *error=nullptr,
               const CombatContext *executionContext=nullptr,bool native3116Receipt=false);
    bool advance(double time,QString *error=nullptr);
    CombatContext contextAt(double time) const;
    QList<int> items() const;
    QList<int> augments() const;
    double time() const {return time_;}
    double nextBoundary(double at,double fallback) const;
    const MechanicLedger &mechanics() const {return ledger_;}
    QJsonObject snapshot() const;
    bool restore(const CombatDatabase &,const QJsonObject &,QString *error=nullptr);
    QJsonObject lastDecision() const {return decision_;}
private:
    CombatContext context_;
    MechanicLedger ledger_;
    // Canonical physical inventory multiset; the mechanic ledger retains only
    // source ownership so a second plain component cannot duplicate a proc.
    QList<int> inventoryItems_;
    QSet<QString> consumedIds_;
    QJsonObject decision_;
    QString revision_;
    double time_=0;
    double gameTimeOffset_=0;
    bool gameTimeKnown_=false;
    bool loaded_=false;
};
