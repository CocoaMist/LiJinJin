#include "live_equipment_cues.h"
#include "analysis.h"
#include "combat.h"
#include <QDateTime>
#include <QSet>
#include <cmath>
#include <limits>

namespace {
QString q(const char *s){return QString::fromLatin1(s);}
int integer(const QJsonValue &v,int minimum=0){
    if(!v.isDouble())return -1;const double n=v.toDouble();
    return std::isfinite(n)&&n>=minimum&&n<=std::numeric_limits<int>::max()&&n==std::floor(n)?int(n):-1;
}
bool teamKnown(const QString &v){return v==q("ORDER")||v==q("CHAOS");}
QJsonArray delta(const QMap<int,int> &now,const QMap<int,int> &before){
    QJsonArray out;for(auto it=now.cbegin();it!=now.cend();++it)if(it.value()>before.value(it.key()))
        out.append(QJsonObject{{q("item_id"),it.key()},{q("count"),it.value()-before.value(it.key())}});
    return out;
}
}
void LiveEquipmentCues::clear(const QString &why){
    inventories.clear();contextId.clear();patch.clear();ownTeam.clear();latestActor.clear();
    latest={};timeline={};changes={};lastGameTime=-1;available=false;reason=why;valueDifference=QJsonValue::Null;
}
QJsonObject LiveEquipmentCues::report() const {
    const bool visible=available&&!latest.isEmpty()&&QDateTime::currentMSecsSinceEpoch()<latest.value(q("expires_at_ms")).toInteger();
    return {{q("schema_version"),1},{q("available"),available},{q("reason"),reason},{q("context_id"),contextId},
        {q("patch"),patch},{q("source"),q("live_client_inventory_difference")},{q("latest"),visible?QJsonValue(latest):QJsonValue(QJsonValue::Null)},
        {q("timeline"),timeline},{q("changes"),changes},{q("estimated_equipment_value_difference"),valueDifference},
        {q("total_economy_known"),false},{q("display_duration_ms"),8000},{q("timeline_limit"),64}};
}
QJsonObject LiveEquipmentCues::update(const Catalog &catalog,const GameState &state){
    const auto match=state.matchContext;const auto nextContext=match.value(q("context_id")).toString();
    if(state.source!=GameState::Source::Live||!state.fresh()){clear(q("stale_or_offline"));return report();}
    if(!match.value(q("active")).toBool()||nextContext.isEmpty()){clear(q("inactive_context"));return report();}
    if(!modelRulesUsable(catalog,state)){clear(q("local_rules_unavailable"));return report();}
    if(!std::isfinite(state.gameTime)||state.gameTime<0){clear(q("invalid_game_time"));return report();}
    QString selfTeam;int selves=0;bool validTeams=!state.players.isEmpty();
    QMap<QString,int> groups,cells;QList<QJsonObject> rows;
    for(const auto &v:state.players){
        if(!v.isObject()){validTeams=false;continue;}const auto p=v.toObject();rows.append(p);
        const auto side=p.value(q("team")).toString();const int cid=integer(p.value(q("champion_id")),1);
        validTeams&=teamKnown(side)&&cid>0;
        if(p.value(q("self")).toBool()){++selves;selfTeam=side;}
        ++groups[side+q(":")+QString::number(cid)];const int cell=integer(p.value(q("cell")));
        if(cell>=0&&cell<100)++cells[side+q(":")+QString::number(cell)];
    }
    if(!validTeams||selves!=1||!teamKnown(selfTeam)){clear(q("own_team_unknown"));return report();}
    if(contextId!=nextContext||patch!=catalog.patch||ownTeam!=selfTeam||(lastGameTime>=0&&state.gameTime<lastGameTime))clear(q("new_baseline"));
    contextId=nextContext;patch=catalog.patch;ownTeam=selfTeam;lastGameTime=state.gameTime;
    available=true;reason=q("ready");changes={};valueDifference=QJsonValue::Null;
    QMap<QString,Inventory> next;QMap<QString,double> values;QMap<QString,int> teamCounts;
    bool allValuesKnown=true;const qint64 observed=state.received.toMSecsSinceEpoch();
    for(const auto &p:rows){
        const auto side=p.value(q("team")).toString();const int cid=integer(p.value(q("champion_id")),1);
        const auto group=side+q(":")+QString::number(cid);const int cell=integer(p.value(q("cell")));
        const auto cellKey=side+q(":")+QString::number(cell);QString actor;
        if(cell>=0&&cell<100&&cells.value(cellKey)==1)actor=q("cell:")+cellKey;
        else if((cell<0||cell>=100)&&groups.value(group)==1)actor=q("champion:")+group;
        ++teamCounts[side];bool complete=p.value(q("items_complete")).isBool()&&p.value(q("items_complete")).toBool()&&p.value(q("items")).isArray();
        QSet<int> utility;for(const auto &v:p.value(q("utility_items")).toArray()){const int id=integer(v,1);if(id>0)utility.insert(id);}
        Inventory inventory;inventory.champion=cid;double total=0;
        for(const auto &v:p.value(q("items")).toArray()){
            const int id=integer(v);if(id==0)continue;
            if(id<0||!catalog.items.contains(id)||!catalog.combat->items.contains(id)){complete=false;continue;}
            if(utility.contains(id))continue;
            ++inventory.counts[id];const auto price=catalog.items.value(id).value(q("price_total"));
            if(!price.isDouble()||!std::isfinite(price.toDouble())||price.toDouble()<0)allValuesKnown=false;else total+=price.toDouble();
        }
        if(!complete)allValuesKnown=false;else values[side]+=total;
        if(actor.isEmpty()||!complete){allValuesKnown=false;continue;}next[actor]=inventory;
    }
    if(allValuesKnown&&teamCounts.value(q("ORDER"))==5&&teamCounts.value(q("CHAOS"))==5)
        valueDifference=values.value(selfTeam)-values.value(selfTeam==q("ORDER")?q("CHAOS"):q("ORDER"));
    for(auto it=next.cbegin();it!=next.cend();++it){
        const auto previous=inventories.constFind(it.key());if(previous==inventories.cend()||previous->champion!=it->champion)continue;
        const auto side=it.key().section(QChar(':'),1,1);if(side==selfTeam)continue;
        const auto added=delta(it->counts,previous->counts),removed=delta(previous->counts,it->counts);
        if(added.isEmpty()&&removed.isEmpty())continue;
        changes.append(QJsonObject{{q("team"),side},{q("champion_id"),it->champion},{q("added"),added},{q("removed"),removed}});
        for(const auto &v:added){const auto addition=v.toObject();const int id=addition.value(q("item_id")).toInt();const auto &entry=catalog.combat->items.constFind(id).value();
            if(!entry.finished&&!entry.boots)continue;
            QJsonObject event{{q("kind"),q("enemy_item_added")},{q("context_id"),contextId},{q("team"),side},{q("champion_id"),it->champion},
                {q("item_id"),id},{q("item_name"),catalog.itemName(id)},{q("count"),addition.value(q("count"))},
                {q("game_time"),state.gameTime},{q("observed_at_ms"),observed},{q("expires_at_ms"),observed+8000},
                {q("source"),q("live_client_inventory_difference")},{q("estimated_equipment_value_difference"),valueDifference}};
            latest=event;latestActor=it.key();timeline.append(event);while(timeline.size()>64)timeline.removeAt(0);
        }
    }
    inventories=next;if(!inventories.contains(latestActor)){latest={};latestActor.clear();}
    return report();
}
