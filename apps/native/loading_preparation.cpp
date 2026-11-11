#include "loading_preparation.h"
#include "coach_model.h"
#include "combat.h"
#include "combat_sandbox.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>

namespace {
QString k(const char *s){return QString::fromLatin1(s);}
struct Input {QString context,reason;QJsonArray roster;int selfChampion=0;bool valid=false;};
bool positiveInt(const QJsonValue &v){return v.isDouble()&&std::isfinite(v.toDouble())&&v.toDouble()>0&&v.toDouble()==v.toInt();}
Input input(const Catalog &catalog,const GameState &state){
    Input out;const auto match=state.matchContext;out.context=match.value(k("context_id")).toString();
    auto fail=[&](const char *why){out.reason=k(why);return out;};
    if(!match.value(k("active")).toBool()||out.context.isEmpty()||match.value(k("stage"))!=k("loading"))return fail("not_active_loading_context");
    if(state.gameMode!=k("KIWI"))return fail("unsupported_or_unverified_mode");
    if(state.phase==k("InProgress")||state.phase==k("Reconnect"))return fail("live_state_requires_live_recompute");
    if(!modelRulesUsable(catalog,state))return fail("local_combat_database_missing");
    auto roster=match.value(k("roster")).toArray();bool standard=true;
    if(roster.isEmpty()){roster=state.players;standard=false;}
    QString own;int selves=0;
    for(const auto &v:roster){const auto p=v.toObject();if(p.value(k("self")).toBool()){++selves;own=p.value(k("team")).toString();}}
    if(selves!=1)return fail("unique_self_not_visible");
    if(standard&&own!=k("ALLY"))return fail("self_team_not_verified");
    if(!standard&&own!=k("ORDER")&&own!=k("CHAOS"))return fail("self_team_not_verified");
    int ally=0,enemy=0;QSet<QString> seen;
    for(const auto &v:roster){const auto p=v.toObject();const auto raw=p.value(k("champion_id"));const int champion=raw.toInt();
        if(!positiveInt(raw)||!catalog.champions.contains(champion)||!catalog.combat->champions.contains(champion))return fail("waiting_for_ten_visible_known_champions");
        const auto side=p.value(k("team")).toString();QString team;
        if(standard){if(side!=k("ALLY")&&side!=k("ENEMY"))return fail("roster_team_unknown");team=side;}
        else{if(side!=k("ORDER")&&side!=k("CHAOS"))return fail("roster_team_unknown");team=side==own?k("ALLY"):k("ENEMY");}
        const auto identity=team+QChar(u':')+QString::number(champion);
        if(seen.contains(identity))return fail("duplicate_same_team_champion_mapping");seen.insert(identity);
        if(team==k("ALLY"))++ally;else ++enemy;
        if(p.value(k("self")).toBool()){if(team!=k("ALLY"))return fail("self_team_not_verified");out.selfChampion=champion;}
        out.roster.append(QJsonObject{{k("champion_id"),champion},{k("team"),team},{k("self"),p.value(k("self")).toBool()}});
    }
    if(out.roster.size()!=10||ally!=5||enemy!=5)return fail("waiting_for_complete_five_vs_five_roster");
    if(state.championId>0&&state.championId!=out.selfChampion)return fail("self_champion_conflicts_with_roster");
    QList<QJsonObject> rows;for(const auto &v:out.roster)rows.append(v.toObject());
    std::sort(rows.begin(),rows.end(),[](const QJsonObject&a,const QJsonObject&b){return QJsonDocument(a).toJson(QJsonDocument::Compact)<QJsonDocument(b).toJson(QJsonDocument::Compact);});
    out.roster={};for(const auto &p:rows)out.roster.append(p);out.valid=true;return out;
}
QJsonValue stableHistoryValue(const QJsonValue &value){
    if(value.isObject()){QJsonObject out;const auto raw=value.toObject();for(auto it=raw.constBegin();it!=raw.constEnd();++it){
        if(QStringList{k("as_of_ms"),k("materialized_at_ms"),k("lookup_elapsed_ms"),k("source_revision"),k("revision"),k("player_index"),k("cell"),k("collected_at"),k("history_checked_at_ms"),k("profile_checked_at_ms")}.contains(it.key()))continue;
        out[it.key()]=stableHistoryValue(it.value());}return out;}
    if(value.isArray()){QList<QJsonValue> rows;for(const auto &v:value.toArray())rows.append(stableHistoryValue(v));
        std::sort(rows.begin(),rows.end(),[](const QJsonValue&a,const QJsonValue&b){return QJsonDocument(QJsonArray{a}).toJson(QJsonDocument::Compact)<QJsonDocument(QJsonArray{b}).toJson(QJsonDocument::Compact);});
        QJsonArray out;for(const auto &v:rows)out.append(v);return out;}
    return value;
}
QJsonObject usableHistory(const Catalog &catalog,const GameState &state,const Input &in){
    auto evidence=state.recommendationEvidence;
    if(state.observedPatch!=catalog.patch||evidence.value(k("context_id")).toString()!=in.context||evidence.value(k("patch")).toString()!=catalog.patch||!evidence.value(k("available")).toBool())return {};
    const auto cutoff=evidence.value(k("as_of_ms"));if(!cutoff.isDouble()||!std::isfinite(cutoff.toDouble())||cutoff.toDouble()<=0||cutoff.toDouble()>QDateTime::currentMSecsSinceEpoch())return {};
    // The loading roster is canonically reordered. Historical pressure indices
    // refer to the original live roster, so rebind every weight by verified side
    // and champion; never copy array indices across the hypothetical branch.
    QJsonArray reboundPlayers;QSet<int> mappedEnemies;
    for(const auto &v:evidence.value(k("players")).toArray()){
        auto p=v.toObject();int found=-1;
        for(int i=0;i<in.roster.size();++i){const auto current=in.roster[i].toObject();if(current.value(k("team"))==p.value(k("team"))&&current.value(k("champion_id"))==p.value(k("champion_id"))){found=i;break;}}
        p[k("player_index")]=found>=0?QJsonValue(found):QJsonValue(QJsonValue::Null);p[k("loading_mapping_verified")]=found>=0;
        if(found>=0&&p.value(k("team"))==k("ENEMY"))mappedEnemies.insert(found);reboundPlayers.append(p);
    }
    evidence[k("players")]=reboundPlayers;
    auto scenario=evidence.value(k("historical_pressure_scenario")).toObject();auto weights=scenario.value(k("weights")).toArray();QJsonArray mapped;QSet<int> used;
    if(scenario.value(k("available")).toBool()){
        for(const auto &v:weights){auto w=v.toObject();const int champion=w.value(k("champion_id")).toInt();int found=-1;
            for(int i=0;i<in.roster.size();++i){const auto p=in.roster[i].toObject();if(p.value(k("team"))==k("ENEMY")&&p.value(k("champion_id")).toInt()==champion){if(found>=0){found=-1;break;}found=i;}}
            if(found<0||!mappedEnemies.contains(found)||used.contains(found)){mapped={};break;}used.insert(found);w[k("player_index")]=found;mapped.append(w);
        }
        if(mapped.size()==5){scenario[k("weights")]=mapped;scenario[k("loading_roster_rebound")]=true;}
        else{scenario[k("available")]=false;scenario[k("reason")]=k("loading_history_pressure_mapping_unverified");scenario[k("weights")]=QJsonArray{};}
    }
    evidence[k("historical_pressure_scenario")]=scenario;return evidence;
}
QMap<QString,int> preparationRanks(const CombatDatabase &db,int champion){
    QMap<QString,int> maximum;
    for(const auto &v:db.champions.value(champion).value(k("spells")).toArray()){const auto spell=v.toObject();maximum[spell.value(k("slot")).toString()]=spell.value(k("max_rank")).toInt();}
    if(maximum.value(k("Q"))>0&&maximum.value(k("W"))>0&&maximum.value(k("E"))>0)return {{k("Q"),1},{k("W"),1},{k("E"),1},{k("R"),0}};
    return referenceCombatRanks(db,champion,3);
}
QJsonObject recordedGrowthInputs(const GameState &state){
    auto ids=[](QList<int> values){std::sort(values.begin(),values.end());QJsonArray out;for(int id:values)out.append(id);return out;};
    auto conditions=state.effectConditions.values();std::sort(conditions.begin(),conditions.end());QJsonObject counters;
    for(auto it=state.effectCounters.cbegin();it!=state.effectCounters.cend();++it)counters[it.key()]=std::isfinite(it.value())?QJsonValue(it.value()):QJsonValue(QJsonValue::Null);
    return {{k("selected_augment_ids"),ids(state.selected)},{k("candidate_augment_ids"),ids(state.offers)},{k("recorded_inventory_ids"),ids(state.owned)},
        {k("recorded_level"),state.level},{k("effect_conditions"),QJsonArray::fromStringList(conditions)},{k("effect_counters"),counters},
        {k("selection_observed_in_loading_context"),false},{k("inventory_observed_in_loading_context"),false},
        {k("carried_into_hypothetical_combat"),false},{k("future_stacks_projected"),false},
        {k("source_contract"),k("recorded_inputs_retained_separately_from_level_three_empty_loadout_hypothesis")}};
}
}

QString loadingPreparationBasis(const Catalog &catalog,const GameState &state,const QString &style){
    Q_UNUSED(style);
    const auto in=input(catalog,state);if(!in.valid)return {};
    const QJsonObject basis{{k("context_id"),in.context},{k("patch"),catalog.patch},{k("observed_patch"),state.observedPatch},{k("combat_revision"),catalog.combat->revision},
        {k("roster"),in.roster},{k("history"),stableHistoryValue(usableHistory(catalog,state,in))},{k("recorded_growth_inputs"),recordedGrowthInputs(state)},
        {k("style"),QStringLiteral("成长联动")},{k("directions_disabled"),true},{k("scenario_level"),3},{k("policy_version"),2}};
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(basis).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
}
QJsonObject runLoadingPreparation(const Catalog &catalog,const GameState &state,const QString &style){
    QElapsedTimer clock;clock.start();const auto in=input(catalog,state);
    QJsonObject out{{k("schema_version"),1},{k("available"),false},{k("status"),k("waiting")},{k("context_id"),in.context},
        {k("catalog_patch"),catalog.patch},{k("rule_basis"),modelRuleBasis(catalog,state)},{k("usable_as_live_recommendation"),false},{k("requires_live_recompute"),true},{k("hypothetical"),true},
        {k("directions_disabled"),true},{k("style"),QStringLiteral("成长联动")},{k("growth_computation_branches"),1},
        {k("recorded_growth_inputs"),recordedGrowthInputs(state)}};
    if(!in.valid){out[k("reason")]=in.reason;return out;}
    out[k("basis")]=loadingPreparationBasis(catalog,state,style);out[k("scenario_level")]=3;
    out[k("assumptions")]=QJsonObject{{k("starting_level_observed"),false},{k("level_policy"),k("explicit_level_three_hypothesis_not_mode_rule")},
        {k("ranks_policy"),k("one_rank_each_qwe_else_reference_hypothesis")},{k("all_inventories"),k("hypothetical_empty_not_observed")},
        {k("all_selected_augments"),k("hypothetical_empty_not_observed")},{k("budget_known"),false},{k("real_skill_cooldowns_known"),false},
        {k("real_hit_or_position_known"),false},{k("patch_observed"),!state.observedPatch.isEmpty()}};
    GameState scenario;scenario.source=GameState::Source::Replay;scenario.phase=k("InProgress");scenario.gameMode=k("KIWI");scenario.queueId=state.queueId;
    scenario.observedPatch=state.observedPatch;scenario.championId=in.selfChampion;scenario.level=3;scenario.goldKnown=false;scenario.matchContext=state.matchContext;
    scenario.matchContext[k("stage")]=k("in_game");scenario.matchContext[k("roster")]=in.roster;
    scenario.recommendationEvidence=usableHistory(catalog,state,in);
    QJsonObject selfRanks;
    for(const auto &v:in.roster){auto p=v.toObject();const int champion=p.value(k("champion_id")).toInt();const auto ranks=preparationRanks(*catalog.combat,champion);QJsonObject abilities;
        for(auto it=ranks.constBegin();it!=ranks.constEnd();++it)abilities[it.key()]=QJsonObject{{k("abilityLevel"),it.value()}};
        p[k("team")]=p.value(k("team"))==k("ALLY")?k("ORDER"):k("CHAOS");p[k("level")]=3;p[k("items")]=QJsonArray{};p[k("items_complete")]=true;
        p[k("augment_ids")]=QJsonArray{};p[k("augments_complete")]=true;p[k("is_dead")]=false;p[k("abilities")]=abilities;p[k("ranks_complete")]=true;
        if(p.value(k("self")).toBool())selfRanks=abilities;scenario.players.append(p);
    }
    scenario.abilities=selfRanks;
    CoachSimulationOptions options;options.samples=256;options.seed=44038;options.horizon=8;options.currentPurchaseOnly=true;options.work=std::make_shared<RecommendationWork>(2800);
    const auto coach=buildCoachSnapshot(catalog,scenario,QStringLiteral("成长联动"),options);const auto plan=coach.value(k("plan")).toObject();const auto sandbox=plan.value(k("purchase_sandbox")).toObject();
    out[k("scenario_growth_state")]=QJsonObject{{k("level"),scenario.level},{k("selected_augment_ids"),QJsonArray{}},{k("inventory_ids"),QJsonArray{}},
        {k("hypothetical"),true},{k("future_stacks_projected"),false},{k("observed_growth_inputs_used"),false}};
    for(const auto &key:{k("growth_reasoning"),k("growth_objective")})if(plan.value(key).isObject())out[key]=plan.value(key);
    if(!coach.value(k("computation_complete")).toBool()){
        out[k("status")]=k("interrupted");out[k("reason")]=coach.value(k("stop_reason"));out[k("computation_complete")]=false;
        out[k("stats")]=QJsonObject{{k("elapsed_ms"),double(clock.nsecsElapsed())/1e6},{k("work_budget_ms"),2800},{k("preparation_ran"),true}};return out;
    }
    QJsonArray directions;
    for(const auto &v:sandbox.value(k("candidates")).toArray()){if(directions.size()>=8)break;const auto row=v.toObject();const int item=row.value(k("item_id")).toInt();const auto score=row.value(k("score"));if(!catalog.items.contains(item)||!score.isDouble()||!std::isfinite(score.toDouble())||score.toDouble()<=0)continue;
        directions.append(QJsonObject{{k("item_id"),item},{k("name"),catalog.itemName(item)},{k("score"),row.value(k("score"))},{k("remaining_cost"),row.value(k("remaining_cost"))},
            {k("candidate_kind"),k("equipment")},{k("direction_matching_used"),false},
            {k("affordable"),QJsonValue(QJsonValue::Null)},{k("unknown_count"),row.value(k("unknown_count"))},{k("conditional"),true},{k("hypothetical_inventory"),true}});
    }
    const bool diagnostic=!sandbox.value(k("selected_sandbox")).isObject();
    const auto detail=sandbox.value(diagnostic?k("diagnostic_sandbox"):k("selected_sandbox")).toObject();
    out[k("combat_windows_basis")]=diagnostic?k("unselected_diagnostic_reference"):k("conditionally_selected_candidate");
    out[k("diagnostic_candidate_item_id")]=diagnostic?sandbox.value(k("diagnostic_candidate_item_id")):QJsonValue(QJsonValue::Null);
    out[k("ranking_available")]=sandbox.value(k("ranking_available"));out[k("critical_unknowns")]=sandbox.value(k("critical_unknowns"));
    QJsonArray windows;for(const auto &v:detail.value(k("windows")).toArray()){const auto w=v.toObject();windows.append(QJsonObject{{k("seconds"),w.value(k("seconds"))},
        {k("score_gain"),w.value(k("score_gain"))},{k("team_score_gain"),w.value(k("team_score_gain"))},{k("historical_pressure_team_score_gain"),w.value(k("historical_pressure_team_score_gain"))},{k("target_count"),w.value(k("targets")).toArray().size()}});}
    out[k("candidate_equipment")]=directions;out[k("candidate_directions")]=directions;out[k("candidate_directions_compatibility_only")]=true;
    out[k("candidate_augments")]=recordedGrowthInputs(state).value(k("candidate_augment_ids"));out[k("combat_windows")]=windows;out[k("simulation")]=coach.value(k("simulation"));
    const auto pressure=detail.value(k("historical_pressure_scenario")).toObject();
    out[k("history_used")]=!scenario.recommendationEvidence.isEmpty();out[k("history_pressure_used")]=pressure.value(k("available")).toBool();
    out[k("history_summary")]=scenario.recommendationEvidence;out[k("historical_pressure_scenario")]=pressure;
    out[k("history_usage")]=pressure.value(k("available")).toBool()?k("summary_and_conditional_pressure_sensitivity"):scenario.recommendationEvidence.isEmpty()?k("unavailable"):k("summary_only");
    out[k("coverage")]=QJsonObject{{k("visible_players"),in.roster.size()},{k("allies"),5},{k("enemies"),5},{k("self_champion"),in.selfChampion},{k("simulated_samples"),256},
        {k("candidate_inventory_observed"),false},{k("budget_observed"),false},{k("actual_selected_augments_observed"),false},{k("complete_combat_model"),false}};
    out[k("available")]=!directions.isEmpty();out[k("status")]=directions.isEmpty()?k("conditional_no_candidates"):k("prepared_conditional_growth_candidates");
    out[k("reason")]=directions.isEmpty()?k("no_legal_positive_candidate_under_preparation_hypothesis"):k("loading_preparation_requires_actual_live_loadout_recompute");
    out[k("stats")]=QJsonObject{{k("elapsed_ms"),double(clock.nsecsElapsed())/1e6},{k("enumerated_candidates"),sandbox.value(k("detailed_candidates"))},{k("candidate_directions"),directions.size()},{k("preparation_ran"),true}};
    return out;
}
