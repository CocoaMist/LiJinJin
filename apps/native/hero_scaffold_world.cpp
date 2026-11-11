#include "hero_scaffold_world.h"
#include "combat.h"
#include "continuous_action_state.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QString k(const char *value) { return QString::fromLatin1(value); }
constexpr double planeBound = 2500.;
bool number(const QJsonValue &value, double minimum, double maximum) {
    return value.isDouble() && std::isfinite(value.toDouble())
        && value.toDouble() >= minimum && value.toDouble() <= maximum;
}
bool only(const QJsonObject &object, const QStringList &fields) {
    for (auto it = object.begin(); it != object.end(); ++it) if (!fields.contains(it.key())) return false;
    return true;
}
bool exact(const QJsonObject &object, const QStringList &fields) {
    return object.size() == fields.size() && only(object, fields);
}
QJsonObject unavailable(const QString &reason) {
    return {{k("available"), false}, {k("kind"), k("hero_scaffold_world_v1")},
        {k("status"), k("invalid_request")}, {k("reason"), reason},
        {k("observation_origin"), k("synthetic_scaffold")}, {k("real_game_calibrated"), false},
        {k("full_game_eligible"), false}, {k("world_complete"), false}};
}
QJsonObject gate(bool legal, const QString &reason) {
    return {{k("legal"), legal}, {k("reason"), reason}, {k("effects_executed"), false}};
}
QJsonObject fromReceipt(const QJsonObject &receipt) {
    return gate(receipt.value(k("accepted")) == QJsonValue(true), receipt.value(k("reason")).toString(k("unknown_admission")));
}
}

struct HeroScaffoldWorld::Data {
    explicit Data(const Catalog &source) : catalog(&source) {}
    const Catalog *catalog;
    bool loaded = false, moving = false;
    int seed = 0;
    quint64 frame = 0, intentSequence = 0;
    double time = 0, x = 0, z = 0, targetX = 0, targetZ = 0, heading = 0;
    CombatContext context;
    CombatStats stats;
    ContinuousActionState action;
    QJsonObject source, last;
    QString patch, revision, name;

    QJsonObject mask() const {
        if (!loaded) return {};
        QJsonObject result{{k("move"), gate(true, k("flat_plane_move_available"))},
            {k("stop"), gate(true, k("flat_plane_stop_available"))},
            {k("R"), gate(false, k("target_required_empty_scene"))}};
        auto attackCopy = action;
        result[k("attack")] = fromReceipt(attackCopy.attack(time, stats[Stat::As], true));
        for (const auto &slot : {k("Q"), k("W"), k("E")}) {
            auto copy = action;
            auto row = fromReceipt(copy.cast(slot, time, stats));
            row[k("scope")] = k("generic_action_admission_only_not_complete_skill_execution");
            result[slot] = row;
        }
        return result;
    }

    QJsonObject observation() const {
        if (!loaded) return unavailable(k("reset_required"));
        return {{k("available"), true}, {k("kind"), k("hero_scaffold_world_v1")}, {k("schema_version"), 1},
            {k("champion_id"), 86}, {k("name"), name}, {k("level"), 6}, {k("seed"), seed},
            {k("patch"), patch}, {k("combat_revision"), revision}, {k("frame"), double(frame)}, {k("time"), time},
            {k("position"), QJsonObject{{k("x"), x}, {k("z"), z}}}, {k("heading"), heading},
            {k("move_target"), moving ? QJsonValue(QJsonObject{{k("x"), targetX}, {k("z"), targetZ}}) : QJsonValue(QJsonValue::Null)},
            {k("stats"), stats.json()}, {k("action_state"), action.snapshot()}, {k("action_mask"), mask()},
            {k("last_action"), last.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(last)}, {k("source"), source},
            {k("observation_origin"), k("synthetic_scaffold")}, {k("real_game_calibrated"), false},
            {k("skill_mechanics_certified"), false}, {k("full_game_eligible"), false}, {k("world_complete"), false},
            {k("training_scope"), k("input_action_and_visual_alignment_only")},
            {k("movement_model"), k("declared_flat_arena_position_model")},
            {k("arena"), QJsonObject{{k("minimum"), -planeBound}, {k("maximum"), planeBound}, {k("obstacles"), QJsonArray{}},
                {k("actors"), 1}, {k("enemies"), 0}, {k("map_objects"), 0}}},
            {k("effects_executed"), false}, {k("hits_generated"), false}, {k("damage_generated"), false},
            {k("gaps"), QJsonArray{k("movement_is_source_speed_on_declared_empty_plane_not_game_navigation_or_collision"),
                k("attack_is_targetless_preview_admission_not_hit_or_damage"),
                k("q_w_e_are_generic_action_admission_not_skill_effects"),
                k("garen_q_attack_reset_delayed_cooldown_and_speed_buff_not_executed"),
                k("garen_w_shield_damage_reduction_and_stacks_not_executed"),
                k("garen_e_duration_ticks_cancel_and_q_e_interactions_not_executed"),
                k("garen_e_missing_cast_time_is_not_evidence_of_real_instant_cast"),
                k("r_requires_a_target_and_is_unavailable_in_empty_scene"),
                k("visual_animation_and_input_alignment_not_real_game_skill_calibration")}}};
    }
};

HeroScaffoldWorld::HeroScaffoldWorld(const Catalog &catalog) : m_data(std::make_unique<Data>(catalog)) {}
HeroScaffoldWorld::~HeroScaffoldWorld() = default;

QJsonObject HeroScaffoldWorld::reset(const QJsonObject &request) {
    if (!only(request, {k("champion_id"), k("seed")})) return unavailable(k("reset_fields"));
    if (request.contains(k("champion_id")) && request.value(k("champion_id")) != QJsonValue(86)) return unavailable(k("only_garen_86_supported"));
    if (request.contains(k("seed")) && (!number(request.value(k("seed")), 0, 2147483647)
        || std::floor(request.value(k("seed")).toDouble()) != request.value(k("seed")).toDouble())) return unavailable(k("invalid_seed"));
    const auto &catalog = *m_data->catalog;
    if (!catalog.combat || !catalog.combat->champions.contains(86)) return unavailable(k("garen_source_unavailable"));
    auto next = std::make_unique<Data>(catalog);
    const auto &db = *catalog.combat;
    next->seed = request.value(k("seed")).toInt(0);
    next->context = db.context(86, 6);
    next->context.ranks = {{k("Q"), 1}, {k("W"), 1}, {k("E"), 3}, {k("R"), 1}};
    next->context.explicitActionCounts = true;
    next->context.restrictExplicitComponents = true;
    next->context.horizon = .001;
    const auto evaluation = evaluateCombat(db, next->context, {}, {});
    if (!evaluation.valid || !std::isfinite(evaluation.stats[Stat::Ms]) || evaluation.stats[Stat::Ms] <= 0
        || !std::isfinite(evaluation.stats[Stat::As]) || evaluation.stats[Stat::As] <= 0) return unavailable(k("source_stats_unavailable"));
    next->stats = evaluation.stats;
    const auto hero = db.champions.value(86), rules = hero.value(k("action_rules_v3")).toObject();
    if (rules.value(k("resource")).toObject().value(k("kind")) != QJsonValue(k("none"))) return unavailable(k("unexpected_garen_resource_source"));
    QJsonObject ready; for (const auto &slot : {k("Q"), k("W"), k("E"), k("R")}) ready[slot] = 0;
    QString error;
    if (!next->action.load(rules, next->context, next->stats,
            {{k("alive"), true}, {k("resource_current"), 0}, {k("ready_at"), ready}}, 0, &error))
        return unavailable(k("source_action_state_unavailable:") + error);
    next->patch = catalog.patch; next->revision = db.revision; next->name = catalog.championName(86);
    next->source = {{k("path"), hero.value(k("source_path"))}, {k("sha256"), hero.value(k("source_sha256"))},
        {k("action_rules_scope"), rules.value(k("scope"))}, {k("stats_origin"), k("active_catalog_level_6_without_equipment_or_augments")},
        {k("rules_origin"), k("local_client_source_generic_action_constraints_not_complete_script")}};
    next->loaded = true;
    m_data = std::move(next);
    return m_data->observation();
}

QJsonObject HeroScaffoldWorld::step(const QJsonObject &request) {
    if (!m_data->loaded) return unavailable(k("reset_required"));
    if (!only(request, {k("dt"), k("action")}) || !number(request.value(k("dt")), .001, .1)) return unavailable(k("step_fields_or_dt"));
    const auto rawAction = request.value(k("action"));
    QJsonObject input;
    if (!rawAction.isUndefined() && !rawAction.isNull()) {
        if (!rawAction.isObject()) return unavailable(k("action_object_or_null_required"));
        input = rawAction.toObject();
        const auto type = input.value(k("type")).toString();
        if (type == k("move")) {
            if (!exact(input, {k("type"), k("x"), k("z")}) || !number(input.value(k("x")), -planeBound, planeBound)
                || !number(input.value(k("z")), -planeBound, planeBound)) return unavailable(k("move_fields_or_bounds"));
        } else if (type == k("stop") || type == k("attack")) {
            if (!exact(input, {k("type")})) return unavailable(k("action_fields"));
        } else if (type == k("cast")) {
            if (!exact(input, {k("type"), k("slot")}) || !QStringList{k("Q"), k("W"), k("E"), k("R")}.contains(input.value(k("slot")).toString())) return unavailable(k("cast_fields_or_slot"));
        } else return unavailable(k("unknown_action_type"));
    }
    const double dt = request.value(k("dt")).toDouble(), end = m_data->time + dt;
    if (!std::isfinite(end) || end > 1e7 || m_data->frame >= 1000000000) return unavailable(k("scaffold_clock_limit"));
    auto next = std::make_unique<Data>(*m_data);
    if (!input.isEmpty()) {
        const auto type = input.value(k("type")).toString();
        QJsonObject receipt;
        if (type == k("move")) {
            next->targetX = input.value(k("x")).toDouble(); next->targetZ = input.value(k("z")).toDouble(); next->moving = true;
            receipt = {{k("accepted"), true}, {k("reason"), k("flat_plane_target_set")}, {k("time"), next->time}};
        } else if (type == k("stop")) {
            next->moving = false;
            receipt = {{k("accepted"), true}, {k("reason"), k("flat_plane_movement_stopped")}, {k("time"), next->time}};
        } else if (type == k("attack")) receipt = next->action.attack(next->time, next->stats[Stat::As], true);
        else if (input.value(k("slot")) == QJsonValue(k("R")))
            receipt = {{k("accepted"), false}, {k("reason"), k("target_required_empty_scene")}, {k("time"), next->time}};
        else receipt = next->action.cast(input.value(k("slot")).toString(), next->time, next->stats);
        next->last = {{k("type"), type}, {k("intent_sequence"), double(++next->intentSequence)}, {k("time"), next->time},
            {k("admitted"), receipt.value(k("accepted")) == QJsonValue(true)}, {k("reason"), receipt.value(k("reason"))},
            {k("receipt"), receipt}, {k("effects_executed"), false}, {k("preview_only"), true},
            {k("scope"), k("movement_or_generic_action_admission_no_skill_effects_or_damage")}};
        if (type == k("cast")) next->last[k("slot")] = input.value(k("slot"));
        if (type == k("move")) next->last[k("target")] = QJsonObject{{k("x"), next->targetX}, {k("z"), next->targetZ}};
    }
    if (next->moving) {
        const double dx = next->targetX - next->x, dz = next->targetZ - next->z, distance = std::hypot(dx, dz);
        const double travel = std::min(distance, next->stats[Stat::Ms] * dt);
        if (distance > 1e-9) {
            next->heading = std::atan2(dx, dz);
            next->x = std::clamp(next->x + dx / distance * travel, -planeBound, planeBound);
            next->z = std::clamp(next->z + dz / distance * travel, -planeBound, planeBound);
        }
        if (travel >= distance) next->moving = false;
    }
    if (!next->action.advance(end, true)) return unavailable(k("native_action_clock_failed"));
    next->time = end; ++next->frame;
    m_data = std::move(next);
    return m_data->observation();
}

QJsonObject HeroScaffoldWorld::observe() const { return m_data->observation(); }
