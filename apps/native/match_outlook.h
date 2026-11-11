#pragma once
#include "analysis.h"
#include <QStringList>
#include <cmath>

// Current observations only. This does not estimate an outcome probability or
// promote inventory catalogue value into earned gold or combat effectiveness.
inline QJsonObject observedMatchOutlook(const Catalog &catalog,const GameState &state){
    QJsonObject out{{"available",false},{"kind","observed_match_comparison"},{"probability",QJsonValue::Null},{"calibrated_win_rate_available",false},{"history_required",false},{"ds_required",false}};
    if(!state.fresh()||!state.matchContext.value("active").toBool()||state.matchContext.value("context_id").toString().isEmpty()
       ||state.gameMode!=QStringLiteral("KIWI")||(state.phase!=QStringLiteral("InProgress")&&state.phase!=QStringLiteral("Reconnect")))return out;
    QString ownTeam;int selves=0;
    for(const auto &v:state.players){const auto p=v.toObject();if(p.value("self")==QJsonValue(true)){++selves;if(p.value("champion_id").toInt()!=state.championId)return out;ownTeam=p.value("team").toString();}}
    if(selves!=1||(ownTeam!="ORDER"&&ownTeam!="CHAOS")||state.players.size()!=10)return out;
    int count[2]{},kills[2]{},levels[2]{},alive[2]{},value[2]{};
    bool killsKnown=true,levelsKnown=true,aliveKnown=true,valueKnown=modelRulesUsable(catalog,state);
    auto observed=[](const QJsonValue &v,int low,int high){return v.isDouble()&&std::isfinite(v.toDouble())&&v.toDouble()==v.toInt()&&v.toInt()>=low&&v.toInt()<=high;};
    for(const auto &v:state.players){const auto p=v.toObject();const auto team=p.value("team").toString();if((team!="ORDER"&&team!="CHAOS")||p.value("champion_id").toInt()<=0)return out;
        const int side=team==ownTeam?0:1;++count[side];
        const auto k=p.value("scores").toObject().value("kills"),l=p.value("level"),d=p.value("is_dead");
        if(observed(k,0,100000))kills[side]+=k.toInt();else killsKnown=false;
        if(observed(l,1,30))levels[side]+=l.toInt();else levelsKnown=false;
        if(d.isBool())alive[side]+=!d.toBool();else aliveKnown=false;
        if(p.value("items_complete")!=QJsonValue(true)||!p.value("items").isArray())valueKnown=false;
        for(const auto &id:p.value("items").toArray()){if(!observed(id,1,1000000)){valueKnown=false;continue;}const auto price=catalog.items.value(id.toInt()).value("price_total");if(observed(price,0,1000000))value[side]+=price.toInt();else valueKnown=false;}
    }
    if(count[0]!=5||count[1]!=5)return out;
    QJsonObject axes;QStringList summary,details;
    const auto axis=[&](const char *key,bool known,int a,int b,const QString &text){axes[key]=QJsonObject{{"available",known},{"ally",known?QJsonValue(a):QJsonValue::Null},{"enemy",known?QJsonValue(b):QJsonValue::Null},{"delta",known?QJsonValue(a-b):QJsonValue::Null}};if(known)details.append(text);};
    axis("kills",killsKnown,kills[0],kills[1],QStringLiteral("击杀 %1 : %2").arg(kills[0]).arg(kills[1]));
    axis("levels",levelsKnown,levels[0],levels[1],QStringLiteral("平均等级 %1 : %2").arg(levels[0]/5.,0,'f',1).arg(levels[1]/5.,0,'f',1));
    axis("alive",aliveKnown,alive[0],alive[1],QStringLiteral("存活 %1 : %2").arg(alive[0]).arg(alive[1]));
    axis("catalogue_equipment_value",valueKnown,value[0],value[1],QStringLiteral("装备目录价 %1 : %2 G").arg(value[0]).arg(value[1]));
    if(valueKnown){const int gap=value[0]-value[1];summary.append(gap==0?QStringLiteral("装备价接近"):QStringLiteral("我方装备价 %1%2G").arg(gap>0?QStringLiteral("+"):QString()).arg(gap));}
    if(aliveKnown)summary.append(QStringLiteral("存活 %1:%2").arg(alive[0]).arg(alive[1]));
    if(summary.isEmpty()&&killsKnown)summary.append(QStringLiteral("击杀 %1:%2").arg(kills[0]).arg(kills[1]));
    if(summary.isEmpty()&&levelsKnown)summary.append(QStringLiteral("均级 %1:%2").arg(levels[0]/5.,0,'f',1).arg(levels[1]/5.,0,'f',1));
    out["available"]=!summary.isEmpty();out["axes"]=axes;out["summary"]=summary.join(QStringLiteral(" · "));
    out["detail"]=details.join(QStringLiteral("\n"))+QStringLiteral("\n装备为目录参考价，未计强化改价、层数、技能和操作差异；这些是当前局面记录，不是胜率预测。");
    out["context_id"]=state.matchContext.value("context_id");out["game_time"]=state.gameTime;return out;
}
