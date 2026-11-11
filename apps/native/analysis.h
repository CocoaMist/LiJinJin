#pragma once
#include "catalog.h"
#include "recommendation_work.h"
#include <QDateTime>
#include <QSet>

struct GameState {
    enum class Source { Offline, Live, Replay };
    Source source = Source::Offline;
    QString phase = QStringLiteral("未连接"), message, gameMode, observedPatch;
    int queueId = 0, championId = 0, level = 1;
    double gameTime = 0, gold = 0, ap = 0, ad = 0, hp = 0;
    bool goldKnown = false;
    QJsonObject abilities;
    QJsonObject liveStats;
    // Validated self-only growth readings and their current capture coverage.
    // Separate from counters: a readable fact need not have an executable rule.
    QJsonObject growthObservations;
    QMap<QString,double> effectCounters;
    QSet<QString> effectConditions;
    QJsonArray mechanicObservations;
    // If supplied, counters/panel already include events up to this clock.
    // A replay must not award their permanent growth for a second time.
    double mechanicBaselineTime = -1;
    // Sanitized current-match observations only, never account progression.
    QJsonObject headlinerContext;
    // Anonymous current match only; local player identities use a separate signal.
    QJsonObject matchContext;
    // Anonymous, context-bound historical evidence. Historical scenarios never
    // overwrite observed combat stats, owned items, augments or live events.
    QJsonObject recommendationEvidence;
    QJsonObject tacticalHistory;
    // Optional caller-declared conditional action chain; never a capture feed.
    QJsonObject explicitPurchaseChain;
    QJsonArray players;
    QList<int> owned, bench, offers, selected;
    // A successful current-session response distinguishes an empty bench from
    // missing coverage. Phase polling alone must not renew this observation.
    bool benchKnown = false;
    QDateTime championSelectReceived;
    bool championSelectFresh() const {
        const auto age=championSelectReceived.msecsTo(QDateTime::currentDateTimeUtc());
        return phase==QStringLiteral("ChampSelect")&&championSelectReceived.isValid()&&age>=0&&age<8000;
    }
    QDateTime received;
    bool fresh() const {
        if(source==Source::Replay)return true;
        const auto age=received.msecsTo(QDateTime::currentDateTimeUtc());
        return source==Source::Live&&received.isValid()&&age>=0&&age<4000;
    }
    QString fingerprint() const;
};
bool modelPatchMatches(const Catalog &,const GameState &);
// Availability and source-version equality are independent. Cross-version
// calculations retain their original rule provenance instead of disabling UI.
bool modelRulesUsable(const Catalog &,const GameState &);
QJsonObject modelRuleBasis(const Catalog &,const GameState &);

struct DamageInput {
    int championId = 1, rank = 1;
    double ap = 100, ad = 60, resistance = 50, percentPen = 0, flatPen = 0, modifier = 1;
};
struct DamageResult { bool supported = false; double raw = 0, effectiveResistance = 0, mitigated = 0; QString formula, scope; };
DamageResult calculateDamage(const QJsonObject &rule, const DamageInput &input);
double resistanceMultiplier(double resistance);
QJsonObject recommendations(const Catalog &, const GameState &, int championId, const QString &style);
// Live-only next-purchase comparison. Does not run the offline six-slot or
// counterfactual offer build searches; an interrupted enumeration has no rank.
QJsonObject currentPurchaseRecommendations(const Catalog &, const GameState &, int championId,
    const QString &style, const RecommendationWorkPtr &work = {});
GameState parseLiveData(const QJsonObject &, const Catalog &);
QJsonArray visibleNumericComponents(const Catalog &, const GameState &, int championId);
bool validateAiPlan(const QJsonObject &, const QList<int> &allowedItems, const QList<int> &allowedAugments, QString *error);
bool validateAiLoadouts(const QJsonObject &, const QJsonArray &loadouts, const QList<int> &selectedAugments, QString *error);
bool resolveAiLoadoutIndices(QJsonObject &plan, const QJsonArray &loadouts, QString *error);
