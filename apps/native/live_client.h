#pragma once
#include "analysis.h"
#include "lcu_discovery.h"
#include "match_context.h"
#include "tactical_context.h"
#include <QNetworkAccessManager>
#include <QElapsedTimer>
#include <QTimer>
#include <functional>

// Local identity observations only. Match a uniquely observed roster slot;
// preserve stable IDs and reject conflicting observations instead of replacing
// the entire loading roster with display aliases. Never export this result.
QJsonArray mergePrivateRosterIdentities(const QJsonArray &prior, const QJsonArray &observed);

class LiveClient : public QObject {
    Q_OBJECT
public:
    LiveClient(const Catalog &, QObject *parent = nullptr);
    void start();
    enum class RefreshResult { Scheduled, Busy, RateLimited, Disabled };
    // Read-only local data sampling; never restarts automatic polling or asks
    // an AI provider to run. Busy/RateLimited retain one merged intent, with a
    // bounded wait; scope changes cancel it. Status is sampling, not readiness.
    RefreshResult requestRefresh();
    static QString refreshResultText(RefreshResult);
    void setEnabled(bool enabled);
    void setClientFolder(const QString &folder);
    void loadReplay(const QString &path);
    // The authenticated current account's nickname, only for local UI display.
    // Keep it out of GameState, anonymous reports, provider inputs and settings.
    QString localPlayerDisplayName() const { return localPlayerName; }
    // Opaque current-account binding for local UI workers only. This is not a
    // public identifier: never persist it, serialize it or send it to a model.
    QString localAccountScope() const { return localAccountScopeKey; }
    // Local window/file routing only. Never put PID or installation paths into
    // anonymous snapshots, public reports or model inputs.
    quint32 localClientProcessId() const { return enabled&&connection.valid()?connection.uxProcessId:0; }
    QString localGameDirectory() const;
    // Local UI artwork from the authenticated League career backdrop. Never
    // include account-linked artwork in GameState, reports or provider inputs.
    QByteArray localPlayerBackdrop() const { return localBackdrop; }
    // Explicit lobby-only acceptance: GET requests, sanitized results, no game API.
    void probeLobby(std::function<void(QJsonObject)> callback);
    // Read-only custom KIWI acceptance; never starts or controls a game.
    void probeCustomGame(std::function<void(QJsonObject)> callback);
    void probeAugmentCapabilities(std::function<void(QJsonObject)> callback);
signals:
    void snapshotChanged(const GameState &state);
    void connectionChanged(const QString &status);
    void manualRefreshStatusChanged(const QString &status);
    void localAccountScopeChanged(const QString &scope);
    void localPlayerDisplayNameChanged(const QString &name);
    void localPlayerBackdropChanged(const QByteArray &imageBytes);
    // Local-only identities for the read-only history bridge. Never serialize
    // this payload in a public report or forward it to an AI provider.
    void privateRosterChanged(const QJsonObject &roster);
private:
    friend struct LiveClientIdentityTestAccess;
    const Catalog &catalog;
    QNetworkAccessManager network;
    QTimer timer;
    QString clientFolder, phase, observedPatch,observedMode;
    LcuConnection connection;
    quint64 credentialGeneration = 0;
    quint64 patchRequestSerial = 0;
    int lastPatchRequestTick = -60;
    bool patchPending = false;
    int ticks = 0, queue = 0;
    bool enabled = true, livePending = false, lcuPending = false;
    QElapsedTimer manualRefreshClock;
    int pendingReadRequests=0;
    bool manualRefreshQueued=false;
    QTimer manualRefreshTimer;
    QElapsedTimer refreshWaitClock;
    quint64 refreshScopeGeneration=0;
    bool queuedRefreshUser=false,manualSampleInFlight=false,manualSampleDispatching=false,manualSampleUser=false;
    bool manualSampleSucceeded=false,manualSampleTimedOut=false;
    int lifecycleStage=-1;
    GameState current;
    MatchContextTracker match;
    TacticalTimeline tacticalTimeline;
    QString selfPuuid;
    QString localPlayerName;
    QString localAccountScopeKey;
    QByteArray localBackdrop;
    QString localBackdropSummonerId,localBackdropAsset;
    int lastBackdropRequestTick=-60;
    quint64 backdropRequestSerial=0;
    bool backdropPending=false;
    int lastIdentityRequestTick = -15;
    QJsonObject lastFlow;
    QJsonArray privatePlayers;
    QByteArray privateRosterKey;
    quint64 phaseRequestSerial=0,identityRequestSerial=0,liveRequestSerial=0;
    bool identityPending=false;
    void resetMatch();
    void publishSnapshot();
    void publishPrivateRoster();
    void refreshSelfIdentity(const QString &base);
    void completeSelfIdentity(quint64 generation, quint64 serial, quint64 matchGeneration,
                              const QJsonValue &, const QString &error, int status);
    void clearLocalPlayerDisplayName();
    void updateLocalAccountScope(const QString &scope);
    void clearLocalPlayerBackdrop();
    void refreshLocalPlayerBackdrop(const QJsonObject &profile);
    void collectChampionSelectIdentities(const QJsonObject &);
    void collectGameflowIdentities(const QJsonObject &);
    void collectLiveIdentities(const QJsonObject &,const GameState &);
    void tick();
    void sample(bool manual);
    void observeRefreshPhase(const QString &phase);
    RefreshResult enqueueRefresh(bool user);
    void attemptRefresh();
    void finishRefreshSample();
    void resetRefreshScope(const QString &status);
    void discover();
    void clearObservedPatch();
    void refreshObservedPatch(const QString &base);
    void get(const QUrl &, bool authenticated, std::function<void(QJsonValue,QString,int)> callback);
    void getBytes(const QUrl &, bool authenticated, std::function<void(QByteArray,QString,int)> callback,
                  qint64 maximumBytes=3*1024*1024);
};
