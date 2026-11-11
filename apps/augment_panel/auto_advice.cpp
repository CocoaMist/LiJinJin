#include "auto_advice.h"
#include "../native/headliner_context.h"
#include "../native/decision_context.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <limits>

bool AutoAdviceGate::observe(const QString &key, qint64 now) {
    if (key == observed) return false;
    observed = key; stableSince = now; retryAt = 0;
    running = false; done = false; attempts = 0;
    return true;
}
bool AutoAdviceGate::due(qint64 now) const {
    return !observed.isEmpty() && !running && !done && attempts < 2
        && now - stableSince >= 800 && now - lastStart >= 12000 && now >= retryAt;
}
bool AutoAdviceGate::begin(qint64 now) {
    if (!due(now)) return false;
    running = true; lastStart = now; ++sent;
    if (attempts++) ++retryCount;
    return true;
}
void AutoAdviceGate::complete(bool success, bool retryable, qint64 now) {
    if (!running) return;
    running = false; done = success || !retryable || attempts >= 2;
    retryAt = now + 30000;
}
void AutoAdviceGate::clear(qint64 now) { observe({}, now); }

QString automaticAdviceBasis(const GameState &s, const QString &preference,
                             double execution, const QString &session) {
    QJsonArray players, owned, offers, selected;
    for (const auto &v : s.players) {
        const auto p = v.toObject();
        QJsonObject row;
        for (const auto &key : {QStringLiteral("champion_id"), QStringLiteral("team"),
                QStringLiteral("self"), QStringLiteral("level"), QStringLiteral("items"),
                QStringLiteral("items_complete"), QStringLiteral("augment_ids"),
                QStringLiteral("augments_complete")}) row[key] = p.value(key);
        players.append(row);
    }
    for (int id : s.owned) owned.append(id);
    for (int id : s.offers) offers.append(id);
    for (int id : s.selected) selected.append(id);
    QStringList conditions(s.effectConditions.begin(), s.effectConditions.end());
    conditions.sort();
    QJsonObject counters;
    for (auto it = s.effectCounters.cbegin(); it != s.effectCounters.cend(); ++it)
        counters[it.key()] = it.value();
    const QJsonObject value{{QStringLiteral("session"), session},
        {QStringLiteral("mode"), s.gameMode}, {QStringLiteral("queue"), s.queueId},
        {QStringLiteral("champion"), s.championId}, {QStringLiteral("level"), s.level},
        {QStringLiteral("abilities"), s.abilities}, {QStringLiteral("players"), players},
        {QStringLiteral("owned"), owned}, {QStringLiteral("offers"), offers},
        {QStringLiteral("selected"), selected}, {QStringLiteral("counters"), counters},
        {QStringLiteral("observed_patch"),s.observedPatch},{QStringLiteral("headliner_context"),headlinerBasis(s.headlinerContext)},
        {QStringLiteral("conditions"), QJsonArray::fromStringList(conditions)},
        {QStringLiteral("preference"), preference}, {QStringLiteral("execution"), execution}};
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(value).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
QString automaticPurchaseCheckpoint(const QJsonObject &coach) {
    const auto p = coach.value(QStringLiteral("economy")).toObject()
        .value(QStringLiteral("next_purchase")).toObject();
    QList<int> ids;for(const auto &v:coach.value(QStringLiteral("plan")).toObject().value(QStringLiteral("items")).toArray())ids.append(v.toObject().value(QStringLiteral("item_id")).toInt());
    std::sort(ids.begin(),ids.end());QStringList route;for(int id:ids)route.append(QString::number(id));
    return route.join(',')+QChar(':')+QString::number(p.value(QStringLiteral("item_id")).toInt()) + QChar(':')
        + QString::number(p.value(QStringLiteral("gold_known")).toBool()) + QChar(':')
        + QString::number(p.value(QStringLiteral("affordable")).toBool());
}
QString automaticOverlayAdviceBasis(const GameState &state,const QJsonObject &coach,const QString &preference,double execution,const QString &session){
    auto stable=state;stable.players={};stable.offers.clear();
    std::sort(stable.owned.begin(),stable.owned.end());std::sort(stable.selected.begin(),stable.selected.end());
    QJsonObject ranks;for(const auto &slot:{QStringLiteral("Q"),QStringLiteral("W"),QStringLiteral("E"),QStringLiteral("R")}){
        const auto ability=state.abilities.value(slot).toObject();
        ranks[slot]=QJsonObject{{QStringLiteral("id"),ability.value(QStringLiteral("id"))},{QStringLiteral("abilityLevel"),ability.value(QStringLiteral("abilityLevel"))}};
    }
    stable.abilities=ranks;
    return automaticAdviceBasis(stable,preference,execution,session)+QChar(':')
        +QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(liveLoadoutBasis(state)).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex())
        +QChar(':')+automaticPurchaseCheckpoint(coach);
}
QString automaticPurchaseStatus(const QJsonObject &purchase){
    if(!purchase.value(QStringLiteral("gold_known")).toBool())return {};
    if(purchase.value(QStringLiteral("affordable")).toBool())return QStringLiteral("金币已够");
    const auto gap=purchase.value(QStringLiteral("gold_gap"));
    if(!gap.isDouble()||!std::isfinite(gap.toDouble())||gap.toDouble()<0||gap.toDouble()>std::numeric_limits<int>::max())return QStringLiteral("金币待核");
    return QStringLiteral("还差 %1 G").arg(static_cast<int>(std::ceil(gap.toDouble())));
}
