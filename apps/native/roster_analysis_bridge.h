#pragma once
#include <QObject>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QStringList>

class QProcess;

// Private identities only enter stdin of the local adapter.
// Neither result() nor report() includes player identities or raw matches.
class RosterAnalysisBridge : public QObject {
    Q_OBJECT
public:
    explicit RosterAnalysisBridge(QObject *parent=nullptr);
    void configure(const QString &root,const QString &python);
    void request(const QJsonObject &privateRoster);
    void setGamePhase(const QString &phase);
    void invalidate();
    QJsonObject report() const;
    static bool usesCachedOnly(const QJsonObject &privateRoster);
    static QStringList adapterArguments(const QString &workspace,const QJsonObject &privateRoster);
    // Pure, bounded public boundary; useful for checking the subprocess contract
    // without opening a database, launching a process or sending a request.
    static QJsonObject sanitizeAnonymousResult(const QJsonObject &,const QString &contextId);
    static bool sameHistoryRoster(const QJsonObject &,const QJsonObject &);
    static QJsonObject retainPregameHistory(const QJsonObject &current,const QJsonObject &previous);
signals:
    void result(const QJsonObject &anonymousResult);
private:
    QString workspace,pythonPath,contextId,status=QStringLiteral("idle");
    QJsonObject privateRoster;
    QJsonObject retainedRoster,retainedResult;
    QByteArray databaseBasis;
    QTimer refreshTimer;
    qint64 nextRefreshAtMs=0;
    QString nextRefreshReason;
    QPointer<QProcess> pending;
    quint64 generation=0;
    int requests=0,completed=0,failures=0,discarded=0;
    int automaticRefreshes=0;
    QByteArray databaseRevision() const;
    void refreshIfNeeded();
    void scheduleNextRefresh(const QJsonObject &anonymousResult);
    void startRequest();
    void fail(QProcess *process,quint64 ticket,const QString &code);
};
