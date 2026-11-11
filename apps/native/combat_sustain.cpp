#include "combat_sustain.h"
#include <algorithm>
#include <cmath>

namespace {
QString k(const char *name) { return QString::fromLatin1(name); }
bool bounded(double value, double maximum = 1e12) {
    return std::isfinite(value) && value >= 0 && value <= maximum;
}
std::optional<double> sourceValue(const CombatEntry &entry, const QJsonValue &value,
                                 const CombatContext &context, const CombatStats &stats) {
    if (value.isDouble()) return bounded(value.toDouble()) ? std::optional<double>(value.toDouble()) : std::nullopt;
    if (value.isObject()) return compileCombatFormula(value.toObject(), context).evaluate(context, stats);
    auto name = value.toString();
    if (name.startsWith(k("ref:"))) name = name.mid(4);
    if (name.startsWith(k("calc:"))) return compileCombatFormula(entry.calculations.value(name.mid(5)).toObject(), context).evaluate(context, stats);
    const auto literal = entry.values.value(name);
    if (literal.isDouble() && bounded(literal.toDouble())) return literal.toDouble();
    return {};
}
std::optional<double> receivedIncrement(const CombatEntry &entry, const QJsonObject &action,
                                       const CombatContext &context, const CombatStats &stats) {
    const auto op = action.value(k("op")).toString();
    if ((op != k("heal") && op != k("shield")) || action.value(k("target")) != k("self") || action.value(k("key")) != k("health")) return {};
    const auto expr = action.value(k("value")).toObject();
    const auto args = expr.value(k("args")).toArray();
    if (expr.value(k("expr")) != k("mul") || args.size() != 2) return {};
    const auto expectedFact = op == k("heal") ? k("fact:self_heal") : k("fact:self_shield");
    if (args[0] == expectedFact) return sourceValue(entry, args[1], context, stats);
    if (args[1] == expectedFact) return sourceValue(entry, args[0], context, stats);
    return {};
}
}

CombatSustainModifiers combatSelfSustainModifiers(const CombatDatabase &db,
    const CombatContext &context, const QList<int> &items, const QList<int> &augments,
    const CombatStats &stats) {
    CombatSustainModifiers out;
    auto collect = [&](const QMap<int, CombatEntry> &entries, const QList<int> &ids, const QString &kind) {
        QSet<int> seen;
        for (int id : ids) {
            if (seen.contains(id)) continue;
            seen.insert(id);
            const auto found = entries.constFind(id);
            if (found == entries.cend() || found->triggerContract.value(k("event")) != k("heal_or_shield_received")) continue;
            if (found->triggerContract.value(k("patch")) != db.patch || found->triggerContract.value(k("source_pointer")).toString().isEmpty()) {
                out.unknowns.append(k("received_sustain_source_unbound:") + kind + QChar(u':') + QString::number(id));
                continue;
            }
            QSet<QString> admitted;
            for (const auto &raw : found->actions) {
                const auto action = raw.toObject();
                const auto op = action.value(k("op")).toString();
                const auto increment = receivedIncrement(*found, action, context, stats);
                if (!increment || !bounded(*increment, 10) || admitted.contains(op)) continue;
                admitted.insert(op);
                if (op == k("heal")) out.healingReceived *= 1 + *increment;
                else out.shieldingReceived *= 1 + *increment;
                out.sources.append(QJsonObject{{k("source"), kind}, {k("id"), id}, {k("kind"), op},
                    {k("received_multiplier"), 1 + *increment}, {k("source_pointer"), found->triggerContract.value(k("source_pointer"))},
                    {k("recipient_only"), true}, {k("producer_heal_shield_power_included"), false}});
            }
            if (admitted.isEmpty()) out.unknowns.append(k("received_sustain_action_not_bound:") + kind + QChar(u':') + QString::number(id));
        }
    };
    collect(db.items, items, k("item"));
    collect(db.augments, augments, k("augment"));
    return out;
}

CombatHealSettlement settleCombatSelfHeal(double raw, double missingHp,
                                         const CombatSustainModifiers &modifiers) {
    CombatHealSettlement out;
    if (!bounded(raw) || !bounded(missingHp) || !bounded(modifiers.healingReceived, 1000)) return out;
    out.raw = raw;
    out.amplified = raw * modifiers.healingReceived;
    if (!bounded(out.amplified)) return out;
    out.effective = std::min(out.amplified, missingHp);
    out.overheal = out.amplified - out.effective;
    out.valid = true;
    return out;
}

std::optional<double> amplifyCombatSelfShield(double raw, const CombatSustainModifiers &modifiers) {
    if (!bounded(raw) || !bounded(modifiers.shieldingReceived, 1000)) return {};
    const double amplified = raw * modifiers.shieldingReceived;
    return bounded(amplified) ? std::optional<double>(amplified) : std::nullopt;
}

CombatDrainProfile combatDrainProfile(const CombatDatabase &db, const CombatContext &context,
                                     const CombatStats &stats, int itemId) {
    CombatDrainProfile out;
    const auto found = db.items.constFind(itemId);
    if (found == db.items.cend()) { out.unknowns.append(k("drain_item_missing")); return out; }
    if (found->triggerContract.value(k("event")) != k("combat_periodic") ||
        found->triggerContract.value(k("patch")) != db.patch ||
        found->triggerContract.value(k("source_pointer")).toString().isEmpty()) {
        out.unknowns.append(k("drain_source_contract_unbound")); return out;
    }
    QJsonObject bound;
    for (const auto &raw : found->effects) {
        const auto effect = raw.toObject();
        if (effect.value(k("op")) != k("proc") || effect.value(k("trigger")) != k("melee_combat") ||
            effect.value(k("source")) != k("bonus_hp") || effect.value(k("stat")) != k("magic") || !effect.contains(k("heal_multiplier"))) continue;
        if (!bound.isEmpty()) { out.unknowns.append(k("ambiguous_drain_effect")); return out; }
        bound = effect;
    }
    const auto coefficient = sourceValue(*found, bound.value(k("value")), context, stats);
    const auto multiplier = sourceValue(*found, bound.value(k("heal_multiplier")), context, stats);
    const auto cooldown = sourceValue(*found, bound.value(k("cooldown")), context, stats);
    const auto range = sourceValue(*found, k("DrainRange"), context, stats);
    if (!coefficient || !multiplier || !cooldown || !range || !bounded(*coefficient, 1) ||
        !bounded(*multiplier, 100) || !bounded(*cooldown, 10000) || *cooldown <= 0 || !bounded(*range, 10000) || *range <= 0) {
        out.unknowns.append(k("drain_numeric_source_unbound")); return out;
    }
    out.rawDamage = *coefficient * std::max(0., stats[Stat::Hp] - context.base[Stat::Hp]);
    out.healingFromDamage = *multiplier;
    out.cooldown = *cooldown;
    out.range = *range;
    out.damageType = bound.value(k("stat")).toString();
    out.valid = bounded(out.rawDamage);
    return out;
}

CombatSelfShieldCapacity combatSelfShieldCapacity(const CombatDatabase &db, const CombatContext &context,
    const CombatStats &stats, const CombatSustainModifiers &modifiers) {
    CombatSelfShieldCapacity out;
    if (!bounded(context.horizon, 1e6) || context.horizon <= 0) return out;
    const auto sourceComponents = db.champions.value(context.championId).value(k("support_components")).toArray();
    QMap<QString,int> slotCounts;
    for (const auto &raw : sourceComponents) {
        const auto component = raw.toObject();
        if (component.value(k("numeric_role")) == k("conditional_self_shield_capacity") &&
            component.value(k("kind")) == k("shield") && component.value(k("target")) == k("self"))
            ++slotCounts[component.value(k("slot")).toString()];
    }
    for (const auto &raw : sourceComponents) {
        const auto component = raw.toObject();
        if (component.value(k("numeric_role")) != k("conditional_self_shield_capacity") ||
            component.value(k("kind")) != k("shield") || component.value(k("target")) != k("self") ||
            component.value(k("rate")).toString(k("cast")) != k("cast")) continue;
        const auto slot = component.value(k("slot")).toString();
        if (slotCounts.value(slot) != 1) { out.unknowns.append(k("ambiguous_self_shield_capacity:") + slot); continue; }
        const int rank = context.ranks.value(slot, 0);
        if (rank <= 0 || rank > component.value(k("max_rank")).toInt(5)) continue;
        const auto condition = component.value(k("condition")).toString();
        if (!condition.isEmpty() && !context.conditions.contains(condition)) { out.unknowns.append(k("self_shield_condition_missing:") + slot); continue; }
        if (context.explicitActionCounts) {
            if (context.explicitCastCounts.value(slot, 0) <= 0) continue;
        } else {
            const double ready = context.skillReadyIn.value(slot, 0);
            if (!bounded(ready) || ready >= context.horizon) continue;
        }
        const auto amount = compileCombatFormula(component.value(k("expression")).toObject(), context, rank).evaluate(context, stats);
        const auto duration = compileCombatFormula(component.value(k("duration_expression")).toObject(), context, rank).evaluate(context, stats);
        if (!amount || !duration || !bounded(*amount) || !bounded(*duration, 10000) || *duration <= 0 ||
            component.value(k("source_pointer")).toString().isEmpty()) {
            out.unknowns.append(k("self_shield_source_or_formula_missing:") + slot); continue;
        }
        const double produced = *amount * std::max(0., 1 + stats[Stat::HealShield]);
        const auto amplified = amplifyCombatSelfShield(produced, modifiers);
        if (!amplified) { out.unknowns.append(k("self_shield_amplification_invalid:") + slot); continue; }
        out.raw += *amount;
        out.amplified += *amplified;
        out.components.append(QJsonObject{{k("slot"), slot}, {k("source_pointer"), component.value(k("source_pointer"))},
            {k("raw_pool"), *amount}, {k("available_pool"), *amplified}, {k("duration"), *duration},
            {k("counted_pools"), 1}, {k("recasts_added_as_absorption"), false}, {k("actual_absorption_known"), false},
            {k("scope"), k("conditional_single_available_self_shield_not_actual_absorption")}});
    }
    return out;
}
