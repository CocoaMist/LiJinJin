#pragma once
#include "analysis.h"

// All times are monotonic milliseconds supplied by the caller.
class AutoAdviceGate {
public:
    bool observe(const QString &key, qint64 now);
    bool due(qint64 now) const;
    bool begin(qint64 now);
    void complete(bool success, bool retryable, qint64 now);
    void clear(qint64 now);
    bool pending() const { return running; }
    int dispatches() const { return sent; }
    int retries() const { return retryCount; }
    QString key() const { return observed; }
private:
    QString observed;
    qint64 stableSince = 0, lastStart = -12000, retryAt = 0;
    bool running = false, done = false;
    int attempts = 0, sent = 0, retryCount = 0;
};

QString automaticAdviceBasis(const GameState &, const QString &preference,
                             double execution, const QString &session);
QString automaticPurchaseCheckpoint(const QJsonObject &coach);
// Both teams' loadouts affect the objective and invalidate its explanation.
// Slot ordering, candidate rerolls, health ticks and gold drift do not.
QString automaticOverlayAdviceBasis(const GameState &,const QJsonObject &coach,
    const QString &preference,double execution,const QString &session);
QString automaticPurchaseStatus(const QJsonObject &purchase);
