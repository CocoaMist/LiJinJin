#include "world_attack_probe_v1.h"
#include "combat.h"
#include "continuous_sandbox.h"
#include <cmath>

namespace {
QString k(const char *value) { return QString::fromLatin1(value); }
const QString attackerKey = k("probe:attacker"), targetKey = k("probe:target");
bool fields(const QJsonObject &object, const QStringList &allowed) {
    if (object.size() != allowed.size()) return false;
    for (const auto &key : allowed) if (!object.contains(key)) return false;
    return true;
}
bool finite(const QJsonValue &value, double minimum, double maximum) {
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= minimum && value.toDouble() <= maximum;
}
QString validatePreContact(const QJsonValue &raw) {
    if (!raw.isObject()) return k("pre_contact_object_required");
    const auto pre = raw.toObject();
    if (!fields(pre, {k("attacker"), k("target")})) return k("pre_contact_fields");
    for (const auto &side : {k("attacker"), k("target")}) {
        if (!pre.value(side).isObject()) return k("pre_contact_actor_object_required");
        const auto row = pre.value(side).toObject();
        const auto stat = side == k("attacker") ? k("attack_damage") : k("armor");
        if (!fields(row, {stat, k("maximum_health"), k("current_health")})) return k("pre_contact_actor_fields");
        if (!finite(row.value(stat), side == k("attacker") ? .001 : 0, 1000)
            || !finite(row.value(k("maximum_health")), 1, 10000)
            || !finite(row.value(k("current_health")), .001, row.value(k("maximum_health")).toDouble())) return k("pre_contact_numeric_bounds");
    }
    if (pre.value(k("target")).toObject().value(k("current_health")).toDouble()
        <= pre.value(k("attacker")).toObject().value(k("attack_damage")).toDouble()) return k("probe_v1_nonlethal_only");
    return {};
}
QJsonObject nativeRequest(const QJsonObject &pre) {
    const auto attacker = pre.value(k("attacker")).toObject(), target = pre.value(k("target")).toObject();
    const QJsonObject ranks{{k("Q"), 0}, {k("W"), 0}, {k("E"), 0}, {k("R"), 0}};
    auto player = [&](bool source) {
        const auto panel = source ? attacker : target;
        const auto observed = source
            ? QJsonObject{{k("attackDamage"), panel.value(k("attack_damage"))}, {k("maxHealth"), panel.value(k("maximum_health"))}}
            : QJsonObject{{k("armor"), panel.value(k("armor"))}, {k("maxHealth"), panel.value(k("maximum_health"))}};
        return QJsonObject{{k("player_key"), source ? attackerKey : targetKey}, {k("champion_id"), source ? 86 : 99},
            {k("team"), source ? k("ORDER") : k("CHAOS")}, {k("level"), 1}, {k("items"), QJsonArray{}}, {k("augments"), QJsonArray{}},
            {k("augments_complete"), true}, {k("ranks"), ranks}, {k("current_health"), panel.value(k("current_health"))},
            {k("probe_observed_stats"), observed}};
    };
    const QJsonObject event{{k("event_id"), k("probe:impact:1")}, {k("type"), k("attack")}, {k("player_key"), attackerKey},
        {k("target_key"), targetKey}, {k("slot"), k("P")}, {k("time"), .01}, {k("hit_confirmed"), true}, {k("synthetic"), true}};
    return {{k("participant_scope"), k("basic_attack_probe_v1")}, {k("players"), QJsonArray{player(true), player(false)}},
        {k("start_time"), 0}, {k("horizon"), .02}, {k("decision_step_seconds"), .01}, {k("auto_actions"), false},
        {k("simulate_natural_regeneration"), false}, {k("policy"), k("hold")}, {k("opponent_policy"), k("hold")},
        {k("objective_team"), k("ORDER")}, {k("action_events"), QJsonArray{event}}};
}
QJsonObject resultBase(const Catalog &catalog) {
    return {{k("available"), false}, {k("schema_version"), 1}, {k("kind"), k("world_basic_attack_probe_v1")},
        {k("scenario_id"), k("garen_lux_basic_attack_v1")}, {k("patch"), catalog.patch},
        {k("combat_revision"), catalog.combat ? catalog.combat->revision : QString()}, {k("measured"), false},
        {k("real_match_calibrated"), false}, {k("full_game_eligible"), false},
        {k("contact_contract"), k("given_contact_instant_impact_not_attack_windup_projectile_or_hit_prediction")},
        {k("scope_unknowns"), QJsonArray{k("mode_damage_multiplier_is_one_unverified"), k("champion_passives_and_rune_procs_excluded_not_certified"),
            k("no_geometry_range_windup_projectile_or_target_response_model"), k("pre_contact_panel_is_input_not_a_measured_outcome")}}};
}
bool near(double left, double right) { return std::isfinite(left) && std::isfinite(right) && std::abs(left - right) <= 1e-6; }
}

QString validateWorldAttackProbeContinuousV1(const QJsonObject &request) {
    // Reconstruct the one permitted request. Equality rejects extra top-level,
    // player and event fields, alternate actions, histories and result leakage.
    const auto players = request.value(k("players")).toArray();
    if (players.size() != 2 || !players.at(0).isObject() || !players.at(1).isObject()) return k("probe_requires_two_fixed_players");
    const auto attacker = players.at(0).toObject(), target = players.at(1).toObject();
    const auto attackerPanel = attacker.value(k("probe_observed_stats")).toObject(), targetPanel = target.value(k("probe_observed_stats")).toObject();
    const QJsonObject pre{{k("attacker"), QJsonObject{{k("attack_damage"), attackerPanel.value(k("attackDamage"))},
        {k("maximum_health"), attackerPanel.value(k("maxHealth"))}, {k("current_health"), attacker.value(k("current_health"))}}},
        {k("target"), QJsonObject{{k("armor"), targetPanel.value(k("armor"))}, {k("maximum_health"), targetPanel.value(k("maxHealth"))},
            {k("current_health"), target.value(k("current_health"))}}}};
    const auto reason = validatePreContact(pre);
    if (!reason.isEmpty()) return reason;
    if (request != nativeRequest(pre)) return k("probe_continuous_contract_mismatch");
    return {};
}

QJsonObject runWorldAttackProbeV1(const Catalog &catalog, const QJsonObject &input) {
    auto out = resultBase(catalog);
    auto fail = [&](const QString &reason) { out[k("status")] = k("invalid_request"); out[k("reason")] = reason; return out; };
    if (!catalog.combat || !catalog.combat->champions.contains(86) || !catalog.combat->champions.contains(99)) return fail(k("probe_catalog_unavailable"));
    if (!fields(input, {k("schema_version"), k("scenario_id"), k("patch"), k("combat_revision"), k("pre_contact")})) return fail(k("probe_input_fields"));
    if (input.value(k("schema_version")) != QJsonValue(1) || input.value(k("scenario_id")) != QJsonValue(k("garen_lux_basic_attack_v1"))) return fail(k("probe_schema_or_scenario_mismatch"));
    if (input.value(k("patch")) != QJsonValue(catalog.patch) || input.value(k("combat_revision")) != QJsonValue(catalog.combat->revision)) return fail(k("probe_patch_or_revision_mismatch"));
    const auto reason = validatePreContact(input.value(k("pre_contact")));
    if (!reason.isEmpty()) return fail(reason);
    const auto pre = input.value(k("pre_contact")).toObject(), request = nativeRequest(pre);
    out[k("native_request")] = request;
    const auto result = runContinuousSandbox(catalog, request);
    out[k("native_result")] = result;
    if (result.value(k("available")) != QJsonValue(true) || result.value(k("events_accepted")) != QJsonValue(1)
        || result.value(k("events_rejected")) != QJsonValue(0)) return fail(k("probe_native_admission_or_execution_failed"));
    QJsonObject attacker, target, receipt; int receiptCount = 0;
    const auto final = result.value(k("players_final")).toArray();
    if (final.size() != 2) return fail(k("probe_native_roster_changed"));
    for (const auto &raw : final) {
        const auto row = raw.toObject();
        if (row.value(k("player_key")).toString() == attackerKey) attacker = row;
        if (row.value(k("player_key")).toString() == targetKey) target = row;
        for (const auto &name : {k("effective_healing_received"), k("health_cost_received"), k("shield_absorbed"), k("shield_damage_dealt"),
             k("remaining_shield"), k("external_health_loss"), k("respawn_health_restored"), k("natural_health_regen_received"),
             k("entity_damage_dealt"), k("entity_damage_received"), k("unit_damage_dealt"), k("health_stat_clamp_loss")})
            if (!finite(row.value(name), 0, 0)) return fail(k("probe_unexpected_non_base_effect"));
    }
    for (const auto &raw : result.value(k("trace")).toArray()) if (raw.toObject().value(k("type")).toString() == k("damage_receipt")) { receipt = raw.toObject(); ++receiptCount; }
    if (attacker.isEmpty() || target.isEmpty() || receiptCount != 1 || receipt.value(k("event_id")) != QJsonValue(k("probe:impact:1"))
        || receipt.value(k("source_key")) != QJsonValue(attackerKey) || receipt.value(k("target_key")) != QJsonValue(targetKey)
        || receipt.value(k("target_death")) != QJsonValue(false) || !finite(receipt.value(k("actual_damage")), 1e-9, 1000)
        || !finite(receipt.value(k("shield_damage")), 0, 0)) return fail(k("probe_native_damage_receipt_mismatch"));
    const double before = pre.value(k("target")).toObject().value(k("current_health")).toDouble();
    const double after = target.value(k("remaining_health")).toDouble(), damage = receipt.value(k("actual_damage")).toDouble();
    if (!near(before - after, damage) || !near(target.value(k("effective_damage_received")).toDouble(), damage)
        || !near(attacker.value(k("effective_damage_dealt")).toDouble(), damage)
        || !near(attacker.value(k("effective_damage_received")).toDouble(), 0)) return fail(k("probe_native_health_conservation_failed"));
    out[k("available")] = true; out[k("status")] = k("predicted_given_contact_awaiting_external_measurement");
    out[k("prediction")] = QJsonObject{{k("source_damage"), attacker.value(k("stats")).toObject().value(k("ad"))},
        {k("actual_health_damage"), damage}, {k("target_health_before"), before}, {k("target_health_after"), after},
        {k("shield_damage"), receipt.value(k("shield_damage"))}};
    out[k("model_unknowns")] = result.value(k("model_unknowns"));
    return out;
}
