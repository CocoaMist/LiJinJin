#include "coach_recommendation_quality.h"
#include "decision_goal.h"
#include "analysis.h"
#include "combat.h"
#include "combat_unknown_policy.h"
#include "combat_observation_scope.h"
#include "role_guidance.h"
#include <QJsonArray>
#include <QSet>
#include <QStringList>
#include <algorithm>
#include <cmath>

namespace {
QString k(const char *s) { return QString::fromLatin1(s); }
bool integer(const QJsonValue &v, int minimum, int maximum) {
    return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() == std::floor(v.toDouble())
        && v.toDouble() >= minimum && v.toDouble() <= maximum;
}
QJsonArray ids(const QList<int> &values) {
    QJsonArray result; for (int value : values) result.append(value); return result;
}
void collectUnknowns(const QJsonObject &object, QSet<QString> &unknowns, QSet<QString> &critical) {
    const auto values = object.value(k("unknowns")).toArray();
    for (int i = 0; i < std::min(512, int(values.size())); ++i) {
        const auto value = values[i];
        if (value.isString() && !value.toString().isEmpty()) unknowns.insert(value.toString().left(256));
    }
    for(const auto &source:{object,object.value(k("comparison_boundary")).toObject()}){
        const auto explicitCritical=source.value(k("critical_unknowns")).toArray();
        for(int i=0;i<std::min(512,int(explicitCritical.size()));++i){const auto value=explicitCritical[i];
            if(value.isString()&&!value.toString().isEmpty()){critical.insert(value.toString().left(256));unknowns.insert(value.toString().left(256));}}
    }
}
QStringList sorted(const QSet<QString> &values) {
    auto result = values.values(); result.sort(); return result;
}
bool criticalUnknown(const QString &value) {
    return combatUnknownBlocksComparison(value);
}
QJsonObject augmentObservation(const Catalog &catalog, const GameState &state) {
    QJsonObject self; int count = 0;
    for (const auto &value : state.players) if (value.toObject().value(k("self")).toBool()) { self = value.toObject(); ++count; }
    const bool identity = count == 1 && integer(self.value(k("champion_id")), 1, 100000)
        && self.value(k("champion_id")).toInt() == state.championId;
    const auto raw = self.value(k("augment_ids")); QList<int> observed; QSet<int> seen;
    bool valid = identity && raw.isArray() && raw.toArray().size() <= 12;
    if (valid) for (const auto &value : raw.toArray()) {
        if (!integer(value, 1, 1000000) || seen.contains(value.toInt()) || !catalog.augments.contains(value.toInt())) { valid = false; break; }
        seen.insert(value.toInt()); observed.append(value.toInt());
    }
    if (!valid) observed.clear();
    const bool complete = valid && self.value(k("augments_complete")).isBool() && self.value(k("augments_complete")).toBool();
    auto recorded = state.selected; auto a = observed; auto b = recorded;
    std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
    const bool matches = complete && a == b;
    const auto status = !identity ? k("self_identity_unverified") : !valid ? k("invalid_or_missing_list")
        : complete ? k("complete") : observed.isEmpty() ? k("unknown") : k("partial");
    return {{k("source"), k("self_roster_augment_ids_and_augments_complete")}, {k("status"), status},
        {k("self_identity_verified"), identity}, {k("complete"), complete}, {k("observed_ids"), ids(observed)},
        {k("observed_count"), observed.size()}, {k("total_count"), complete ? QJsonValue(observed.size()) : QJsonValue(QJsonValue::Null)},
        {k("recorded_model_ids"), ids(recorded)}, {k("model_selection_matches_observation"), complete ? QJsonValue(matches) : QJsonValue(QJsonValue::Null)},
        {k("unknown_is_zero"), false}, {k("stats_residual_identifies_augments"), false}};
}
QJsonObject candidateActions(const GameState &state, const QJsonObject &sandbox) {
    QJsonArray available, save, unknown, budgetConflictIds; int skipped = 0, budgetConflicts = 0;
    const bool budget = state.goldKnown && std::isfinite(state.gold) && state.gold >= 0
        && sandbox.value(k("budget_known")).isBool() && sandbox.value(k("budget_known")).toBool();
    const auto values = sandbox.value(k("candidates")).toArray();
    for (int i = 0; i < std::min(64, int(values.size())); ++i) {
        const auto raw = values[i]; auto row = raw.toObject();
        if (!raw.isObject() || !integer(row.value(k("item_id")), 1, 1000000)
            || !integer(row.value(k("remaining_cost")), 1, 100000000)) { ++skipped; continue; }
        // Preserve source facts and order within each action. In particular a
        // larger modeled gain for an unaffordable item is not proof to wait.
        if (budget && row.value(k("affordable")).isBool()) {
            const bool reported = row.value(k("affordable")).toBool();
            if (reported != (row.value(k("remaining_cost")).toDouble() <= state.gold)) {
                // A cached label is not a current budget fact. Preserve the
                // calculation while withdrawing the contradictory action.
                ++budgetConflicts;budgetConflictIds.append(row.value(k("item_id")));row[k("reported_affordable")] = reported;
                row[k("affordable")] = QJsonValue(QJsonValue::Null);
                row[k("budget_status")] = k("reported_affordability_conflicts_with_current_gold");
                unknown.append(row);
            } else if (reported) available.append(row); else save.append(row);
        } else unknown.append(row);
    }
    return {{k("available_now"), available}, {k("save_for"), save}, {k("budget_unknown"), unknown},
        {k("budget_known"), budget}, {k("source"), k("existing_purchase_sandbox_candidates_only")},
        {k("scope"), k("reported_candidates_not_all_actions_or_a_future_purchase_forecast")},
        {k("unified_value_order_known"), false}, {k("future_affordability_predicted"), false}, {k("order_changed"), false},
        {k("candidate_rows_budget_conflicts"), budgetConflicts},
        {k("budget_conflict_item_ids"), budgetConflictIds},
        {k("candidate_rows_skipped"), skipped + std::max(0, int(values.size()) - 64)}};
}
void conditionAdvice(QJsonObject &plan, bool augmentClaimAllowed) {
    const auto advice = plan.value(k("advice")); if (!advice.isArray()) return;
    QJsonArray revised;
    for (const auto &value : advice.toArray()) {
        if (!value.isString()) { revised.append(value); continue; }
        const auto text = value.toString();
        if (text.contains(QStringLiteral("绝对最优")) || text.contains(QStringLiteral("唯一最优"))
            || text.contains(QStringLiteral("最优解")) || text.contains(QStringLiteral("必买")) || text.contains(QStringLiteral("必须购买")))
            revised.append(QStringLiteral("当前结果只是在已建模条件下比较的候选，不代表实战最优或唯一购买方案。"));
        else if (!augmentClaimAllowed && (text.contains(QStringLiteral("当前强化")) || text.contains(QStringLiteral("强化已"))))
            revised.append(QStringLiteral("海克斯记录未完整或与计算记录不一致，当前结果仅供条件比较。"));
        else {
            auto conditional = text;
            conditional.replace(QStringLiteral("先按顺序出装，再结合商店调整。"), QStringLiteral("装备顺序仅供条件比较，需结合当前战况与商店取舍。"));
            revised.append(conditional);
        }
    }
    plan[k("advice")] = revised;
}
}

QJsonObject coachPurchaseInputReadiness(const Catalog &catalog,const GameState &state) {
    const auto observation=augmentObservation(catalog,state);
    const bool complete=observation.value(k("complete")).toBool()
        &&observation.value(k("model_selection_matches_observation")).toBool();
    return {{k("ready"),complete},{k("reason"),complete?k("ready"):k("self_augments_unconfirmed")},
        {k("summary"),complete?QStringLiteral("已选强化已核对"):QStringLiteral("请在主页补全已选海克斯")},
        {k("augment_observation"),observation},{k("unknown_is_zero"),false}};
}

QJsonObject recommendationHistoryCoverage(const GameState &state) {
    const auto evidence=state.recommendationEvidence;
    const auto input=evidence.value(k("players")).toArray();
    int evaluated=0,known=0,fresh=0,analyzed=0,multidimensional=0;QSet<int> seen;
    QJsonArray missing;
    for(const auto &value:input){
        if(evaluated==10)break;const auto row=value.toObject();
        if(!integer(row.value(k("player_index")),0,9)||seen.contains(row.value(k("player_index")).toInt()))continue;
        const int index=row.value(k("player_index")).toInt();seen.insert(index);++evaluated;
        const bool hasHistory=row.value(k("status"))==k("mapped");
        known+=hasHistory;
        fresh+=hasHistory&&row.value(k("freshness")).toObject().value(k("status"))==k("fresh");
        analyzed+=hasHistory&&row.value(k("player_analysis_consumed")).toBool();
        multidimensional+=hasHistory&&row.value(k("multidimensional_profile_consumed")).toBool();
        if(!hasHistory)missing.append(index);
    }
    for(int index=0;index<10;++index)if(!seen.contains(index))missing.append(index);
    const bool allHistory=evaluated==10&&known==10,allFresh=allHistory&&fresh==10,allAnalyzed=allHistory&&analyzed==10;
    return {{k("expected_players"),10},{k("evaluated_players"),evaluated},{k("known_history_players"),known},
        {k("fresh_history_players"),fresh},{k("player_analysis_consumed_players"),analyzed},
        {k("multidimensional_profile_consumed_players"),multidimensional},{k("missing_player_indices"),missing},
        {k("all_ten_have_history"),allHistory},{k("all_ten_history_fresh"),allFresh},{k("all_ten_source_analyses_consumed"),allAnalyzed},
        {k("status"),allFresh&&allAnalyzed?k("ready"):known?k("partial"):k("unavailable")},
        {k("history_query_status"),evidence.value(k("status"))},{k("historical_reference_only"),true},
        {k("missing_history_is_zero_performance"),false},{k("causal_effect_known"),false},{k("skill_multiplier"),false}};
}

void annotateCoachRecommendationQuality(const Catalog &catalog, const GameState &state, QJsonObject &coach) {
    auto plan = coach.value(k("plan")).toObject();
    const auto sandbox = plan.value(k("purchase_sandbox")).toObject();
    const auto detail = sandbox.value(k("selected_sandbox")).toObject();
    const auto roles = catalog.champion(state.championId).value(k("roles")).toArray();
    const bool supportRole = roles.contains(k("support"));
    const bool tankRole = roles.contains(k("tank"));
    const auto allyReference=plan.value(k("growth_objective")).toObject().value(k("ally_utility_reference")).toObject();
    const bool conditionalProtection=allyReference.value(k("available")).toBool();
    const bool compiledHero = catalog.combat && catalog.combat->champions.contains(state.championId);
    const auto supportComponents = compiledHero
        ? catalog.combat->champions.value(state.championId).value(k("support_components")).toArray() : QJsonArray{};
    // A damage comparison cannot establish an ally-protection winner, even
    // when a heal/shield component is available as a source-bound formula.
    const auto requestedStyle = coach.value(k("requested_combat_style")).toString();
    auto roleGuidance = roleAwareRecommendation(catalog, state, requestedStyle, plan);
    if(plan.value(k("build_reasoning")).toObject().value(k("available")).toBool()&&!plan.value(k("conditional_equipment_options")).toArray().isEmpty()){
        roleGuidance[k("mechanism_reference_available")]=true;roleGuidance[k("prefer_mechanism_reference_for_display")]=true;
    }
    const auto roleIntent = roleGuidance.value(k("role_intent")).toObject();
    const bool protection = roleIntent.value(k("intent")) == k("ally_protection");
    const bool growthConditioned = plan.value(k("growth_conditioned_candidates")).toBool();
    const auto growthLoadouts = plan.value(k("candidate_loadouts")).toArray();
    int growthCandidateCount = 0;
    for (const auto &value : growthLoadouts) {
        const auto row = value.toObject();
        const auto itemIds = row.value(k("item_ids")).toArray();
        bool usable = !itemIds.isEmpty() && itemIds.size() <= 6;
        for (const auto &id : itemIds) usable &= integer(id, 1, 1000000);
        if (usable) ++growthCandidateCount;
    }
    if (growthConditioned) {
        // These combinations come from the selected mechanics, not from a
        // public support build or a verified current shopping decision.
        roleGuidance[k("growth_conditioned_candidates")] = true;
        roleGuidance[k("prefer_growth_conditioned_candidates_for_display")] = !protection;
        roleGuidance[k("prefer_source_route_for_display")] = false;
        roleGuidance[k("prefer_mechanism_reference_for_display")] = false;
        roleGuidance[k("next_purchase_reference")] = QJsonObject{};
        if (!protection) roleGuidance[k("comparison_label")] = QStringLiteral("成长联动候选");
    }
    const bool referenceAvailable = protection && (roleGuidance.value(k("has_ally_support_reference_route")).toBool()
        || !plan.value(k("reference_routes")).toArray().isEmpty() || roleGuidance.value(k("mechanism_reference_available")).toBool());
    const bool explicitOutput = false;
    const auto observation = augmentObservation(catalog, state);
    const bool augmentsComplete = observation.value(k("complete")).toBool();
    const bool selectionMatches = observation.value(k("model_selection_matches_observation")).toBool();
    QSet<QString> unknowns,critical;
    const auto health=combatHealthContract(catalog,state);
    for(const auto &value:combatHealthUnknowns(health))unknowns.insert(value);
    collectUnknowns(sandbox,unknowns,critical);
    collectUnknowns(plan.value(k("comparison")).toObject(), unknowns,critical);
    collectUnknowns(detail, unknowns,critical);
    collectUnknowns(detail.value(k("current")).toObject().value(k("raw_eval")).toObject(), unknowns,critical);
    collectUnknowns(detail.value(k("candidate")).toObject().value(k("raw_eval")).toObject(), unknowns,critical);
    collectUnknowns(sandbox.value(k("team_context")).toObject(), unknowns,critical);
    if (growthConditioned) for (const auto &value : growthLoadouts) {
        const auto row = value.toObject(); collectUnknowns(row, unknowns,critical);
        collectUnknowns(row.value(k("comparison")).toObject(), unknowns,critical);
        collectUnknowns(row.value(k("raw_eval")).toObject(), unknowns,critical);
    }
    for (const auto &window : detail.value(k("windows")).toArray()) for (const auto &target : window.toObject().value(k("targets")).toArray()) {
        const auto row = target.toObject(); collectUnknowns(row, unknowns,critical);
        for (const auto &side : {k("current"), k("candidate")}) collectUnknowns(row.value(side).toObject().value(k("raw_eval")).toObject(), unknowns,critical);
    }
    const bool manualGrowth=state.growthObservations.value(k("manual_scenario")).isObject();
    const bool manualScenario=manualGrowth||state.growthObservations.value(k("manual_form_scenario")).isObject();
    if(manualScenario)critical.insert(k("manual_input_scenario_not_live_observation"));
    for (const auto &value : unknowns) if (criticalUnknown(value)) critical.insert(value);
    if (!augmentsComplete) critical.insert(k("self_augments_unobserved_or_incomplete"));
    else if (!selectionMatches) critical.insert(k("self_augment_selection_mismatch"));
    if (protection) critical.insert(k("ally_support_objective_unmodeled"));
    const bool computationInterrupted = coach.value(k("status")) == k("interrupted")
        || plan.value(k("status")) == k("interrupted") || sandbox.value(k("status")) == k("interrupted")
        || (coach.value(k("computation_complete")).isBool() && !coach.value(k("computation_complete")).toBool())
        || (plan.value(k("computation_complete")).isBool() && !plan.value(k("computation_complete")).toBool());
    // Validation failures can also leave enumeration incomplete. Keep that
    // distinct from a timed-out/cancelled worker while refusing either prefix.
    const bool comparisonIncomplete = !plan.value(k("reference_only")).toBool() &&
        (plan.value(k("enumeration_complete")) != QJsonValue(true)
        || sandbox.value(k("enumeration_complete")) != QJsonValue(true));
    const bool hasCandidate = !computationInterrupted && !comparisonIncomplete && integer(sandbox.value(k("selected_item_id")), 1, 1000000)
        && sandbox.value(k("status")) != k("unavailable");
    const bool hasGrowthAnalysis = growthConditioned && !computationInterrupted && growthCandidateCount > 0;
    const bool hasDiagnosticComparison=!computationInterrupted&&!comparisonIncomplete&&sandbox.value(k("diagnostic_comparison_available")).toBool();
    const bool analysisAvailable = protection ? referenceAvailable : growthConditioned ? hasGrowthAnalysis : (hasCandidate||hasDiagnosticComparison);
    const bool nonPurchaseReference = protection || growthConditioned || plan.value(k("reference_only")).toBool();
    QJsonArray reasons;
    auto reason = [&](const char *code, const QString &text, const QString &severity = QStringLiteral("warning")) {
        reasons.append(QJsonObject{{k("code"), k(code)}, {k("summary"), text}, {k("severity"), severity}});
    };
    if (!augmentsComplete) reason("self_augments_unobserved_or_incomplete", QStringLiteral("海克斯记录未完整"), k("critical"));
    else if (!selectionMatches) reason("self_augment_selection_mismatch", QStringLiteral("海克斯观测与计算记录不一致"), k("critical"));
    bool skillGap = false, numericGap = false, parameterGap = false;
    for (const auto &value : critical) {
        skillGap |= value.contains(k("skill")) || value.contains(k("passive"));
        numericGap |= value.startsWith(k("unobserved_event:")) || value.startsWith(k("unobserved_stack:"))
            || value.startsWith(k("condition:")) || value.startsWith(k("target_current_hp:")) || value.startsWith(k("effect_value:"));
        parameterGap |= value.startsWith(k("effect_parameter:")) || value.startsWith(k("effect_damage_type:"))
            || value.startsWith(k("unsupported_stat:")) || value.startsWith(k("derived_value:"))
            || value == k("conversion_value") || value == k("invalid_crit_damage_modifier");
    }
    if (skillGap) reason("critical_skill_model_gap", QStringLiteral("关键技能或被动仍有缺口"), k("critical"));
    if (numericGap) reason("effect_state_unobserved", QStringLiteral("触发或成长状态未完整观测"), k("critical"));
    if (parameterGap) reason("effect_parameters_unverified", QStringLiteral("效果参数无效或尚未核实"), k("critical"));
    if (!critical.isEmpty() && reasons.isEmpty()) reason("critical_model_gap", QStringLiteral("关键计算条件尚未核实"), k("critical"));
    if(!combatHealthUnknowns(health).isEmpty())reason("required_health_field_unavailable",health.value(k("summary")).toString(),k("critical"));
    QJsonArray objectiveGaps;
    if (supportRole) { objectiveGaps.append(k("ally_support_objective_unmodeled"));
        reason("ally_support_objective_unmodeled", conditionalProtection?QStringLiteral("已计算单名队友的条件保护收益；实际命中、走位与全部增益仍未知"):QStringLiteral("队友增益收益尚未完整计入")); }
    if (tankRole) { objectiveGaps.append(k("ally_protection_and_control_objective_unmodeled"));
        reason("ally_protection_and_control_objective_unmodeled", conditionalProtection?QStringLiteral("保护与控制已纳入条件场景；实际命中与位置收益仍未知"):QStringLiteral("保护与控制收益尚未完整计入")); }
    if (!unknowns.isEmpty() || !sandbox.value(k("all_effects_complete")).toBool()) reason("partial_combat_model", QStringLiteral("仅比较已建模的效果"));
    auto actions = candidateActions(state, sandbox);
    const bool budgetConflict = !nonPurchaseReference && actions.value(k("candidate_rows_budget_conflicts")).toInt() > 0;
    if (budgetConflict) reason("purchase_budget_labels_conflict", QStringLiteral("预算信息不一致，购买候选待刷新"));
    if (nonPurchaseReference) {
        actions[k("available_now")]=QJsonArray{};actions[k("save_for")]=QJsonArray{};actions[k("budget_unknown")]=QJsonArray{};
        actions[k("source")]=growthConditioned ? k("growth_conditioned_candidates_not_purchase_actions")
            : protection ? k("protection_reference_not_numeric_purchase_actions") : k("reference_only_not_purchase_actions");
        actions[k("numeric_damage_candidates_are_diagnostic_only")]=true;
    }
    if (!actions.value(k("budget_known")).toBool()) reason("budget_unobserved", QStringLiteral("金币未核实，暂不区分可买与存钱"));
    if (computationInterrupted) reason("purchase_computation_incomplete", QStringLiteral("购买比较未完成，暂不输出排名"));
    else if (comparisonIncomplete) reason("purchase_comparison_incomplete", QStringLiteral("购买候选比较不完整，暂不输出排名"));
    else if (growthConditioned && !hasGrowthAnalysis) reason("growth_candidates_unavailable", QStringLiteral("当前没有可核实的成长条件候选"));
    else if (!hasCandidate&&!hasDiagnosticComparison&&!protection&&!growthConditioned) reason("current_purchase_unavailable", QStringLiteral("当前没有可核实的购买候选"));
    const auto history= recommendationHistoryCoverage(state);
    const int historyKnown=history.value(k("known_history_players")).toInt(),historyFresh=history.value(k("fresh_history_players")).toInt(),historyAnalyzed=history.value(k("player_analysis_consumed_players")).toInt();
    if(historyKnown<10)reason(historyKnown?"ten_player_history_partial":"ten_player_history_unavailable",
        QStringLiteral("历史 %1/10 人，缺失者保留未知").arg(historyKnown));
    else if(historyFresh<10)reason("ten_player_history_stale",QStringLiteral("历史已覆盖十人，%1 人资料仍需刷新").arg(10-historyFresh));
    if(historyAnalyzed<historyKnown)reason("player_analysis_partial",QStringLiteral("多维分析已接入 %1/%2 人").arg(historyAnalyzed).arg(historyKnown));
    const bool candidateOnly = !critical.isEmpty();
    if(candidateOnly){actions[k("available_now")]=QJsonArray{};actions[k("save_for")]=QJsonArray{};actions[k("budget_unknown")]=QJsonArray{};actions[k("source")]=k("diagnostic_candidates_without_purchase_selection");}
    const bool purchaseRankingAvailable = hasCandidate && !candidateOnly && !nonPurchaseReference && !budgetConflict
        && plan.value(k("ranking_available")) == QJsonValue(true) && sandbox.value(k("ranking_available")) == QJsonValue(true);
    if(!purchaseRankingAvailable&&!nonPurchaseReference&&!candidateOnly){
        actions[k("available_now")]=QJsonArray{};actions[k("save_for")]=QJsonArray{};actions[k("budget_unknown")]=QJsonArray{};
        actions[k("source")]=k("diagnostic_candidates_without_verified_purchase_ranking");
    }
    const auto confidence = protection ? (referenceAvailable?k("conditional_reference"):k("reference_unavailable"))
        : !analysisAvailable ? k("unavailable") : candidateOnly ? k("candidate_only")
        : growthConditioned ? k("conditional_candidates") : k("conditional");
    const QString protectionSummary = !augmentsComplete ? QStringLiteral("海克斯未完整，先看条件候选")
        : !selectionMatches ? QStringLiteral("强化记录不一致，先看条件候选")
        : skillGap ? QStringLiteral("技能模型有缺口，先看条件候选")
        : parameterGap ? QStringLiteral("效果参数待核实，仅供候选比较")
        : numericGap ? QStringLiteral("触发或成长状态待核实，仅供候选比较")
        : referenceAvailable ? QStringLiteral("治疗与保护路线参考，装备效果按条件核对")
        : QStringLiteral("保护收益尚未完整建模，暂不按伤害推荐购买");
    const bool inputRequired=coach.value(k("status"))==k("input_required");
    const QString summary = inputRequired?coach.value(k("input_readiness")).toObject().value(k("summary")).toString()
        : manualScenario?QStringLiteral("手动输入情景 · 仅供条件比较")
        : computationInterrupted ? (growthConditioned ? QStringLiteral("本次成长条件比较未完成，暂不输出结论") : QStringLiteral("本次购买计算未完成，暂不排序"))
        : comparisonIncomplete ? QStringLiteral("购买候选比较不完整，暂不排序")
        : budgetConflict && !nonPurchaseReference ? QStringLiteral("预算信息不一致，购买候选待刷新")
        : protection ? protectionSummary
        : !analysisAvailable ? (growthConditioned ? QStringLiteral("成长条件候选待刷新") : QStringLiteral("当前购买候选待刷新")) : !augmentsComplete ? QStringLiteral("海克斯未完整，先看条件候选")
        : !selectionMatches ? QStringLiteral("强化记录不一致，先看条件候选") : skillGap ? QStringLiteral("技能模型有缺口，先看条件候选")
        : parameterGap ? QStringLiteral("效果参数待核实，仅供候选比较")
        : numericGap ? QStringLiteral("触发或成长状态待核实，仅供候选比较")
        : candidateOnly ? QStringLiteral("关键条件待核实，仅供候选比较")
        : growthConditioned ? QStringLiteral("成长条件与装备取舍已分析，组合仅供条件比较")
        : historyKnown<10 ? QStringLiteral("历史 %1/10 人，按已知条件比较").arg(historyKnown)
        : QStringLiteral("已知条件下比较，仍有模型边界");
    QJsonObject quality{{k("schema_version"), 1}, {k("confidence"), confidence}, {k("conditional"), analysisAvailable},
        {k("analysis_available"), analysisAvailable}, {k("growth_conditioned_candidates"), growthConditioned},
        {k("growth_candidate_count"), growthCandidateCount}, {k("reference_only"), nonPurchaseReference},
        {k("computation_interrupted"), computationInterrupted},{k("manual_growth_scenario"),manualGrowth},{k("manual_input_scenario"),manualScenario},{k("input_required"),inputRequired},
        {k("purchase_comparison_incomplete"), comparisonIncomplete},
        {k("purchase_budget_conflict"), budgetConflict},
        {k("candidate_only"), candidateOnly}, {k("summary"), summary}, {k("reasons"), reasons}, {k("objective_gaps"), objectiveGaps},
        {k("health_basis"),health},{k("health_summary"),health.value(k("summary"))},{k("diagnostic_comparison_available"),hasDiagnosticComparison},
        {k("ranking_scope"), protection?k("source_and_mechanism_reference_without_numeric_ally_ranking")
            : growthConditioned?k("source_conditioned_growth_candidate_tradeoffs_without_purchase_rank"):k("conditional_known_numeric_effects_only")},
        {k("comparison_label"), protection?roleGuidance.value(k("comparison_label")):roleGuidance.value(k("numeric_comparison_label"))},
        {k("objective_scope"), protection?k("source_and_mechanism_ally_protection_reference"):k("source_conditioned_growth_model_comparison")},
        {k("fixed_direction_matching_enabled"), false},
        {k("ranking_available"), purchaseRankingAvailable}, {k("primary_purchase_visible"), purchaseRankingAvailable},
        {k("primary_purchase_actionable"), false}, {k("explicit_output_comparison_available"), hasCandidate && supportRole && explicitOutput},
        {k("objective_coverage"), QJsonObject{{k("catalog_roles"), roles}, {k("support_role"), supportRole}, {k("tank_role"), tankRole},
            {k("compiled_hero_available"), compiledHero}, {k("compiled_support_components"), compiledHero ? QJsonValue(supportComponents.size()) : QJsonValue(QJsonValue::Null)},
            {k("support_components_connected_to_live_objective"), conditionalProtection}, {k("ally_support_objective_included"), false},
            {k("conditional_ally_reference_included"),conditionalProtection},{k("ally_utility_reference"),allyReference},
            {k("ally_protection_included"), false}, {k("control_value_included"), false}, {k("objective_gaps"), objectiveGaps},
            {k("role_usage"), k("capability_boundary_only_no_role_weighting")},
            {k("requested_combat_style"), requestedStyle}, {k("explicit_output_preference"), explicitOutput},
            {k("source"), k("catalog_roles_and_conditional_numeric_scoring_path")}}},
        {k("augment_observation"), observation}, {k("critical_model_gaps"), QJsonArray::fromStringList(sorted(critical))},
        {k("history_coverage"),history},{k("ten_player_history_complete"),history.value(k("all_ten_have_history"))},
        {k("historical_roster_scenario"),detail.value(k("historical_roster_scenario"))},
        {k("model_unknowns"), QJsonArray::fromStringList(sorted(unknowns))}, {k("candidate_actions"), actions},
        {k("full_combat_model_claim_allowed"), false}, {k("absolute_optimum_claim_allowed"), false},
        {k("unique_purchase_instruction"), false}, {k("applied_augment_claim_allowed"), augmentsComplete && selectionMatches},
        {k("calculated_values_preserved"), true}, {k("selection_changed"), false}, {k("actual_win_rate"), QJsonValue(QJsonValue::Null)}};
    quality[k("decision_goal")] = decisionGoalContract(state, quality, false);
    if (protection) {
        plan[k("reference_only")]=true;plan[k("conditional_reference")]=true;plan[k("objective")]=k("ally_protection");
        plan[k("ranking_available")]=false;plan[k("primary_purchase_visible")]=false;
        plan[k("damage_purchase_comparison_is_diagnostic_only")]=hasCandidate;
    }
    if (growthConditioned) {
        plan[k("reference_only")] = true; plan[k("conditional_reference")] = true;
        plan[k("ranking_available")] = false; plan[k("primary_purchase_visible")] = false;
        plan[k("primary_purchase_actionable")] = false;
        plan[k("candidate_loadouts_are_conditional_tradeoffs")] = true;
        plan[k("analysis_available")] = analysisAvailable;
    }
    conditionAdvice(plan, augmentsComplete && selectionMatches);
    if (supportRole || tankRole) {
        auto advice = plan.value(k("advice")).toArray();
        const auto boundary = growthConditioned
            ? QStringLiteral("当前仅比较已建模的成长、输出与生存条件；队友增益、保护和控制等收益仍有缺口，组合不代表购买顺序或完整团战最优。")
            : QStringLiteral("当前排序只比较已建模的输出与生存收益；队友增益、保护和控制等收益仍有缺口，不代表完整团战最优。");
        if (!advice.contains(boundary)) advice.append(boundary);
        plan[k("advice")] = advice;
    }
    plan[k("role_aware_recommendation")] = roleGuidance;
    coach[k("role_aware_recommendation")] = roleGuidance;
    plan[k("recommendation_quality")] = quality; coach[k("plan")] = plan; coach[k("recommendation_quality")] = quality;
}
