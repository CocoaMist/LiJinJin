#pragma once
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>

// Independent consumer of LiXin's sanitized event contract; no vendor SDK.
struct PanelState {
    explicit PanelState(bool historical = false) : replay(historical) {}
    bool replay = false;
    QString session;
    QString status = QStringLiteral("waiting");
    QJsonArray candidates;
    QSet<int> seenCandidates;
    int round = 0, resolvedRound = 0;
    quint64 offerRevision = 0;
    QString offerSourceRef;
    bool awaitingReroll = false;
    QJsonArray selectionRoundEvidence;
    QDateTime observed;
    QJsonObject headlinerContext;
    QDateTime headlinerObserved;
    QString liveContextId, liveMatchRef;
    bool bindLiveMatch(const QString &contextId,const QString &matchRef);
    bool liveBoundTo(const QString &contextId) const;
    void clearLiveOffers();
    bool apply(const QJsonObject &, QDateTime now = QDateTime::currentDateTimeUtc());
    bool expire(QDateTime now = QDateTime::currentDateTimeUtc());
    int providerOfferRound() const { return offerProviderRound.toInt(); }
    QString decisionBinding() const;
    QJsonObject json() const;

private:
    QString offerSourceFile;
    qint64 offerSourceOffset = -1;
    QJsonValue offerProviderRound;
};
