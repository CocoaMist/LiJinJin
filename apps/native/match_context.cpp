#include "match_context.h"
#include <QJsonDocument>
#include <QDateTime>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace {
QString q(const char *s){return QString::fromUtf8(s);}
int integer(const QJsonValue &v,int fallback=-1){return v.isDouble()&&std::isfinite(v.toDouble())&&v.toDouble()==v.toInt()?v.toInt():fallback;}
QString sessionGameId(const QJsonValue &v){
    if(v.isString()){static const QRegularExpression digits(q("^[1-9][0-9]{0,18}$"));if(digits.match(v.toString()).hasMatch())return v.toString();}
    if(v.isDouble()&&std::isfinite(v.toDouble())&&v.toDouble()>0&&v.toDouble()<=9007199254740991.&&std::floor(v.toDouble())==v.toDouble())return QString::number(v.toDouble(),'f',0);
    return {};
}
QJsonObject row(const QJsonObject &raw,const QString &team,bool self,bool live=false,const QString &contextId={}){
    const int champion=integer(raw.value(live?q("champion_id"):q("championId")),0);
    const int cell=integer(raw.value(live?q("cell"):q("cellId")));
    QString position=raw.value(live?q("role"):q("assignedPosition")).toString().toUpper();
    if(!QStringList{q("TOP"),q("JUNGLE"),q("MIDDLE"),q("BOTTOM"),q("UTILITY")}.contains(position))position.clear();
    QJsonObject result{{q("champion_id"),champion>0?QJsonValue(champion):QJsonValue()},
        {q("team"),team},{q("self"),self},{q("cell"),cell>=0&&cell<100?QJsonValue(cell):QJsonValue()},
        {q("role"),position.isEmpty()?QJsonValue():QJsonValue(position)}};
    const auto token=rosterSlotToken(contextId,raw.value(q("puuid")).toString());
    if(!token.isEmpty())result[q("slot_token")]=token;
    return result;
}
QJsonObject visibility(const QJsonArray &roster,const QString &team,bool observed,bool ownKnown){
    if(!ownKnown)return {{q("status"),q("own_team_unknown")},{q("visible"),0},{q("total"),QJsonValue()}};
    int total=0,visible=0;for(const auto &v:roster){const auto p=v.toObject();if(p.value(q("team"))!=team)continue;++total;visible+=p.value(q("champion_id")).toInt()>0;}
    const auto status=!observed?q("not_observed"):total==0?q("empty"):visible==0?q("hidden_or_unselected"):visible<total?q("partial"):q("visible");
    return {{q("status"),status},{q("visible"),visible},{q("total"),observed?QJsonValue(total):QJsonValue()}};
}
}
QString rosterSlotToken(const QString &contextId,const QString &puuid){
    if(contextId.isEmpty()||contextId.size()>128||puuid.isEmpty()||puuid.size()>256)return {};
    for(const auto c:puuid)if(c.category()==QChar::Other_Control)return {};
    const auto bytes=QJsonDocument(QJsonArray{contextId,puuid}).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex().left(32));
}
QString parseObservedGamePatch(const QJsonValue &version){
    if(!version.isString())return {};const auto value=version.toString();
    if(value.isEmpty()||value.size()>192)return {};
    static const QRegularExpression format(q("^([0-9]{1,3})\\.([0-9]{1,3})(?:\\.[0-9]{1,12}){0,3}(?:[+-][A-Za-z0-9][A-Za-z0-9._+-]{0,127})?$"));
    const auto match=format.match(value);if(!match.hasMatch()||match.capturedLength()!=value.size())return {};
    return QString::number(match.captured(1).toInt())+q(".")+QString::number(match.captured(2).toInt());
}
bool MatchContextTracker::isLoadingPhase(const QString &value){return QStringList{q("GameStart"),q("LoadingScreen"),q("Loading"),q("PreStart"),q("Start")}.contains(value);}
bool MatchContextTracker::isMatchPhase(const QString &value){return value==q("ChampSelect")||value==q("InProgress")||value==q("Reconnect")||isLoadingPhase(value);}
void MatchContextTracker::clear(){
    if(!id.isEmpty())++epoch;id.clear();gameId.clear();phase.clear();source.clear();roster={};revision=0;
    alliesObserved=enemiesObserved=ownTeamKnown=loadingRosterMissing=false;lastLiveTime=-1;startedAtMs=0;
}
void MatchContextTracker::begin(const QString &value){
    ++epoch;id=QUuid::createUuid().toString(QUuid::WithoutBraces);gameId.clear();phase=value;source.clear();roster={};revision=1;
    alliesObserved=enemiesObserved=ownTeamKnown=loadingRosterMissing=false;lastLiveTime=-1;startedAtMs=QDateTime::currentMSecsSinceEpoch();
}
void MatchContextTracker::observePhase(const QString &value){
    if(!isMatchPhase(value)){clear();return;}
    if(!active()){begin(value);return;}
    // Going from a played/loading game back to selection starts a new lobby.
    if(value==q("ChampSelect")&&phase!=q("ChampSelect")){begin(value);return;}
    if(phase!=value){phase=value;++revision;}
}
void MatchContextTracker::setRoster(QJsonArray rows,const QString &from,bool allies,bool enemies,bool selfKnown){
    QMap<QString,int> tokens;for(const auto &v:rows){const auto token=v.toObject().value(q("slot_token")).toString();if(!token.isEmpty())++tokens[token];}
    for(int i=0;i<rows.size();++i){auto p=rows[i].toObject();if(tokens.value(p.value(q("slot_token")).toString())>1){p.remove(q("slot_token"));rows[i]=p;}}
    QList<QJsonObject> ordered;for(const auto &v:rows)ordered.append(v.toObject());
    std::sort(ordered.begin(),ordered.end(),[](const QJsonObject &a,const QJsonObject &b){return QJsonDocument(a).toJson(QJsonDocument::Compact)<QJsonDocument(b).toJson(QJsonDocument::Compact);});
    QJsonArray next;for(const auto &v:ordered)next.append(v);
    if(roster!=next||source!=from||alliesObserved!=allies||enemiesObserved!=enemies||ownTeamKnown!=selfKnown)++revision;
    roster=next;source=from;alliesObserved=allies;enemiesObserved=enemies;ownTeamKnown=selfKnown;
}
bool MatchContextTracker::observeChampionSelect(const QJsonObject &session,quint64 expected){
    if(expected!=epoch||!active()||phase!=q("ChampSelect"))return false;
    const int local=integer(session.value(q("localPlayerCellId")));int selfCells=0;
    for(const auto &v:session.value(q("myTeam")).toArray())if(local>=0&&integer(v.toObject().value(q("cellId")))==local)++selfCells;
    QJsonArray next;
    for(const auto &v:session.value(q("myTeam")).toArray()){const auto p=v.toObject();next.append(row(p,q("ALLY"),local>=0&&selfCells==1&&integer(p.value(q("cellId")))==local,false,id));}
    for(const auto &v:session.value(q("theirTeam")).toArray())next.append(row(v.toObject(),q("ENEMY"),false,false,id));
    loadingRosterMissing=false;setRoster(next,q("champ_select"),session.value(q("myTeam")).isArray(),session.value(q("theirTeam")).isArray(),true);return true;
}
bool MatchContextTracker::observeGameflow(const QJsonObject &session,const QString &selfPuuid,quint64 expected){
    if(expected!=epoch||!active())return false;
    const auto game=session.value(q("gameData")).toObject();const auto nextId=sessionGameId(game.value(q("gameId")));
    if(!nextId.isEmpty()){if(!gameId.isEmpty()&&nextId!=gameId)begin(phase);if(gameId!=nextId){gameId=nextId;++revision;}}
    if(phase==q("ChampSelect"))return true;
    const bool first=game.value(q("teamOne")).isArray(),second=game.value(q("teamTwo")).isArray();
    if(!first&&!second){if(!loadingRosterMissing){loadingRosterMissing=true;++revision;}return true;}
    int selfCount=0,ownSide=0;for(int side=1;side<=2;++side)for(const auto &v:game.value(side==1?q("teamOne"):q("teamTwo")).toArray())if(!selfPuuid.isEmpty()&&v.toObject().value(q("puuid")).toString()==selfPuuid){++selfCount;ownSide=side;}
    const bool ownKnown=selfCount==1;QJsonArray next;
    for(int side=1;side<=2;++side)for(const auto &v:game.value(side==1?q("teamOne"):q("teamTwo")).toArray()){
        const auto p=v.toObject();next.append(row(p,ownKnown?(side==ownSide?q("ALLY"):q("ENEMY")):q("UNKNOWN"),ownKnown&&p.value(q("puuid")).toString()==selfPuuid,false,id));
    }
    loadingRosterMissing=false;setRoster(next,q("gameflow_loading"),ownKnown&&(ownSide==1?first:second),ownKnown&&(ownSide==1?second:first),ownKnown);return true;
}
bool MatchContextTracker::observeLive(const QJsonArray &players,double time,quint64 expected){
    if(expected!=epoch||players.isEmpty())return false;
    if(!active())begin(q("InProgress"));
    if(std::isfinite(time)&&time>=0){if(lastLiveTime>=0&&time+10<lastLiveTime)begin(q("InProgress"));lastLiveTime=time;}
    if(phase!=q("InProgress")&&phase!=q("Reconnect")){phase=q("InProgress");++revision;}
    QString own;int selves=0;for(const auto &v:players){const auto p=v.toObject();if(p.value(q("self")).toBool()){++selves;own=p.value(q("team")).toString();}}
    const bool ownKnown=selves==1&&(own==q("ORDER")||own==q("CHAOS"));QJsonArray next;
    for(const auto &v:players){const auto p=v.toObject();const auto side=p.value(q("team")).toString();const bool sideKnown=side==q("ORDER")||side==q("CHAOS");next.append(row(p,ownKnown&&sideKnown?(side==own?q("ALLY"):q("ENEMY")):q("UNKNOWN"),ownKnown&&p.value(q("self")).toBool(),true));}
    // Live telemetry has no stable IDs. Carry a loading proof only through an
    // unchanged unique slot in this context; selection swaps cannot reuse it.
    if(source==q("gameflow_loading")||source==q("live_client")){
        const auto slot=[](const QJsonObject &p){const auto team=p.value(q("team")).toString();const int champion=p.value(q("champion_id")).toInt();return (team==q("ALLY")||team==q("ENEMY"))&&champion>0?team+q(":")+QString::number(champion):QString();};
        QMap<QString,int> oldCounts,newCounts;QMap<QString,QJsonObject> oldRows;
        for(const auto &v:roster){const auto p=v.toObject();const auto key=slot(p);if(!key.isEmpty()){++oldCounts[key];oldRows[key]=p;}}
        for(const auto &v:next){const auto key=slot(v.toObject());if(!key.isEmpty())++newCounts[key];}
        for(int i=0;i<next.size();++i){auto p=next[i].toObject();const auto key=slot(p);const auto previous=oldRows.value(key);const int oldCell=integer(previous.value(q("cell"))),newCell=integer(p.value(q("cell")));const bool cellConflict=oldCell>=0&&newCell>=0&&oldCell!=newCell;if(oldCounts.value(key)==1&&newCounts.value(key)==1&&!cellConflict&&previous.value(q("self"))==p.value(q("self"))&&!previous.value(q("slot_token")).toString().isEmpty()){p[q("slot_token")]=previous.value(q("slot_token"));next[i]=p;}}
    }
    loadingRosterMissing=false;setRoster(next,q("live_client"),ownKnown,ownKnown,ownKnown);return true;
}
QJsonObject MatchContextTracker::json()const{
    if(!active())return {};
    const auto allies=visibility(roster,q("ALLY"),alliesObserved,ownTeamKnown),enemies=visibility(roster,q("ENEMY"),enemiesObserved,ownTeamKnown);QJsonArray unknowns;
    if(!ownTeamKnown)unknowns.append(q("own_team_unverified"));
    if(allies.value(q("status"))!=q("visible"))unknowns.append(q("allies_")+allies.value(q("status")).toString());
    if(enemies.value(q("status"))!=q("visible"))unknowns.append(q("enemies_")+enemies.value(q("status")).toString());
    if(loadingRosterMissing)unknowns.append(q("loading_roster_not_observed"));
    return {{q("schema_version"),1},{q("context_id"),id},{q("generation"),qint64(epoch)},{q("revision"),qint64(revision)},{q("active"),true},
        {q("phase"),phase},{q("stage"),phase==q("ChampSelect")?q("champ_select"):isLoadingPhase(phase)?q("loading"):q("in_game")},
        {q("game_id_observed"),!gameId.isEmpty()},{q("started_at_ms"),startedAtMs},{q("cutoff_source"),q("local_context_first_observation")},{q("roster_source"),source},{q("roster"),roster},
        {q("visibility"),QJsonObject{{q("allies"),allies},{q("enemies"),enemies}}},{q("unknowns"),unknowns}};
}
QJsonObject analyzeMatchComposition(const QJsonObject &context,const QMap<int,QJsonObject> &champions){
    if(!context.value(q("active")).toBool())return {};
    QJsonObject teams;for(const auto &side:{q("ALLY"),q("ENEMY")}){QMap<QString,int> roles;int visible=0,known=0;
        for(const auto &v:context.value(q("roster")).toArray()){const auto p=v.toObject();if(p.value(q("team"))!=side)continue;const int cid=p.value(q("champion_id")).toInt();if(cid<=0)continue;++visible;if(!champions.contains(cid))continue;++known;for(const auto &r:champions.value(cid).value(q("roles")).toArray())++roles[r.toString()];}
        QJsonObject counts;for(auto it=roles.cbegin();it!=roles.cend();++it)counts[it.key()]=it.value();QStringList labels;
        for(const auto &pair:QList<QPair<QString,QString>>{{q("tank"),q("坦克")},{q("marksman"),q("射手")},{q("mage"),q("法师")},{q("fighter"),q("战士")},{q("support"),q("辅助")},{q("assassin"),q("刺客")}})if(roles.value(pair.first)>0)labels.append(pair.second+QString::number(roles.value(pair.first)));
        QJsonArray needs;if(known>=3&&roles.value(q("tank"))+roles.value(q("fighter"))==0)needs.append(q("前排定位少"));
        teams[side]=QJsonObject{{q("visible_champions"),visible},{q("known_champions"),known},{q("role_counts"),counts},{q("needs"),needs},{q("summary"),labels.isEmpty()?q("英雄待确认"):labels.join(q(" · "))}};
    }
    const auto visibility=context.value(q("visibility")).toObject(),ally=visibility.value(q("allies")).toObject(),enemy=visibility.value(q("enemies")).toObject();
    const auto count=[](const QJsonObject &v){return QString::number(v.value(q("visible")).toInt())+(v.value(q("total")).isDouble()?q("/")+QString::number(v.value(q("total")).toInt()):QString());};
    const auto enemies=enemy.value(q("visible")).toInt()>0?q("对手 ")+count(enemy):context.value(q("stage"))==q("champ_select")?q("对手待公开"):q("对手待补齐");
    return {{q("source"),q("local_champion_roles")},{q("historical_performance_included"),false},{q("summary"),q("队友 ")+count(ally)+q(" · ")+enemies},{q("teams"),teams}};
}
