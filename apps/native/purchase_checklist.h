#pragma once
#include "analysis.h"
#include "combat.h"
#include "purchase_planner.h"
#include <algorithm>
#include <cmath>

// A catalogue recipe checklist, separate from combat ranking and live shop
// offers. Later rows are conditional on physically completing previous rows.
inline QJsonObject purchaseChecklist(const Catalog &catalog,const GameState &game,int target,const QJsonObject &reference){
    QJsonObject out{{"available",false},{"reference_only",true},{"ranking_available",false},{"actual_shop_prices_verified",false},{"automatically_buys_items",false},{"rows",QJsonArray{}}};
    if(!catalog.combat||!game.fresh()||!purchaseReferenceItemCompatible(catalog,game,target)||game.gameMode!="KIWI"||(game.phase!="InProgress"&&game.phase!="Reconnect")||!game.matchContext.value("active").toBool()||game.matchContext.value("context_id").toString().isEmpty()||target<=0)return out;
    int selves=0;bool inventory=false;
    for(const auto &v:game.players){const auto p=v.toObject();if(p.value("self")!=QJsonValue(true))continue;++selves;
        QList<int> ids;bool valid=p.value("items").isArray();for(const auto &id:p.value("items").toArray()){valid&=id.isDouble()&&id.toDouble()==id.toInt()&&id.toInt()>0;ids.append(id.toInt());}
        auto held=game.owned;std::sort(ids.begin(),ids.end());std::sort(held.begin(),held.end());
        inventory=valid&&ids==held&&p.value("items_complete")==QJsonValue(true)&&p.value("champion_id").toInt()==game.championId&&p.value("level").toInt()==game.level;
    }
    if(selves!=1||!inventory||game.owned.contains(target))return out;
    auto owned=game.owned;const bool budget=game.goldKnown&&std::isfinite(game.gold)&&game.gold>=0;
    const auto quote=[&](const QList<int> &items,int id){return planInventoryRecipeReference(catalog,game.championId,game.level,items,id);};
    auto goal=quote(owned,target);if(!goal.value("legal").toBool()||goal.value("remaining_cost").toInt()<=0)return out;
    QJsonArray rows;
    const auto append=[&](int id,const QJsonObject &transaction,const char *kind,bool dependent){
        const int cost=transaction.value("remaining_cost").toInt();QJsonArray before;for(int item:owned)before.append(item);
        rows.append(QJsonObject{{"item_id",id},{"name",catalog.itemName(id)},{"estimated_cost",cost},{"step_kind",kind},{"dependent_on_previous",dependent},
            {"affordable_now",dependent||!budget?QJsonValue::Null:QJsonValue(cost<=game.gold)},{"gold_gap",dependent||!budget?QJsonValue::Null:QJsonValue(std::max(0,cost-int(game.gold)))},
            {"inventory_before",before},{"consume_item_ids",transaction.value("consume_item_ids")},{"inventory_after",transaction.value("inventory_after")}});
        owned.clear();for(const auto &item:transaction.value("inventory_after").toArray())owned.append(item.toInt());
    };
    // Preserve exact multiset consumption: a component is admitted only if its
    // physical purchase reduces this particular goal by the amount paid.
    if(!budget||goal.value("remaining_cost").toInt()>game.gold){
        QList<int> pending=catalog.combat->items.value(target).from;
        const int referencedAction=reference.value("action_item_id").toInt();if(referencedAction>0&&referencedAction!=target)pending.prepend(referencedAction);
        QSet<int> seen;int chosen=0,chosenCost=0;QJsonObject chosenQuote;bool chosenAffordable=false;
        while(!pending.isEmpty()&&seen.size()<4096){const int id=pending.takeFirst();if(id<=0||seen.contains(id))continue;seen.insert(id);
            pending.append(catalog.combat->items.value(id).from);
            const auto step=quote(owned,id);const int cost=step.value("remaining_cost").toInt();if(!step.value("legal").toBool()||cost<=0)continue;
            QList<int> after;for(const auto &part:step.value("inventory_after").toArray())after.append(part.toInt());const auto finish=quote(after,target);
            if(!finish.value("legal").toBool()||finish.value("remaining_cost").toInt()+cost!=goal.value("remaining_cost").toInt())continue;
            const bool affordable=budget&&cost<=game.gold;
            if(chosen==0||(affordable&&!chosenAffordable)||(affordable==chosenAffordable&&(affordable?cost>chosenCost:cost<chosenCost))){chosen=id;chosenCost=cost;chosenQuote=step;chosenAffordable=affordable;}
        }
        if(chosen>0){append(chosen,chosenQuote,"component",false);goal=quote(owned,target);}
    }
    if(!goal.value("legal").toBool())return out;
    append(target,goal,"complete",!rows.isEmpty());
    // Future items remain a public build combination, never an optimal order.
    if(reference.value("available").toBool()&&reference.value("context_id")==game.matchContext.value("context_id")&&reference.value("patch")==catalog.patch&&reference.value("target_item_id").toInt()==target){
        for(const auto &id:reference.value("route_item_ids").toArray()){
            const int item=id.toInt();if(rows.size()>=3)break;if(item<=0||item==target||owned.contains(item)||!purchaseReferenceItemCompatible(catalog,game,item))continue;const auto future=quote(owned,item);
            if(future.value("legal").toBool()&&future.value("remaining_cost").toInt()>0)append(item,future,"later",true);
        }
    }
    out["available"]=!rows.isEmpty();out["rows"]=rows;out["target_item_id"]=target;out["context_id"]=game.matchContext.value("context_id");out["patch"]=catalog.patch;out["observed_patch"]=game.observedPatch;out["combat_version_match"]=modelPatchMatches(catalog,game);out["source_url"]=reference.value("source_url");return out;
}
