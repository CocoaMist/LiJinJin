#pragma once
#include "analysis.h"
#include "settings.h"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>

class DeepSeek : public QObject {
    Q_OBJECT
public:
    explicit DeepSeek(QObject *parent = nullptr);
    // Optional test transport; production uses the owned HTTPS manager.
    DeepSeek(QObject *parent, QNetworkAccessManager *transport);
    ~DeepSeek() override;
    // Explicit opt-in for a real provider diagnostic using an injected manager.
    // Such requests remain outside the live-match mean.
    void enableUsageRecordingForInjectedTransport(bool enabled = true) { recordInjectedUsage = enabled; }
    void request(const QJsonObject &context, const QString &key, const QString &model,
                 const QList<int> &items, const QList<int> &augments, quint64 revision);
    void invalidate();
    void configureWebEvidence(const QString &root, const QString &python = {});
    void configureModelConnection(const ModelConnection &connection);
    void analyzeMatch(const QJsonObject &context, const QString &key, const QString &model, quint64 revision);
    void analyzeEngine(const QJsonObject &context, const QString &key, const QString &model, quint64 revision);
    QJsonObject report() const;
    bool configurationBlocked() const { return configurationFailure; }
signals:
    void result(const QJsonObject &, quint64 revision);
    void status(const QString &);
    void settled(quint64 revision, bool success, bool retryable);
    void matchAnalysis(const QJsonObject &, quint64 revision);
    void matchSettled(quint64 revision, bool success, bool retryable);
    void engineAnalysis(const QJsonObject &, quint64 revision);
    void engineSettled(quint64 revision, bool success, bool retryable);
private:
    QNetworkAccessManager network;
    QNetworkAccessManager *transport = nullptr;
    QNetworkReply *pending = nullptr;
    quint64 generation = 0;
    bool configurationFailure = false;
    bool recordInjectedUsage = false;
    ModelConnection modelConnection;
    bool modelConnectionConfigured = false;
    QProcess *searchProcess = nullptr;
    QString workspace, pythonPath, apiKey, modelName, activeContextId, evidenceBasis;
    QJsonObject requestContext, cachedEvidence, diagnostics;
    QJsonArray messages;
    QList<int> allowedItems, allowedAugments;
    quint64 requestRevision = 0;
    bool rosterTask = false, overlayTask = false, engineTask = false;
    int searchRounds = 0;
    void begin(const QJsonObject &, const QString &, const QString &, const QList<int> &, const QList<int> &, quint64, bool, bool engineReview = false);
    void searchRules(const QString &, const QString &toolCallId, quint64 ticket);
    void seedMessages();
    void post(quint64 ticket);
    void finish(bool success, bool retryable);
    void publishValidated(const QJsonObject &);
};
