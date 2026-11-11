#pragma once
#include "catalog.h"
#include "history_refresh_lifecycle.h"
#include <QWidget>
#include <QProcess>
#include <QTimer>
#include <QVector>
#include <QHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <functional>
class QLineEdit;
class QComboBox;
class QLabel;
class QPushButton;
class PlayerMatchView;

class HistoryPane : public QWidget {
    Q_OBJECT
public:
    explicit HistoryPane(const Catalog &,QWidget *parent=nullptr);
    ~HistoryPane() override;
    void query(const QString &riotId,int limit=100);
    void personal();
    void setPersonalAccountName(const QString &);
    void setAccountScope(const QString &);
    void refreshPersonalHome(const QString &lifecycleReason={},const QString &eventId={},int attempt=0);
    QJsonObject personalProfileSnapshot() const;
    bool openPersonalMatch(const QString &matchRef={});
    void refreshDecisionReplay();
    void returnToDecisionMatch(const QString &matchRef);
    void setGamePhase(const QString &phase);
    void closeProfile();
    bool profileVisible() const;
    void showProfile();
    void openArchive();
    QJsonObject report() const;
    QJsonObject selfCheck(const QString &outputDir={});
    std::function<void()> onProfileOpened;
    std::function<bool(const QString &)> decisionReplayAvailable;
    std::function<void(const QString &)> onDecisionReplay;
signals:
    void updated();
    void personalProfileUpdated();
private:
    void start();
    void lookup(const QString &action,bool manualRefresh=false);
    void collect(bool manualRefresh=false);
    void cancel();
    void complete(int,QProcess::ExitStatus);
    void render();
    void publishNotice();
    void updateDetailPresentation();
    void updateDetectedServer(const QString &);
    QString selectedServer() const;
    void controls();
    void openPlayer(const QJsonObject &);
    void backToPlayer();
    void clearNavigation();
    bool interruptQuery();
    void pollRevision();
    void reloadCached();
    bool busy() const;
    void stopPersonalHome();
    void completePersonalHome(QProcess *,quint64,int,QProcess::ExitStatus);
    void acceptPersonalHome(const QJsonObject &,quint64);
    void queueLifecycleRefresh(const QString &);
    void dispatchLifecycleRefresh();
    void recordLifecycleCompletion(QProcess *,const QJsonObject &);
    void applyPendingPersonalHistory();
    QJsonObject lifecycleRefreshReport() const;
    const Catalog &catalog;
    QProcess *worker=nullptr;
    QTimer deadline,revisionTimer;
    QProcess *personalHomeWorker=nullptr;
    QTimer personalHomeDeadline;
    QTimer lifecycleDispatchTimer,lifecycleFollowupTimer;
    HistoryRefreshLifecycle historyLifecycle;
    HistoryRefreshWork lifecycleWork;
    HistoryAccountScope accountScope;
    QElapsedTimer lifecycleClock;
    QString lifecycleEventId,lifecycleReason,lifecycleLastReason,lifecycleMatchRef,lifecycleAccountScope;
    QJsonArray lifecycleBaselineRefs;
    QJsonObject lifecyclePublication;
    bool lifecycleBaselineKnown=false;
    HistoryPageReload personalHistoryReload;
    int lifecycleLastAttempt=0;
    int lifecycleEnterRequests=0,lifecycleLeaveRequests=0,lifecycleFollowupRequests=0;
    int lifecycleCompleted=0,lifecycleFailed=0,lifecycleSkipped=0,lifecycleCancelled=0;
    QJsonObject personalHomeData;
    QString personalAccountHint,personalHomeState=QStringLiteral("idle");
    QString detectedServer;
    quint64 personalHomeGeneration=0;
    QLineEdit *name;
    QComboBox *region;
    QPushButton *search,*personalButton,*refresh,*previousPlayer;
    QLabel *status,*recentScore,*poolScore;
    PlayerMatchView *results;
    QWidget *profileBody,*scoreRow,*metadataRow;
    QJsonObject data;
    QString phase,riotId,server,gamePhase,revision;
    QString playerRef;
    struct NavigationFrame {QJsonObject data,view;QString riotId,server,playerRef,name,state;int region=0;bool personal=false;};
    QVector<NavigationFrame> navigation;
    QHash<QString,qint64> refreshTimes;
    quint64 queryGeneration=0;
    QString lastNoticeState;
    int noticeTransitions=0;
    bool quietNoticeRequest=false,suppressNotices=false;
    bool cancelled=false;
    bool paused=false,pendingRefresh=false,refreshAfterLookup=false,personalProfile=false,backgroundLookup=false;
    int limit=100;
    QString state=QStringLiteral("idle");
};
