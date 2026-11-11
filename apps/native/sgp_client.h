#pragma once
#include "lcu_discovery.h"
#include "sgp_history.h"
#include <QNetworkAccessManager>
#include <QPointer>
#include <QSslCertificate>
#include <QTimer>
#include <functional>

// Asynchronous, bounded, read-only access using the already logged-in client.
// Credentials are never written to disk, returned in signals or sent to AI.
class SgpClient : public QObject {
    Q_OBJECT
public:
    explicit SgpClient(const QString &root,QObject *parent=nullptr);
    ~SgpClient() override;
    void search(const QString &riotId,const QString &server={},int maxMatches=200);
    void cancel();
    bool busy() const { return running; }
signals:
    void progress(const QString &);
    void completed(const QJsonObject &summary,const QJsonArray &records);
    void failed(const QString &);
private:
    using Done=std::function<void(const QJsonValue &,int)>;
    void request(const QUrl &,const QByteArray &auth,const QByteArray &body,Done,quint32 localPid=0);
    void local(const QString &path,Done,bool riot=false);
    void aliases();
    void legacyAlias();
    void profiles();
    void page();
    void stop(const QString &);
    void finish(bool exhausted,const QString &reason={});
    void clearSecrets();
    QString workspace,name,tag,requestedServer,selectedServer,selectedPuuid;
    QStringList serverOrder;
    LcuConnection connection;
    QNetworkAccessManager network;
    QPointer<QNetworkReply> pending;
    QList<QSslCertificate> certificates;
    QTimer deadline;
    QByteArray entitlement,leagueSession;
    QJsonArray puuids;
    SgpHistory history;
    quint64 generation=0;
    int aliasAttempt=0,serverIndex=0,offset=0,limit=200;
    bool running=false,aliasLocalFallback=false;
};
