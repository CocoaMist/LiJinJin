#pragma once
#include "analysis.h"
#include <cmath>

// Confirmations belong to the current roster (cleared by the runtime on a
// roster/context change). A new offer round revokes completeness, not the
// previously observed IDs. Growth input is an explicit short-lived scenario;
// it never registers an automatic observation provider or creates proc events.
inline void applyConfirmedMatchInputs(const Catalog &catalog,GameState &state,
    const QMap<int,QJsonObject> &entries,int round){
    const auto previous=state.growthObservations.take(QStringLiteral("manual_scenario")).toObject();
    if(previous.value("source")==QStringLiteral("manual_confirmation"))state.effectCounters.remove(QStringLiteral("item:3084:permanent_hp"));
    const auto previousForm=state.growthObservations.take(QStringLiteral("manual_form_scenario")).toObject();
    for(const auto &v:previousForm.value("conditions").toArray())state.effectConditions.remove(v.toString());
    for(auto it=entries.cbegin();it!=entries.cend();++it){
        if(it.key()<0||it.key()>=state.players.size())continue;
        const auto entry=it.value();auto player=state.players[it.key()].toObject();
        QList<int> ids;bool valid=entry.value("ids").isArray()&&entry.value("ids").toArray().size()<=8;
        for(const auto &v:entry.value("ids").toArray()){
            const int id=v.toInt();if(!v.isDouble()||v.toDouble()!=id||!catalog.augments.contains(id)||ids.contains(id)){valid=false;break;}ids.append(id);
        }
        if(!valid)continue;
        const bool complete=entry.value("complete").toBool()&&entry.value("confirmed_round").toInt(-1)==round
            &&(!player.value("self").toBool()||state.offers.isEmpty());
        const bool retainAutomatic=entry.value("confirmed_round").toInt(-1)!=round&&player.value("augments_complete").toBool()
            &&player.value("augment_source").toString()!=QStringLiteral("manual_confirmation");
        if(!retainAutomatic){player["augment_ids"]=entry.value("ids");player["augments_complete"]=complete;
            player["augment_source"]="manual_confirmation";state.players[it.key()]=player;}
        if(!player.value("self").toBool())continue;
        if(!retainAutomatic)state.selected=ids;
        const auto formAt=entry.value("conditions_at_game_time");
        if(formAt.isDouble()&&std::isfinite(formAt.toDouble())&&state.gameTime>=formAt.toDouble()&&state.gameTime-formAt.toDouble()<=30&&!entry.value("conditions").toArray().isEmpty()){
            for(const auto &v:entry.value("conditions").toArray())state.effectConditions.insert(v.toString());
            state.growthObservations["manual_form_scenario"]=QJsonObject{{"conditions",entry.value("conditions")},{"source","manual_confirmation"},{"game_time",formAt},{"valid_for_seconds",30},{"current_state_automatically_observed",false}};
        }
        const auto hp=entry.value("growth_hp"),at=entry.value("growth_at_game_time");
        if(!state.owned.contains(3084)||!hp.isDouble()||!std::isfinite(hp.toDouble())||hp.toDouble()<0||hp.toDouble()>1e9
            ||!at.isDouble()||!std::isfinite(at.toDouble())||state.gameTime<at.toDouble()||state.gameTime-at.toDouble()>30)continue;
        // A registered producer always wins. Manual assumptions cannot replace
        // fresh structured facts or turn a scenario into an actionable rank.
        if(state.effectCounters.contains(QStringLiteral("item:3084:permanent_hp")))continue;
        state.effectCounters[QStringLiteral("item:3084:permanent_hp")]=hp.toDouble();
        state.growthObservations["manual_scenario"]=QJsonObject{{"source","manual_confirmation"},{"counter_key","item:3084:permanent_hp"},
            {"value",hp},{"unit","health_points"},{"game_time",at},{"valid_for_seconds",30},
            {"current_value_automatically_observed",false},{"ranking_available",false}};
    }
}
