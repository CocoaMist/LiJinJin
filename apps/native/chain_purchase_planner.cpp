#include "chain_purchase_planner.h"
#include "analysis.h"
#include "chain_equipment_comparison.h"
#include "chain_mechanism_contract.h"
#include "combat.h"
#include "combat_observation_scope.h"
#include "decision_context.h"
#include "growth.h"
#include "headliner.h"
#include "purchase_planner.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>

namespace {
QString k(const char *s){return QString::fromLatin1(s);}
QString hash(const QJsonObject &o){return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(o).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());}
bool number(const QJsonValue &v,double low=0,double high=1e12){return v.isDouble()&&std::isfinite(v.toDouble())&&v.toDouble()>=low&&v.toDouble()<=high;}
QJsonArray ids(const QList<int> &v){QJsonArray out;for(int id:v)out.append(id);return out;}
QList<int> list(const QJsonArray &v){QList<int> out;for(const auto &id:v)out.append(id.toInt());return out;}
bool sameIds(const QJsonArray &a,const QJsonArray &b){auto x=list(a),y=list(b);std::sort(x.begin(),x.end());std::sort(y.begin(),y.end());return x==y;}
QJsonObject stateBinding(const GameState &s){
    QJsonObject counters;for(auto it=s.effectCounters.cbegin();it!=s.effectCounters.cend();++it)counters[it.key()]=it.value();
    auto conditions=s.effectConditions.values();conditions.sort();
    return {{k("source"),int(s.source)},{k("source_received_at_ms"),s.received.isValid()?double(s.received.toMSecsSinceEpoch()):-1.},
        {k("context"),s.matchContext},{k("observed_patch"),s.observedPatch},{k("mode"),s.gameMode},{k("phase"),s.phase},
        {k("game_time"),s.gameTime},{k("champion_id"),s.championId},{k("level"),s.level},{k("players"),s.players},
        {k("items"),ids(s.owned)},{k("selected"),ids(s.selected)},{k("abilities"),s.abilities},{k("live_stats"),s.liveStats},
        {k("gold_known"),s.goldKnown},{k("gold"),s.gold},{k("counters"),counters},{k("conditions"),QJsonArray::fromStringList(conditions)},
        {k("growth_observations"),s.growthObservations},{k("headliner_context"),s.headlinerContext},
        {k("mechanic_observations"),s.mechanicObservations},{k("mechanic_baseline_time"),s.mechanicBaselineTime}};
}
QJsonObject rejected(const QString &reason){return {{k("available"),false},{k("reason"),reason},{k("binding_is_authentication"),false}};}
QJsonValue withdrawn(const QJsonValue &v){
    if(v.isArray()){QJsonArray result;for(const auto &child:v.toArray())result.append(withdrawn(child));return result;}
    if(!v.isObject())return v;
    QJsonObject result;const auto object=v.toObject();bool revoked=false;
    for(auto it=object.begin();it!=object.end();++it){const auto key=it.key();
        if(key==k("selected_inventory")||key==k("selected_sandbox")||key==k("selected_score_basis")||key==k("recommendation_kind")){revoked=true;continue;}
        if(key==k("ranking_available")||key==k("selection_available")||key==k("ranking_eligible")){result[key]=false;revoked=true;}
        else if(key==k("selected_item_id")){result[key]=0;revoked=true;}
        else if(key==k("conditional_ranking")){result[key]=QJsonArray{};revoked=true;}
        else result[key]=withdrawn(it.value());
    }
    if(revoked){result[k("diagnostic_only")]=true;result[k("ranking_scope")]=k("withdrawn_no_selection");
        if(result.contains(k("score"))||result.contains(k("score_gain")))result[k("score_scope")]=k("diagnostic_only_withdrawn_no_selection");}
    return result;
}
}

QJsonObject withdrawExplicitChainPurchaseSelection(const QJsonObject &result){return withdrawn(result).toObject();}
QString explicitChainPurchaseCacheBasis(const GameState &s){
    return s.explicitPurchaseChain.isEmpty()?QString():hash({{k("state"),stateBinding(s)},{k("chain"),s.explicitPurchaseChain}});
}
QJsonObject explicitChainPurchaseBasis(const Catalog &c,const GameState &s){
    if(!c.combat||!s.fresh()||!modelRulesUsable(c,s)||s.gameMode!=k("KIWI")||!s.matchContext.value(k("active")).toBool()
        ||s.matchContext.value(k("stage"))!=k("in_game")||s.matchContext.value(k("context_id")).toString().isEmpty())return rejected(k("fresh_bound_same_patch_in_game_required"));
    if(s.players.size()!=10)return rejected(k("complete_ten_player_roster_required"));
    const auto health=combatHealthContract(c,s,true);
    if(s.source==GameState::Source::Live&&!combatHealthUnknowns(health).isEmpty())return rejected(k("required_live_health_inputs_unavailable"));
    const bool panel=health.value(k("current_health_used")).toBool();
    QJsonArray players,healthInputs;QMap<QString,int> duplicates;QString selfKey;int selves=0,order=0,chaos=0;
    for(const auto &raw:s.players){
        if(!raw.isObject())return rejected(k("invalid_roster_row"));auto row=raw.toObject();
        if(!row.value(k("self")).isBool())return rejected(k("explicit_roster_self_boolean_required"));const bool self=row.value(k("self")).toBool();
        if(self){++selves;if(row.value(k("champion_id")).toInt()!=s.championId||row.value(k("level")).toInt()!=s.level
            ||!sameIds(row.value(k("items")).toArray(),ids(s.owned))||!sameIds(row.value(k("augment_ids")).toArray(),ids(effectiveSelectedAugments(c,s))))return rejected(k("self_loadout_conflicts_with_state"));
            if(!s.abilities.isEmpty())row[k("abilities")]=s.abilities;
            auto counters=row.value(k("effect_counters")).toObject();for(auto it=s.effectCounters.cbegin();it!=s.effectCounters.cend();++it)counters[it.key()]=it.value();row[k("effect_counters")]=counters;
            auto conditions=row.value(k("effect_conditions")).toArray();auto ordered=s.effectConditions.values();ordered.sort();for(const auto &v:ordered)if(!conditions.contains(v))conditions.append(v);row[k("effect_conditions")]=conditions;
        }
        const auto ctx=playerCombatContext(*c.combat,row);
        if(!ctx.available)return rejected(ctx.report.value(k("reason")).toString(k("player_context_unavailable")));
        if(ctx.report.value(k("rank_source"))!=k("observed"))return rejected(k("all_actor_skill_ranks_required"));
        if(!row.value(k("items")).isArray()||row.value(k("items_complete"))!=QJsonValue(true)
            ||!row.value(k("augment_ids")).isArray()||row.value(k("augments_complete"))!=QJsonValue(true))return rejected(k("complete_visible_loadouts_required"));
        const auto team=row.value(k("team")).toString();if(team==k("ORDER"))++order;else if(team==k("CHAOS"))++chaos;else return rejected(k("invalid_team"));
        const auto stem=team+k(":")+QString::number(ctx.context.championId);const auto key=stem+k(":")+QString::number(++duplicates[stem]);
        QJsonObject p{{k("player_key"),key},{k("team"),team},{k("champion_id"),ctx.context.championId},{k("level"),ctx.context.level},
            {k("items"),row.value(k("items"))},{k("augments"),row.value(k("augment_ids"))},{k("augments_complete"),true},{k("ranks"),ctx.report.value(k("resolved_ranks"))}};
        QJsonObject counters;for(auto it=ctx.context.counters.cbegin();it!=ctx.context.counters.cend();++it)counters[it.key()]=it.value();p[k("counters")]=counters;
        auto conditions=ctx.context.conditions.values();conditions.sort();p[k("conditions")]=QJsonArray::fromStringList(conditions);
        auto action=row.value(k("action_state")).toObject();
        if(row.contains(k("action_state"))&&!row.value(k("action_state")).isObject())return rejected(k("invalid_action_state"));
        if(self){selfKey=key;if(!action.contains(k("resource_current"))&&number(s.liveStats.value(k("resourceValue"))))action[k("resource_current")]=s.liveStats.value(k("resourceValue"));
            auto remaining=action.value(k("cooldown_remaining")).toObject();for(const auto &slot:{k("Q"),k("W"),k("E"),k("R")}){
                const auto v=s.abilities.value(slot).toObject().value(k("cooldownRemaining"));if(!remaining.contains(slot)&&number(v))remaining[slot]=v;
            }if(!remaining.isEmpty())action[k("cooldown_remaining")]=remaining;
        }
        if(c.combat->champions.value(ctx.context.championId).value(k("uses_mana")).toBool()&&!number(action.value(k("resource_current"))))return rejected(k("current_resource_required"));
        if(action.contains(k("ready_at"))){if(!action.value(k("ready_at")).isObject())return rejected(k("invalid_ready_at"));auto ready=action.value(k("ready_at")).toObject();
            for(auto it=ready.begin();it!=ready.end();++it){if(!number(it.value()))return rejected(k("invalid_ready_at"));it.value()=std::max(0.,it.value().toDouble()-s.gameTime);}action[k("ready_at")]=ready;
        }
        p[k("action_state")]=action;
        QString healthSource=k("explicit_roster_current_health_reference");
        if(row.contains(k("current_health"))){if(!number(row.value(k("current_health"))))return rejected(k("invalid_current_health"));p[k("current_health")]=row.value(k("current_health"));}
        if(self&&panel){p[k("current_health")]=s.liveStats.value(k("currentHealth"));healthSource=s.source==GameState::Source::Live?k("live_panel_currentHealth"):k("caller_replay_panel_currentHealth_reference");}
        if(row.contains(k("is_dead"))){if(!row.value(k("is_dead")).isBool())return rejected(k("invalid_is_dead"));p[k("is_dead")]=row.value(k("is_dead"));}
        if(!p.contains(k("current_health"))&&p.value(k("is_dead"))!=QJsonValue(true))return rejected(k("alive_actor_current_health_required"));
        if(!p.contains(k("current_health")))healthSource=k("explicit_roster_is_dead_zero_reference");
        healthInputs.append(QJsonObject{{k("player_key"),key},{k("self"),self},{k("requested_source"),healthSource},{k("input_value"),p.value(k("current_health"))},
            {k("is_dead_input"),p.value(k("is_dead"))},{k("panel_bound"),self&&panel}});
        players.append(p);
    }
    if(selves!=1||order!=5||chaos!=5)return rejected(k("standard_five_vs_five_unique_self_required"));
    QJsonObject bound{{k("schema_version"),1},{k("patch"),c.patch},{k("combat_revision"),c.combat->revision},{k("state"),stateBinding(s)},
        {k("players"),players},{k("focus_player_key"),selfKey}};
    return {{k("available"),true},{k("binding_sha256"),hash(bound)},{k("patch"),c.patch},{k("combat_revision"),c.combat->revision},
        {k("players"),players},{k("focus_player_key"),selfKey},{k("context_id"),s.matchContext.value(k("context_id"))},
        {k("source_received_at_ms"),s.received.isValid()?double(s.received.toMSecsSinceEpoch()):-1.},{k("game_time"),s.gameTime},
        {k("health_inputs"),healthInputs},{k("binding_is_authentication"),false},{k("scope"),k("caller_declared_conditional_scenario_not_live_observation")}};
}

QJsonObject buildExplicitChainPurchaseSandbox(const Catalog &c,const GameState &s,const RecommendationWorkPtr &work){
    QJsonObject out{{k("schema_version"),1},{k("policy"),k("explicit_chain_next_purchase_v1")},{k("status"),k("unavailable")},
        {k("selected_item_id"),0},{k("ranking_available"),false},{k("enumeration_complete"),false},{k("screening_complete"),false},{k("detailed_comparisons_complete"),false},
        {k("candidates"),QJsonArray{}},{k("conditional_purchase_candidates"),QJsonArray{}},
        {k("purchase_availability_scope"),k("ranked_candidates_require_current_purchase_conditions_passed")},
        {k("all_effects_complete"),false},{k("globally_optimal_full_build"),false},{k("actual_win_rate"),QJsonValue::Null},
        {k("mechanic_snapshot_time"),s.gameTime},{k("mechanic_valid_until"),-1},{k("context_id"),s.matchContext.value(k("context_id"))},
        {k("score_basis"),k("explicit_chain_condition_comparison")},{k("fixed_formula_windows_evaluated"),false}};
    auto health=combatHealthContract(c,s,true);const bool panel=health.value(k("current_health_used")).toBool();
    health[k("current_health_used")]=false;health[k("current_health_bound_to_scenario")]=false;health[k("numeric_scenario_available")]=false;
    health[k("summary")]=QStringLiteral("显式动作链尚未产生可用数值结果；这里只保留血量输入依据。");out[k("health_basis")]=health;
    auto fail=[&](const QString &why,bool interrupted=false){out[k("status")]=interrupted?k("interrupted"):k("unavailable");out[k("unavailable_reason")]=why;
        if(interrupted)out[k("stop_reason")]=why;out[k("ranking_available")]=false;out[k("selected_item_id")]=0;out.remove(k("selected_inventory"));out.remove(k("selected_sandbox"));
        out=withdrawExplicitChainPurchaseSelection(out);return out;};
    auto stop=[&](){if(work&&work->stopped())return work->stopReason();if(!s.fresh())return k("snapshot_expired");
        // A worker holds an immutable game clock. Bound a live source lease
        // conservatively by elapsed receipt age too; publication has its own
        // current-game-clock guard, but direct callers must also lose a winner.
        const auto until=s.explicitPurchaseChain.value(k("source")).toObject().value(k("valid_until_game_time"));
        if(s.source==GameState::Source::Live&&number(until)&&s.received.isValid()
            &&s.gameTime+std::max(0.,double(s.received.msecsTo(QDateTime::currentDateTimeUtc()))/1000.)>=until.toDouble())return k("explicit_chain_source_expired_during_comparison");
        return QString();};
    auto why=stop();if(!why.isEmpty())return fail(why,true);
    const auto basis=explicitChainPurchaseBasis(c,s);out[k("chain_binding")]=basis;
    if(!basis.value(k("available")).toBool())return fail(basis.value(k("reason")).toString());
    const auto envelope=s.explicitPurchaseChain;const auto source=envelope.value(k("source")).toObject();
    if(envelope.value(k("schema_version"))!=QJsonValue(1)||envelope.value(k("binding_sha256"))!=basis.value(k("binding_sha256")))return fail(k("explicit_chain_binding_mismatch"));
    if(source.value(k("kind"))!=k("caller_declared_scenario")||source.value(k("source_id")).toString().trimmed().isEmpty()||source.value(k("source_id")).toString().size()>256
        ||source.value(k("context_id"))!=basis.value(k("context_id"))||source.value(k("patch"))!=basis.value(k("patch"))||source.value(k("combat_revision"))!=basis.value(k("combat_revision"))
        ||source.value(k("source_received_at_ms"))!=basis.value(k("source_received_at_ms")))return fail(k("explicit_chain_source_binding_invalid"));
    if(!envelope.value(k("scenario")).isObject())return fail(k("explicit_chain_scenario_required"));auto scene=envelope.value(k("scenario")).toObject();
    const QSet<QString> sceneKeys{k("start_time"),k("horizon"),k("action_events"),k("players"),k("auto_actions"),k("condition_source"),
        k("item_zone_models"),k("acknowledgement_item_zone_model_v1")};
    for(auto it=scene.begin();it!=scene.end();++it)if(!sceneKeys.contains(it.key()))return fail(k("unsupported_explicit_chain_scenario_field:")+it.key());
    if((scene.contains(k("start_time"))&&scene.value(k("start_time"))!=QJsonValue(0))||!number(scene.value(k("horizon")),.001,120)
        ||!scene.value(k("action_events")).isArray()||scene.value(k("action_events")).toArray().isEmpty()||scene.value(k("action_events")).toArray().size()>512
        ||(scene.contains(k("auto_actions"))&&scene.value(k("auto_actions"))!=QJsonValue(false)))return fail(k("invalid_explicit_chain_clock_or_events"));
    const double horizon=scene.value(k("horizon")).toDouble();
    if(!number(source.value(k("valid_from_game_time")))||!number(source.value(k("valid_until_game_time")))||source.value(k("valid_from_game_time")).toDouble()>s.gameTime
        ||source.value(k("valid_until_game_time")).toDouble()<s.gameTime+horizon)return fail(k("explicit_chain_source_validity_missing_or_expired"));
    out[k("mechanic_valid_until")]=source.value(k("valid_until_game_time"));
    if(scene.contains(k("players"))&&scene.value(k("players"))!=basis.value(k("players")))return fail(k("explicit_chain_roster_conflicts_with_current_binding"));
    scene[k("players")]=basis.value(k("players"));scene[k("start_time")]=0;scene[k("auto_actions")]=false;
    QJsonObject conditionSource{{k("kind"),k("caller_declared_scenario")},{k("source_id"),source.value(k("source_id"))},{k("patch"),c.patch},
        {k("combat_revision"),c.combat->revision},{k("valid_from"),0},{k("valid_until"),horizon}};
    if(scene.contains(k("condition_source"))&&scene.value(k("condition_source"))!=QJsonValue(conditionSource))return fail(k("conflicting_chain_condition_source"));scene[k("condition_source")]=conditionSource;
    QMap<QString,QJsonObject> actors;for(const auto &v:scene.value(k("players")).toArray())actors[v.toObject().value(k("player_key")).toString()]=v.toObject();
    for(const auto &v:scene.value(k("action_events")).toArray()){const auto e=v.toObject();const auto type=e.value(k("type")).toString();
        // Other event families need a separate, reviewed observation contract.
        if(type!=k("cast")&&type!=k("skill_state")&&type!=k("attack")&&type!=k("item_zone_contact"))return fail(k("unsupported_purchase_chain_event_type"));
        const auto actor=actors.value(e.value(k("player_key")).toString());if(actor.isEmpty())return fail(k("chain_actor_not_in_current_roster"));
        const auto contract=chainActionContract(c,e,actor);
        if(type==k("skill_state")&&!contract.value(k("known")).toBool())return fail(k("source_backed_skill_state_contract_required"));
        if(contract.value(k("requires_admitted_cast")).toBool()){const auto slot=contract.value(k("slot")).toString();const auto a=actor.value(k("action_state")).toObject();
            if(!number(a.value(k("cooldown_remaining")).toObject().value(slot))&&!number(a.value(k("ready_at")).toObject().value(slot)))return fail(k("referenced_skill_current_cooldown_required"));
        }
    }
    QJsonArray transactions;bool transitionComplete=true;auto growthContext=c.combat->context(s.championId,s.level);growthContext.counters=s.effectCounters;growthContext.conditions=s.effectConditions;
    const auto selected=effectiveSelectedAugments(c,s);
    out[k("candidate_universe")]=k("finished_items_and_selected_augment_stat_scaling_plain_components");
    for(int itemId:nextPurchaseCandidateIds(c,s)){const auto it=c.combat->items.constFind(itemId);
        why=stop();if(!why.isEmpty())return fail(why,true);
        auto transaction=planInventoryPurchase(c,s.championId,s.level,s.owned,it.key());if(!transaction.value(k("legal")).toBool()) {
            if(transaction.value(k("conditional_purchase")).toBool()) {
                auto pending=out.value(k("conditional_purchase_candidates")).toArray();
                pending.append(QJsonObject{{k("item_id"),it.key()},{k("static_candidate"),true},{k("ranking_eligible"),false},
                    {k("purchase_availability"),transaction.value(k("purchase_availability"))}});
                out[k("conditional_purchase_candidates")]=pending;
            }
            continue;
        }
        const auto transition=equipmentGrowthInventoryTransition(*c.combat,growthContext,s.owned,list(transaction.value(k("inventory_after")).toArray()),selected,s.growthObservations);
        transitionComplete&=transition.value(k("numeric_transition_verified")).toBool();transaction[k("growth_inventory_transition")]=transition;
        transaction[k("item_id")]=it.key();transaction[k("candidate_id")]=QString::number(it.key());
        transaction[k("affordable_now")]=s.goldKnown&&std::isfinite(s.gold)&&s.gold>=0&&transaction.value(k("remaining_cost")).toDouble()<=s.gold;
        transactions.append(transaction);
    }
    out[k("enumeration_complete")]=true;out[k("legal_candidates")]=transactions.size();out[k("transactions")]=transactions;
    if(transactions.isEmpty())return fail(k("no_legal_next_purchase"));
    QJsonArray rows,batches;QJsonObject first;bool complete=true;QStringList criticals;int compared=0;
    for(int start=0;start<transactions.size();start+=16){
        why=stop();if(!why.isEmpty())return fail(why,true);
        QJsonArray candidates;for(int i=start;i<std::min(start+16,int(transactions.size()));++i){const auto t=transactions[i].toObject();candidates.append(QJsonObject{{k("candidate_id"),t.value(k("candidate_id"))},{k("items"),t.value(k("inventory_after"))}});}
        QJsonObject request{{k("scenario"),scene},{k("focus_player_key"),basis.value(k("focus_player_key"))},{k("candidate_inventories"),candidates}};
        if(envelope.contains(k("objective")))request[k("objective")]=envelope.value(k("objective"));
        const auto result=compareChainEquipment(c,request);batches.append(result);out[k("batches")]=batches;
        why=stop();if(!why.isEmpty())return fail(why,true);
        if(first.isEmpty()){first=result;out[k("explicit_chain_condition_comparison")]=first;
            const auto initial=first.value(k("baseline_initial_states")).toArray();const bool native=!initial.isEmpty();health[k("numeric_scenario_available")]=native;
            health[k("actor_health_inputs")]=basis.value(k("health_inputs"));health[k("native_initial_states")]=initial;
            bool used=false;for(const auto &v:basis.value(k("health_inputs")).toArray()){const auto input=v.toObject();if(!input.value(k("self")).toBool())continue;
                for(const auto &p:initial){const auto actual=p.toObject();if(actual.value(k("player_key"))==input.value(k("player_key")))used=input.value(k("panel_bound")).toBool()&&actual.value(k("initial_health"))==input.value(k("input_value"))&&input.value(k("is_dead_input"))!=QJsonValue(true);}
            }
            health[k("current_health_used")]=used;health[k("current_health_bound_to_scenario")]=native&&panel;
            if(native)health[k("summary")]=QStringLiteral("血量初始值由动作账本实际结果列出；调用者声明的动作链仍是条件参考。");out[k("health_basis")]=health;
        }else for(const auto &name:{k("scenario_sha256"),k("baseline_items"),k("baseline_metrics"),k("baseline_initial_states"),k("baseline_score"),k("objective_basis"),k("baseline_event_admissions"),k("baseline_mechanism_contract")})if(result.value(name)!=first.value(name))complete=false;
        complete&=result.value(k("available")).toBool()&&result.value(k("ranking_available")).toBool()&&result.value(k("complete_candidate_comparison")).toBool();
        for(const auto &u:result.value(k("critical_unknowns")).toArray())criticals.append(u.toString());
        const auto comparedRows=result.value(k("candidates")).toArray();complete&=comparedRows.size()==candidates.size();
        for(int offset=0;offset<std::min(int(comparedRows.size()),int(candidates.size()));++offset){auto row=comparedRows[offset].toObject();const auto t=transactions[start+offset].toObject();
            if(row.value(k("candidate_id"))!=t.value(k("candidate_id")))complete=false;
            row[k("item_id")]=t.value(k("item_id"));row[k("transaction")]=t;row[k("remaining_cost")]=t.value(k("remaining_cost"));row[k("affordable_now")]=t.value(k("affordable_now"));rows.append(row);++compared;
        }
        out[k("candidates")]=rows;out[k("compared_candidates")]=compared;
    }
    out[k("screening_complete")]=true;out[k("detailed_comparisons_complete")]=compared==transactions.size();out[k("diagnostic_comparison_available")]=!rows.isEmpty();
    criticals.removeDuplicates();criticals.sort();out[k("critical_unknowns")]=QJsonArray::fromStringList(criticals);
    auto comparison=first;comparison[k("candidates")]=rows;comparison[k("batch_count")]=batches.size();comparison[k("complete_candidate_universe")]=complete&&compared==transactions.size();
    out[k("explicit_chain_condition_comparison")]=comparison;
    why=stop();if(!why.isEmpty())return fail(why,true);
    if(!transitionComplete)return fail(k("growth_inventory_transition_unverified"));
    if(!complete||compared!=transactions.size()||!criticals.isEmpty())return fail(k("critical_inputs_or_complete_chain_comparison_unavailable"));
    if(!s.goldKnown||!std::isfinite(s.gold)||s.gold<0)return fail(k("current_gold_unobserved"));
    QList<QJsonObject> ordered;for(const auto &v:rows)ordered.append(v.toObject());std::sort(ordered.begin(),ordered.end(),[](const auto &a,const auto &b){
        const double x=a.value(k("score_gain")).toDouble(),y=b.value(k("score_gain")).toDouble();return x!=y?x>y:a.value(k("item_id")).toInt()<b.value(k("item_id")).toInt();});
    QJsonArray ranking,sorted;QJsonObject winner;
    for(const auto &row:ordered){sorted.append(row);ranking.append(row.value(k("item_id")));if(winner.isEmpty()&&row.value(k("affordable_now")).toBool()&&row.value(k("score_gain")).isDouble()&&row.value(k("score_gain")).toDouble()>1e-8)winner=row;}
    comparison[k("candidates")]=sorted;comparison[k("conditional_ranking")]=ranking;comparison[k("ranking_available")]=true;comparison[k("source")]=source;comparison[k("binding_sha256")]=basis.value(k("binding_sha256"));
    comparison[k("selection_scope")]=k("complete_declared_next_purchase_universe_same_chain_and_objective");comparison[k("selection_available")]=!winner.isEmpty();
    out[k("explicit_chain_condition_comparison")]=comparison;out[k("candidates")]=sorted;out[k("conditional_ranking")]=ranking;out[k("ranking_available")]=true;
    out[k("status")]=winner.isEmpty()?k("no_affordable_positive_chain_gain"):k("conditional_chain_purchase_available");
    if(!winner.isEmpty()){out[k("selected_item_id")]=winner.value(k("item_id"));out[k("selected_inventory")]=winner.value(k("transaction")).toObject().value(k("inventory_after"));
        out[k("selected_sandbox")]=winner;out[k("selected_score_basis")]=k("explicit_chain_condition_comparison");out[k("recommendation_kind")]=k("caller_declared_chain_conditional_purchase");}
    return out;
}
