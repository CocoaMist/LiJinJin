#include "sgp_history.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QUrl>
#include <QTimeZone>
#include <algorithm>
#include <cmath>

namespace {
QJsonValue number(const QJsonObject &o,const QString &key,double maximum=1e12) {
    const auto v=o.value(key);
    return v.isDouble()&&std::isfinite(v.toDouble())&&v.toDouble()>=0&&v.toDouble()<=maximum?v:QJsonValue(QJsonValue::Null);
}
int positiveId(const QJsonValue &v) {
    return v.isDouble()&&v.toDouble()>0&&v.toDouble()<=10000000&&v.toDouble()==v.toInt()?v.toInt():0;
}
QJsonObject unwrap(const QJsonValue &v) {
    auto o=v.toObject();
    if(o.value("json").isObject())return o.value("json").toObject();
    if(o.value("json").isString())return QJsonDocument::fromJson(o.value("json").toString().toUtf8()).object();
    return o;
}
QJsonArray counts(const QMap<int,QPair<int,int>> &values) {
    QList<int> ids=values.keys();
    std::sort(ids.begin(),ids.end(),[&](int a,int b){return values[a].first==values[b].first?a<b:values[a].first>values[b].first;});
    QJsonArray out;
    for(int id:ids)out.append(QJsonObject{{"id",id},{"appearances",values[id].first},{"wins",values[id].second}});
    return out;
}
QString date(qint64 ms) { return ms>0?QDateTime::fromMSecsSinceEpoch(ms,QTimeZone::UTC).toString(Qt::ISODate):QString(); }
}

QJsonValue sgpJsonValue(const QByteArray &bytes) {
    const auto doc=QJsonDocument::fromJson(bytes);
    if(doc.isObject())return doc.object();
    if(doc.isArray())return doc.array();
    const auto a=QJsonDocument::fromJson(QByteArrayLiteral("[")+bytes+QByteArrayLiteral("]")).array();
    return a.size()==1?a.first():QJsonValue{};
}
QJsonArray exactRiotAliases(const QJsonValue &value,const QString &name,const QString &tag) {
    QJsonArray candidates=value.isArray()?value.toArray():QJsonArray{};
    if(value.isObject())candidates.append(value);
    QJsonArray out;
    for(const auto &v:candidates) {
        const auto p=v.toObject(),alias=p.value("alias").toObject();
        const auto n=alias.value("game_name").toString(p.value("gameName").toString());
        const auto t=alias.value("tag_line").toString(p.value("tagLine").toString());
        const auto id=p.value("puuid").toString();
        const bool exact=n.compare(name,Qt::CaseInsensitive)==0&&t.compare(tag,Qt::CaseInsensitive)==0;
        if((exact||p.value("displayName").toString()==name+QChar('#')+tag)&&!id.isEmpty()&&id.size()<=256&&!out.contains(id))out.append(id);
    }
    return out;
}
QMap<QString,QString> sgpServers() {
    return {{"HN1","hn1-k8s-sgp.lol.qq.com"},{"HN10","hn10-k8s-sgp.lol.qq.com"},
        {"TJ100","tj100-sgp.lol.qq.com"},{"TJ101","tj101-sgp.lol.qq.com"},
        {"NJ100","nj100-sgp.lol.qq.com"},{"GZ100","gz100-sgp.lol.qq.com"},
        {"CQ100","cq100-sgp.lol.qq.com"},{"BGP2","bgp2-k8s-sgp.lol.qq.com"}};
}
bool allowedSgpUrl(const QUrl &url,const QString &server) {
    return sgpServers().contains(server)&&url.isValid()&&url.scheme()==QStringLiteral("https")
        &&url.host()==sgpServers().value(server)&&url.port()==21019&&url.userInfo().isEmpty()&&!url.hasFragment();
}

SgpHistory::Page SgpHistory::append(const QJsonArray &batch,const QString &target) {
    Page result;result.received=batch.size();
    for(const auto &v:batch) {
        const auto game=unwrap(v);
        const auto id=game.value("gameId");const auto platform=game.value("platformId").toString();
        if(!id.isDouble()||!std::isfinite(id.toDouble())||id.toInteger()<=0||double(id.toInteger())!=id.toDouble()
            ||platform.isEmpty()||platform.size()>32){++invalidGames;++result.invalid;continue;}
        const auto key=platform+QChar(':')+QString::number(id.toInteger());
        if(seen.contains(key))continue;
        seen.insert(key);++result.added;
        if(game.value("gameMode")!=QStringLiteral("KIWI")||game.value("queueId").toInt()!=2400){++otherGames;continue;}
        const auto players=game.value("participants").toArray();
        QJsonArray rows;QSet<int> participants;QMap<int,int> teams;int selfCount=0;
        bool valid=players.size()==10&&!target.isEmpty();
        for(const auto &pv:players) {
            const auto p=pv.toObject();auto stats=p.value("stats").isObject()?p.value("stats").toObject():p;
            const int cid=positiveId(p.value("championId")),pid=positiveId(p.value("participantId")),team=p.value("teamId").toInt();
            if(!cid||!pid||participants.contains(pid)||(team!=100&&team!=200)){valid=false;break;}
            participants.insert(pid);++teams[team];
            const bool self=p.value("puuid").toString()==target;if(self)++selfCount;
            QJsonArray items,augments;QSet<int> found;
            for(int i=0;i<7;++i){const int item=positiveId(stats.value(QStringLiteral("item%1").arg(i)));if(item)items.append(item);}
            for(int i=1;i<=6;++i){const int aug=positiveId(stats.value(QStringLiteral("playerAugment%1").arg(i)));if(aug&&!found.contains(aug)){found.insert(aug);augments.append(aug);}}
            QJsonObject row{{"participant",pid},{"champion_id",cid},{"team_id",team},{"self",self},
                {"item_ids",items},{"augment_ids",augments},{"unspent_gold",QJsonValue::Null}};
            const QMap<QString,QString> fields{{"kills","kills"},{"deaths","deaths"},{"assists","assists"},
                {"gold_earned","goldEarned"},{"gold_spent","goldSpent"},{"level","champLevel"},{"cs","totalMinionsKilled"}};
            for(auto it=fields.cbegin();it!=fields.cend();++it)row[it.key()]=number(stats,it.value());
            row["win"]=stats.value("win").isBool()?stats.value("win"):QJsonValue(QJsonValue::Null);
            rows.append(row);
        }
        if(!valid||selfCount!=1||teams.value(100)!=5||teams.value(200)!=5){++invalidKiwiGames;continue;}
        const auto rawVersion=game.value("gameVersion").toString();
        const auto parts=rawVersion.split(QChar('.'));
        bool majorOk=false,minorOk=false;const int major=parts.value(0).toInt(&majorOk),minor=parts.value(1).toInt(&minorOk);
        const auto version=majorOk&&minorOk&&major>=1&&major<=100&&minor>=0&&minor<=100?QStringLiteral("%1.%2").arg(major).arg(minor):QStringLiteral("unknown");
        ++versions[version];
        const auto started=number(game,"gameCreation",4102444800000.);const qint64 ms=started.toInteger();
        if(ms>0){first=first>0?std::min(first,ms):ms;last=std::max(last,ms);}
        normalized.append(QJsonObject{{"match_ref",QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(),QCryptographicHash::Sha256).toHex())},
            {"mode","KIWI"},{"queue",2400},{"patch",version},{"started_at_ms",started},
            {"duration_seconds",number(game,"gameDuration",86400)},{"players",rows},
            {"stage","end_of_game"},{"timeline_available",false},{"real_time_prior_eligible",false}});
    }
    return result;
}
QJsonObject SgpHistory::summary(const QString &server,bool exhausted,const QString &reason) const {
    QSet<int> heroes,augments,items;QMap<int,QPair<int,int>> selfHeroes,allAugments;
    int selfGames=0,wins=0,knownWins=0,goldConflicts=0,rows=0;
    double kills=0,deaths=0,assists=0;int knownKda=0;
    QMap<QString,QPair<double,int>> averages;
    for(const auto &gv:normalized)for(const auto &pv:gv.toObject().value("players").toArray()) {
        const auto p=pv.toObject();++rows;const int cid=p.value("champion_id").toInt();heroes.insert(cid);
        const bool won=p.value("win").isBool()&&p.value("win").toBool();
        for(const auto &v:p.value("item_ids").toArray())items.insert(v.toInt());
        for(const auto &v:p.value("augment_ids").toArray()){augments.insert(v.toInt());auto &a=allAugments[v.toInt()];++a.first;if(won)++a.second;}
        if(p.value("gold_earned").isDouble()&&p.value("gold_spent").isDouble()&&p.value("gold_spent").toDouble()>p.value("gold_earned").toDouble())++goldConflicts;
        if(!p.value("self").toBool())continue;
        ++selfGames;auto &a=selfHeroes[cid];++a.first;if(won)++a.second;
        if(p.value("win").isBool()){++knownWins;if(won)++wins;}
        for(const auto &key:{QStringLiteral("gold_earned"),QStringLiteral("gold_spent"),QStringLiteral("cs"),QStringLiteral("level")})
            if(p.value(key).isDouble()){auto &a=averages[key];a.first+=p.value(key).toDouble();++a.second;}
        if(p.value("kills").isDouble()&&p.value("deaths").isDouble()&&p.value("assists").isDouble()){
            kills+=p.value("kills").toDouble();deaths+=p.value("deaths").toDouble();assists+=p.value("assists").toDouble();++knownKda;}
    }
    QJsonObject patchCounts;for(auto it=versions.cbegin();it!=versions.cend();++it)patchCounts[it.key()]=it.value();
    QJsonObject player{{"games",selfGames},{"known_outcomes",knownWins},{"wins",wins},
        {"historical_win_rate",knownWins?QJsonValue(double(wins)/knownWins):QJsonValue(QJsonValue::Null)},
        {"kda_games",knownKda},{"average_kills",knownKda?QJsonValue(kills/knownKda):QJsonValue(QJsonValue::Null)},
        {"average_deaths",knownKda?QJsonValue(deaths/knownKda):QJsonValue(QJsonValue::Null)},
        {"average_assists",knownKda?QJsonValue(assists/knownKda):QJsonValue(QJsonValue::Null)}};
    for(auto it=averages.cbegin();it!=averages.cend();++it){player[QStringLiteral("average_")+it.key()]=it.value().first/it.value().second;player[it.key()+QStringLiteral("_games")]=it.value().second;}
    return {{"schema_version",1},{"source","authenticated_sgp_summary"},{"queried_at",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"server",server},{"accessible_games",size()},{"kiwi_games",normalized.size()},{"other_games",otherGames},
        {"invalid_games",invalidGames},{"invalid_kiwi_games",invalidKiwiGames},{"participant_rows",rows},
        {"unique_champions",heroes.size()},{"unique_augments",augments.size()},{"unique_items",items.size()},
        {"patches",patchCounts},{"first_game_utc",date(first)},{"last_game_utc",date(last)},
        {"accessible_history_exhausted",exhausted},{"all_time_history_complete",false},{"stop_reason",reason},
        {"player",player},{"self_champions",counts(selfHeroes)},{"augment_occurrences",counts(allAugments)},
        {"gold_spent_exceeds_earned",goldConflicts},{"identities_exported",false},
        {"timeline_available",false},{"real_time_prior_eligible",false},{"calibrated_match_win_rate",false}};
}
