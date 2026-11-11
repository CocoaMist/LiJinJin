#pragma once
#include "analysis.h"
#include "headliner.h"
#include <QJsonArray>
#include <QJsonObject>

// The product goal and the quantity optimized today are deliberately explicit.
// A complete observation or a high proxy score does not certify match benefit.
inline QJsonObject decisionGoalContract(const GameState &state,const QJsonObject &quality,bool augmentDecision) {
    const auto key=[](const char *value){return QString::fromLatin1(value);};
    const auto observation=quality.value(key("augment_observation")).toObject();
    // Augment quality is produced before the individual offers are evaluated.
    // Passing that input gate cannot establish comparable complete candidates.
    const bool inputGate=quality.value(key(augmentDecision?"ranking_allowed":"ranking_available")).toBool();
    const bool numericRanking=!augmentDecision&&inputGate;
    QJsonArray blockers{
        key("full_hero_mechanisms_not_independently_validated"),
        key("match_outcome_objective_not_optimized"),
        key("future_item_augment_joint_policy_not_evaluated"),
        key("real_counterfactual_decision_benefit_not_validated")};
    if(!observation.value(key("complete")).toBool())blockers.append(key("selected_augments_incomplete"));
    const bool headlinerKnown=!observedHeadliner(state).isEmpty();
    if(!headlinerKnown)blockers.append(key("headliner_context_unobserved"));
    int incompletePlayers=0;
    for(const auto &v:state.players){const auto row=v.toObject();
        if(!row.value(key("items_complete")).toBool()||!row.value(key("augments_complete")).toBool())++incompletePlayers;
    }
    if(state.players.size()!=10||incompletePlayers)blockers.append(key("ten_player_mechanic_state_incomplete"));
    if(!quality.value(key("objective_gaps")).toArray().isEmpty())blockers.append(key("role_objectives_incomplete"));
    return {{key("schema_version"),1},
        {key("user_goal"),key("best_expected_real_match_choice_given_available_information")},
        {key("evaluated_scope"),key(augmentDecision?"actual_offer_known_numeric_comparison":"legal_next_purchase_known_numeric_comparison")},
        {key("current_objective"),key("short_window_modeled_output_and_durability_proxy")},
        {key("claim"),key(augmentDecision&&inputGate?"numeric_comparison_requires_candidate_validation":
            numericRanking?"conditional_numeric_comparison":"insufficient_comparison_basis")},
        {key("input_ranking_gate_passed"),inputGate},
        {key("numeric_ranking_admission_checked_here"),!augmentDecision},
        {key("numeric_ranking_available"),augmentDecision?QJsonValue(QJsonValue::Null):QJsonValue(numericRanking)},
        {key("headliner_observed"),headlinerKnown},
        {key("incomplete_player_rows"),incompletePlayers},
        {key("real_match_best_choice_claim_allowed"),false},
        {key("match_outcome_optimized"),false},
        {key("future_item_augment_joint_policy_evaluated"),false},
        {key("real_counterfactual_benefit_validated"),false},
        {key("choice_accuracy"),QJsonValue::Null},
        {key("blockers"),blockers}};
}
