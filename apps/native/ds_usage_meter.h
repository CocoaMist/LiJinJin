#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <memory>

// Anonymous, local estimates from provider usage. No balance or message access.
// An unconfigured instance is memory-only, suitable for deterministic tests.
class DsUsageMeter final {
public:
    DsUsageMeter();
    ~DsUsageMeter();
    DsUsageMeter(const DsUsageMeter &) = delete;
    DsUsageMeter &operator=(const DsUsageMeter &) = delete;
    bool configure(const QString &workspaceRoot);
    void observeContext(const QString &contextId, bool active, bool finishedReliable = false);
    QString beginCall(const QString &contextId, const QString &requestedModel,
                      const QString &task, bool liveEligible,
                      const QDateTime &startedUtc = QDateTime::currentDateTimeUtc());
    void finishCall(const QString &callId, const QJsonObject &providerEnvelope,
                    const QDateTime &finishedUtc = QDateTime::currentDateTimeUtc());
    void finishUnknown(const QString &callId, const QString &reason = QStringLiteral("response_unavailable"),
                       const QDateTime &finishedUtc = QDateTime::currentDateTimeUtc());
    QJsonObject summary(const QString &currentContextId = {}) const;
private:
    struct State;
    std::unique_ptr<State> d;
};

DsUsageMeter &dsUsageMeter();
