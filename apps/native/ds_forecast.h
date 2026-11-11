#pragma once
#include "analysis.h"
#include "settings.h"
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QProcess>
#include <QThreadPool>

// Forecasts are conditional proposals. They never mutate the observed match,
// local ranking, inventory, selected cards, or the purchase executor.
QJsonObject dsForecastSnapshot(const Catalog &, const GameState &);
QJsonObject dsForecastLocalChoiceEvidence(const Catalog &, const GameState &, const QJsonObject &, qint64 nowMs);
QString dsForecastBasis(const QJsonObject &snapshot);
QJsonObject dsForecastReference(const Catalog &, const QJsonObject &snapshot);
QJsonArray projectDsForecastRoutes(const Catalog &, const GameState &, const QJsonArray &routes);
QJsonArray remainingDsForecastRoutes(const GameState &, const QJsonArray &previouslyValidatedRoutes);
QJsonObject selectDsForecastRoute(const QJsonObject &answer, const QJsonArray &projections);
QString dsForecastText(const Catalog &, const QJsonObject &selection);

class DsForecast final : public QObject {
    Q_OBJECT
public:
    static constexpr int maximumFullRequestBytes = 64 * 1024;
    static constexpr int maximumQuickRequestBytes = 48 * 1024;
    static constexpr int maximumLocalChoiceEvidenceBytes = 4096;
    explicit DsForecast(const Catalog &, QObject *parent=nullptr, QNetworkAccessManager *transport=nullptr);
    ~DsForecast() override;
    void enableUsageRecordingForInjectedTransport(bool enabled = true) { recordInjectedUsage = enabled; }
    void observe(const GameState &, const QString &key, const QString &model, bool enabled,
                 const QJsonObject &localChoiceEvidence = {});
    void configureWebEvidence(const QString &root, const QString &python = {});
    void configureModelConnection(const ModelConnection &connection);
    void invalidate();
    QJsonObject report() const;
    QJsonObject selection() const { return selected; }
    QJsonObject currentSelection(const GameState &) const;
signals:
    void changed();
private:
    friend struct DsForecastTransportTest;
    const Catalog &catalog;
    QNetworkAccessManager network;
    QNetworkAccessManager *transport;
    QPointer<QNetworkReply> background, quick;
    QPointer<QProcess> searchProcess;
    QThreadPool pool;
    QFutureWatcher<QJsonObject> worker;
    QElapsedTimer clock;
    GameState latest;
    QString contextId, basis, plannedBasis, credentials, modelName, workBasis, lastFastSignature;
    QJsonObject snapshot, selected, diagnostics;
    QJsonObject localChoiceEvidence, fullChoiceEvidence;
    QJsonArray proposals, projections;
    QString workspace, pythonPath, fullRequestBasis, fullRequestRoster;
    QJsonArray fullMessages;
    QHash<QString,QPair<qint64,QJsonObject>> searchCache;
    quint64 generation=0, workGeneration=0;
    quint64 fullSerial=0;
    quint64 proposalRevision=0, workProposalRevision=0;
    qint64 nextBackground=0, nextQuick=0;
    int backgroundCalls=0, quickCalls=0;
    int searchRounds=0;
    bool fullPlanPending=false;
    bool recordInjectedUsage=false;
    ModelConnection modelConnection;
    bool modelConnectionConfigured=false;
    bool active=false, blocked=false, prepareBackground=false, preparing=false, validatedRoutes=false;
    void prepare(bool full);
    void post(bool full, const QJsonArray &messages, const QString &requestBasis, bool continuation=false);
    bool webSearchConfigured() const;
    void cancelBackgroundPlan();
    void searchRules(const QString &query, const QString &callId, quint64 ticket, quint64 serial);
    void deliverSearch(QJsonObject evidence, const QString &callId, quint64 ticket, quint64 serial);
};
