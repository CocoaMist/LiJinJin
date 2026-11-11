#include "live_client.h"
#include <QFile>
#include <QCryptographicHash>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QPointer>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QFileInfo>
#include <QDir>
#include <QRegularExpression>
#include <algorithm>

namespace {
int observedChampionId(const QJsonObject &p){const auto v=p.value(QStringLiteral("championId"));const int id=v.toInt();return v.isDouble()&&v.toDouble()==id&&id>0?id:0;}
QString localIdentityText(const QJsonValue &v){const auto value=v.toString();return value.size()<=160&&!value.contains(QChar(u'\r'))&&!value.contains(QChar(u'\n'))?value:QString();}
QString currentSummonerNickname(const QJsonObject &profile){
    for(const auto &key:{QStringLiteral("gameName"),QStringLiteral("displayName")}){
        auto name=localIdentityText(profile.value(key)).trimmed();
        bool control=false;for(const auto character:name)control|=character.category()==QChar::Other_Control;
        if(control)continue;
        const auto tag=name.indexOf(QChar(u'#'));if(tag>=0)name=name.left(tag).trimmed();
        if(!name.isEmpty())return name;
    }
    return {};
}
QString observedRiotId(const QJsonObject &p){
    const auto direct=localIdentityText(p.value(QStringLiteral("riotId"))).trimmed();
    if(direct.count(QChar(u'#'))==1&&!direct.startsWith(QChar(u'#'))&&!direct.endsWith(QChar(u'#')))return direct;
    const auto name=p.value(QStringLiteral("gameName")).toString(p.value(QStringLiteral("riotIdGameName")).toString()).trimmed();
    const auto tag=p.value(QStringLiteral("tagLine")).toString(p.value(QStringLiteral("riotIdTagLine")).toString()).trimmed();
    const auto combined=name+QChar(u'#')+tag;return !name.isEmpty()&&!tag.isEmpty()&&!name.contains(QChar(u'#'))&&!tag.contains(QChar(u'#'))?localIdentityText(combined):QString();
}
QJsonObject privatePlayer(const QJsonObject &p,const QString &team,bool self,const QString &stage,int champion,int cell=-1){
    return {{QStringLiteral("puuid"),localIdentityText(p.value(QStringLiteral("puuid")))},
        {QStringLiteral("riot_id"),observedRiotId(p)},{QStringLiteral("champion_id"),champion},
        {QStringLiteral("team"),team},{QStringLiteral("self"),self},
        {QStringLiteral("cell"),cell>=0?QJsonValue(cell):QJsonValue()},
        {QStringLiteral("visible"),champion>0},{QStringLiteral("source_stage"),stage},
        {QStringLiteral("identity_source"),stage}};
}
}
QJsonArray mergePrivateRosterIdentities(const QJsonArray &prior,const QJsonArray &observed){
    const auto slot=[](const QJsonObject &p){
        const auto team=p.value(QStringLiteral("team")).toString();
        const int champion=p.value(QStringLiteral("champion_id")).toInt();
        if((team!=QStringLiteral("ALLY")&&team!=QStringLiteral("ENEMY"))||champion<=0||!p.value(QStringLiteral("self")).isBool())return QString();
        return team+QChar(u':')+QString::number(champion);
    };
    QMap<QString,int> priorCounts,observedCounts;
    for(const auto &v:prior){const auto key=slot(v.toObject());if(!key.isEmpty())++priorCounts[key];}
    for(const auto &v:observed){const auto key=slot(v.toObject());if(!key.isEmpty())++observedCounts[key];}
    QJsonArray merged;
    for(const auto &v:observed){
        auto row=v.toObject();const auto key=slot(row);
        if(key.isEmpty()||observedCounts.value(key)!=1||priorCounts.value(key)!=1){merged.append(row);continue;}
        QJsonObject previous;for(const auto &p:prior)if(slot(p.toObject())==key){previous=p.toObject();break;}
        const auto cell=[](const QJsonObject &p){const auto value=p.value(QStringLiteral("cell"));const int n=value.toInt(-1);return value.isDouble()&&value.toDouble()==n&&n>=0?n:-1;};
        const bool cellConflict=cell(row)>=0&&cell(previous)>=0&&cell(row)!=cell(previous);
        const auto oldId=localIdentityText(previous.value(QStringLiteral("puuid"))),newId=localIdentityText(row.value(QStringLiteral("puuid")));
        const auto oldAlias=localIdentityText(previous.value(QStringLiteral("riot_id"))),newAlias=localIdentityText(row.value(QStringLiteral("riot_id")));
        const bool conflict=cellConflict||previous.value(QStringLiteral("self"))!=row.value(QStringLiteral("self"))||(!oldId.isEmpty()&&!newId.isEmpty()&&oldId!=newId)
            ||(!oldAlias.isEmpty()&&!newAlias.isEmpty()&&oldAlias!=newAlias);
        if(conflict){
            // A verified loading stable ID may remain usable, but no conflicting
            // live alias is attached to it. An alias-only disagreement has no
            // safe identity and is retried when a better observation arrives.
            if(!oldId.isEmpty()&&!cellConflict&&previous.value(QStringLiteral("self"))==row.value(QStringLiteral("self")))row=previous;
            else{row[QStringLiteral("puuid")]=QString();row[QStringLiteral("riot_id")]=QString();}
            row[QStringLiteral("identity_merge_status")]=QStringLiteral("conflict");merged.append(row);continue;
        }
        if(!oldId.isEmpty())row[QStringLiteral("puuid")]=oldId;
        if(newAlias.isEmpty()&&!oldAlias.isEmpty())row[QStringLiteral("riot_id")]=oldAlias;
        if(cell(row)<0&&cell(previous)>=0)row[QStringLiteral("cell")]=cell(previous);
        if(!oldId.isEmpty()||(newId.isEmpty()&&!oldAlias.isEmpty()&&(newAlias.isEmpty()||oldAlias==newAlias))){
            row[QStringLiteral("source_stage")]=previous.value(QStringLiteral("source_stage"));
            row[QStringLiteral("identity_source")]=previous.value(QStringLiteral("identity_source"));
        }
        row[QStringLiteral("identity_merge_status")]=QStringLiteral("matched_unique_roster_slot");merged.append(row);
    }
    return merged;
}
LiveClient::LiveClient(const Catalog &c, QObject *p) : QObject(p), catalog(c) {
    network.setProxy(QNetworkProxy::NoProxy);
    timer.setInterval(1000);
    connect(&timer, &QTimer::timeout, this, &LiveClient::tick);
    manualRefreshTimer.setSingleShot(true);manualRefreshTimer.setTimerType(Qt::PreciseTimer);
    connect(&manualRefreshTimer,&QTimer::timeout,this,&LiveClient::attemptRefresh);
}
void LiveClient::start() { timer.start(); tick(); }
QString LiveClient::refreshResultText(RefreshResult result){
    switch(result){
    case RefreshResult::Scheduled:return QStringLiteral("已安排重新读取");
    case RefreshResult::Busy:return QStringLiteral("已排队，等待采集");
    case RefreshResult::RateLimited:return QStringLiteral("已排队，稍后采集");
    case RefreshResult::Disabled:return QStringLiteral("自动连接已暂停");
    }return {};
}
LiveClient::RefreshResult LiveClient::requestRefresh() { return enqueueRefresh(true); }
LiveClient::RefreshResult LiveClient::enqueueRefresh(bool user) {
    if(!enabled)return RefreshResult::Disabled;
    const bool alreadyQueued=manualRefreshQueued;
    if(!alreadyQueued){manualRefreshQueued=true;refreshWaitClock.start();}
    queuedRefreshUser=queuedRefreshUser||user;
    const auto result=alreadyQueued||manualSampleInFlight||pendingReadRequests>0?RefreshResult::Busy:
        manualRefreshClock.isValid()&&manualRefreshClock.elapsed()<1000?RefreshResult::RateLimited:RefreshResult::Scheduled;
    if(!manualRefreshTimer.isActive())manualRefreshTimer.start(0);
    if(user)emit manualRefreshStatusChanged(refreshResultText(result));
    return result;
}
void LiveClient::attemptRefresh(){
    manualRefreshTimer.stop();
    if(!manualRefreshQueued||!enabled)return;
    if(refreshWaitClock.elapsed()>=6000){
        const bool notify=queuedRefreshUser;manualRefreshQueued=false;queuedRefreshUser=false;
        if(notify)emit manualRefreshStatusChanged(QStringLiteral("刷新等待超时，请重试"));
        return;
    }
    if(manualSampleInFlight||pendingReadRequests>0){manualRefreshTimer.start(100);return;}
    const auto wait=manualRefreshClock.isValid()?1000-manualRefreshClock.elapsed():0;
    if(wait>0){manualRefreshTimer.start(int(std::min(qint64(100),wait)));return;}
    manualRefreshQueued=false;manualSampleUser=queuedRefreshUser;queuedRefreshUser=false;
    manualSampleInFlight=true;manualSampleDispatching=true;manualSampleSucceeded=false;manualSampleTimedOut=false;
    manualRefreshClock.start();const auto scope=refreshScopeGeneration;
    if(manualSampleUser)emit manualRefreshStatusChanged(QStringLiteral("正在采集对局数据"));
    sample(true);
    if(scope!=refreshScopeGeneration)return;
    manualSampleDispatching=false;
    if(pendingReadRequests==0)finishRefreshSample();
}
void LiveClient::finishRefreshSample(){
    if(!manualSampleInFlight||manualSampleDispatching||pendingReadRequests>0)return;
    const bool notify=manualSampleUser;manualSampleInFlight=false;manualSampleUser=false;
    if(notify)emit manualRefreshStatusChanged(manualSampleTimedOut?QStringLiteral("采样超时，请重试"):
        manualSampleSucceeded?QStringLiteral("采样已返回"):QStringLiteral("采样未成功，请重试"));
    if(manualRefreshQueued){
        if(queuedRefreshUser)emit manualRefreshStatusChanged(QStringLiteral("已排队，稍后采集"));
        manualRefreshTimer.start(0);
    }
}
void LiveClient::resetRefreshScope(const QString &status){
    const bool notify=queuedRefreshUser||manualSampleUser;
    ++refreshScopeGeneration;manualRefreshTimer.stop();manualRefreshClock.invalidate();
    manualRefreshQueued=queuedRefreshUser=manualSampleInFlight=manualSampleDispatching=manualSampleUser=false;
    manualSampleSucceeded=manualSampleTimedOut=false;pendingReadRequests=0;lifecycleStage=-1;
    if(notify)emit manualRefreshStatusChanged(status);
}
void LiveClient::observeRefreshPhase(const QString &value) {
    if(!enabled)return;
    const bool inside=MatchContextTracker::isLoadingPhase(value)||value==QStringLiteral("InProgress")||value==QStringLiteral("Reconnect");
    static const QStringList outside{QStringLiteral("None"),QStringLiteral("Lobby"),QStringLiteral("Matchmaking"),QStringLiteral("ReadyCheck"),QStringLiteral("ChampSelect"),QStringLiteral("WaitingForStats"),QStringLiteral("PreEndOfGame"),QStringLiteral("EndOfGame"),QStringLiteral("TerminatedInError")};
    if(!inside&&!outside.contains(value))return;
    const int next=inside?1:0;
    const bool changed=(lifecycleStage>=0&&next!=lifecycleStage)||(lifecycleStage<0&&inside);
    lifecycleStage=next;
    if(!changed)return;
    enqueueRefresh(false);
}
void LiveClient::setEnabled(bool value) {
    enabled = value;
    if (!enabled) { resetRefreshScope(QStringLiteral("刷新已取消"));++phaseRequestSerial;++identityRequestSerial;++liveRequestSerial;identityPending=false;lcuPending=false;livePending=false;clearLocalPlayerDisplayName();clearObservedPatch();resetMatch(); current = {}; emit snapshotChanged(current); emit connectionChanged(QStringLiteral("自动连接已暂停")); }
    else tick();
}
void LiveClient::setClientFolder(const QString &folder) { clientFolder = folder; discover(); }
void LiveClient::discover() {
    auto found = discoverLcu(clientFolder);
    if (found.port != connection.port || found.processId != connection.processId || found.authorization != connection.authorization) {
        resetRefreshScope(QStringLiteral("会话已变化，刷新已取消"));
        ++credentialGeneration;++phaseRequestSerial;++identityRequestSerial;++liveRequestSerial;identityPending=false;selfPuuid.clear();clearLocalPlayerDisplayName();clearObservedPatch();resetMatch();phase.clear();queue=0;observedMode.clear();current={};lcuPending=false;livePending=false;emit snapshotChanged(current);
    }
    if (!found.folder.isEmpty()) clientFolder = found.folder;
    connection = std::move(found);
}
void LiveClient::clearObservedPatch() {
    ++patchRequestSerial;patchPending=false;lastPatchRequestTick=-60;
    observedPatch.clear();current.observedPatch.clear();
}
void LiveClient::refreshObservedPatch(const QString &base) {
    if (!enabled || !connection.valid() || patchPending || (!observedPatch.isEmpty() && ticks-lastPatchRequestTick<60)) return;
    patchPending=true;lastPatchRequestTick=ticks;
    const auto generation=credentialGeneration,serial=++patchRequestSerial;
    get(QUrl(base+QStringLiteral("/lol-patch/v1/game-version")),true,[this,generation,serial](QJsonValue value,QString error,int status){
        if (generation!=credentialGeneration || serial!=patchRequestSerial || !enabled) return;
        patchPending=false;QString next;
        if (error.isEmpty() && status==200)next=parseObservedGamePatch(value);
        observedPatch=next;
        if (current.observedPatch!=next) { current.observedPatch=next;publishSnapshot(); }
    });
}
void LiveClient::resetMatch(){
    tacticalTimeline.clear();
    match.clear();lastFlow={};privatePlayers={};privateRosterKey.clear();
    emit privateRosterChanged({});
}
QString LiveClient::localGameDirectory() const {
    if(!enabled||!connection.valid()||connection.folder.isEmpty())return {};
    const QDir folder(connection.folder);
    for(const auto &relative:{QStringLiteral("Game"),QStringLiteral("../Game")}){
        const QFileInfo candidate(folder.absoluteFilePath(relative));
        const auto canonical=candidate.canonicalFilePath();
        const QFileInfo executable(QDir(canonical).filePath(QStringLiteral("League of Legends.exe")));
        if(!canonical.isEmpty()&&candidate.isDir()&&!candidate.isSymLink()&&executable.isFile()&&!executable.isSymLink())return canonical;
    }
    return {};
}
void LiveClient::publishSnapshot(){
    current.matchContext=match.json();
    if(!current.matchContext.isEmpty())current.matchContext[QStringLiteral("composition")]=analyzeMatchComposition(current.matchContext,catalog.champions);
    current.observedPatch=observedPatch;
    if(queue>0)current.queueId=queue;
    if(current.source==GameState::Source::Offline){current.phase=phase;current.gameMode=!observedMode.isEmpty()?observedMode:queue==2400?QStringLiteral("KIWI"):QString();}
    current.tacticalHistory=tacticalTimeline.observe(current);
    if(current.source==GameState::Source::Live)observeRefreshPhase(current.phase);
    emit snapshotChanged(current);publishPrivateRoster();
}
void LiveClient::publishPrivateRoster(){
    if(!match.active())return;
    auto server=connection.rsoPlatformId;if(server.startsWith(QStringLiteral("TENCENT_")))server.remove(0,8);
    static const QRegularExpression validServer(QStringLiteral("^[A-Z0-9_]{2,24}$")),validPlatform(QStringLiteral("^[A-Za-z0-9_-]{2,32}$"));
    if(!validServer.match(server).hasMatch())server.clear();
    auto platform=lastFlow.value(QStringLiteral("gameData")).toObject().value(QStringLiteral("platformId")).toString();if(!validPlatform.match(platform).hasMatch())platform.clear();
    const auto gameId=match.privateGameId();const auto matchSnapshot=match.json();
    QList<QJsonObject> sorted;for(const auto &v:privatePlayers){auto p=v.toObject();p.remove(QStringLiteral("slot_token"));const auto token=rosterSlotToken(match.contextId(),p.value(QStringLiteral("puuid")).toString());if(p.value(QStringLiteral("identity_merge_status"))!=QStringLiteral("conflict")&&!token.isEmpty())p[QStringLiteral("slot_token")]=token;sorted.append(p);}std::sort(sorted.begin(),sorted.end(),[](const QJsonObject &a,const QJsonObject &b){return QJsonDocument(a).toJson(QJsonDocument::Compact)<QJsonDocument(b).toJson(QJsonDocument::Compact);});QJsonArray players;for(const auto &v:sorted)players.append(v);
    QJsonObject payload{{QStringLiteral("schema_version"),1},{QStringLiteral("context_id"),match.contextId()},
        {QStringLiteral("server"),server.isEmpty()?QJsonValue():QJsonValue(server)},{QStringLiteral("server_source"),server.isEmpty()?QJsonValue():QJsonValue(QStringLiteral("lcu_process_rso_platform"))},
        {QStringLiteral("game_id"),gameId.isEmpty()?QJsonValue():QJsonValue(gameId)},{QStringLiteral("platform"),platform.isEmpty()?QJsonValue():QJsonValue(platform)},
        {QStringLiteral("phase"),phase},{QStringLiteral("stage"),matchSnapshot.value(QStringLiteral("stage"))},
        {QStringLiteral("patch"),observedPatch.isEmpty()?QJsonValue():QJsonValue(observedPatch)},
        {QStringLiteral("mode"),current.gameMode.isEmpty()?QJsonValue():QJsonValue(current.gameMode)},
        {QStringLiteral("queue_id"),queue>0?QJsonValue(queue):QJsonValue()},{QStringLiteral("players"),players}};
    // The observation clock must not restart the local history subprocess.
    const auto key=QJsonDocument(payload).toJson(QJsonDocument::Compact);if(key==privateRosterKey)return;privateRosterKey=key;
    payload[QStringLiteral("as_of_ms")]=matchSnapshot.value(QStringLiteral("started_at_ms"));emit privateRosterChanged(payload);
}
void LiveClient::collectChampionSelectIdentities(const QJsonObject &o){
    QJsonArray rows;const int local=o.value(QStringLiteral("localPlayerCellId")).toInt(-1);int count=0;
    for(const auto &v:o.value(QStringLiteral("myTeam")).toArray())if(local>=0&&v.toObject().value(QStringLiteral("cellId")).toInt(-2)==local)++count;
    for(const auto &side:{QStringLiteral("myTeam"),QStringLiteral("theirTeam")})for(const auto &v:o.value(side).toArray()){
        const auto p=v.toObject();const int champion=observedChampionId(p);if(champion<=0)continue;
        const bool self=side==QStringLiteral("myTeam")&&count==1&&p.value(QStringLiteral("cellId")).toInt(-2)==local;
        if(self&&!localIdentityText(p.value(QStringLiteral("puuid"))).isEmpty())selfPuuid=localIdentityText(p.value(QStringLiteral("puuid")));
        rows.append(privatePlayer(p,side==QStringLiteral("myTeam")?QStringLiteral("ALLY"):QStringLiteral("ENEMY"),self,QStringLiteral("champ_select"),champion,p.value(QStringLiteral("cellId")).toInt(-1)));
    }privatePlayers=rows;
}
void LiveClient::collectGameflowIdentities(const QJsonObject &o){
    const auto data=o.value(QStringLiteral("gameData")).toObject();if(!data.value(QStringLiteral("teamOne")).isArray()&&!data.value(QStringLiteral("teamTwo")).isArray())return;
    int selves=0,own=0;for(int side=1;side<=2;++side)for(const auto &v:data.value(side==1?QStringLiteral("teamOne"):QStringLiteral("teamTwo")).toArray())if(!selfPuuid.isEmpty()&&v.toObject().value(QStringLiteral("puuid")).toString()==selfPuuid){++selves;own=side;}
    // A weaker incomplete phase read must not erase identities already joined
    // within this match. The next verified read can still add stable IDs.
    if(selves!=1&&!privatePlayers.isEmpty())return;
    QJsonArray rows;for(int side=1;side<=2;++side)for(const auto &v:data.value(side==1?QStringLiteral("teamOne"):QStringLiteral("teamTwo")).toArray()){
        const auto p=v.toObject();const int champion=observedChampionId(p);if(champion<=0)continue;
        rows.append(privatePlayer(p,selves==1?(side==own?QStringLiteral("ALLY"):QStringLiteral("ENEMY")):QStringLiteral("UNKNOWN"),selves==1&&p.value(QStringLiteral("puuid")).toString()==selfPuuid,QStringLiteral("loading"),champion));
    }privatePlayers=mergePrivateRosterIdentities(privatePlayers,rows);
}
void LiveClient::collectLiveIdentities(const QJsonObject &o,const GameState &s){
    // A partial loading roster must not prevent the other nine observations.
    // Preserve verified stable IDs per unique slot, while filling empty aliases.
    QString own;int selves=0;for(const auto &v:s.players)if(v.toObject().value(QStringLiteral("self")).toBool()){++selves;own=v.toObject().value(QStringLiteral("team")).toString();}
    QJsonArray rows;const auto raw=o.value(QStringLiteral("allPlayers")).toArray();
    if(raw.size()!=s.players.size())return;
    for(int i=0;i<raw.size();++i){const auto p=s.players[i].toObject();const int champion=p.value(QStringLiteral("champion_id")).toInt();if(champion<=0)continue;const auto team=p.value(QStringLiteral("team")).toString();rows.append(privatePlayer(raw[i].toObject(),selves==1&&(team==QStringLiteral("ORDER")||team==QStringLiteral("CHAOS"))?(team==own?QStringLiteral("ALLY"):QStringLiteral("ENEMY")):QStringLiteral("UNKNOWN"),selves==1&&p.value(QStringLiteral("self")).toBool(),QStringLiteral("in_game"),champion));}privatePlayers=mergePrivateRosterIdentities(privatePlayers,rows);
}
void LiveClient::refreshSelfIdentity(const QString &base){
    // The account can rename or switch while the same client stays open. This
    // refresh also runs in the lobby; a match roster is not an account source.
    if(identityPending||!connection.valid()||ticks-lastIdentityRequestTick<15)return;
    identityPending=true;lastIdentityRequestTick=ticks;
    const auto generation=credentialGeneration,serial=++identityRequestSerial,matchGeneration=match.generation();
    get(QUrl(base+QStringLiteral("/lol-summoner/v1/current-summoner")),true,[this,generation,serial,matchGeneration](QJsonValue v,QString e,int status){
        completeSelfIdentity(generation,serial,matchGeneration,v,e,status);
    });
}
void LiveClient::clearLocalPlayerDisplayName(){
    // Revoke the in-flight callback too, so a reply from before logout or a
    // client change cannot restore the old nickname after the UI was cleared.
    ++identityRequestSerial;identityPending=false;lastIdentityRequestTick=-15;
    selfPuuid.clear();
    updateLocalAccountScope({});
    clearLocalPlayerBackdrop();
    if(localPlayerName.isEmpty())return;localPlayerName.clear();emit localPlayerDisplayNameChanged({});
}
void LiveClient::updateLocalAccountScope(const QString &scope){
    if(localAccountScopeKey==scope)return;
    const bool revoke=!localAccountScopeKey.isEmpty();localAccountScopeKey=scope;
    if(revoke){
        resetRefreshScope(QStringLiteral("账号已变化，刷新已取消"));
        ++phaseRequestSerial;++liveRequestSerial;lcuPending=livePending=false;
        clearObservedPatch();clearLocalPlayerBackdrop();resetMatch();
        phase.clear();queue=0;observedMode.clear();current={};
        // Revoke old GET continuations before abort() can synchronously finish
        // them. Identity discovery remains enabled on the normal polling timer.
        const auto replies=network.findChildren<QNetworkReply*>(QString(),Qt::FindDirectChildrenOnly);
        for(auto reply:replies)if(!reply->isFinished())reply->abort();
    }
    emit localAccountScopeChanged(scope);
    if(revoke)emit snapshotChanged(current);
}
void LiveClient::clearLocalPlayerBackdrop(){
    ++backdropRequestSerial;backdropPending=false;lastBackdropRequestTick=-60;
    localBackdropSummonerId.clear();localBackdropAsset.clear();
    if(localBackdrop.isEmpty())return;localBackdrop.clear();emit localPlayerBackdropChanged({});
}
void LiveClient::refreshLocalPlayerBackdrop(const QJsonObject &profile){
    const auto id=profile.value(QStringLiteral("summonerId"));const auto numeric=id.toInteger();
    if(!id.isDouble()||numeric<=0||id.toDouble()!=double(numeric)){clearLocalPlayerBackdrop();return;}
    const auto summoner=QString::number(numeric);
    if(summoner!=localBackdropSummonerId){clearLocalPlayerBackdrop();localBackdropSummonerId=summoner;}
    if(backdropPending||ticks-lastBackdropRequestTick<60||!connection.valid())return;
    backdropPending=true;lastBackdropRequestTick=ticks;
    const auto generation=credentialGeneration,serial=++backdropRequestSerial;
    const auto base=QStringLiteral("https://127.0.0.1:%1").arg(connection.port);
    const auto current=[this,generation,serial,summoner]{return enabled&&generation==credentialGeneration&&serial==backdropRequestSerial&&summoner==localBackdropSummonerId;};
    const auto missing=[this]{localBackdropAsset.clear();if(!localBackdrop.isEmpty()){localBackdrop.clear();emit localPlayerBackdropChanged({});}};
    get(QUrl(base+QStringLiteral("/lol-collections/v1/inventories/")+summoner+QStringLiteral("/backdrop")),true,
        [this,base,current,missing](QJsonValue value,QString error,int status){
        if(!current())return;
        const auto path=value.toObject().value(QStringLiteral("backdropImage")).toString();
        static const QRegularExpression imagePath(QStringLiteral("^/lol-game-data/assets/[A-Za-z0-9_./-]+\\.(?:jpg|jpeg|png)$"),QRegularExpression::CaseInsensitiveOption);
        if(!error.isEmpty()||status!=200||!value.isObject()||path.contains(QStringLiteral(".."))||!imagePath.match(path).hasMatch()){
            backdropPending=false;missing();return;
        }
        if(path==localBackdropAsset&&!localBackdrop.isEmpty()){backdropPending=false;return;}
        getBytes(QUrl(base+path),true,[this,path,current,missing](QByteArray bytes,QString assetError,int assetStatus){
            if(!current())return;backdropPending=false;
            if(!assetError.isEmpty()||assetStatus!=200||bytes.isEmpty()){missing();return;}
            localBackdropAsset=path;localBackdrop=std::move(bytes);emit localPlayerBackdropChanged(localBackdrop);
        },8*1024*1024);
    });
}
void LiveClient::completeSelfIdentity(quint64 generation,quint64 serial,quint64 matchGeneration,
                                      const QJsonValue &value,const QString &error,int status){
    if(generation!=credentialGeneration||serial!=identityRequestSerial||!enabled)return;identityPending=false;
    if(!error.isEmpty()||status!=200||!value.isObject()){clearLocalPlayerDisplayName();return;}
    const auto profile=value.toObject();
    static const QRegularExpression canonicalPuuid(QStringLiteral("^[A-Za-z0-9_-]{8,128}$"));
    const auto observedPuuid=localIdentityText(profile.value(QStringLiteral("puuid")));
    selfPuuid=canonicalPuuid.match(observedPuuid).hasMatch()?observedPuuid:QString();
    // The local history scope uses the same exact bytes as the Python bridge.
    // A display nickname (including a legacy response without PUUID) cannot
    // authorize account-bound work. Publish scope before its display label.
    const auto scope=selfPuuid.isEmpty()?QString():QString::fromLatin1(QCryptographicHash::hash(
        QByteArrayLiteral("lixin.local-account-scope.v1")+'\0'+selfPuuid.toUtf8(),QCryptographicHash::Sha256).toHex());
    updateLocalAccountScope(scope);
    const auto nickname=currentSummonerNickname(profile);
    if(localPlayerName!=nickname){localPlayerName=nickname;emit localPlayerDisplayNameChanged(localPlayerName);}
    refreshLocalPlayerBackdrop(profile);
    if(matchGeneration==match.generation()&&match.active()&&!lastFlow.isEmpty()&&phase!=QStringLiteral("ChampSelect")){match.observeGameflow(lastFlow,selfPuuid,matchGeneration);collectGameflowIdentities(lastFlow);publishSnapshot();}
}
void LiveClient::get(const QUrl &url, bool authenticated, std::function<void(QJsonValue,QString,int)> cb) {
    getBytes(url,authenticated,[cb](QByteArray bytes,QString error,int status){
        QJsonValue value;
        if(error.isEmpty()&&bytes.size()<3*1024*1024){
            const auto doc=QJsonDocument::fromJson(bytes);
            if(doc.isObject())value=doc.object();
            else if(doc.isArray())value=doc.array();
            else{const auto array=QJsonDocument::fromJson(QByteArrayLiteral("[")+bytes+QByteArrayLiteral("]")).array();if(!array.isEmpty())value=array.at(0);}
        }
        cb(value,error,status);
    });
}
void LiveClient::getBytes(const QUrl &url,bool authenticated,std::function<void(QByteArray,QString,int)> cb,qint64 maximumBytes){
    ++pendingReadRequests;const auto readScope=refreshScopeGeneration;
    auto completed=[this,readScope,cb=std::move(cb)](QByteArray bytes,QString error,int status){
        if(readScope!=refreshScopeGeneration)return;
        --pendingReadRequests;
        const QPointer<LiveClient> guard(this);
        cb(std::move(bytes),std::move(error),status);
        if(!guard||readScope!=refreshScopeGeneration)return;
        finishRefreshSample();
        if(pendingReadRequests==0&&manualRefreshQueued)manualRefreshTimer.start(0);
    };
    // Credentials can only be sent to the discovered local LCU endpoint.
    if ((authenticated && (!allowedLcuRequest(url, connection) || !lcuPortOwnedBy(connection.port, connection.processId)))
        || (!authenticated && (url.scheme() != QStringLiteral("https") || url.host() != QStringLiteral("127.0.0.1") || url.port() != 2999))) {
        completed({}, QStringLiteral("拒绝未经核实的本机请求"), 0); return;
    }
    QNetworkRequest request(url);
    request.setTransferTimeout(1800);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    if (authenticated) request.setRawHeader(QByteArrayLiteral("Authorization"), connection.authorization);
    auto ssl = QSslConfiguration::defaultConfiguration();
    const auto cas = QSslCertificate::fromPath(catalog.root + QStringLiteral("/data/riotgames.pem"));
    if (cas.isEmpty()) { completed({}, QStringLiteral("缺少客户端信任证书"), 0); return; }
    ssl.setCaCertificates(cas);
    ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
    request.setSslConfiguration(ssl);
    // Both CN LCU and the live game API expose rclient certificates with an
    // explicit 127.0.0.1 SAN. Verify the actual loopback endpoint identity.
    request.setPeerVerifyName(QStringLiteral("127.0.0.1"));
    auto reply = network.get(request);
    connect(reply, &QIODevice::readyRead, reply, [reply,maximumBytes] { if (reply->bytesAvailable() > maximumBytes) reply->abort(); });
    const auto generation = credentialGeneration;
    connect(reply, &QNetworkReply::finished, this, [this, reply, completed, authenticated, generation,readScope,maximumBytes] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (authenticated && generation != credentialGeneration) { reply->deleteLater(); completed({}, QStringLiteral("客户端会话已更新"), status); return; }
        const auto error = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
        if(readScope==refreshScopeGeneration&&manualSampleInFlight&&reply->error()==QNetworkReply::TimeoutError)manualSampleTimedOut=true;
        auto bytes = reply->readAll();
        if(bytes.size()>maximumBytes)bytes.clear();
        reply->deleteLater();
        completed(std::move(bytes), error, status);
    });
}
void LiveClient::tick() { sample(false); }
void LiveClient::sample(bool manual) {
    if (!enabled) return;
    if(!manual){++ticks;if(manualRefreshQueued||manualSampleInFlight)return;}
    const auto refreshScope=refreshScopeGeneration;
    if (manual || ticks % 5 == 1) discover();
    if(manual&&refreshScope!=refreshScopeGeneration)return;
    if (connection.valid() && !lcuPending && (manual || ticks % 3 == 1)) {
        lcuPending = true;
        const QString base = QStringLiteral("https://127.0.0.1:%1").arg(connection.port);
        const auto generation=credentialGeneration,serial=++phaseRequestSerial,phaseMatchGeneration=match.generation();
        get(QUrl(base + QStringLiteral("/lol-gameflow/v1/gameflow-phase")), true, [this, base, generation,serial,phaseMatchGeneration](QJsonValue value, QString error, int status) {
            if (generation!=credentialGeneration||serial!=phaseRequestSerial) return;
            lcuPending = false;
            if (!enabled||phaseMatchGeneration!=match.generation()) return;
            if (!error.isEmpty() || status != 200 || !value.isString() || value.toString().isEmpty()) {
                clearLocalPlayerDisplayName();clearObservedPatch();resetMatch();phase.clear(); queue = 0;observedMode.clear(); current = {};
                emit snapshotChanged(current);
                emit connectionChanged(QStringLiteral("暂未连接客户端，稍后自动重试")); return;
            }
            phase = value.toString();
            if(manualSampleInFlight)manualSampleSucceeded=true;
            observeRefreshPhase(phase);
            const auto before=match.contextId();match.observePhase(phase);
            if(before!=match.contextId()){lastFlow={};privatePlayers={};privateRosterKey.clear();current={};queue=0;observedMode.clear();emit privateRosterChanged({});}
            refreshObservedPatch(base);
            refreshSelfIdentity(base);
            if(!enabled||generation!=credentialGeneration||serial!=phaseRequestSerial)return;
            emit connectionChanged(QStringLiteral("客户端已连接"));
            if (!MatchContextTracker::isMatchPhase(phase)) {
                current = {}; current.phase = phase; current.observedPatch=observedPatch;queue = 0;observedMode.clear();publishSnapshot();return;
            }
            if(phase!=QStringLiteral("InProgress")&&phase!=QStringLiteral("Reconnect")){
                // Keep the last verified selection while its next read is in
                // flight. Its separate timestamp still expires after 8s.
                if(phase!=QStringLiteral("ChampSelect")||current.phase!=phase||current.source!=GameState::Source::Offline)current={};
                current.phase=phase;publishSnapshot();
            }
            const auto matchGeneration=match.generation();
            get(QUrl(base + QStringLiteral("/lol-gameflow/v1/session")), true, [this,generation,serial,matchGeneration](QJsonValue v, QString e, int status) {
                if (!enabled || generation!=credentialGeneration||serial!=phaseRequestSerial||matchGeneration!=match.generation()) return;
                if (!e.isEmpty()||status!=200||!v.isObject()) return;
                const auto session=v.toObject();const auto queueData=session.value(QStringLiteral("gameData")).toObject().value(QStringLiteral("queue")).toObject();queue=queueData.value(QStringLiteral("id")).toInt();
                const auto mode=queueData.value(QStringLiteral("gameMode")).toString();static const QRegularExpression validMode(QStringLiteral("^[A-Z_]{2,24}$"));observedMode=validMode.match(mode).hasMatch()?mode:QString();
                const auto before=match.contextId();if(!match.observeGameflow(session,selfPuuid,matchGeneration))return;
                if(before!=match.contextId()){current={};privatePlayers={};privateRosterKey.clear();emit privateRosterChanged({});}
                lastFlow=session;if(phase!=QStringLiteral("ChampSelect"))collectGameflowIdentities(session);publishSnapshot();
            });
            if (phase == QStringLiteral("ChampSelect")) get(QUrl(base + QStringLiteral("/lol-champ-select/v1/session")), true, [this,generation,serial,matchGeneration](QJsonValue v, QString e, int status) {
                if (!enabled || generation!=credentialGeneration||serial!=phaseRequestSerial||matchGeneration!=match.generation()||phase!=QStringLiteral("ChampSelect")) return;
                if(!e.isEmpty()||status!=200||!v.isObject()){
                    current={};current.phase=phase;publishSnapshot();return;
                }
                GameState s; s.phase = phase; s.queueId = queue;s.observedPatch=observedPatch;
                const auto o = v.toObject();
                if(!match.observeChampionSelect(o,matchGeneration))return;collectChampionSelectIdentities(o);
                const auto selfCell=o.value(QStringLiteral("localPlayerCellId"));
                const int self=selfCell.toInt(-1);int selfMatches=0,candidate=0;
                if(selfCell.isDouble()&&selfCell.toDouble()==self&&self>=0)
                    for(const auto &x:o.value(QStringLiteral("myTeam")).toArray()){
                        const auto row=x.toObject();const auto cell=row.value(QStringLiteral("cellId"));
                        if(cell.isDouble()&&cell.toDouble()==self){++selfMatches;candidate=observedChampionId(row);}
                    }
                s.championId=selfMatches==1&&catalog.champions.contains(candidate)?candidate:0;
                s.benchKnown=o.value(QStringLiteral("benchChampions")).isArray();
                for (const auto &x : o.value(QStringLiteral("benchChampions")).toArray()) {
                    const auto id=x.toObject().value(QStringLiteral("championId"));
                    if(id.isDouble()&&id.toDouble()==id.toInt()&&catalog.champions.contains(id.toInt())&&!s.bench.contains(id.toInt()))s.bench.append(id.toInt());
                }
                s.championSelectReceived=QDateTime::currentDateTimeUtc();
                current = s;publishSnapshot();
            });
        });
    }
    if (!livePending && (!connection.valid() || phase == QStringLiteral("InProgress") || phase == QStringLiteral("Reconnect")||MatchContextTracker::isLoadingPhase(phase))) {
        livePending = true;
        const auto generation=credentialGeneration,matchGeneration=match.generation(),serial=++liveRequestSerial;
        get(QUrl(QStringLiteral("https://127.0.0.1:2999/liveclientdata/allgamedata")), false, [this,generation,matchGeneration,serial](QJsonValue v, QString error, int) {
            if (generation!=credentialGeneration||serial!=liveRequestSerial) return;
            livePending = false;
            if (!enabled||matchGeneration!=match.generation()) return;
            auto s = parseLiveData(v.toObject(), catalog);
            if (error.isEmpty() && s.source == GameState::Source::Live) {
                const auto before=match.contextId();if(!match.observeLive(s.players,s.gameTime,matchGeneration))return;
                if(before!=match.contextId()){lastFlow={};privatePlayers={};privateRosterKey.clear();emit privateRosterChanged({});}
                collectLiveIdentities(v.toObject(),s);
                s.queueId = queue;s.observedPatch=observedPatch;current = s;
                if(manualSampleInFlight)manualSampleSucceeded=true;
                emit connectionChanged(QStringLiteral("对局已连接 · %1:%2").arg(int(s.gameTime)/60).arg(int(s.gameTime)%60,2,10,QChar(u'0')));
                publishSnapshot();
            } else if (!current.fresh()) {
                current = {}; current.phase = phase.isEmpty() ? QStringLiteral("未连接") : phase;
                publishSnapshot();
                if (!connection.valid()) emit connectionChanged(QStringLiteral("等待游戏 · 自动连接"));
            }
        });
    }
}
void LiveClient::probeLobby(std::function<void(QJsonObject)> callback) {
    discover();
    QJsonObject report{{QStringLiteral("checked_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("authenticated_lobby_verified"), false}, {QStringLiteral("game_api_requests"), 0},
        {QStringLiteral("source"), connection.source}, {QStringLiteral("credential_persisted"), false},
        {QStringLiteral("process_id"), qint64(connection.processId)}, {QStringLiteral("ux_process_id"), qint64(connection.uxProcessId)},
        {QStringLiteral("port"), connection.port}, {QStringLiteral("enable_lockfile_flag"), connection.lockfileEnabled}};
    const QFileInfo lock(clientFolder + QStringLiteral("/lockfile"));
    report.insert(QStringLiteral("lockfile"), QJsonObject{{QStringLiteral("exists"), lock.exists()}, {QStringLiteral("bytes"), lock.size()},
        {QStringLiteral("modified_at"), lock.lastModified().toUTC().toString(Qt::ISODateWithMs)}});
    if (!connection.valid()) { report.insert(QStringLiteral("error"), connection.diagnostic); callback(report); return; }
    const auto base = QStringLiteral("https://127.0.0.1:%1").arg(connection.port);
    const auto generation = credentialGeneration;
    get(QUrl(base + QStringLiteral("/lol-gameflow/v1/gameflow-phase")), true,
        [this, base, generation, report, callback](QJsonValue value, QString error, int status) mutable {
        QJsonArray requests{QJsonObject{{QStringLiteral("endpoint"), QStringLiteral("/lol-gameflow/v1/gameflow-phase")}, {QStringLiteral("http_status"), status}}};
        report.insert(QStringLiteral("requests"), requests);
        if (!error.isEmpty() || status != 200 || !value.isString() || value.toString().isEmpty()) {
            report.insert(QStringLiteral("error"), error.isEmpty() ? QStringLiteral("无效阶段响应") : error); callback(report); return;
        }
        const auto observedPhase = value.toString();
        report.insert(QStringLiteral("phase"), observedPhase);
        if (observedPhase == QStringLiteral("InProgress") || observedPhase == QStringLiteral("Reconnect")) {
            report.insert(QStringLiteral("error"), QStringLiteral("当前处于对局阶段，大厅限定探测已停止")); callback(report); return;
        }
        struct Result { QJsonObject report; int pending = 4; bool version = false, region = false; };
        auto result = std::make_shared<Result>(); result->report = report;
        const QStringList paths{QStringLiteral("/lol-patch/v1/game-version"), QStringLiteral("/riotclient/region-locale"),
            QStringLiteral("/lol-game-queues/v1/queues"), QStringLiteral("/lol-gameflow/v1/session")};
        for (const auto &path : paths) get(QUrl(base + path), true,
            [this, result, generation, callback, path, observedPhase](QJsonValue data, QString requestError, int code) {
            QJsonObject row{{QStringLiteral("endpoint"), path}, {QStringLiteral("http_status"), code}};
            if (!requestError.isEmpty()) row.insert(QStringLiteral("error"), requestError);
            if (code == 404 && observedPhase == QStringLiteral("None") && path.endsWith(QStringLiteral("session")))
                row.insert(QStringLiteral("expected_without_active_session"), true);
            if (code == 200 && requestError.isEmpty()) {
                if (path.endsWith(QStringLiteral("game-version")) && data.isString()) {
                    row.insert(QStringLiteral("version"), data); result->version = true;
                } else if (path.endsWith(QStringLiteral("region-locale")) && data.isObject()) {
                    const auto object = data.toObject();
                    row.insert(QStringLiteral("region"), object.value(QStringLiteral("region")));
                    row.insert(QStringLiteral("locale"), object.value(QStringLiteral("locale")));
                    result->region = object.value(QStringLiteral("region")).isString() && object.value(QStringLiteral("locale")).isString();
                } else if (path.endsWith(QStringLiteral("queues")) && data.isArray()) {
                    QJsonArray modes;
                    for (const auto &entry : data.toArray()) {
                        const auto queueData = entry.toObject(); const auto mode = queueData.value(QStringLiteral("gameMode")).toString();
                        if (mode != QStringLiteral("ARAM") && mode != QStringLiteral("KIWI") && mode != QStringLiteral("KIWI_JADE")) continue;
                        QJsonObject summary;
                        for (const auto &key : {QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("gameMode"), QStringLiteral("mapId"), QStringLiteral("queueAvailability")})
                            summary.insert(key, queueData.value(key));
                        modes.append(summary);
                    }
                    row.insert(QStringLiteral("modes"), modes);
                } else if (path.endsWith(QStringLiteral("session")) && data.isObject()) {
                    const auto game = data.toObject().value(QStringLiteral("gameData")).toObject();
                    row.insert(QStringLiteral("queue_id"), game.value(QStringLiteral("queue")).toObject().value(QStringLiteral("id")));
                }
            }
            auto rows = result->report.value(QStringLiteral("requests")).toArray(); rows.append(row); result->report.insert(QStringLiteral("requests"), rows);
            if (--result->pending != 0) return;
            result->report.insert(QStringLiteral("authenticated_lobby_verified"), generation == credentialGeneration && result->version && result->region);
            result->report.insert(QStringLiteral("strict_riot_ca_and_hostname_verification"), true);
            callback(result->report);
        });
    });
}
void LiveClient::loadReplay(const QString &path) {
    auto doc = loadJson(path);
    auto s = parseLiveData(doc.object(), catalog);
    if (s.source != GameState::Source::Live) { emit connectionChanged(QStringLiteral("样本格式不支持")); return; }
    enabled = false;resetRefreshScope(QStringLiteral("刷新已取消"));++phaseRequestSerial;++identityRequestSerial;++liveRequestSerial;identityPending=false;lcuPending=false;livePending=false;clearLocalPlayerDisplayName();clearObservedPatch();resetMatch();s.source = GameState::Source::Replay; s.message = QStringLiteral("文档样本回放，并非实时对局");
    current = s;
    emit connectionChanged(s.message); emit snapshotChanged(current);
}
