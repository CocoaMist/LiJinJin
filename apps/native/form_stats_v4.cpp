#include "form_stats_v4.h"
#include <QJsonArray>
#include <algorithm>
#include <cmath>

namespace {
QString k(const char *s) { return QString::fromLatin1(s); }
bool finiteStats(const CombatStats &stats) {
    for (double n : stats.values) if (!std::isfinite(n)) return false;
    return true;
}
}

QJsonObject FormStatsV4Evaluation::json() const {
    return {{k("valid"), valid}, {k("applied"), applied}, {k("ranged"), ranged},
            {k("bonus_delta_before_conversions"), delta.json()},
            {k("base_replacement_delta"), baseDelta.json()},
            {k("verified_order_stat_transforms"), transforms},
            {k("health_transition_policy"), healthTransitionPolicy},
            {k("unknowns"), QJsonArray::fromStringList(unknowns)}};
}

FormStatsV4Evaluation evaluateFormStatsV4(const QJsonObject &hero, const QString &form,
                                        const CombatContext &baselineContext,
                                        const CombatStats &baselineStats) {
    FormStatsV4Evaluation out;
    out.context = baselineContext;
    out.ranged = baselineContext.ranged;
    if (hero.value(k("id")).toInt() != baselineContext.championId ||
        baselineContext.level < 1 || baselineContext.level > 18 ||
        !finiteStats(baselineStats) || !finiteStats(baselineContext.base) ||
        !finiteStats(baselineContext.eventStats)) {
        out.unknowns.append(k("invalid_form_stat_baseline"));
        return out;
    }
    if (baselineContext.conditions.contains(k("form_stats_v4_applied"))) {
        out.unknowns.append(k("form_stats_require_fresh_untransformed_context"));
        return out;
    }
    const auto rules = hero.value(k("form_stats_v4")).toObject();
    if (rules.value(k("schema_version")).toInt() != 4 ||
        rules.value(k("baseline")).toString() != k("fresh_untransformed_context")) {
        out.unknowns.append(k("form_stat_review_unavailable"));
        return out;
    }
    const auto forms = rules.value(k("forms")).toObject();
    if (form == k("default") || form == k("unknown") || form.isEmpty()) {
        out.valid = true;
        if (!forms.isEmpty()) out.unknowns.append(k("unobserved_form_attributes_withheld"));
        return out;
    }
    if (!forms.contains(form) || !forms.value(form).isObject()) {
        out.unknowns.append(k("unreviewed_form:") + form);
        return out;
    }
    const auto rule = forms.value(form).toObject();
    out.healthTransitionPolicy = rule.value(k("health_transition_policy")).toString(out.healthTransitionPolicy);
    if (out.healthTransitionPolicy != k("preserve_absolute_current_health_clamp")) {
        out.unknowns.append(k("unimplemented_form_health_transition_policy"));
        return out;
    }
    for (const auto &v : rule.value(k("exclusive_condition_groups")).toArray()) {
        int count = 0;
        for (const auto &condition : v.toArray()) count += baselineContext.conditions.contains(condition.toString());
        if (count > 1) {
            out.unknowns.append(k("conflicting_form_stat_conditions"));
            return out;
        }
    }
    const bool before = baselineContext.conditions.contains(k("form_stat_transform_before_conversions"));
    const bool after = baselineContext.conditions.contains(k("form_stat_transform_after_derived_and_overflow"));
    if (before && after) {
        out.unknowns.append(k("conflicting_form_stat_transform_stages"));
        return out;
    }
    CombatStats bonusDelta, replacementDelta;
    QStringList unknowns;
    for (const auto &v : rule.value(k("unknowns")).toArray()) unknowns.append(v.toString());
    for (const auto &v : rule.value(k("known_unbound_numeric")).toArray()) {
        const auto item = v.toObject();
        unknowns.append(k("form_numeric_application_unbound:") + item.value(k("source_field")).toString()
                        + k(":") + item.value(k("unknown_reason")).toString());
    }
    for (const auto &v : rule.value(k("modifiers")).toArray()) {
        const auto modifier = v.toObject();
        if (!modifier.value(k("numeric_execution")).toBool()) continue;
        const auto slot = modifier.value(k("rank_slot")).toString();
        if (slot != k("P") && !baselineContext.ranks.contains(slot)) {
            unknowns.append(k("form_stat_skill_rank_unobserved:") + slot);
            continue;
        }
        const int rank = slot == k("P") ? 1 : baselineContext.ranks.value(slot);
        if (rank == 0 && !modifier.value(k("allow_rank_zero")).toBool()) continue;
        bool eligible = true;
        for (const auto &condition : modifier.value(k("required_conditions")).toArray())
            if (!baselineContext.conditions.contains(condition.toString())) eligible = false;
        if (!eligible) {
            unknowns.append(k("form_stat_condition_unobserved:") + modifier.value(k("source_field")).toString());
            continue;
        }
        const auto zeroStatName = modifier.value(k("requires_zero_baseline_stat")).toString();
        if (!zeroStatName.isEmpty()) {
            const auto zeroStat = combatStat(zeroStatName);
            if (!zeroStat || std::abs(baselineStats[*zeroStat]) > 1e-9) {
                unknowns.append(k("form_stat_multiplier_stacking_order_unverified:") + zeroStatName);
                continue;
            }
        }
        const auto stat = combatStat(modifier.value(k("stat")).toString());
        if (!stat || rank < 0 || rank > modifier.value(k("max_rank")).toInt(6) ||
            modifier.value(k("source_pointer")).toString().isEmpty() ||
            modifier.value(k("source_field")).toString().isEmpty()) {
            out.unknowns.append(k("invalid_reviewed_form_modifier"));
            return out;
        }
        auto expression = modifier.value(k("expression")).toObject();
        // Intrinsic form grants can exist before the first skill point. The
        // public compiler requires a learned outer rank; fixed_rank preserves
        // the exact source rank-zero row without changing the actor's ranks.
        if (rank == 0) expression = QJsonObject{{k("op"), k("fixed_rank")},
            {k("rank"), 0}, {k("arg"), expression}};
        const auto formula = compileCombatFormula(expression, baselineContext, std::max(1, rank));
        const auto value = formula.evaluate(baselineContext, baselineStats);
        if (!formula.valid || !value || !std::isfinite(*value) || std::abs(*value) > 1e7) {
            unknowns.append(k("form_stat_formula_unavailable:") + modifier.value(k("source_field")).toString());
            continue;
        }
        const auto operation = modifier.value(k("operation")).toString();
        if (operation == k("add_bonus")) bonusDelta[*stat] += *value;
        else if (operation == k("replace_base")) replacementDelta[*stat] = *value - baselineContext.base[*stat];
        else {
            out.unknowns.append(k("unsupported_form_stat_operation:") + operation);
            return out;
        }
    }
    QJsonArray stagedTransforms;
    for (const auto &v : rule.value(k("stat_transforms")).toArray()) {
        const auto modifier = v.toObject();
        const auto source = modifier.value(k("source_field")).toString();
        const auto slot = modifier.value(k("rank_slot")).toString();
        if (slot != k("P") && !baselineContext.ranks.contains(slot)) {
            unknowns.append(k("form_stat_transform_rank_unobserved:") + slot);
            continue;
        }
        const int rank = slot == k("P") ? 1 : baselineContext.ranks.value(slot);
        if (rank == 0) continue;
        if (!baselineContext.conditions.contains(k("form_stat_transform_order_verified")) || (!before && !after)) {
            unknowns.append(k("form_stat_transform_order_unverified:") + source);
            continue;
        }
        if (rank <= 0 || rank > modifier.value(k("max_rank")).toInt(6)) {
            unknowns.append(k("form_stat_transform_rank_unobserved:") + slot);
            continue;
        }
        const auto stat = modifier.value(k("stat")).toString(), basis = modifier.value(k("basis")).toString();
        if (!QStringList{k("ad"), k("hp"), k("armor"), k("mr")}.contains(stat) ||
            !QStringList{k("total"), k("bonus")}.contains(basis) ||
            modifier.value(k("source_pointer")).toString().isEmpty() || source.isEmpty()) {
            out.unknowns.append(k("invalid_reviewed_form_stat_transform"));
            return out;
        }
        const auto formula = compileCombatFormula(modifier.value(k("multiplier_expression")).toObject(), baselineContext, rank);
        const auto value = formula.evaluate(baselineContext, baselineStats);
        if (!formula.valid || !value || !std::isfinite(*value) || *value <= 0 || *value > 100) {
            unknowns.append(k("form_stat_transform_formula_unavailable:") + source);
            continue;
        }
        stagedTransforms.append(QJsonObject{{k("stat"), stat}, {k("basis"), basis}, {k("multiplier"), *value},
            {k("stage"), before ? k("before_conversions") : k("after_derived_and_overflow")},
            {k("source_pointer"), modifier.value(k("source_pointer"))}, {k("source_field"), source}});
    }
    if (!finiteStats(bonusDelta) || !finiteStats(replacementDelta)) {
        out.unknowns.append(k("nonfinite_form_stat_delta"));
        return out;
    }
    out.delta = bonusDelta;
    out.baseDelta = replacementDelta;
    out.context.base += replacementDelta;
    out.context.eventStats += bonusDelta;
    out.transforms = stagedTransforms;
    for (const auto &v : stagedTransforms) out.context.formStatTransforms.append(v);
    out.context.conditions.insert(k("form_stats_v4_applied"));
    if (rule.value(k("ranged")).isBool()) {
        out.ranged = rule.value(k("ranged")).toBool();
        out.context.ranged = out.ranged;
    }
    out.applied = true;
    out.valid = true;
    unknowns.removeDuplicates();
    std::sort(unknowns.begin(), unknowns.end());
    out.unknowns = unknowns;
    return out;
}
