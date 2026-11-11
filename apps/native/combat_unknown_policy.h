#pragma once
#include <QString>
#include <QStringList>
#include <QJsonArray>
#include <QJsonObject>

// Explicit missing state or rejected numeric mechanics can change a marginal
// gain even when the same code appears in both evaluations. This allowlist is
// intentionally separate from generic partial-coverage/scope disclosures.
inline bool combatUnknownBlocksComparison(const QString &value) {
    static const QStringList prefixes{
        QStringLiteral("condition:"), QStringLiteral("effect_parameter:"),
        QStringLiteral("effect_value:"), QStringLiteral("effect_damage_type:"),
        QStringLiteral("target_current_hp:"), QStringLiteral("invalid_event:"),
        QStringLiteral("unobserved_event:"), QStringLiteral("unobserved_stack:"),
        QStringLiteral("unsupported_stat:"), QStringLiteral("derived_value:"),
        QStringLiteral("champion_crit_conversion:"),
        QStringLiteral("unknown_augment:"), QStringLiteral("headliner:"),
        QStringLiteral("skill_formula_or_state:"), QStringLiteral("skill_semantic_quarantine:"),
        QStringLiteral("invalid_skill_rank:"), QStringLiteral("invalid_passive_rank:"), QStringLiteral("skill_condition:"),
        QStringLiteral("skill_explicit_scenario_attack_count_unknown_or_invalid:"),
        QStringLiteral("unobserved_passive_stats:"), QStringLiteral("unobserved_passive_counter:"),
        QStringLiteral("passive_condition:"), QStringLiteral("conflicting_"),
        QStringLiteral("explicit_nth_hit_state_missing:"), QStringLiteral("explicit_burn_tick_state_missing:"),
        QStringLiteral("explicit_item_or_augment_proc_state_missing:"),
        QStringLiteral("purchase_candidate_model_unavailable:"),
        QStringLiteral("purchase_candidate_comparison_incomplete:"),
        QStringLiteral("unsupported_champion_level:")};
    if (value.startsWith(QStringLiteral("required_observed_field:"))
        || value.startsWith(QStringLiteral("3116_qualified_hit_withheld:"))
        || value.startsWith(QStringLiteral("3116_consume_heal_withheld:"))) return true;
    for (const auto &prefix : prefixes) if (value.startsWith(prefix)) return true;
    if ((value.startsWith(QStringLiteral("augment:")) || value.startsWith(QStringLiteral("item:")))
        && value.endsWith(QStringLiteral(":activation_unverified"))) return true;
    static const QStringList exact{
        QStringLiteral("target_crit_damage_multiplier"), QStringLiteral("conversion_value"),
        QStringLiteral("unobserved_passive_stat_value"), QStringLiteral("invalid_crit_damage_modifier"),
        QStringLiteral("copied_true_damage_amplifier_order"),
        QStringLiteral("invalid_or_unverified_form_stat_transforms"), QStringLiteral("invalid_explicit_action_counts"),
        QStringLiteral("invalid_combat_numeric_result"),
        QStringLiteral("combat_target_comparison_incomplete"),
        QStringLiteral("enemy_augments_incomplete"), QStringLiteral("teammate_augments_unobserved_or_incomplete"),
        QStringLiteral("incomplete_skill_cast_model"), QStringLiteral("self_skill_ranks_use_partial_qwe_priority_reference"),
        QStringLiteral("unknown_current_item_id"), QStringLiteral("unknown_candidate_item_id"),
        QStringLiteral("unknown_selected_augment_id"), QStringLiteral("unknown_candidate_augment_id"),
        QStringLiteral("champion_combat_model_unavailable")};
    return exact.contains(value);
}

inline QStringList combatCriticalUnknowns(const QStringList &values) {
    QStringList result;
    for (const auto &value : values) if (combatUnknownBlocksComparison(value) && !result.contains(value)) result.append(value);
    result.sort(); return result;
}
inline QJsonObject combatComparisonBoundary(const QStringList &unknowns, bool numericAvailable) {
    const auto critical = combatCriticalUnknowns(unknowns);
    return {{QStringLiteral("critical_unknowns"), QJsonArray::fromStringList(critical)},
        {QStringLiteral("numeric_comparison_available"), numericAvailable},
        {QStringLiteral("diagnostic_only"), numericAvailable && !critical.isEmpty()},
        {QStringLiteral("ranking_eligible"), numericAvailable && critical.isEmpty()},
        {QStringLiteral("comparison_scope"), QStringLiteral("known_model_only_shared_unknowns_not_assumed_to_cancel")},
        {QStringLiteral("live_certified"), false}};
}
