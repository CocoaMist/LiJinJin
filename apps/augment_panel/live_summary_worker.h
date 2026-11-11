#pragma once
#include "analysis.h"
#include "live_combat_summary.h"
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QObject>
#include <QThreadPool>
#include <QTimer>
#include <functional>
#include <optional>

// The normal Live path never evaluates combat on its caller's thread. Cached
// model fields are merged only into an identical, still-current observation.
// Call update/report only from the owning (GUI) thread.
class AsyncLiveCombatSummary final : public QObject {
public:
    using ComputeFunction = std::function<QJsonObject(const Catalog &, const GameState &, const RecommendationWorkPtr &)>;
    explicit AsyncLiveCombatSummary(ComputeFunction compute = {}, QObject *parent = nullptr, int budgetMs = 200);
    ~AsyncLiveCombatSummary() override;
    QJsonObject update(const Catalog &, const GameState &);
    QJsonObject report() const;
    void setSynchronousInspection(bool enabled);
    // Called on the owner thread once a worker completes; callers may refresh
    // the widgets. It never runs on the computation thread.
    std::function<void()> onUpdated;

private:
    struct Request {
        Catalog catalog;
        GameState state;
        QByteArray basis;
        qint64 requestedAtMs = 0;
    };
    bool eligible(const Request &) const;
    bool modelCurrent(const Request &) const;
    void maybeDispatch();
    void completed();
    QJsonObject project(const Request &) const;
    ComputeFunction computeFunction;
    std::shared_ptr<LiveCombatSummary> workerEngine;
    LiveCombatSummary inspectionEngine;
    QThreadPool pool;
    QFutureWatcher<QJsonObject> watcher;
    QTimer dispatchTimer;
    QElapsedTimer clock;
    RecommendationWorkPtr activeWork;
    std::optional<Request> desired, active, published;
    QJsonObject publishedSummary, nativeReport;
    QByteArray failedBasis;
    double lastGameTime = -1, lastWorkerMs = 0, maxWorkerMs = 0, lastGuiMs = 0, maxGuiMs = 0;
    qint64 updates = 0, dispatched = 0, completions = 0, discarded = 0, coalesced = 0;
    qint64 lastDispatchAt = -1000, failedAt = -1000;
    int workBudgetMs;
    bool synchronousInspection = false, finishing = false;
    QString lastStatus = QStringLiteral("waiting_snapshot"), lastDiscardReason;
};
