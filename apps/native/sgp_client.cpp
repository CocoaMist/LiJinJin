#include "sgp_client.h"
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QSslConfiguration>
#include <QUrlQuery>
#include <memory>

SgpClient::SgpClient(const QString &root,QObject *parent):QObject(parent),workspace(root),network(this) {
    network.setProxy(QNetworkProxy::NoProxy);
    certificates=QSslCertificate::fromPath(root+QStringLiteral("/data/riotgames.pem"));
    deadline.setSingleShot(true);
    connect(&deadline,&QTimer::timeout,this,[this]{
        if(history.size())finish(false,QStringLiteral("已达到本次查询时限，显示已完成的记录"));
        else stop(QStringLiteral("查询超时，请稍后重试"));
    });
}
SgpClient::~SgpClient() { cancel(); }
void SgpClient::clearSecrets() {
    entitlement.fill('\0');leagueSession.fill('\0');
    entitlement.clear();leagueSession.clear();
    connection.authorization.fill('\0');connection.riotAuthorization.fill('\0');connection={};
    puuids={};selectedPuuid.clear();
}
void SgpClient::cancel() {
    ++generation;running=false;deadline.stop();
    if(pending){pending->disconnect(this);pending->abort();pending->deleteLater();pending=nullptr;}
    clearSecrets();
}
void SgpClient::stop(const QString &reason) { cancel();emit failed(reason); }
void SgpClient::finish(bool exhausted,const QString &reason) {
    const auto summary=history.summary(selectedServer,exhausted,reason);
    const auto records=history.records();
    cancel();emit completed(summary,records);
}
void SgpClient::search(const QString &riotId,const QString &server,int maxMatches) {
    cancel();history={};offset=0;serverIndex=0;aliasAttempt=0;aliasLocalFallback=false;selectedServer.clear();
    const auto parts=riotId.trimmed().split(QChar('#'));
    if(parts.size()!=2||parts[0].trimmed().isEmpty()||parts[1].trimmed().isEmpty()||riotId.size()>160){emit failed(QStringLiteral("请输入完整的 名称#编号"));return;}
    if(!server.isEmpty()&&!sgpServers().contains(server)){emit failed(QStringLiteral("大区不在已验证的查询列表内"));return;}
    if(maxMatches<20||maxMatches>1000||maxMatches%20){emit failed(QStringLiteral("查询上限须为 20–1000，且是 20 的倍数"));return;}
    name=parts[0].trimmed();tag=parts[1].trimmed();requestedServer=server;limit=maxMatches;
    connection=discoverLcu();
    if(!connection.valid()){stop(QStringLiteral("请先打开并登录英雄联盟客户端，再查询"));return;}
    if(certificates.isEmpty()){stop(QStringLiteral("本地客户端证书缺失，请检查安装目录"));return;}
    auto current=connection.rsoPlatformId;current.remove(QStringLiteral("TENCENT_"));
    serverOrder=server.isEmpty()?sgpServers().keys():QStringList{server};
    if(server.isEmpty()&&serverOrder.contains(current)){serverOrder.removeAll(current);serverOrder.prepend(current);}
    running=true;deadline.start(120000);emit progress(QStringLiteral("正在验证当前登录会话…"));
    local(QStringLiteral("/entitlements/v1/token"),[this](const QJsonValue &v,int){
        entitlement=v.toObject().value("accessToken").toString().toUtf8();
        if(entitlement.isEmpty()){stop(QStringLiteral("当前会话未提供战绩凭据，请确认客户端已登录"));return;}
        local(QStringLiteral("/lol-league-session/v1/league-session-token"),[this](const QJsonValue &v,int){
            leagueSession=v.toString().trimmed().toUtf8();
            if(leagueSession.isEmpty()){stop(QStringLiteral("当前会话未提供召唤师查询凭据"));return;}
            emit progress(QStringLiteral("正在精确匹配召唤师名称与编号…"));aliases();
        });
    });
}
void SgpClient::request(const QUrl &url,const QByteArray &auth,const QByteArray &body,Done done,quint32 pid) {
    if(!running)return;
    const bool localUrl=url.host()==QStringLiteral("127.0.0.1");
    const bool allowedLocal=localUrl&&url.scheme()==QStringLiteral("https")&&url.isValid()&&url.userInfo().isEmpty()&&!url.hasFragment()
        &&pid&&((url.port()==connection.port&&pid==connection.processId)||(url.port()==connection.riotPort&&pid==connection.riotProcessId))
        &&lcuPortOwnedBy(quint16(url.port()),pid);
    const auto server=serverOrder.value(serverIndex);
    if(auth.isEmpty()||(!allowedLocal&&!(!localUrl&&allowedSgpUrl(url,server)))){stop(QStringLiteral("查询地址或客户端归属检查未通过"));return;}
    QNetworkRequest req(url);req.setTransferTimeout(localUrl?5000:9000);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
    req.setRawHeader("Authorization",auth);req.setRawHeader("Accept","application/json");
    req.setRawHeader("User-Agent","LiXin-ReadOnly-History/2.0");
    if(localUrl){auto ssl=QSslConfiguration::defaultConfiguration();ssl.setCaCertificates(certificates);ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
        req.setSslConfiguration(ssl);req.setPeerVerifyName(QStringLiteral("127.0.0.1"));}
    if(!body.isEmpty())req.setHeader(QNetworkRequest::ContentTypeHeader,QStringLiteral("application/json"));
    auto reply=body.isEmpty()?network.get(req):network.post(req,body);pending=reply;
    const auto ticket=generation;auto bytes=std::make_shared<QByteArray>();auto exceeded=std::make_shared<bool>(false);
    reply->setReadBufferSize(16*1024*1024+1);
    auto timer=new QTimer(reply);timer->setSingleShot(true);
    connect(timer,&QTimer::timeout,reply,[reply]{reply->abort();});timer->start(localUrl?6500:11000);
    connect(reply,&QIODevice::readyRead,this,[reply,bytes,exceeded]{
        if(bytes->size()+reply->bytesAvailable()>16*1024*1024){*exceeded=true;reply->abort();return;}*bytes+=reply->readAll();
    });
    connect(reply,&QNetworkReply::finished,this,[this,reply,ticket,bytes,exceeded,done=std::move(done)]{
        const int http=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto error=reply->error();
        if(!*exceeded){if(bytes->size()+reply->bytesAvailable()>16*1024*1024)*exceeded=true;else *bytes+=reply->readAll();}
        reply->deleteLater();
        if(ticket!=generation||!running||pending!=reply)return;
        pending=nullptr;
        if(http==401||http==403||http==429){
            const auto reason=http==429?QStringLiteral("查询被限流，已停止；请稍后重试"):QStringLiteral("登录授权已失效或该大区拒绝访问，已停止查询");
            if(history.size())finish(false,reason);else stop(reason);return;
        }
        if(*exceeded){stop(QStringLiteral("返回数据过大，已停止查询"));return;}
        // 404 is an expected absence for alias/profile fallback. Other network
        // failures are returned as status only; error strings may contain IDs.
        if(error!=QNetworkReply::NoError&&http!=404){
            if(history.size())finish(false,QStringLiteral("连接中断，只显示已完成的记录"));
            else stop(QStringLiteral("查询连接失败（HTTP %1），请确认客户端登录与网络").arg(http));return;
        }
        done(sgpJsonValue(*bytes),http);
    });
}
void SgpClient::local(const QString &path,Done done,bool riot) {
    const auto port=riot?connection.riotPort:connection.port;
    if(!port){stop(QStringLiteral("本地查询服务尚未就绪"));return;}
    request(QUrl(QStringLiteral("https://127.0.0.1:%1").arg(port)+path),
        riot?connection.riotAuthorization:connection.authorization,{},std::move(done),riot?connection.riotProcessId:connection.processId);
}
void SgpClient::aliases() {
    QUrlQuery query;query.addQueryItem(QStringLiteral("gameName"),aliasAttempt==1?name+QChar('#')+tag:name);
    if(aliasAttempt==0)query.addQueryItem(QStringLiteral("tagLine"),tag);
    const bool riot=connection.riotPort&&!aliasLocalFallback;
    local(QStringLiteral("/player-account/aliases/v1/lookup?")+query.toString(QUrl::FullyEncoded),[this,riot](const QJsonValue &v,int http){
        if(http==404&&riot){aliasLocalFallback=true;aliases();return;}
        puuids=exactRiotAliases(v,name,tag);
        if(!puuids.isEmpty()){profiles();return;}
        if(++aliasAttempt<3){aliases();return;}
        legacyAlias();
    },riot);
}
void SgpClient::legacyAlias() {
    QUrlQuery query;query.addQueryItem(QStringLiteral("name"),name);
    local(QStringLiteral("/lol-summoner/v1/summoners?")+query.toString(QUrl::FullyEncoded),[this](const QJsonValue &v,int){
        puuids=exactRiotAliases(v,name,tag);if(!puuids.isEmpty()){profiles();return;}
        local(QStringLiteral("/lol-summoner/v1/current-summoner"),[this](const QJsonValue &v,int){
            puuids=exactRiotAliases(v,name,tag);if(!puuids.isEmpty()){profiles();return;}
            stop(QStringLiteral("未找到完整名称与编号一致的召唤师；请核对名称和大区"));
        });
    });
}
void SgpClient::profiles() {
    if(serverIndex>=serverOrder.size()){stop(QStringLiteral("已确认名称，可访问大区未返回该召唤师"));return;}
    const auto server=serverOrder[serverIndex];emit progress(QStringLiteral("正在定位召唤师大区：%1…").arg(server));
    const auto url=QUrl(QStringLiteral("https://%1:21019/summoner-ledge/v1/regions/%2/summoners/puuids").arg(sgpServers()[server],server.toLower()));
    request(url,QByteArrayLiteral("Bearer ")+leagueSession,QJsonDocument(puuids).toJson(QJsonDocument::Compact),[this,server](const QJsonValue &v,int http){
        QJsonArray rows=v.isArray()?v.toArray():QJsonArray{};if(v.isObject())rows.append(v);
        if(http==200)for(const auto &pv:rows){const auto id=pv.toObject().value("puuid").toString();if(puuids.contains(id)){selectedPuuid=id;selectedServer=server;page();return;}}
        ++serverIndex;profiles();
    });
}
void SgpClient::page() {
    if(offset>=limit){finish(false,QStringLiteral("已达到本次 %1 条查询上限").arg(limit));return;}
    emit progress(QStringLiteral("%1 · 已读取 %2 条，正在继续…").arg(selectedServer).arg(history.size()));
    QUrl url(QStringLiteral("https://%1:21019/match-history-query/v1/products/lol/player/%2/SUMMARY")
        .arg(sgpServers()[selectedServer],QString::fromUtf8(QUrl::toPercentEncoding(selectedPuuid))));
    QUrlQuery query;query.addQueryItem(QStringLiteral("startIndex"),QString::number(offset));query.addQueryItem(QStringLiteral("count"),QStringLiteral("20"));url.setQuery(query);
    request(url,QByteArrayLiteral("Bearer ")+entitlement,{},[this](const QJsonValue &v,int http){
        const auto payload=v.toObject();
        if(http!=200||!payload.value("games").isArray()){if(history.size())finish(false,QStringLiteral("分页格式不完整，保留已验证记录"));else stop(QStringLiteral("战绩接口尚未返回可用的记录"));return;}
        const auto batch=payload.value("games").toArray();
        if(batch.isEmpty()){finish(true);return;}
        if(batch.size()>20){stop(QStringLiteral("接口返回超出请求的分页大小，已停止查询"));return;}
        const auto result=history.append(batch,selectedPuuid);
        if(result.invalid){finish(false,QStringLiteral("遇到无效记录，已停止后续分页"));return;}
        if(!result.added){finish(false,QStringLiteral("接口重复返回同一页，已停止"));return;}
        offset+=batch.size();
        const auto total=payload.value("totalCount");
        if(batch.size()<20||(total.isDouble()&&total.toDouble()>=0&&offset>=total.toDouble()))finish(true);
        else page();
    });
}
