#pragma once
#include "../native/analysis.h"

// De-identified provider game-ID scope, not a secret or a global platform ID.
QString offerMatchRefForGameId(const QJsonValue &);
bool validOfferMatchRef(const QString &);
bool liveOfferContextReady(const GameState &,const QString &contextId,const QString &matchRef,
    const QString &modelPatch,QDateTime now=QDateTime::currentDateTimeUtc());
QJsonObject liveOfferAdmission(const QJsonObject &event,const GameState &,const QString &contextId,
    const QString &matchRef,const QString &modelPatch,QDateTime now=QDateTime::currentDateTimeUtc());
bool unboundOfferRevokesCurrent(const QJsonObject &,const GameState &,QDateTime priorOffer,
    QDateTime now=QDateTime::currentDateTimeUtc());
