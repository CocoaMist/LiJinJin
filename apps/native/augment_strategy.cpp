#include "augment_strategy.h"
#include "augment_advisory.h"
#include "external_strategy_reference.h"
#include "tactical_context.h"
#include "headliner.h"
#include "combat.h"
#include "combat_unknown_policy.h"
#include "combat_observation_scope.h"
#include "combat_sandbox.h"
#include "decision_context.h"
#include "coach_recommendation_quality.h"
#include "decision_goal.h"
#include "growth_objective.h"
#include "loadout_outcome_reference.h"
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>

namespace {
double gain(double next,double base) { return 100*(next/base-1); }

bool boundedInteger(const QJsonValue &value,int minimum,int maximum) {
    return value.isDouble()&&std::isfinite(value.toDouble())&&std::floor(value.toDouble())==value.toDouble()
        &&value.toDouble()>=minimum&&value.toDouble()<=maximum;
}

QJsonObject augmentRankingQuality(const Catalog &catalog,const GameState &state) {
    // Same self-roster observation contract as purchase recommendation quality.
    // This independent card path neither evaluates purchases nor interprets an
    // empty/unobserved selected list as proof that the player has no augments.
    QJsonObject self;int selfCount=0;
    for(const auto &v:state.players)if(v.toObject().value(QStringLiteral("self")).toBool()){self=v.toObject();++selfCount;}
    const bool identity=selfCount==1&&boundedInteger(self.value(QStringLiteral("champion_id")),1,100000)
        &&self.value(QStringLiteral("champion_id")).toInt()==state.championId;
    const auto raw=self.value(QStringLiteral("augment_ids"));QList<int> observed;QSet<int> seen;
    bool valid=identity&&raw.isArray()&&raw.toArray().size()<=12;
    if(valid)for(const auto &v:raw.toArray()){
        if(!boundedInteger(v,1,1000000)||seen.contains(v.toInt())||!catalog.augments.contains(v.toInt())){valid=false;break;}
        seen.insert(v.toInt());observed.append(v.toInt());
    }
    if(!valid)observed.clear();
    const bool complete=valid&&self.value(QStringLiteral("augments_complete")).isBool()&&self.value(QStringLiteral("augments_complete")).toBool();
    auto recorded=state.selected,sortedObserved=observed;std::sort(recorded.begin(),recorded.end());std::sort(sortedObserved.begin(),sortedObserved.end());
    const bool matches=complete&&recorded==sortedObserved;
    QJsonArray observedIds,recordedIds;for(int id:observed)observedIds.append(id);for(int id:state.selected)recordedIds.append(id);
    const QString observationStatus=!identity?QStringLiteral("self_identity_unverified"):!valid?QStringLiteral("invalid_or_missing_list")
        :complete?QStringLiteral("complete"):observed.isEmpty()?QStringLiteral("unknown"):QStringLiteral("partial");
    const QJsonObject observation{{QStringLiteral("source"),QStringLiteral("self_roster_augment_ids_and_augments_complete")},
        {QStringLiteral("status"),observationStatus},{QStringLiteral("self_identity_verified"),identity},{QStringLiteral("complete"),complete},
        {QStringLiteral("observed_ids"),observedIds},{QStringLiteral("observed_count"),observed.size()},
        {QStringLiteral("total_count"),complete?QJsonValue(observed.size()):QJsonValue(QJsonValue::Null)},
        {QStringLiteral("recorded_model_ids"),recordedIds},{QStringLiteral("model_selection_matches_observation"),complete?QJsonValue(matches):QJsonValue(QJsonValue::Null)},
        {QStringLiteral("unknown_is_zero"),false},{QStringLiteral("stats_residual_identifies_augments"),false}};
    const auto roles=catalog.champion(state.championId).value(QStringLiteral("roles")).toArray();
    const QSet<QString> knownRoles{QStringLiteral("marksman"),QStringLiteral("mage"),QStringLiteral("fighter"),QStringLiteral("assassin"),QStringLiteral("support"),QStringLiteral("tank")};
    bool rolesVerified=!roles.isEmpty();for(const auto &role:roles)if(!role.isString()||!knownRoles.contains(role.toString()))rolesVerified=false;
    const bool support=roles.contains(QStringLiteral("support")),tank=roles.contains(QStringLiteral("tank"));
    const bool model=catalog.combat&&catalog.combat->champions.contains(state.championId)&&state.fresh()
        &&modelRulesUsable(catalog,state)&&state.gameMode==QStringLiteral("KIWI")&&state.level>=1&&state.level<=18;
    int allies=0,enemies=0;const auto ownTeam=self.value(QStringLiteral("team")).toString();
    if(identity&&(ownTeam==QStringLiteral("ORDER")||ownTeam==QStringLiteral("CHAOS")))for(const auto &v:state.players){const auto side=v.toObject().value(QStringLiteral("team")).toString();
        if(side==ownTeam)++allies;else if(side==QStringLiteral("ORDER")||side==QStringLiteral("CHAOS"))++enemies;}
    const bool fullTeams=state.players.size()==10&&allies==5&&enemies==5;
    QJsonArray reasons,gaps;auto reason=[&](const QString &code,const QString &summary){reasons.append(QJsonObject{{QStringLiteral("code"),code},{QStringLiteral("summary"),summary}});};
    auto gap=[&](const QString &code,const QString &summary){gaps.append(QJsonObject{{QStringLiteral("code"),code},{QStringLiteral("summary"),summary}});};
    if(support)gap(QStringLiteral("ally_support_objective_unmodeled"),QStringLiteral("队友增益收益尚未完整计入"));
    if(tank)gap(QStringLiteral("ally_protection_and_control_objective_unmodeled"),QStringLiteral("保护与控制收益尚未完整计入"));
    if(!rolesVerified)gap(QStringLiteral("champion_roles_unverified"),QStringLiteral("英雄目标覆盖尚未核实"));
    if(!identity)reason(QStringLiteral("self_roster_unverified"),QStringLiteral("本人阵容记录未核实，暂不排序"));
    else if(!complete)reason(QStringLiteral("self_augments_unobserved_or_incomplete"),QStringLiteral("海克斯记录未完整，暂不排序"));
    else if(!matches)reason(QStringLiteral("self_augment_selection_mismatch"),QStringLiteral("强化观测与计算记录不一致，暂不排序"));
    if(!model)reason(QStringLiteral("current_model_unavailable"),QStringLiteral("当前同版局内模型未就绪，暂不排序"));
    if(catalog.combat&&!catalog.combat->champions.value(state.championId).value(QStringLiteral("skill_mechanism_gaps_v10")).toArray().isEmpty())
        reason(QStringLiteral("hero_mechanism_incomplete"),QStringLiteral("英雄技能机制尚未完整接入，保留条件数值"));
    QJsonObject skillObservation;
    if(model){const auto ranks=selfSkillRankContext(*catalog.combat,state);skillObservation=ranks.report;
        if(!ranks.available)reason(QStringLiteral("skill_ranks_invalid"),QStringLiteral("当前技能等级不合法，等待刷新"));
        else if(!ranks.report.value(QStringLiteral("complete")).toBool())reason(QStringLiteral("skill_ranks_missing"),QStringLiteral("当前技能等级未完整，等待刷新"));}
    const bool allowed=reasons.isEmpty();
    const QString summary=allowed?QStringLiteral("仅按已知数值作条件比较"):reasons.first().toObject().value(QStringLiteral("summary")).toString();
    const QJsonObject objective{{QStringLiteral("catalog_roles"),roles},{QStringLiteral("roles_verified"),rolesVerified},
        {QStringLiteral("roster_verified"),identity},{QStringLiteral("roster_verification_scope"),QStringLiteral("unique_matching_self_row")},
        {QStringLiteral("complete_team_roster_verified"),fullTeams},{QStringLiteral("complete_team_roster_verification_scope"),QStringLiteral("ten_rows_five_per_team_mapping_only")},
        {QStringLiteral("full_team_roster_required_for_personal_numbers"),false},
        {QStringLiteral("self_damage_included"),model},{QStringLiteral("self_effective_health_included"),model},
        {QStringLiteral("ally_protection_included"),false},{QStringLiteral("control_value_included"),false},{QStringLiteral("ally_support_objective_included"),false},
        {QStringLiteral("ranking_scope"),QStringLiteral("conditional_known_numeric_effects_only")},
        {QStringLiteral("role_usage"),QStringLiteral("identify_missing_objectives_only_no_role_score")},{QStringLiteral("objective_gaps"),gaps}};
    QJsonObject quality{{QStringLiteral("schema_version"),1},{QStringLiteral("ranking_allowed"),allowed},{QStringLiteral("summary"),summary},
        {QStringLiteral("reasons"),reasons},{QStringLiteral("objective_gaps"),gaps},{QStringLiteral("objective_coverage"),objective},{QStringLiteral("augment_observation"),observation},
        {QStringLiteral("numeric_card_values_preserved"),true},{QStringLiteral("role_heuristic_scores_used"),false},
        {QStringLiteral("skill_rank_observation"),skillObservation},
        {QStringLiteral("calculation_availability_checked_separately"),true},{QStringLiteral("overall_optimum_claim_allowed"),false}};
    quality[QStringLiteral("decision_goal")]=decisionGoalContract(state,quality,true);
    return quality;
}

QJsonValue anonymousHistoryStatistics(const QJsonValue &value,int depth=0) {
    if(depth>6)return QJsonValue();
    // A second allowlist keeps hypothetical/test states from smuggling private
    // identities through the otherwise validated native history adapter.
    static const QSet<QString> allowed{
        QStringLiteral("status"),QStringLiteral("source"),QStringLiteral("engine_version"),QStringLiteral("as_of_ms"),
        QStringLiteral("historical_reference_only"),QStringLiteral("causal_effect_known"),QStringLiteral("skill_multiplier"),
        QStringLiteral("recent"),QStringLiteral("limit"),QStringLiteral("games"),QStringLiteral("eligible_games"),QStringLiteral("sampled_games"),
        QStringLiteral("known_outcomes"),QStringLiteral("latest_ended_at_ms"),QStringLiteral("available"),QStringLiteral("sample_limited"),
        QStringLiteral("metrics"),QStringLiteral("field_samples"),QStringLiteral("kills"),QStringLiteral("deaths"),QStringLiteral("assists"),
        QStringLiteral("gold_earned"),QStringLiteral("damage_to_champions"),QStringLiteral("damage_taken"),QStringLiteral("kda"),
        QStringLiteral("gold_per_minute"),QStringLiteral("damage_per_minute"),QStringLiteral("taken_per_minute"),QStringLiteral("deaths_per_minute"),
        QStringLiteral("teammate_heal_per_minute"),QStringLiteral("shield_per_minute"),QStringLiteral("physical_damage_per_minute"),
        QStringLiteral("magic_damage_per_minute"),QStringLiteral("true_damage_per_minute"),QStringLiteral("damage_mitigated"),
        QStringLiteral("gpm"),QStringLiteral("dpm"),QStringLiteral("takenpm"),QStringLiteral("deathspm"),QStringLiteral("healpm"),QStringLiteral("shieldpm"),
        QStringLiteral("process_coverage"),QStringLiteral("frame_observations"),QStringLiteral("item_events"),QStringLiteral("real_candidate_event_records"),
        QStringLiteral("selected_augment_event_records"),QStringLiteral("permanent_growth_observed_games"),QStringLiteral("maximum_observed_level"),
        QStringLiteral("position_samples"),QStringLiteral("inferred_unobserved_states"),QStringLiteral("evidence_dimensions"),QStringLiteral("samples"),
        QStringLiteral("damage"),QStringLiteral("tanking"),QStringLiteral("support"),QStringLiteral("control"),QStringLiteral("engage"),QStringLiteral("builds"),
        QStringLiteral("growth"),QStringLiteral("basis"),QStringLiteral("champion_id"),QStringLiteral("all_patch_sampled_games"),QStringLiteral("same_patch_sampled_games"),
        QStringLiteral("champions"),QStringLiteral("items"),QStringLiteral("fixed_items"),QStringLiteral("augments"),QStringLiteral("id"),
        QStringLiteral("build_combinations"),QStringLiteral("item_ids"),QStringLiteral("augment_ids"),QStringLiteral("evidence_stage"),QStringLiteral("choice_quality_known"),
        QStringLiteral("timeline_games"),QStringLiteral("timeline_sampled_games"),QStringLiteral("process_item_games"),QStringLiteral("process_augment_games"),
        QStringLiteral("growth_observation_games"),QStringLiteral("permanent_growth_observation_games"),QStringLiteral("real_offer_observations"),
        QStringLiteral("confirmed_choice_observations"),QStringLiteral("all_time_history_complete"),QStringLiteral("historical_alternatives_complete"),
        QStringLiteral("refresh_status"),QStringLiteral("source_halted"),QStringLiteral("refresh_requested"),
        QStringLiteral("overall"),QStringLiteral("same_champion"),QStringLiteral("same_patch"),QStringLiteral("same_patch_champion"),
        QStringLiteral("conditional_performance_reference"),QStringLiteral("components"),QStringLiteral("scope"),
        QStringLiteral("own_participant_rows"),QStringLiteral("other_participant_rows"),QStringLiteral("other_distinct_players"),QStringLiteral("direction"),
        QStringLiteral("gold_earned_per_minute"),QStringLiteral("damage_to_champions_per_minute"),QStringLiteral("mechanical_quality_known"),
        QStringLiteral("recorded_augment_events"),QStringLiteral("freshness_verified"),QStringLiteral("requires_materialization")};
    if(value.isObject()){QJsonObject clean;const auto object=value.toObject();for(auto it=object.constBegin();it!=object.constEnd();++it)
        if(allowed.contains(it.key()))clean[it.key()]=anonymousHistoryStatistics(it.value(),depth+1);return clean;}
    if(value.isArray()){QJsonArray clean;for(const auto &v:value.toArray()){if(clean.size()==10)break;clean.append(anonymousHistoryStatistics(v,depth+1));}return clean;}
    if(value.isDouble())return std::isfinite(value.toDouble())?value:QJsonValue();
    if(value.isString())return value.toString().left(160);
    return value.isBool()?value:QJsonValue();
}

QJsonObject recommendationEvidenceSummary(const GameState &state) {
    // The adapter owns identity/cutoff validation. Export only its anonymous,
    // bounded statistical summary, never raw player payloads or identities.
    const auto evidence=state.recommendationEvidence;QJsonObject out;
    for(const auto &name:{QStringLiteral("schema_version"),QStringLiteral("available"),QStringLiteral("status"),QStringLiteral("reason"),
        QStringLiteral("context_id"),QStringLiteral("patch"),QStringLiteral("as_of_ms"),QStringLiteral("mapped_players"),QStringLiteral("history_summary_cutoff_policy"),
        QStringLiteral("evaluated_players"),QStringLiteral("known_history_players"),QStringLiteral("fresh_history_players"),
        QStringLiteral("player_analysis_consumed_players"),QStringLiteral("multidimensional_profile_consumed_players")})
        if(evidence.contains(name))out[name]=evidence.value(name);
    QJsonArray players;
    for(const auto &v:evidence.value(QStringLiteral("players")).toArray()){
        if(players.size()==10)break;const auto raw=v.toObject();QJsonObject row;
        for(const auto &name:{QStringLiteral("player_index"),QStringLiteral("champion_id"),QStringLiteral("team"),QStringLiteral("self"),QStringLiteral("status"),QStringLiteral("selected_pressure_scope"),
            QStringLiteral("history_status"),QStringLiteral("history_analysis_status"),QStringLiteral("player_analysis_consumed"),QStringLiteral("multidimensional_profile_consumed")})
            if(raw.contains(name))row[name]=raw.value(name);
        for(const auto &name:{QStringLiteral("player_analysis"),QStringLiteral("profile_summary"),QStringLiteral("champion_references"),QStringLiteral("freshness"),QStringLiteral("coverage"),
            QStringLiteral("history"),QStringLiteral("same_champion"),QStringLiteral("same_patch"),QStringLiteral("same_patch_champion")})
            if(raw.value(name).isObject())row[name]=anonymousHistoryStatistics(raw.value(name));
        if(raw.value(QStringLiteral("multidimensional_profile_consumed")).toBool()&&raw.value(QStringLiteral("multidimensional_profile")).isObject())
            row[QStringLiteral("multidimensional_profile")]=anonymousHistoryStatistics(raw.value(QStringLiteral("multidimensional_profile")));
        const auto scope=raw.value(QStringLiteral("selected_pressure_scope")).toString();
        if(scope==QStringLiteral("same_patch_champion")||scope==QStringLiteral("same_patch")){
            const auto reference=raw.value(scope).toObject();
            row[QStringLiteral("pressure_reference")]=QJsonObject{{QStringLiteral("scope"),scope},
                {QStringLiteral("games"),reference.value(QStringLiteral("games"))},{QStringLiteral("sampled_games"),reference.value(QStringLiteral("sampled_games"))},
                {QStringLiteral("damage_per_minute"),reference.value(QStringLiteral("metrics")).toObject().value(QStringLiteral("damage_per_minute"))},
                {QStringLiteral("samples"),reference.value(QStringLiteral("field_samples")).toObject().value(QStringLiteral("damage_per_minute"))}};
        }
        players.append(row);
    }
    const auto coverage=recommendationHistoryCoverage(state);
    out[QStringLiteral("players")]=players;out[QStringLiteral("history_known_players")]=coverage.value(QStringLiteral("known_history_players"));
    out[QStringLiteral("coverage")]=coverage;out[QStringLiteral("full_roster_history_available")]=coverage.value(QStringLiteral("all_ten_have_history"));
    for(const auto &name:{QStringLiteral("evaluated_players"),QStringLiteral("known_history_players"),QStringLiteral("fresh_history_players"),
        QStringLiteral("player_analysis_consumed_players"),QStringLiteral("multidimensional_profile_consumed_players")})out[name]=coverage.value(name);
    out[QStringLiteral("history_query_status")]=evidence.value(QStringLiteral("status"));out[QStringLiteral("status")]=coverage.value(QStringLiteral("status"));
    out[QStringLiteral("historical_reference_only")]=true;out[QStringLiteral("causal_effect_known")]=false;
    out[QStringLiteral("identity_exported")]=false;out[QStringLiteral("actual_win_rate")]=QJsonValue(QJsonValue::Null);
    return out;
}

bool annotateMultiWindowOffer(QJsonObject &row,const Catalog &catalog,const GameState &state,int id,const QString &style,int historyKnown,
                              const std::shared_ptr<CombatSandboxReuse> &reuse,const RecommendationWorkPtr &work) {
    CombatSandboxOptions options;options.style=style;options.teamObjective=true;options.compareCandidateAugments=true;
    // Card rankings use the current snapshot and the 3/8/20-second comparisons.
    options.includeGrowthProjection=false;
    options.reuse=reuse;options.work=work;
    options.candidateAugments=effectiveSelectedAugments(catalog,state);options.candidateAugments.append(id);
    const auto detailed=evaluateCombatSandbox(catalog,state,state.owned,options);
    if(detailed.value(QStringLiteral("status"))==QStringLiteral("interrupted")||(work&&work->stopped()))return false;
    QSet<QString> baselineUnknowns,additionalUnknowns,blockingUnknowns;bool unknownComparisonComplete=true;
    // Subtract within each identical target/window pair, then union. A code
    // shared with another target's baseline cannot excuse a candidate gap here.
    auto compareUnknowns=[&](const QJsonObject &pair){
        // Aggregate codes may originate in the enemy or teammate reference,
        // outside self's raw evaluation, but still affect this comparison.
        for(const auto &v:pair.value(QStringLiteral("unknowns")).toArray())
            if(v.isString()&&combatUnknownBlocksComparison(v.toString()))blockingUnknowns.insert(v.toString());
        const auto before=pair.value(QStringLiteral("current")).toObject().value(QStringLiteral("raw_eval")).toObject().value(QStringLiteral("unknowns"));
        const auto after=pair.value(QStringLiteral("candidate")).toObject().value(QStringLiteral("raw_eval")).toObject().value(QStringLiteral("unknowns"));
        if(!before.isArray()||!after.isArray()){unknownComparisonComplete=false;return;}
        QSet<QString> localBaseline;
        for(const auto &v:before.toArray())if(v.isString()&&!v.toString().isEmpty()){
            localBaseline.insert(v.toString());baselineUnknowns.insert(v.toString());
            if(combatUnknownBlocksComparison(v.toString()))blockingUnknowns.insert(v.toString());
        }
        for(const auto &v:after.toArray())if(v.isString()&&!v.toString().isEmpty()){
            if(!localBaseline.contains(v.toString()))additionalUnknowns.insert(v.toString());
            if(combatUnknownBlocksComparison(v.toString()))blockingUnknowns.insert(v.toString());
        }
    };
    compareUnknowns(detailed);
    // Preserve independently identified current-card/headliner gaps too. These
    // have an attributable candidate origin, unlike aggregate sandbox unknowns.
    for(const auto &v:row.value(QStringLiteral("additional_unknowns")).toArray())if(v.isString()&&!v.toString().isEmpty())additionalUnknowns.insert(v.toString());
    // The reference evaluation can include teammate/held-effect gaps that are
    // not candidate-attributable. Sharing them never proves cancellation.
    for(const auto &v:row.value(QStringLiteral("marginal_blocking_unknowns")).toArray())
        if(v.isString()&&!v.toString().isEmpty())blockingUnknowns.insert(v.toString());
    QJsonArray windows;QSet<int> secondsSeen;double matchup=0,historical=0;bool complete=detailed.value(QStringLiteral("available")).toBool(),historyComplete=true;
    for(const auto &v:detailed.value(QStringLiteral("windows")).toArray()){
        const auto w=v.toObject();const auto seconds=w.value(QStringLiteral("seconds"));
        if(!boundedInteger(seconds,3,20)||!QSet<int>{3,8,20}.contains(seconds.toInt())||secondsSeen.contains(seconds.toInt())){complete=false;continue;}
        secondsSeen.insert(seconds.toInt());const auto value=w.value(QStringLiteral("team_score_gain")),history=w.value(QStringLiteral("historical_pressure_team_score_gain"));
        const auto targets=w.value(QStringLiteral("targets")).toArray();bool targetsValid=targets.size()==5;
        for(const auto &target:targets){targetsValid&=target.toObject().value(QStringLiteral("available")).toBool();compareUnknowns(target.toObject());}
        const bool valid=targetsValid&&w.value(QStringLiteral("supported_targets")).toInt()==5&&w.value(QStringLiteral("team_supported_targets")).toInt()==5
            &&value.isDouble()&&std::isfinite(value.toDouble());
        const bool historyValid=valid&&w.value(QStringLiteral("historical_pressure_supported_targets")).toInt()==5&&history.isDouble()&&std::isfinite(history.toDouble());
        complete&=valid;historyComplete&=historyValid;if(valid)matchup+=value.toDouble();if(historyValid)historical+=history.toDouble();
        QJsonObject summary{{QStringLiteral("seconds"),seconds},{QStringLiteral("target_count"),targets.size()},
            {QStringLiteral("supported_targets"),w.value(QStringLiteral("team_supported_targets"))},{QStringLiteral("team_score_gain"),value}};
        if(historyValid)summary[QStringLiteral("historical_pressure_team_score_gain")]=history;
        windows.append(summary);
    }
    complete&=secondsSeen.size()==3;const auto pressure=detailed.value(QStringLiteral("historical_pressure_scenario")).toObject();
    historyComplete&=complete&&pressure.value(QStringLiteral("available")).toBool();
    const auto rosterScenarios=summarizeRosterHistoryScenarios(detailed);
    const auto scenarioSummary=summarizeCombatScenarios(detailed);
    row[QStringLiteral("decision_scenarios")]=scenarioSummary;
    const bool rosterHistoryComplete=complete&&rosterScenarios.value(QStringLiteral("available")).toBool()
        &&rosterScenarios.value(QStringLiteral("score_gain")).isDouble()&&std::isfinite(rosterScenarios.value(QStringLiteral("score_gain")).toDouble());
    const auto boundedCodes=[](const QSet<QString> &values){auto sorted=values.values();sorted.sort();QJsonArray out;
        for(const auto &value:sorted){if(out.size()==64)break;out.append(value.left(256));}return out;};
    QJsonArray comparabilityReasons;
    if(!complete)comparabilityReasons.append(QJsonObject{{QStringLiteral("code"),QStringLiteral("five_enemy_three_window_team_comparison_incomplete")}});
    if(!unknownComparisonComplete)comparabilityReasons.append(QJsonObject{{QStringLiteral("code"),QStringLiteral("paired_window_unknowns_unavailable")}});
    if(!additionalUnknowns.isEmpty())comparabilityReasons.append(QJsonObject{{QStringLiteral("code"),QStringLiteral("candidate_additional_window_unknowns")},
        {QStringLiteral("count"),additionalUnknowns.size()}});
    if(!blockingUnknowns.isEmpty())comparabilityReasons.append(QJsonObject{{QStringLiteral("code"),QStringLiteral("explicit_mechanic_state_or_parameter_unverified")},
        {QStringLiteral("count"),blockingUnknowns.size()},{QStringLiteral("unknowns"),boundedCodes(blockingUnknowns)}});
    const bool candidateGap=row.value(QStringLiteral("evaluation_model_gap")).toBool()||!row.value(QStringLiteral("evaluation_missing_inputs")).toArray().isEmpty();
    if(candidateGap)comparabilityReasons.append(QJsonObject{{QStringLiteral("code"),QStringLiteral("candidate_required_effect_or_model_gap")},
        {QStringLiteral("evaluation_reason_code"),row.value(QStringLiteral("evaluation_reason_code"))}});
    if(!scenarioSummary.value(QStringLiteral("complete")).toBool())comparabilityReasons.append(QJsonObject{
        {QStringLiteral("code"),QStringLiteral("declared_combat_scenarios_incomplete")}});
    const bool comparable=complete&&unknownComparisonComplete&&additionalUnknowns.isEmpty()&&blockingUnknowns.isEmpty()&&!candidateGap
        &&scenarioSummary.value(QStringLiteral("complete")).toBool();
    row[QStringLiteral("decision_baseline_unknowns")]=boundedCodes(baselineUnknowns);row[QStringLiteral("decision_baseline_unknown_count")]=baselineUnknowns.size();
    row[QStringLiteral("decision_additional_unknowns")]=boundedCodes(additionalUnknowns);row[QStringLiteral("decision_additional_unknown_count")]=additionalUnknowns.size();
    row[QStringLiteral("decision_blocking_unknowns")]=boundedCodes(blockingUnknowns);row[QStringLiteral("decision_blocking_unknown_count")]=blockingUnknowns.size();
    row[QStringLiteral("decision_comparable")]=comparable;row[QStringLiteral("decision_marginal_comparable")]=comparable;
    row[QStringLiteral("decision_comparability_reasons")]=comparabilityReasons;
    row[QStringLiteral("decision_unknown_comparison_scope")]=QStringLiteral("per_target_per_window_candidate_minus_own_baseline_then_union_shared_unknowns_not_assumed_to_cancel");
    QJsonArray unknowns;for(const auto &v:detailed.value(QStringLiteral("unknowns")).toArray()){if(unknowns.size()==32)break;unknowns.append(v.toString().left(160));}
    row[QStringLiteral("decision_trace")]=QJsonObject{{QStringLiteral("status"),detailed.value(QStringLiteral("status"))},
        {QStringLiteral("comparison_kind"),detailed.value(QStringLiteral("comparison_kind"))},{QStringLiteral("windows"),windows},
        {QStringLiteral("historical_pressure_available"),historyComplete},{QStringLiteral("historical_pressure_reason"),pressure.value(QStringLiteral("reason"))},
        {QStringLiteral("historical_roster_scenarios"),rosterScenarios},{QStringLiteral("historical_roster_available"),rosterHistoryComplete},
        {QStringLiteral("unknowns"),unknowns},{QStringLiteral("bounded_summary"),true}};
    row[QStringLiteral("decision_numeric_available")]=complete;
    row[QStringLiteral("decision_fallback")]=!complete;
    if(!complete){row[QStringLiteral("decision_fallback_reason")]=QStringLiteral("five_enemy_three_window_team_comparison_incomplete");
        row[QStringLiteral("decision_fallback_basis")]=QStringLiteral("legacy_eight_second_conditional_gain");return true;}
    matchup=100*matchup/3;historical=100*historical/3;
    row[QStringLiteral("matchup_gain_percent")]=matchup;
    if(historyComplete)row[QStringLiteral("historical_pressure_gain_percent")]=historical;
    const double rosterGain=rosterHistoryComplete?100*rosterScenarios.value(QStringLiteral("score_gain")).toDouble():historical;
    if(rosterHistoryComplete)row[QStringLiteral("historical_roster_gain_percent")]=rosterGain;
    if(scenarioSummary.value(QStringLiteral("complete")).toBool())
        row[QStringLiteral("decision_gain_percent")]=100*scenarioSummary.value(QStringLiteral("score_gain")).toDouble();
    row[QStringLiteral("decision_score_basis")]=QStringLiteral("multi_target_multi_window_robust_conditional_gain");
    row[QStringLiteral("decision_scope")]=QJsonObject{{QStringLiteral("windows"),QJsonArray{3,8,20}},{QStringLiteral("target_count"),5},
        {QStringLiteral("history_known_players"),historyKnown},{QStringLiteral("historical_pressure_included"),historyComplete},
        {QStringLiteral("historical_roster_included"),rosterHistoryComplete},{QStringLiteral("selected_historical_scenario"),rosterScenarios.value(QStringLiteral("selected_scenario"))},
        {QStringLiteral("historical_pressure_observational_only"),true},{QStringLiteral("aggregation"),QStringLiteral("equal_window_mean_then_minimum_available_pressure_scenarios")},
        {QStringLiteral("conditional_known_numeric_effects_only"),true},{QStringLiteral("absolute_optimum"),false},
        {QStringLiteral("initial_skills_ready_hypothesis"),true},{QStringLiteral("full_live_combat_model"),false}};
    return true;
}

void evaluationStatus(QJsonObject &row,const QString &status,const QString &code,const QString &reason,double seconds=8) {
    row[QStringLiteral("evaluation_status")]=status;
    row[QStringLiteral("evaluation_reason_code")]=code;
    row[QStringLiteral("evaluation_reason")]=reason;
    row[QStringLiteral("evaluation_scope")]=QJsonObject{{QStringLiteral("seconds"),seconds},
        {QStringLiteral("conditional"),true},{QStringLiteral("known_effects_only"),true},
        {QStringLiteral("initial_skills_ready_hypothesis"),true},{QStringLiteral("full_live_combat_model"),false}};
    row[QStringLiteral("evaluation_numeric_available")]=row.value(QStringLiteral("available")).toBool();
    row[QStringLiteral("evaluation_complete")]=false;
}

QString triggerLabel(const QString &event) {
    // Human labels translate source event names; they do not assert that an
    // event occurred or infer a hit count from ordinary attacks.
    if(event==QStringLiteral("summoner_spin_hit"))return QStringLiteral("摆荡命中");
    if(event==QStringLiteral("snowball_hit"))return QStringLiteral("雪球命中");
    if(event==QStringLiteral("death"))return QStringLiteral("死亡");
    return QStringLiteral("实际触发");
}

void missingFormulaInputs(const QJsonValue &value,const CombatContext &ctx,QStringList &missing) {
    if(value.isArray()){for(const auto &v:value.toArray())missingFormulaInputs(v,ctx,missing);return;}
    if(!value.isObject())return;
    const auto object=value.toObject();const auto op=object.value(QStringLiteral("op")).toString();
    if(op==QStringLiteral("ranged")){missingFormulaInputs(object.value(ctx.ranged?QStringLiteral("yes"):QStringLiteral("no")),ctx,missing);return;}
    if(op==QStringLiteral("context")||op==QStringLiteral("source_rank")){
        const auto key=object.value(QStringLiteral("key")).toString();
        if(!key.isEmpty()&&key!=QStringLiteral("target_hp")&&!ctx.counters.contains(key))missing.append(key);
    }
    if(op==QStringLiteral("health")&&object.value(QStringLiteral("measure")).toString()!=QStringLiteral("max")){
        const auto subject=object.value(QStringLiteral("subject")).toString();
        if((subject==QStringLiteral("self")||subject==QStringLiteral("target"))
            &&!ctx.counters.contains(subject+QStringLiteral("_health_fraction"))&&!ctx.counters.contains(subject+QStringLiteral("_current_hp")))
            missing.append(subject+QStringLiteral("_health_fraction_or_current_hp"));
    }
    for(auto it=object.constBegin();it!=object.constEnd();++it)missingFormulaInputs(it.value(),ctx,missing);
}

void annotateEvaluatedOffer(QJsonObject &row,const CombatEntry &entry,const CombatContext &ctx,
                            const CombatEvaluation &base,const CombatEvaluation &value,const QStringList &added) {
    const auto event=entry.triggerContract.value(QStringLiteral("event")).toString();
    row[QStringLiteral("evaluation_trigger")]=QJsonObject{{QStringLiteral("event"),event},
        {QStringLiteral("requires"),entry.triggerContract.value(QStringLiteral("requires")).toArray()}};
    row[QStringLiteral("evaluation_required_inputs")]=entry.triggerContract.value(QStringLiteral("requires")).toArray();
    row[QStringLiteral("evaluation_event_actions_consumed")]=false;
    row[QStringLiteral("evaluation_coverage")]=entry.coverage;
    row[QStringLiteral("evaluation_numeric_effect_count")]=0;
    QJsonObject statChanges;const auto before=base.stats.json(),after=value.stats.json();
    for(auto it=after.constBegin();it!=after.constEnd();++it)if(it.value().isDouble()&&before.value(it.key()).isDouble()){
        const double delta=it.value().toDouble()-before.value(it.key()).toDouble();
        if(std::isfinite(delta)&&std::abs(delta)>1e-9)statChanges[it.key()]=delta;
    }
    row[QStringLiteral("evaluation_stat_changes")]=statChanges;
    QStringList missing,formulaMissing;bool eventMissing=false,conditionMissing=false,invalidInput=false,unsupported=false;
    int numericEffects=0;
    // These operations are the executable op names consumed by combat.cpp.
    // Event-ledger actions are intentionally not treated as numeric effects by
    // this snapshot scorer (for example summoner-spell damage actions).
    const QSet<QString> numericOps{QStringLiteral("add"),QStringLiteral("transform"),QStringLiteral("convert"),QStringLiteral("amp"),QStringLiteral("bonus_amp"),
        QStringLiteral("penetration"),QStringLiteral("resist"),QStringLiteral("range"),QStringLiteral("derived"),QStringLiteral("magic_damage_amp"),QStringLiteral("damage_type_amp"),
        QStringLiteral("flag"),QStringLiteral("extra_true"),QStringLiteral("speed_damage"),QStringLiteral("spell_crit"),QStringLiteral("crit_defense"),
        QStringLiteral("observed_damage"),QStringLiteral("observed_heal"),QStringLiteral("shield"),QStringLiteral("onhit"),QStringLiteral("current_hp_onhit"),
        QStringLiteral("nth_onhit"),QStringLiteral("burn_onhit"),QStringLiteral("proc"),QStringLiteral("spellblade"),QStringLiteral("spell_percent_hp"),QStringLiteral("stack")};
    for(const auto &v:entry.effects){const auto effect=v.toObject();const auto op=effect.value(QStringLiteral("op")).toString();
        if(op==QStringLiteral("mechanic"))continue;
        if(!numericOps.contains(op)){unsupported=true;continue;}
        ++numericEffects;
        for(const auto &field:{QStringLiteral("value"),QStringLiteral("cooldown"),QStringLiteral("ap_ratio"),QStringLiteral("cap"),QStringLiteral("ranged_value"),QStringLiteral("duration"),QStringLiteral("count")}){
            auto formula=effect.value(field);const auto text=formula.toString();
            if(text.startsWith(QStringLiteral("calc:")))formula=entry.calculations.value(text.mid(5));
            missingFormulaInputs(formula,ctx,formulaMissing);
        }
    }
    for(const auto &code:added){
        if(code.startsWith(QStringLiteral("unobserved_event:"))){eventMissing=true;missing.append(code.mid(17));}
        else if(code.startsWith(QStringLiteral("condition:"))){conditionMissing=true;missing.append(code);}
        else if(code.startsWith(QStringLiteral("unobserved_stack:"))){missing.append(code.mid(17));}
        else if(code.startsWith(QStringLiteral("invalid_event:"))){invalidInput=true;missing.append(code.mid(14));}
        else if(code.startsWith(QStringLiteral("target_current_hp:"))||code.startsWith(QStringLiteral("headliner:"))||code.endsWith(QStringLiteral(":activation_unverified"))){missing.append(code);}
        else if(code.startsWith(QStringLiteral("effect_value:"))||code.startsWith(QStringLiteral("effect_parameter:"))
            ||code.startsWith(QStringLiteral("effect_damage_type:"))||code.startsWith(QStringLiteral("unsupported_stat:")))unsupported=true;
    }
    if(unsupported)missing.append(formulaMissing);
    missing.removeDuplicates();
    row[QStringLiteral("evaluation_numeric_effect_count")]=numericEffects;
    row[QStringLiteral("evaluation_missing_inputs")]=QJsonArray::fromStringList(missing);
    const bool partial=entry.coverage!=QStringLiteral("modeled_numeric_effects");
    row[QStringLiteral("evaluation_model_gap")]=partial||numericEffects==0||unsupported||event==QStringLiteral("unresolved_semantic_mechanic");
    if(numericEffects==0||event==QStringLiteral("unresolved_semantic_mechanic")){
        const bool ledgerOnly=!entry.actions.isEmpty()||!entry.eventRules.isEmpty();
        evaluationStatus(row,QStringLiteral("effect_unmodeled"),ledgerOnly?QStringLiteral("event_actions_not_consumed_by_snapshot_score"):QStringLiteral("no_executable_numeric_effects"),
            ledgerOnly?QStringLiteral("%1的事件机制尚未接入当前短窗评分").arg(triggerLabel(event)):QStringLiteral("该强化的机制尚未接入数值评分"),ctx.horizon);
    } else if(eventMissing||conditionMissing){
        evaluationStatus(row,QStringLiteral("trigger_unobserved"),eventMissing?QStringLiteral("actual_event_count_missing"):QStringLiteral("activation_condition_unobserved"),
            eventMissing?QStringLiteral("缺少%1数据，未假设触发次数").arg(triggerLabel(event)):QStringLiteral("缺少实际触发条件，未假设效果生效"),ctx.horizon);
    } else if(!missing.isEmpty()||invalidInput){
        evaluationStatus(row,QStringLiteral("input_missing"),invalidInput?QStringLiteral("invalid_observed_input"):QStringLiteral("required_effect_state_missing"),
            invalidInput?QStringLiteral("触发数据无效，暂不计入该部分收益"):QStringLiteral("缺少层数、状态或局内参数，仅保留可算部分"),ctx.horizon);
    } else if(unsupported){
        evaluationStatus(row,QStringLiteral("effect_unmodeled"),QStringLiteral("numeric_effect_formula_not_supported"),QStringLiteral("部分效果公式尚未接入，当前结果仅含可算部分"),ctx.horizon);
    } else {
        const bool zero=std::abs(row.value(QStringLiteral("current_gain_percent")).toDouble())<=1e-9;
        evaluationStatus(row,zero?QStringLiteral("current_window_zero"):QStringLiteral("computed"),zero?QStringLiteral("known_effects_no_score_change_in_window"):QStringLiteral("known_numeric_effects_evaluated"),
            zero?(partial?QStringLiteral("%1秒内已建模收益无变化；其余机制未覆盖").arg(ctx.horizon):QStringLiteral("%1秒内已建模收益无变化，不代表强化无效").arg(ctx.horizon))
                :(partial?QStringLiteral("已计算可建模部分；其余机制未覆盖"):QStringLiteral("已计算当前条件下的数值收益")),ctx.horizon);
        row[QStringLiteral("evaluation_complete")]=!partial&&added.isEmpty();
    }
}


}

QJsonObject conditionalRerollExpectation(QVector<double> pool,double current) {
    QJsonObject out{{QStringLiteral("available"),false},{QStringLiteral("live_probability"),false},{QStringLiteral("uniform_known_pool"),true},
        {QStringLiteral("scope"),QStringLiteral("hypothetical_uniform_resource_pool")},{QStringLiteral("not_live_drop_rate"),true}};
    if(pool.isEmpty()||!std::isfinite(current)||std::any_of(pool.begin(),pool.end(),[](double v){return !std::isfinite(v);}))return out;
    std::sort(pool.begin(),pool.end());double sum=0;int better=0;
    for(double v:pool){sum+=v;if(v>current+1e-9)++better;}
    const double mean=sum/pool.size();
    out[QStringLiteral("available")]=true;out[QStringLiteral("pool_size")]=pool.size();out[QStringLiteral("mean")]=mean;out[QStringLiteral("expected_gain")]=mean-current;
    out[QStringLiteral("better_probability")]=double(better)/pool.size();
    // Distribution of the best of three distinct equal-weight draws. With
    // sorted scores the kth score is the maximum in C(k,2) triples.
    if(pool.size()>=3){double best=0;const double n=double(pool.size()),den=n*(n-1)*(n-2)/6.;
        for(qsizetype i=2;i<pool.size();++i)best+=pool[i]*(double(i)*(i-1)/2.)/den;
        out[QStringLiteral("best_of_three_mean")]=best;out[QStringLiteral("best_of_three_gain")]=best-current;}
    return out;
}

QJsonObject singleSlotRerollTradeoff(const QVector<double> &scores,int slot) {
    QJsonObject out{{QStringLiteral("available"),false},{QStringLiteral("live_probability"),false},
        {QStringLiteral("scope"),QStringLiteral("one_slot_if_other_two_and_score_basis_unchanged")}};
    const auto finite=[](double value){return std::isfinite(value);};
    if(scores.size()!=3||slot<0||slot>=3||!std::all_of(scores.cbegin(),scores.cend(),finite))return out;

    const double kept=std::max(scores[(slot+1)%3],scores[(slot+2)%3]);
    const double current=std::max(kept,scores[slot]);
    const double floor=kept-current;
    if(!finite(floor))return out;
    out[QStringLiteral("available")]=true;
    out[QStringLiteral("retained_best")]=kept;
    out[QStringLiteral("current_best")]=current;
    out[QStringLiteral("floor_delta")]=floor;
    out[QStringLiteral("preserves_current_best")]=kept==current;
    return out;
}

QString observedRerollRule(const GameState &s) {
    const auto rule=observedHeadliner(s).value(QStringLiteral("rule_key")).toString();
    if(rule.isEmpty())return QStringLiteral("unknown");
    if(rule==QStringLiteral("CommandLine"))return QStringLiteral("special_upgrade_possible");
    return QStringLiteral("ordinary_single_slot");
}

QJsonObject coachTeamNeeds(const Catalog &c,const GameState &s) {
    QString team;for(const auto &v:s.players)if(v.toObject().value(QStringLiteral("self")).toBool())team=v.toObject().value(QStringLiteral("team")).toString();
    int known=0,unknown=0;
    if(!team.isEmpty())for(const auto &v:s.players){const auto player=v.toObject();if(player.value(QStringLiteral("team")).toString()!=team)continue;
        if(c.champions.contains(player.value(QStringLiteral("champion_id")).toInt()))++known;else ++unknown;}
    return {{QStringLiteral("available"),false},{QStringLiteral("directions_disabled"),true},{QStringLiteral("role_gap_matching_enabled"),false},
        {QStringLiteral("known_allies"),known},{QStringLiteral("unknown_allies"),unknown},{QStringLiteral("axes"),QJsonArray{}},
        {QStringLiteral("tactical_context"),analyzeTacticalContext(c,s)},
        {QStringLiteral("method"),QStringLiteral("factual_tactical_context_without_role_gap_score")},
        {QStringLiteral("scope"),QStringLiteral("保留已核对的技能与阵容条件；不按固定角色缺口打分，也不生成阵容星级。")}};
}
QJsonObject buildAugmentDecision(const Catalog &c,const GameState &s,const QJsonObject &plan,const QString &style,const QSet<int> &seen,bool currentOffersOnly,const RecommendationWorkPtr &work) {
    Q_UNUSED(style);const auto growthStyle=QStringLiteral("成长联动");
    QElapsedTimer elapsed;elapsed.start();
    const auto reuse=std::make_shared<CombatSandboxReuse>(c,s);QJsonArray references;
    QHash<int,QJsonObject> offeredHistory;
    QJsonObject out{{QStringLiteral("available"),false},{QStringLiteral("live_roll_probability"),false},{QStringLiteral("patch"),c.patch},
        {QStringLiteral("directions_disabled"),true},{QStringLiteral("role_matching_enabled"),false},{QStringLiteral("status"),QStringLiteral("unavailable")}};
    const auto stopped=[&]{return work&&work->stopped();};
    const auto finish=[&](bool complete=true){
        out[QStringLiteral("rule_basis")]=modelRuleBasis(c,s);
        const bool interrupted=!complete||stopped();
        if(interrupted){
            // Rebuild from pre-evaluation references, never from a completed
            // candidate prefix. Even legacy eight-second fallback must not
            // expose a numerical winner from a stopped round.
            QJsonArray rows;
            for(int id:s.offers){QJsonObject row{{QStringLiteral("augment_id"),id}};
                for(const auto &reference:references)if(reference.toObject().value(QStringLiteral("augment_id")).toInt()==id){row=reference.toObject();break;}
                row[QStringLiteral("available")]=false;
                evaluationStatus(row,QStringLiteral("unavailable"),QStringLiteral("comparison_work_interrupted"),QStringLiteral("本轮计算已停止，等待刷新"));
                for(const auto &field:{QStringLiteral("ranking_eligible"),QStringLiteral("marginal_comparable"),QStringLiteral("decision_comparable"),
                    QStringLiteral("decision_marginal_comparable"),QStringLiteral("decision_numeric_available"),QStringLiteral("decision_fallback")})row[field]=false;
                rows.append(row);
            }
            out[QStringLiteral("offers")]=rows;out[QStringLiteral("available")]=false;
            out[QStringLiteral("status")]=QStringLiteral("interrupted");
            const auto reason=work?work->stopReason():QString();out[QStringLiteral("stop_reason")]=reason.isEmpty()?QStringLiteral("cancelled"):reason;
            out[QStringLiteral("ranking_available")]=false;out[QStringLiteral("selected_augment_id")]=0;
            auto quality=out.value(QStringLiteral("ranking_quality")).toObject();quality[QStringLiteral("ranking_allowed")]=false;
            auto reasons=quality.value(QStringLiteral("reasons")).toArray();reasons.append(QJsonObject{{QStringLiteral("code"),QStringLiteral("comparison_work_interrupted")}});
            quality[QStringLiteral("reasons")]=reasons;quality[QStringLiteral("summary")]=QStringLiteral("本轮计算未完成，暂不排序");out[QStringLiteral("ranking_quality")]=quality;
            out.remove(QStringLiteral("baseline_score"));
        }
        if(!offeredHistory.isEmpty()){
            auto rows=out.value(QStringLiteral("offers")).toArray();
            for(qsizetype index=0;index<rows.size();++index){
                auto row=rows.at(index).toObject();
                const auto history=offeredHistory.constFind(row.value(QStringLiteral("augment_id")).toInt());
                if(history==offeredHistory.cend())continue;
                row[QStringLiteral("historical_loadout_reference")]=history.value();
                rows[index]=row;
            }
            out[QStringLiteral("offers")]=rows;
        }
        out[QStringLiteral("computation_complete")]=!interrupted;
        QJsonArray scenarioCandidates;
        for(const auto &v:out.value(QStringLiteral("offers")).toArray()){
            const auto row=v.toObject();scenarioCandidates.append(QJsonObject{
                {QStringLiteral("candidate_id"),row.value(QStringLiteral("augment_id"))},
                {QStringLiteral("comparison_eligible"),!interrupted&&out.value(QStringLiteral("ranking_quality")).toObject().value(QStringLiteral("ranking_allowed")).toBool()
                    &&row.value(QStringLiteral("decision_comparable")).toBool()},
                {QStringLiteral("scenario_summary"),row.value(QStringLiteral("decision_scenarios"))}});
        }
        out[QStringLiteral("scenario_comparison")]=compareCombatScenarioCandidates(scenarioCandidates);
        out[QStringLiteral("elapsed_ms")]=double(elapsed.nsecsElapsed())/1e6;
        out[QStringLiteral("sandbox_reuse")]=reuse->stats();
        out[QStringLiteral("work_control")]=QJsonObject{{QStringLiteral("enabled"),bool(work)},{QStringLiteral("interrupted"),interrupted},
            {QStringLiteral("job_elapsed_ms"),work?QJsonValue(double(work->elapsedMs())):QJsonValue(QJsonValue::Null)},
            {QStringLiteral("remaining_ms"),work?QJsonValue(double(work->remainingMs())):QJsonValue(QJsonValue::Null)},
            {QStringLiteral("scope"),QStringLiteral("one_offer_round_cooperative_between_finite_calls_and_inside_multiwindow_sandbox")}};
        return out;
    };
    if(stopped())return finish(false);
    for(const auto &key:{QStringLiteral("growth_reasoning"),QStringLiteral("growth_objective")})if(plan.value(key).isObject())out[key]=plan.value(key);
    out[QStringLiteral("ranking_quality")]=augmentRankingQuality(c,s);
    if(stopped())return finish(false);
    out[QStringLiteral("recommendation_evidence")]=recommendationEvidenceSummary(s);
    out[QStringLiteral("external_strategy_reference")]=externalStrategyReference(c,s);
    QJsonObject batchedReferences;
    out[QStringLiteral("opening_reference_prior")]=buildAugmentAdvisory(c,s,&batchedReferences).value(QStringLiteral("opening_reference_prior"));
    if(stopped())return finish(false);
    if(s.offers.isEmpty())return finish();
    offeredHistory=LoadoutOutcomeReference::offeredPairs(c,s);
    if(batchedReferences.isEmpty()&&std::all_of(s.offers.cbegin(),s.offers.cend(),[](int id){return id>0;}))
        batchedReferences=externalAugmentStrategyReferences(c,s,s.offers);
    const auto offerReference=[&](int id){
        const auto key=QString::number(id);
        return batchedReferences.contains(key)?batchedReferences.value(key).toObject():externalStrategyReference(c,s,{},QSet<int>{id},id,true);
    };
    auto unavailableOffers=[&](const QString &status,const QString &code,const QString &reason){
        QJsonArray rows;
        const auto prior=out.value(QStringLiteral("offers")).toArray();
        for(int id:s.offers){QJsonObject row{{QStringLiteral("augment_id"),id},{QStringLiteral("available"),false}};
            for(const auto &v:prior)if(v.toObject().value(QStringLiteral("augment_id")).toInt()==id){row=v.toObject();break;}
            const auto previous=row.value(QStringLiteral("evaluation_status")).toString();
            if(!row.contains(QStringLiteral("external_strategy_reference")))row[QStringLiteral("external_strategy_reference")]=offerReference(id);
            if(previous.isEmpty()||row.value(QStringLiteral("evaluation_reason_code"))==QStringLiteral("evaluation_not_completed"))evaluationStatus(row,status,code,reason);
            rows.append(row);
        }
        out[QStringLiteral("offers")]=rows;out[QStringLiteral("reason")]=reason;
    };
    if(!c.combat){unavailableOffers(QStringLiteral("unavailable"),QStringLiteral("combat_database_missing"),QStringLiteral("缺少当前版本数值规则"));return finish();}
    if(!s.fresh()){unavailableOffers(QStringLiteral("input_missing"),QStringLiteral("live_state_not_fresh"),QStringLiteral("等待新鲜局内数据"));return finish();}
    if(!modelRulesUsable(c,s)){unavailableOffers(QStringLiteral("unavailable"),QStringLiteral("model_patch_mismatch"),QStringLiteral("局内版本与数值规则未匹配"));return finish();}
    if(s.gameMode!=QStringLiteral("KIWI")){unavailableOffers(QStringLiteral("unavailable"),QStringLiteral("unsupported_game_mode"),QStringLiteral("当前模式未使用海克斯大乱斗规则"));return finish();}
    if(!c.combat->champions.contains(s.championId)){unavailableOffers(QStringLiteral("unavailable"),QStringLiteral("champion_model_missing"),QStringLiteral("缺少当前英雄的同版数值模型"));return finish();}
    if(s.level<1||s.level>18){unavailableOffers(QStringLiteral("unavailable"),QStringLiteral("champion_growth_level_unsupported"),QStringLiteral("当前等级的英雄成长规则尚未核实"));return finish();}
    const auto selection=out.value(QStringLiteral("ranking_quality")).toObject().value(QStringLiteral("augment_observation")).toObject();
    if(currentOffersOnly&&s.source==GameState::Source::Live&&(!selection.value(QStringLiteral("complete")).toBool()||!selection.value(QStringLiteral("model_selection_matches_observation")).toBool())){
        unavailableOffers(QStringLiteral("input_missing"),QStringLiteral("selected_augments_unconfirmed"),QStringLiteral("已选强化尚未确认；先显示原卡条件，不把未知当作未选"));
        out[QStringLiteral("numeric_calculation_skipped")]=true;
        out[QStringLiteral("calculation_policy")]=QStringLiteral("source_conditions_until_selected_augments_are_confirmed");return finish();
    }
    const auto &db=*c.combat;auto ctx=db.context(s.championId,s.level);const auto selected=effectiveSelectedAugments(c,s);
    ctx.discreteSkillCasts=true;
    out[QStringLiteral("timing_policy")]=QStringLiteral("integer_recasts; all_skills_initially_ready_hypothesis; actual_cooldowns_unknown");
    out[QStringLiteral("target_context")]=applyOpponentLoadoutContext(c,s,ctx);
    if(stopped())return finish(false);
    const auto pref=resolvedCombatPreference(c,s,growthStyle);
    out[QStringLiteral("resolved_preference")]=pref;
    out[QStringLiteral("headliner")]=evaluateHeadliner(c,s);
    if(stopped())return finish(false);
    for(int id:s.offers){QJsonObject row{{QStringLiteral("augment_id"),id},{QStringLiteral("available"),false}};
        if(stopped())return finish(false);
        row[QStringLiteral("external_strategy_reference")]=offerReference(id);
        evaluationStatus(row,QStringLiteral("unavailable"),QStringLiteral("evaluation_not_completed"),QStringLiteral("当前条件计算尚未完成"));
        row[QStringLiteral("ordinary_offer_eligible")]=c.ordinaryOfferEligible(id);
        if(!c.ordinaryOfferEligible(id)){
            row[QStringLiteral("reason")]=c.offerEligibilityReason(id);
            evaluationStatus(row,QStringLiteral("not_eligible"),c.offerEligibilityReason(id),QStringLiteral("当前规则未确认该强化可作为普通候选"));
        } else if(!db.augments.contains(id))evaluationStatus(row,QStringLiteral("unavailable"),QStringLiteral("augment_model_missing"),QStringLiteral("缺少该强化的同版数值规则"));
        else if(selected.contains(id))evaluationStatus(row,QStringLiteral("not_eligible"),QStringLiteral("augment_already_selected"),QStringLiteral("该强化已持有，不作为新增候选比较"));
        else if(db.augments[id].requiredLevel>s.level){
            row[QStringLiteral("evaluation_required_level")]=db.augments[id].requiredLevel;
            evaluationStatus(row,QStringLiteral("not_eligible"),QStringLiteral("augment_level_requirement_not_met"),QStringLiteral("当前等级未满足该强化的规则限制"));
        }
        row[QStringLiteral("needs_current_growth_inputs")]=true;
        if(c.ordinaryOfferEligible(id)&&db.augments.contains(id)&&!selected.contains(id)&&db.augments[id].requiredLevel<=s.level
            &&db.augments[id].triggerContract.value(QStringLiteral("event"))==QStringLiteral("death"))row[QStringLiteral("conditional_risk")]=QStringLiteral("on_death");
        references.append(row);
    }
    out[QStringLiteral("offers")]=references;
    if(stopped())return finish(false);
    ctx.conditions=s.effectConditions;ctx.counters=s.effectCounters;
    const auto health=combatHealthContract(c,s);out[QStringLiteral("health_basis")]=health;
    const auto skillRanks=selfSkillRankContext(db,s);out[QStringLiteral("skill_rank_observation")]=skillRanks.report;
    if(!skillRanks.available){unavailableOffers(QStringLiteral("input_missing"),QStringLiteral("skill_ranks_invalid"),QStringLiteral("当前技能等级不合法，等待刷新"));return finish();}
    const int missingRanks=skillRanks.report.value(QStringLiteral("missing_slots")).toArray().size();
    if(missingRanks){unavailableOffers(QStringLiteral("input_missing"),QStringLiteral("skill_ranks_missing"),QStringLiteral("等待当前技能等级"));return finish();}
    ctx.ranks=skillRanks.context.ranks;
    if(stopped())return finish(false);
    auto base=evaluateCombat(db,ctx,s.owned,selected,false,pref);
    if(stopped())return finish(false);
    if(!base.valid){unavailableOffers(QStringLiteral("unavailable"),QStringLiteral("baseline_evaluation_invalid"),QStringLiteral("当前基础组合暂不可算"));return finish();}
    out[QStringLiteral("observed_anchor")]=anchorObservedCombatStats(db,ctx,s.owned,selected,s.liveStats,pref);
    if(stopped())return finish(false);
    bindGrowthObjective(db,ctx,s.owned,selected);
    if(stopped())return finish(false);
    base=evaluateCombat(db,ctx,s.owned,selected,false,pref);
    if(stopped())return finish(false);
    if(!base.valid||base.score<=0){unavailableOffers(QStringLiteral("unavailable"),QStringLiteral("baseline_score_unavailable"),QStringLiteral("当前基础组合缺少可比较的评分"));return finish();}
    out[QStringLiteral("growth_reasoning")]=ctx.growthObjectiveBasis;out[QStringLiteral("growth_objective")]=base.growthObjective;
    const auto teammates=evaluateTeammateContext(c,s,ctx,pref);
    if(stopped())return finish(false);
    out[QStringLiteral("team_context")]=teammates.report;
    out[QStringLiteral("current_gain_objective")]=teammates.available?QStringLiteral("marginal_team_output_and_durability"):QStringLiteral("individual_model_incomplete_team_fallback");
    auto baselineCodes=base.unknowns;
    if(teammates.available)for(const auto &u:teammates.report.value(QStringLiteral("unknowns")).toArray())baselineCodes.append(u.toString());
    baselineCodes.removeDuplicates();
    const QSet<QString> baselineUnknowns(baselineCodes.cbegin(),baselineCodes.cend());
    out[QStringLiteral("baseline_unknowns")]=QJsonArray::fromStringList(baselineCodes);
    auto evaluate=[&](int id,const QList<int> &selected,const CombatContext &context){auto ids=selected;if(!ids.contains(id))ids.append(id);return evaluateCombat(db,context,s.owned,ids,false,pref);};
    QSet<int> excluded=seen;for(int id:selected)excluded.insert(id);for(int id:s.offers)excluded.insert(id);
    QMap<int,CombatEvaluation> values;
    const auto evaluationIds=currentOffersOnly?s.offers:c.augments.keys();
    for(int id:evaluationIds){
        if(stopped())return finish(false);
        const auto entry=db.augments.constFind(id);
        if(entry!=db.augments.cend()&&c.ordinaryOfferEligible(id)&&!selected.contains(id)&&!values.contains(id)&&entry->requiredLevel<=s.level)
            values[id]=evaluate(id,selected,ctx);
        if(stopped())return finish(false);
    }
    const auto team=coachTeamNeeds(c,s);
    CombatEvaluation without;if(!currentOffersOnly)without=evaluateCombat(db,ctx,s.owned,{},false,pref);
    if(stopped())return finish(false);
    QMap<int,QJsonObject> adapted;for(const auto &v:plan.value(QStringLiteral("offer_comparisons")).toArray())adapted[v.toObject().value(QStringLiteral("augment_id")).toInt()]=v.toObject();
    const double plannedBase=plan.value(QStringLiteral("comparison")).toObject().value(QStringLiteral("comparison_score")).toDouble();
    QJsonArray rows;
    for(int id:s.offers){QJsonObject row;
        if(stopped())return finish(false);
        for(const auto &ref:references)if(ref.toObject().value(QStringLiteral("augment_id")).toInt()==id){row=ref.toObject();break;}
        row[QStringLiteral("selected")]=selected.contains(id);
        if(!c.ordinaryOfferEligible(id)){rows.append(row);continue;}
        const auto value=values.value(id);if(!value.valid){row[QStringLiteral("reason")]=QStringLiteral("已持有、资格未知或组合暂不支持");
            if(row.value(QStringLiteral("evaluation_reason_code"))==QStringLiteral("evaluation_not_completed"))evaluationStatus(row,QStringLiteral("unavailable"),QStringLiteral("candidate_evaluation_invalid"),QStringLiteral("该候选与当前组合暂不可算"));
            rows.append(row);continue;}
        const double personalCurrent=gain(value.score,base.score);
        const auto teamGain=compareTeamObjective(teammates,base,value,pref);
        const double current=teamGain.value(QStringLiteral("available")).toBool()?100*teamGain.value(QStringLiteral("score_gain")).toDouble():personalCurrent;
        row[QStringLiteral("available")]=true;row[QStringLiteral("current_gain_percent")]=current;
        row[QStringLiteral("needs_current_growth_inputs")]=false;
        row[QStringLiteral("individual_gain_percent")]=personalCurrent;row[QStringLiteral("team_objective")]=teamGain;
        row[QStringLiteral("current_gain_objective")]=out.value(QStringLiteral("current_gain_objective"));
        row[QStringLiteral("missing_skill_ranks")]=missingRanks;
        QStringList added;for(const auto &unknown:value.unknowns)if(!baselineUnknowns.contains(unknown))added.append(unknown);
        const auto headlinerUnknowns=headlinerComparisonUnknowns(s,id);for(const auto &unknown:headlinerUnknowns)if(!added.contains(unknown))added.append(unknown);
        QSet<QString> allUnknowns(value.unknowns.cbegin(),value.unknowns.cend());for(const auto &unknown:headlinerUnknowns)allUnknowns.insert(unknown);
        if(teammates.available)for(const auto &u:teammates.report.value(QStringLiteral("unknowns")).toArray())allUnknowns.insert(u.toString());
        QSet<QString> blockingUnknowns;
        for(const auto &unknown:combatHealthUnknowns(health))blockingUnknowns.insert(unknown);
        for(const auto &unknown:baselineUnknowns)if(combatUnknownBlocksComparison(unknown))blockingUnknowns.insert(unknown);
        for(const auto &unknown:allUnknowns)if(combatUnknownBlocksComparison(unknown))blockingUnknowns.insert(unknown);
        auto blockingCodes=blockingUnknowns.values();blockingCodes.sort();
        row[QStringLiteral("unknown_count")]=allUnknowns.size();
        row[QStringLiteral("baseline_unknown_count")]=baselineUnknowns.size();row[QStringLiteral("additional_unknowns")]=QJsonArray::fromStringList(added);
        row[QStringLiteral("additional_unknown_count")]=added.size();row[QStringLiteral("marginal_comparable")]=added.isEmpty()&&blockingUnknowns.isEmpty();
        row[QStringLiteral("marginal_blocking_unknowns")]=QJsonArray::fromStringList(blockingCodes);
        row[QStringLiteral("marginal_blocking_unknown_count")]=blockingUnknowns.size();
        row[QStringLiteral("health_basis")]=health;
        const auto boundary=combatComparisonBoundary(blockingCodes,value.valid&&base.valid);
        row[QStringLiteral("comparison_boundary")]=boundary;
        row[QStringLiteral("critical_unknowns")]=boundary.value(QStringLiteral("critical_unknowns"));
        row[QStringLiteral("ranking_eligible")]=row.value(QStringLiteral("marginal_comparable"));
        row[QStringLiteral("diagnostic_only")]=!row.value(QStringLiteral("marginal_comparable")).toBool();
        row[QStringLiteral("comparison_scope")]=QStringLiteral("known_model_only_shared_unknowns_not_assumed_to_cancel");
        // Reuse these exact evaluations; these axes are model changes, not
        // observed damage or a second recommendation/ranking calculation.
        const double baseOutput=base.attackDamage+base.skillDamage+base.procDamage;
        const double nextOutput=value.attackDamage+value.skillDamage+value.procDamage;
        if(std::isfinite(baseOutput)&&baseOutput>0&&std::isfinite(nextOutput))row[QStringLiteral("output_gain_percent")]=gain(nextOutput,baseOutput);
        if(std::isfinite(base.survival)&&base.survival>0&&std::isfinite(value.survival))row[QStringLiteral("effective_health_gain_percent")]=gain(value.survival,base.survival);
        row[QStringLiteral("axis_gain_semantics")]=QStringLiteral("same_snapshot_model_estimate");
        annotateEvaluatedOffer(row,db.augments[id],ctx,base,value,added);
        if(!annotateMultiWindowOffer(row,c,s,id,growthStyle,out.value(QStringLiteral("recommendation_evidence")).toObject().value(QStringLiteral("history_known_players")).toInt(),reuse,work)||stopped())return finish(false);
        if(currentOffersOnly){rows.append(row);continue;}
        const auto future=adapted.value(id).value(QStringLiteral("adapted_comparison")).toObject(),same=adapted.value(id).value(QStringLiteral("same_build_comparison")).toObject();
        if(plannedBase>0&&future.value(QStringLiteral("valid")).toBool())row[QStringLiteral("planned_gain_percent")]=gain(future.value(QStringLiteral("comparison_score")).toDouble(),plannedBase);
        if(plannedBase>0&&same.value(QStringLiteral("valid")).toBool())row[QStringLiteral("same_build_gain_percent")]=gain(same.value(QStringLiteral("comparison_score")).toDouble(),plannedBase);
        if(row.value(QStringLiteral("planned_gain_percent")).isDouble()&&row.value(QStringLiteral("same_build_gain_percent")).isDouble())row[QStringLiteral("adaptation_gain_points")]=row.value(QStringLiteral("planned_gain_percent")).toDouble()-row.value(QStringLiteral("same_build_gain_percent")).toDouble();
        if(without.valid&&without.score>0&&!selected.isEmpty()){const auto solo=evaluate(id,{},ctx);if(stopped())return finish(false);if(solo.valid)row[QStringLiteral("selected_synergy_points")]=personalCurrent-gain(solo.score,without.score);}
        if(s.level<18){auto later=ctx;const auto adult=db.context(s.championId,18);
            // Change champion growth only; the comparison keeps the same
            // opponent, health observation, window, ranks and observed state.
            later.level=adult.level;later.base=adult.base;later.baseAs=adult.baseAs;later.asRatio=adult.asRatio;later.ranged=adult.ranged;later.usesMana=adult.usesMana;
            const auto futureBase=evaluateCombat(db,later,s.owned,selected,false,pref);if(stopped())return finish(false);
            const auto futureValue=evaluate(id,selected,later);if(stopped())return finish(false);
            if(futureBase.valid&&futureBase.score>0&&futureValue.valid)row[QStringLiteral("level18_same_loadout_gain_percent")]=gain(futureValue.score,futureBase.score);}
        QVector<double> pool;int eligible=0,partial=0;
        const auto rarity=c.augments.value(id).value(QStringLiteral("rarity")).toString();
        const bool knownRarity=rarity==QStringLiteral("kSilver")||rarity==QStringLiteral("kGold")||rarity==QStringLiteral("kPrismatic");
        if(knownRarity)for(auto it=values.cbegin();it!=values.cend();++it){if(excluded.contains(it.key())||c.augments.value(it.key()).value(QStringLiteral("rarity")).toString()!=rarity)continue;
            if(stopped())return finish(false);
            ++eligible;if(!it.value().valid)continue;const auto hypothetical=compareTeamObjective(teammates,base,it.value(),pref);
            pool.append(hypothetical.value(QStringLiteral("available")).toBool()?100*hypothetical.value(QStringLiteral("score_gain")).toDouble():gain(it.value().score,base.score));if(!it.value().unknowns.isEmpty())++partial;}
        auto ev=conditionalRerollExpectation(pool,current);ev[QStringLiteral("eligible_catalog_entries")]=eligible;ev[QStringLiteral("partial_model_entries")]=partial;
        ev[QStringLiteral("excluded_seen_count")]=seen.size();ev[QStringLiteral("pool_eligibility_verified")]=false;
        ev[QStringLiteral("kind")]=QStringLiteral("conditional_uniform_local_catalog_example");ev[QStringLiteral("server_enabled_verified")]=false;
        if(!knownRarity)ev[QStringLiteral("reason")]=QStringLiteral("offer_rarity_unverified");
        ev[QStringLiteral("headliner_pool_known")]=false;row[QStringLiteral("reroll")]=ev;rows.append(row);
    }
    out[QStringLiteral("available")]=true;out[QStringLiteral("offers")]=rows;out[QStringLiteral("team_needs")]=team;
    out[QStringLiteral("baseline_score")]=base.score;out[QStringLiteral("missing_skill_ranks")]=missingRanks;
    out[QStringLiteral("assumptions")]=QJsonArray{QStringLiteral("收益为当前比较目标的相对变化，不是伤害百分比或胜率；比较窗口 %1 秒，目标 %2 生命、%3 护甲、%4 魔抗；目标来源见 target_context").arg(ctx.horizon).arg(ctx.targetHp).arg(ctx.targetArmor).arg(ctx.targetMr),
        QStringLiteral("当前收益在四名队友装备可建模时比较全队已知边际变化；所有候选共享同一成长基准、目标和窗口。改装、协同和18级对照仍为条件数值参考"),
        QStringLiteral("重随参考只在同品质目录内等权计算，排除已持有、当前候选和本局已见卡；等级、互斥、权重与本局池未完全确认，不能作为实测掉落率"),
        QStringLiteral("已停用固定方向匹配、角色相似度与阵容星级；无已建模数值不代表强化无作用")};
    auto assumptions=out.value(QStringLiteral("assumptions")).toArray();
    assumptions.append(QStringLiteral("decision_gain_percent 单独比较五名对手的 3/8/20 秒全队已知数值收益；已声明可用的历史情景均须完整，按各情景均值取较小值。历史统计不是当前伤害观测，也不是胜率或实战绝对最优"));
    out[QStringLiteral("assumptions")]=assumptions;
    out[QStringLiteral("status")]=QStringLiteral("completed");
    return finish();
}
