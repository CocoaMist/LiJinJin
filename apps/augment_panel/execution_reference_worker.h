#pragma once
#include "coach_model.h"
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QObject>
#include <QThreadPool>
#include <QTimer>
#include <functional>
#include <optional>

// Owner-thread API. Optional explanations never own or overwrite a purchase
// result. Same-match soft updates coalesce without repeatedly cancelling a
// bounded native scenario; a changed match/rule scope cancels the old work.
class AsyncExecutionReference final : public QObject {
public:
    using ComputeFunction=std::function<QJsonObject(const Catalog &,const GameState &,
        const QString &,int,const RecommendationWorkPtr &)>;
    explicit AsyncExecutionReference(ComputeFunction compute={},QObject *parent=nullptr);
    ~AsyncExecutionReference() override;
    QJsonObject update(const Catalog &,const GameState &,const QString &preference);
    // Applies only the allowlisted explanation fields after current binding
    // checks. Never copies role guidance or any purchase/ranking fields.
    bool apply(const Catalog &,const GameState &,const QString &preference,QJsonObject &coach) const;
    void invalidate();
    QJsonObject report() const;
    std::function<void()> onUpdated;
private:
    struct Request {
        Catalog catalog;
        GameState state;
        QString preference;
        QByteArray scope,referenceBasis,currentBasis;
        qint64 requestedAtMs=0;
    };
    Request request(const Catalog &,const GameState &,const QString &) const;
    bool eligible(const Request &) const;
    bool referenceCurrent(const Request &) const;
    QJsonObject project(const Request &) const;
    void maybeDispatch();
    void completed();
    ComputeFunction computeFunction;
    QThreadPool pool;
    QFutureWatcher<QJsonObject> watcher;
    QTimer dispatchTimer;
    QElapsedTimer clock;
    RecommendationWorkPtr activeWork;
    std::optional<Request> desired,active,published;
    QJsonObject publishedResult;
    QByteArray failedReferenceBasis;
    qint64 retryAtMs=0;
    qint64 updates=0,dispatched=0,completions=0,discarded=0,coalesced=0,cancelled=0;
    qint64 lastDispatchMs=-1000;
    double lastWorkerMs=0,maxWorkerMs=0,lastGameTime=-1;
    QString lastStatus=QStringLiteral("waiting_snapshot"),lastDiscardReason;
    bool finishing=false;
};
