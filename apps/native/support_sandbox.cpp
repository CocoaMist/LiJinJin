#include "support_sandbox.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QString k(const char *s) { return QString::fromLatin1(s); }
bool number(const QJsonValue &v, double lo, double hi) { return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= lo && v.toDouble() <= hi; }
std::optional<double> formula(const QJsonObject &expression, CombatContext context, const SandboxSupportActor &actor, int rank) {
    context.counters[k("self_current_hp")] = actor.hp;
    context.counters[k("self_health_fraction")] = actor.hp / actor.maximumHp;
    const auto value = compileCombatFormula(expression, context, rank).evaluate(context, actor.stats);
    if (!value || !std::isfinite(*value) || *value < 0) return std::nullopt;
    return value;
}
int findActor(const QVector<SandboxSupportActor> &actors, const QString &key) {
    for (int i = 0; i < actors.size(); ++i) if (actors[i].key == key) return i;
    return -1;
}
double hasteFor(const SandboxSupportActor &actor, const QString &slot) {
    double haste = actor.stats[Stat::Haste];
    if (slot == k("Q")) haste += actor.stats[Stat::QHaste];
    if (slot == k("W")) haste += actor.stats[Stat::WHaste];
    if (slot == k("E")) haste += actor.stats[Stat::EHaste];
    if (slot == k("R")) haste += actor.stats[Stat::RHaste];
    return std::max(0., std::min(actor.context.abilityHasteCap, haste));
}
}

bool SandboxSupportTimeline::load(const CombatDatabase &db, const QJsonValue &value,
                                  const QVector<SandboxSupportActor> &actors, double horizon, QString *error) {
    *this = SandboxSupportTimeline{}; db_ = &db;
    auto fail = [&](const char *reason) { if (error) *error = k(reason); return false; };
    if (value.isUndefined()) return true;
    if (!value.isArray() || value.toArray().size() > 512) return fail("support_events_must_be_bounded_array");
    QSet<QString> eventIds;
    for (const auto &raw : value.toArray()) {
        if (!raw.isObject()) return fail("support_event_must_be_object");
        auto event = raw.toObject();
        if (!number(event.value(k("time")), 0, horizon) || event.value(k("time")).toDouble() >= horizon) return fail("support_event_time_outside_half_open_horizon");
        const auto id = event.value(k("event_id")).toString();
        if (id.isEmpty() || id.size() > 128 || eventIds.contains(id)) return fail("support_event_id_missing_or_duplicate");
        eventIds.insert(id);
        const auto slot = event.value(k("slot")).toString();
        if (!QStringList{k("Q"), k("W"), k("E"), k("R")}.contains(slot)) return fail("invalid_support_skill_slot");
        if (!event.value(k("available")).isBool() || !number(event.value(k("hit_factor")), 0, 1)) return fail("explicit_support_availability_and_hit_factor_required");
        QString key = event.value(k("player_key")).toString();
        if (key.isEmpty()) {
            const int champion = event.value(k("champion_id")).toInt(-1); const auto team = event.value(k("team")).toString(); int matches = 0;
            for (const auto &actor : actors) if (actor.champion == champion && actor.team == team) { key = actor.key; ++matches; }
            if (matches != 1) return fail("support_actor_identity_not_unique");
        }
        if (findActor(actors, key) < 0) return fail("support_actor_unknown"); event[k("player_key")] = key;
        if (event.contains(k("target_key")) && (!event.value(k("target_key")).isString() || findActor(actors, event.value(k("target_key")).toString()) < 0)) return fail("support_target_unknown");
        if (event.contains(k("target_policy")) && !QStringList{k("lowest_health_fraction"), k("carry")}.contains(event.value(k("target_policy")).toString())) return fail("invalid_support_target_policy");
        if (event.contains(k("conditions"))) {
            if (!event.value(k("conditions")).isArray()) return fail("support_conditions_must_be_array");
            for (const auto &condition : event.value(k("conditions")).toArray()) if (!condition.isString() || condition.toString().isEmpty()) return fail("invalid_support_condition");
        }
        events_.append(event);
    }
    std::stable_sort(events_.begin(), events_.end(), [](const QJsonObject &a, const QJsonObject &b) { return a.value(k("time")).toDouble() < b.value(k("time")).toDouble(); });
    if (!events_.isEmpty()) for (const auto &code : {k("support_events_are_explicit_synthetic_actions_not_observed_casts"),
        k("support_mana_range_cast_time_control_and_grievous_wounds_not_simulated"),
        k("support_uses_snapshot_stats_and_standard_haste_not_triggered_cooldown_resets"),
        k("support_and_damage_at_same_timestamp_resolve_support_first_by_explicit_contract")}) unknowns_.insert(code);
    return true;
}

void SandboxSupportTimeline::apply(double time, QVector<SandboxSupportActor> &actors) {
    expire(time);
    while (cursor_ < events_.size() && events_[cursor_].value(k("time")).toDouble() <= time + 1e-9) {
        const auto event = events_[cursor_++]; const auto slot = event.value(k("slot")).toString();
        const int actorIndex = findActor(actors, event.value(k("player_key")).toString()); auto &actor = actors[actorIndex];
        QJsonObject result{{k("event_id"), event.value(k("event_id"))}, {k("time"), event.value(k("time"))}, {k("player_key"), actor.key}, {k("slot"), slot}, {k("accepted"), false}};
        auto reject = [&](const char *reason) { result[k("reason")] = k(reason); results_.append(result); unknowns_.insert(k(reason)); };
        if (!event.value(k("available")).toBool()) { reject("support_explicitly_unavailable"); continue; }
        if (actor.hp <= 1e-9) { reject("dead_actor_cannot_support"); continue; }
        const int rank = actor.context.ranks.value(slot, 0);
        if (rank <= 0) { reject("support_skill_unlearned"); continue; }
        bool sealed = false, specialCooldown = false;
        for (int id : actor.augments) for (const auto &raw : db_->augments.value(id).effects) {
            const auto effect = raw.toObject(); if (effect.value(k("op")).toString() != k("flag")) continue;
            const auto name = effect.value(k("stat")).toString();
            sealed |= slot == k("R") && name == k("seal_r");
            specialCooldown |= name == k("ultimate_haste_only");
        }
        if (sealed) { reject("support_ultimate_sealed_by_augment"); continue; }
        if (specialCooldown) { reject("support_special_augment_cooldown_requires_action_timeline"); continue; }
        const auto hero = db_->champions.value(actor.champion); QJsonArray components;
        for (const auto &raw : hero.value(k("support_components")).toArray()) if (raw.toObject().value(k("slot")).toString() == slot) components.append(raw);
        if (components.isEmpty()) { reject("support_component_unavailable"); continue; }
        bool mixed = false;
        for (const auto &raw : hero.value(k("components")).toArray()) if (raw.toObject().value(k("slot")).toString() == slot) mixed = true;
        if (mixed) { reject("mixed_damage_support_cast_requires_action_timeline"); continue; }
        const auto castKey = actor.key + k("/") + slot;
        const double ready = actor.context.skillReadyIn.value(slot, 0.);
        if (time + 1e-9 < ready) { reject("support_skill_not_ready"); continue; }
        auto context = actor.context;
        for (const auto &condition : event.value(k("conditions")).toArray()) context.conditions.insert(condition.toString());
        struct Effect { QString kind, target; double amount = 0, duration = 0; };
        QVector<Effect> effects; double cooldown = 0, cost = 0, minimum = 0; QString failure;
        for (const auto &raw : components) {
            const auto component = raw.toObject(); const auto kind = component.value(k("kind")).toString(), condition = component.value(k("condition")).toString();
            if (!component.value(k("damage_filter")).toString().isEmpty() && component.value(k("damage_filter")).toString() != k("all")) { failure = k("typed_magic_shield_requires_damage_breakdown"); break; }
            if (rank > component.value(k("max_rank")).toInt(6) || component.value(k("rate")).toString(k("cast")) != k("cast")) { failure = k("support_rank_or_rate_unavailable"); break; }
            if (!condition.isEmpty() && !context.conditions.contains(condition)) { failure = k("support_explicit_condition_required"); break; }
            const auto amount = formula(component.value(k("expression")).toObject(), context, actor, rank);
            if (!amount || (kind != k("heal") && kind != k("shield"))) { failure = k("support_formula_unavailable"); break; }
            double duration = 0;
            if (kind == k("shield")) {
                const auto evaluated = formula(component.value(k("duration_expression")).toObject(), context, actor, rank);
                if (!evaluated || *evaluated <= 0) { failure = k("support_shield_duration_unavailable"); break; } duration = *evaluated;
            }
            const auto cds = component.value(k("cooldown")).toArray();
            const double baseCd = rank < cds.size() ? cds[rank].toDouble() : 0;
            if (!std::isfinite(baseCd) || baseCd < 0) { failure = k("support_cooldown_unavailable"); break; }
            cooldown = std::max(cooldown, baseCd * (1 - std::clamp(actor.stats[Stat::CooldownReduction], 0., .99)) / (1 + hasteFor(actor, slot) / 100.));
            for (const auto &pair : {qMakePair(k("health_cost_fraction_expression"), &cost), qMakePair(k("health_minimum_fraction_expression"), &minimum)}) if (component.contains(pair.first)) {
                const auto fraction = formula(component.value(pair.first).toObject(), context, actor, rank);
                if (!fraction || *fraction > 1) { failure = k("support_health_cost_formula_unavailable"); break; }
                *pair.second = std::max(*pair.second, *fraction * actor.maximumHp);
            }
            if (!failure.isEmpty()) break;
            effects.append({kind, component.value(k("target")).toString(), *amount * actor.factor * event.value(k("hit_factor")).toDouble() * std::max(0., 1 + actor.stats[Stat::HealShield]), duration});
        }
        if (!failure.isEmpty()) { result[k("reason")] = failure; results_.append(result); unknowns_.insert(failure); continue; }
        if (castAt_.contains(castKey) && (cooldown <= 0 || time - castAt_.value(castKey) < cooldown - 1e-9)) { reject("support_duplicate_cast_or_cooldown_not_ready"); continue; }
        if (actor.hp - cost < minimum - 1e-9 || actor.hp - cost <= 1e-9) { reject("support_health_cost_unaffordable"); continue; }
        int selected = -1;
        const auto targetKey = event.value(k("target_key")).toString();
        if (!targetKey.isEmpty()) selected = findActor(actors, targetKey);
        bool requiresOther = false, permitsSelf = false;
        for (const auto &effect : effects) { requiresOther |= effect.target == k("ally_other"); permitsSelf |= effect.target == k("self_or_ally") || effect.target == k("self_and_ally_champion"); }
        if (selected >= 0 && (actors[selected].team != actor.team || actors[selected].hp <= 1e-9 || (requiresOther && selected == actorIndex))) { reject("support_target_not_legal_living_ally"); continue; }
        bool needsTarget = false;
        for (const auto &effect : effects) needsTarget |= effect.target == k("ally_other") || effect.target == k("self_or_ally") || effect.target == k("self_and_ally_champion");
        if (needsTarget && selected < 0) {
            if (!event.contains(k("target_policy"))) { reject("support_target_or_explicit_policy_required"); continue; }
            double priority = -std::numeric_limits<double>::infinity(); const bool carry = event.value(k("target_policy")).toString() == k("carry");
            for (int i = 0; i < actors.size(); ++i) if (actors[i].team == actor.team && actors[i].hp > 1e-9 && (i != actorIndex || (permitsSelf && !requiresOther))) {
                const double candidate = carry ? actors[i].threat : 1 - actors[i].hp / actors[i].maximumHp;
                if (candidate > priority + 1e-12) { priority = candidate; selected = i; }
            }
            if (selected < 0) { reject("support_no_legal_living_target"); continue; }
        }
        bool targetKindValid = true;
        for (const auto &effect : effects) targetKindValid &= QStringList{k("self"), k("ally_other"), k("self_or_ally"), k("all_allies_including_self"), k("self_and_ally_champion")}.contains(effect.target);
        if (!targetKindValid) { reject("support_target_kind_unavailable"); continue; }
        actor.hp -= cost; costs_[actor.key] += cost; castAt_[castKey] = time;
        QJsonArray applied;
        for (const auto &effect : effects) {
            QVector<int> targets;
            if (effect.target == k("all_allies_including_self")) { for (int i = 0; i < actors.size(); ++i) if (actors[i].team == actor.team && actors[i].hp > 1e-9) targets.append(i); }
            else if (effect.target == k("self")) targets = {actorIndex};
            else if (effect.target == k("self_and_ally_champion")) { targets = {actorIndex}; if (selected != actorIndex) targets.append(selected); }
            else targets = {selected};
            for (int target : targets) {
                auto &recipient = actors[target]; double effective = effect.amount;
                if (effect.kind == k("heal")) { effective = std::min(effect.amount, std::max(0., recipient.maximumHp - recipient.hp)); recipient.hp += effective; healing_[recipient.key] += effective; }
                else {
                    for (int i = shields_.size() - 1; i >= 0; --i) if (shields_[i].source == actor.key && shields_[i].slot == slot && shields_[i].target == recipient.key) shields_.removeAt(i);
                    shields_.append({actor.key, slot, recipient.key, effect.amount, time + effect.duration});
                }
                applied.append(QJsonObject{{k("target_key"), recipient.key}, {k("kind"), effect.kind}, {k("raw_amount"), effect.amount}, {k("effective_amount"), effective},
                    {k("measure"), effect.kind == k("shield") ? k("shield_pool_created_not_damage_absorbed") : k("actual_hp_restored_capped_by_missing_health")},
                    {k("expires_at"), effect.kind == k("shield") ? QJsonValue(time + effect.duration) : QJsonValue(QJsonValue::Null)}});
            }
        }
        result[k("accepted")] = true; result[k("health_cost")] = cost; result[k("cooldown_seconds")] = cooldown;
        result[k("effects")] = applied; results_.append(result);
    }
}

void SandboxSupportTimeline::expire(double time) {
    for (int i = shields_.size() - 1; i >= 0; --i) if (shields_[i].expires <= time + 1e-9) shields_.removeAt(i);
}

double SandboxSupportTimeline::nextBoundary(double time, double fallback) const {
    double boundary = fallback;
    if (cursor_ < events_.size()) { const double at = events_[cursor_].value(k("time")).toDouble(); if (at > time + 1e-9) boundary = std::min(boundary, at); }
    for (const auto &shield : shields_) if (shield.expires > time + 1e-9) boundary = std::min(boundary, shield.expires);
    return boundary;
}
double SandboxSupportTimeline::absorb(const QString &target, double incoming) {
    double remaining = incoming;
    std::stable_sort(shields_.begin(), shields_.end(), [](const Shield &a, const Shield &b) { return a.expires < b.expires; });
    for (auto &shield : shields_) if (shield.target == target && remaining > 0) { const double used = std::min(shield.amount, remaining); shield.amount -= used; remaining -= used; }
    const double used = incoming - remaining; absorbed_[target] += used; return used;
}
double SandboxSupportTimeline::healthCost(const QString &actor) const { return costs_.value(actor); }
double SandboxSupportTimeline::healingReceived(const QString &actor) const { return healing_.value(actor); }
double SandboxSupportTimeline::absorbed(const QString &actor) const { return absorbed_.value(actor); }
double SandboxSupportTimeline::shieldRemaining(const QString &actor) const { double amount = 0; for (const auto &shield : shields_) if (shield.target == actor) amount += shield.amount; return amount; }
QSet<QString> SandboxSupportTimeline::unknowns() const { return unknowns_; }
QJsonObject SandboxSupportTimeline::summary() const {
    int accepted = 0; for (const auto &raw : results_) accepted += raw.toObject().value(k("accepted")).toBool();
    return {{k("events_requested"), events_.size()}, {k("events_processed"), results_.size()}, {k("events_accepted"), accepted}, {k("events_rejected"), results_.size() - accepted}, {k("event_results"), results_},
        {k("timing_contract"), k("explicit_half_open_horizon_events_split_damage_intervals_expiry_then_support_then_damage")}, {k("automatic_casts_invented"), false}};
}
