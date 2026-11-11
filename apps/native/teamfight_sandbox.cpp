#include "teamfight_sandbox.h"
#include "combat.h"
#include "combat_sandbox.h"
#include "support_sandbox.h"
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QString k(const char *s) { return QString::fromLatin1(s); }
bool finiteNumber(const QJsonValue &v, double lo, double hi) {
    return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= lo && v.toDouble() <= hi;
}
bool integer(const QJsonValue &v, int lo, int hi, int &n) {
    if (!finiteNumber(v, lo, hi) || std::floor(v.toDouble()) != v.toDouble()) return false;
    n = int(v.toDouble()); return true;
}
double damage(const CombatEvaluation &e) { return e.attackDamage + e.skillDamage + e.procDamage; }
QJsonArray codes(const QSet<QString> &s) {
    auto values = s.values(); values.sort(); return QJsonArray::fromStringList(values);
}
struct Fighter {
    QString key, team;
    int champion = 0, level = 0;
    QList<int> items, augments;
    CombatContext context;
    CombatStats stats;
    double maximumHp = 0, initialHp = 0, hp = 0, dealt = 0, received = 0, shieldDealt = 0;
    double outputFactor = 1, takenFactor = 1, supportFactor = 1, threat = 0, deathTime = -1;
    bool initiallyDead = false, referenceRanks = false, initialHealthAssumed = false;
    bool augmentsComplete = false;
    QSet<QString> unknowns;
    QByteArray canonical;
};
struct Objective { double damage = 1, survival = 1, death = 2, uncertainty = .1; };
const QStringList policies{k("focus_damage"), k("focus_frontline"), k("spread"), k("protect_carry"), k("hold")};

QJsonObject invalid(const QString &reason) {
    return {{k("schema_version"), 1}, {k("available"), false}, {k("status"), k("invalid_request")},
        {k("reason"), reason}, {k("actual_win_probability"), QJsonValue::Null}};
}

bool readIds(const QJsonValue &v, const QMap<int, CombatEntry> &entries, QList<int> &ids, bool unique) {
    if (!v.isArray() || v.toArray().size() > 6) return false;
    for (const auto &x : v.toArray()) {
        int id = 0;
        if (!integer(x, 1, std::numeric_limits<int>::max(), id) || !entries.contains(id) || (unique && ids.contains(id))) return false;
        ids.append(id);
    }
    std::sort(ids.begin(), ids.end()); return true;
}

// Both evaluations use this interval's same target state. Absolute time keeps
// cast counts and item cooldown budgets continuous when the selected target changes.
CombatEvaluation cumulative(const CombatDatabase &db, const Fighter &attacker, const Fighter &target, double time) {
    auto ctx = attacker.context; ctx.horizon = time; ctx.targetArmor = target.stats[Stat::Armor];
    ctx.targetMr = target.stats[Stat::Mr]; ctx.targetHp = target.maximumHp; ctx.targetMs = target.stats[Stat::Ms];
    ctx.counters[k("self_current_hp")] = attacker.hp;
    ctx.counters[k("self_health_fraction")] = attacker.hp / attacker.maximumHp;
    ctx.counters[k("target_current_hp")] = target.hp;
    ctx.counters[k("target_health_fraction")] = target.hp / target.maximumHp;
    return evaluateCombat(db, ctx, attacker.items, attacker.augments);
}

QJsonObject simulate(const CombatDatabase &db, QVector<Fighter> players, const QString &policy, const QString &opponentPolicy,
                     double horizon, double step, const Objective &weights, const QString &objectiveTeam, SandboxSupportTimeline support) {
    QJsonArray trace, transitions; QSet<QString> unknowns;
    const QStringList common{k("numeric_cast_components_not_complete_action_or_hit_timeline"),
        k("all_initial_skills_ready_is_an_explicit_scenario"), k("no_movement_range_control_or_target_access_simulation"),
        k("support_requires_reviewed_formula_and_explicit_cast_events_lifesteal_and_auras_partial"),
        k("incoming_resist_stats_applied_but_special_damage_reductions_partial"),
        k("death_and_target_switch_times_are_step_resolution_approximations"),
        k("mana_cast_time_channels_recasts_and_animation_constraints_partial"),
        k("execution_factors_are_subjective_scenario_inputs_not_observed_skill"),
        k("target_choices_are_abstract_policy_assumptions"),
        k("no_respawn_during_this_bounded_teamfight")};
    for (const auto &x : common) unknowns.insert(x);
    double elapsed = 0; int intervals = 0;
    for (double t = 0; t < horizon - 1e-9;) {
        int aliveOrder = 0, aliveChaos = 0;
        for (const auto &p : players) if (p.hp > 1e-9) (p.team == k("ORDER") ? aliveOrder : aliveChaos)++;
        if (!aliveOrder || !aliveChaos) break;
        QVector<SandboxSupportActor> supportActors;
        for (const auto &p : players) supportActors.append({p.key, p.team, p.champion, p.context, p.stats, p.maximumHp, p.hp, p.supportFactor, p.threat, p.augments});
        support.apply(t, supportActors);
        for (int i = 0; i < players.size(); ++i) players[i].hp = supportActors[i].hp;
        const double end = support.nextBoundary(t, std::min(horizon, t + step));
        std::array<std::array<double, 10>, 10> pending{};
        QJsonArray decisions;
        for (int a = 0; a < players.size(); ++a) {
            auto &attacker = players[a];
            const auto actorPolicy = attacker.team == objectiveTeam ? policy : opponentPolicy;
            if (attacker.hp <= 1e-9 || attacker.outputFactor == 0 || actorPolicy == k("hold")) continue;
            QVector<int> targets;
            for (int b = 0; b < players.size(); ++b) if (players[b].team != attacker.team && players[b].hp > 1e-9) targets.append(b);
            if (targets.isEmpty()) continue;
            if (actorPolicy != k("spread")) {
                int best = targets.first(); double bestPriority = -1;
                int carry = a;
                if (actorPolicy == k("protect_carry")) for (int c = 0; c < players.size(); ++c)
                    if (players[c].team == attacker.team && players[c].hp > 1e-9 && players[c].threat > players[carry].threat) carry = c;
                for (int b : targets) {
                    const auto &target = players[b]; double priority = target.threat;
                    if (actorPolicy == k("focus_frontline")) priority = target.stats[Stat::Hp] * (1 + std::max(0., target.stats[Stat::Armor] + target.stats[Stat::Mr]) / 200.);
                    if (actorPolicy == k("protect_carry")) {
                        const auto threat = cumulative(db, target, players[carry], horizon);
                        priority = threat.valid ? damage(threat) * target.outputFactor : target.threat;
                    }
                    if (priority > bestPriority + 1e-9) { bestPriority = priority; best = b; }
                }
                targets = {best};
            }
            QJsonArray targetKeys; const double share = 1. / targets.size();
            for (int b : targets) {
                const auto before = cumulative(db, attacker, players[b], t), after = cumulative(db, attacker, players[b], end);
                for (const auto &x : after.unknowns) { attacker.unknowns.insert(x); unknowns.insert(x); }
                if (!before.valid || !after.valid || !std::isfinite(damage(before)) || !std::isfinite(damage(after))) {
                    attacker.unknowns.insert(k("interval_evaluation_unavailable")); unknowns.insert(k("interval_evaluation_unavailable")); continue;
                }
                const double delta = damage(after) - damage(before);
                if (delta < -1e-7) { attacker.unknowns.insert(k("nonmonotone_numeric_component_clamped")); unknowns.insert(k("nonmonotone_numeric_component_clamped")); }
                pending[a][b] = std::max(0., delta) * share * attacker.outputFactor * players[b].takenFactor;
                targetKeys.append(players[b].key);
            }
            decisions.append(QJsonObject{{k("player_key"), attacker.key}, {k("policy"), actorPolicy}, {k("targets"), targetKeys}, {k("budget_share_per_target"), share}});
        }
        QJsonArray deaths; double effectiveDamage = 0, shieldDamage = 0;
        // Settle all outgoing budgets from the same interval-start state. Damage
        // directed at one victim is proportionally capped once, across attackers.
        for (int b = 0; b < players.size(); ++b) {
            double attempted = 0; for (int a = 0; a < players.size(); ++a) attempted += pending[a][b];
            const double absorbed = players[b].hp > 1e-9 ? support.absorb(players[b].key, attempted) : 0;
            const double effective = std::min(players[b].hp, attempted - absorbed), scale = attempted > 0 ? effective / attempted : 0;
            for (int a = 0; a < players.size(); ++a) {
                players[a].dealt += pending[a][b] * scale;
                players[a].shieldDealt += attempted > 0 ? pending[a][b] * absorbed / attempted : 0;
            }
            shieldDamage += absorbed;
            players[b].hp = std::max(0., players[b].hp - effective); players[b].received += effective; effectiveDamage += effective;
            if (effective > 0 && players[b].hp <= 1e-9 && players[b].deathTime < 0) {
                players[b].hp = 0; players[b].deathTime = end; deaths.append(players[b].key);
            }
        }
        double orderHp = 0, chaosHp = 0;
        for (const auto &p : players) (p.team == k("ORDER") ? orderHp : chaosHp) += p.hp;
        if (trace.size() < 400) trace.append(QJsonObject{{k("time_start"), t}, {k("time_end"), end},
            {k("effective_damage"), effectiveDamage}, {k("shield_damage"), shieldDamage}, {k("ORDER_remaining_hp"), orderHp}, {k("CHAOS_remaining_hp"), chaosHp},
            {k("deaths"), deaths}, {k("decisions"), decisions}});
        // Anonymous regression trace only. Combined damage is neither the
        // selected team's J increment nor a state/action/next-state reward.
        if (transitions.size() < 400) transitions.append(QJsonObject{{k("time"), t}, {k("next_time"), end}, {k("policy"), policy},
            {k("source"), k("synthetic_numeric_regression_trace")}, {k("eligible_for_action_value_training"), false},
            {k("real_world_reward"), QJsonValue::Null}, {k("reward_semantics"), k("combined_damage_not_objective_reward")},
            {k("effective_damage"), effectiveDamage}, {k("deaths"), deaths}, {k("terminal"), orderHp <= 1e-9 || chaosHp <= 1e-9}});
        elapsed = end; ++intervals; t = end;
    }
    support.expire(elapsed);
    for (const auto &code : support.unknowns()) unknowns.insert(code);
    QJsonArray final; QJsonObject totals, objectives;
    for (const auto &side : {k("ORDER"), k("CHAOS")}) {
        double initial = 0, maximum = 0, remaining = 0, dealt = 0, received = 0, shieldDealt = 0, shieldAbsorbed = 0, healed = 0, healthCost = 0, remainingShield = 0; int alive = 0, deaths = 0, uncertain = 0;
        for (const auto &p : players) if (p.team == side) {
            initial += p.initialHp; remaining += p.hp; dealt += p.dealt; received += p.received;
            maximum += p.maximumHp;
            shieldDealt += p.shieldDealt; shieldAbsorbed += support.absorbed(p.key); healed += support.healingReceived(p.key);
            healthCost += support.healthCost(p.key); remainingShield += support.shieldRemaining(p.key);
            alive += p.hp > 1e-9; deaths += !p.initiallyDead && p.deathTime >= 0;
            uncertain += !p.unknowns.isEmpty() || !p.augmentsComplete;
        }
        double enemyInitial = 0, enemyMaximum = 0, enemyRemaining = 0, enemyHealthCost = 0;
        for (const auto &p : players) if (p.team != side) { enemyInitial += p.initialHp; enemyMaximum += p.maximumHp; enemyRemaining += p.hp; enemyHealthCost += support.healthCost(p.key); }
        const double enemyNetLoss = std::max(0., enemyInitial - enemyRemaining - enemyHealthCost);
        const double dealtNorm = std::clamp(enemyNetLoss / std::max(1., enemyMaximum), 0., 1.), survivalNorm = std::clamp(remaining / std::max(1., maximum), 0., 1.), deadNorm = deaths / 5., uncertainty = uncertain / 5.;
        const double value = weights.damage * dealtNorm + weights.survival * survivalNorm - weights.death * deadNorm - weights.uncertainty * uncertainty;
        totals[side] = QJsonObject{{k("initial_hp"), initial}, {k("maximum_hp"), maximum}, {k("remaining_hp"), remaining}, {k("effective_damage_dealt"), dealt},
            {k("effective_damage_received"), received}, {k("shield_damage_dealt"), shieldDealt}, {k("shield_absorbed"), shieldAbsorbed},
            {k("effective_healing_received"), healed}, {k("health_cost_received"), healthCost}, {k("remaining_shield"), remainingShield}, {k("alive"), alive}, {k("new_deaths"), deaths}};
        objectives[side] = QJsonObject{{k("value"), value}, {k("effective_enemy_damage_normalized"), dealtNorm}, {k("remaining_own_health_normalized"), survivalNorm},
            {k("new_own_deaths_normalized"), deadNorm}, {k("model_uncertain_player_fraction"), uncertainty},
            {k("enemy_net_health_loss"), enemyNetLoss}, {k("damage_measure"), k("net_enemy_hp_loss_excluding_enemy_self_cost_not_repeated_healed_damage")},
            {k("damage_normalizer_enemy_maximum_hp"), std::max(1., enemyMaximum)}, {k("survival_normalizer_own_maximum_hp"), std::max(1., maximum)},
            {k("death_normalizer"), 5}, {k("uncertainty_is_probability"), false}};
    }
    for (const auto &p : players) {
        for (const auto &x : p.unknowns) unknowns.insert(x);
        QJsonObject ranks; for (auto it = p.context.ranks.cbegin(); it != p.context.ranks.cend(); ++it) ranks[it.key()] = it.value();
        final.append(QJsonObject{{k("player_key"), p.key}, {k("champion_id"), p.champion}, {k("team"), p.team}, {k("level"), p.level},
            {k("maximum_health"), p.maximumHp}, {k("initial_health"), p.initialHp}, {k("remaining_health"), p.hp}, {k("initially_dead"), p.initiallyDead},
            {k("alive"), p.hp > 1e-9}, {k("death_time"), p.deathTime >= 0 ? QJsonValue(p.deathTime) : QJsonValue(QJsonValue::Null)},
            {k("effective_damage_dealt"), p.dealt}, {k("effective_damage_received"), p.received}, {k("skill_ranks"), ranks},
            {k("shield_damage_dealt"), p.shieldDealt}, {k("shield_absorbed"), support.absorbed(p.key)}, {k("effective_healing_received"), support.healingReceived(p.key)},
            {k("health_cost_received"), support.healthCost(p.key)}, {k("remaining_shield"), support.shieldRemaining(p.key)},
            {k("reference_ranks_assumed"), p.referenceRanks}, {k("augments_complete"), p.augmentsComplete}, {k("unknowns"), codes(p.unknowns)}});
    }
    const int orderAlive = totals.value(k("ORDER")).toObject().value(k("alive")).toInt(), chaosAlive = totals.value(k("CHAOS")).toObject().value(k("alive")).toInt();
    const QString endState = orderAlive == 0 && chaosAlive == 0 ? k("mutual_elimination") : orderAlive == 0 ? k("ORDER_eliminated") : chaosAlive == 0 ? k("CHAOS_eliminated") : k("horizon_reached");
    return {{k("policy"), policy}, {k("opponent_policy"), opponentPolicy}, {k("objective_team"), objectiveTeam},
        {k("objective_value"), objectives.value(objectiveTeam).toObject().value(k("value"))}, {k("elapsed_seconds"), elapsed}, {k("intervals"), intervals},
        {k("end_state"), endState}, {k("players_final"), final}, {k("team_totals"), totals}, {k("objective_components"), objectives},
        {k("eligible_for_action_value_training"), false}, {k("real_world_reward"), QJsonValue::Null},
        {k("training_trace_source"), k("synthetic_numeric_regression_trace")}, {k("reward_semantics"), k("combined_damage_not_objective_reward")},
        {k("trace"), trace}, {k("training_transitions"), transitions}, {k("model_unknowns"), codes(unknowns)}, {k("support"), support.summary()}};
}
}

bool sandboxSkillRanksLegal(const CombatDatabase &db, int champion, int level,
                           const QMap<QString, int> &ranks, QStringList *unknowns) {
    if (!db.champions.contains(champion) || level < 1 || level > 18) return false;
    QMap<QString, int> maximum;
    for (const auto &v : db.champions.value(champion).value(k("spells")).toArray()) {
        const auto spell = v.toObject(); maximum[spell.value(k("slot")).toString()] = spell.value(k("max_rank")).toInt();
    }
    int spent = 0;
    for (auto it = ranks.cbegin(); it != ranks.cend(); ++it) {
        if (!QStringList{k("Q"), k("W"), k("E"), k("R")}.contains(it.key()) || it.value() < 0 || it.value() > maximum.value(it.key(), 6)) return false;
        spent += it.value();
    }
    const bool freeTransformR = (champion == 43 || champion == 60 || champion == 76) && maximum.value(k("R")) == 4;
    const bool ordinary = maximum.value(k("Q")) == 5 && maximum.value(k("W")) == 5 && maximum.value(k("E")) == 5 && maximum.value(k("R")) == 3
        && champion != 126 && champion != 77 && champion != 523;
    if (ordinary || freeTransformR) {
        for (const auto &slot : {k("Q"), k("W"), k("E")}) if (ranks.value(slot) > (level + 1) / 2) return false;
        const int standardR = level >= 16 ? 3 : level >= 11 ? 2 : level >= 6 ? 1 : 0;
        if (ranks.value(k("R")) > standardR + (freeTransformR ? 1 : 0)) return false;
        if (spent > level + (freeTransformR && ranks.value(k("R")) > 0 ? 1 : 0)) return false;
        return true;
    }
    // Jayce's automatic transform, Udyr's four basic stances and Aphelios's
    // stat allocation cannot be certified by the ordinary point budget.
    if (unknowns) unknowns->append(k("special_skill_unlock_schedule_not_fully_validated"));
    return true;
}

QJsonObject runTeamfightSandbox(const Catalog &catalog, const QJsonObject &request) {
    if (!catalog.combat) return invalid(k("combat_database_unavailable"));
    const auto &db = *catalog.combat;
    if (!request.value(k("players")).isArray() || request.value(k("players")).toArray().size() != 10) return invalid(k("exactly_ten_players_required"));
    const double horizon = request.value(k("horizon")).toDouble(8), step = request.value(k("step_seconds")).toDouble(.25);
    if ((request.contains(k("horizon")) && !finiteNumber(request.value(k("horizon")), .5, 40)) ||
        (request.contains(k("step_seconds")) && !finiteNumber(request.value(k("step_seconds")), .1, 1))) return invalid(k("invalid_horizon_or_step"));
    if (request.contains(k("seed")) && (!finiteNumber(request.value(k("seed")), 0, 4294967295.) || std::floor(request.value(k("seed")).toDouble()) != request.value(k("seed")).toDouble())) return invalid(k("invalid_seed"));
    const QString objectiveTeam = request.value(k("objective_team")).toString(k("ORDER"));
    if ((request.contains(k("objective_team")) && !request.value(k("objective_team")).isString()) || (objectiveTeam != k("ORDER") && objectiveTeam != k("CHAOS"))) return invalid(k("invalid_objective_team"));
    QStringList requestedPolicies;
    if (request.contains(k("policies"))) {
        if (!request.value(k("policies")).isArray() || request.value(k("policies")).toArray().isEmpty() || request.value(k("policies")).toArray().size() > 5) return invalid(k("invalid_policies"));
        for (const auto &v : request.value(k("policies")).toArray()) { if (!v.isString() || !policies.contains(v.toString()) || requestedPolicies.contains(v.toString())) return invalid(k("invalid_policy")); requestedPolicies.append(v.toString()); }
    } else {
        const auto policy = request.value(k("policy")).toString(k("focus_damage"));
        if ((request.contains(k("policy")) && !request.value(k("policy")).isString()) || !policies.contains(policy)) return invalid(k("invalid_policy"));
        requestedPolicies.append(policy);
    }
    if (request.contains(k("team_policies")) && !request.value(k("team_policies")).isObject()) return invalid(k("invalid_team_policies"));
    const auto teamPolicies = request.value(k("team_policies")).toObject();
    for (auto it = teamPolicies.begin(); it != teamPolicies.end(); ++it)
        if ((it.key() != k("ORDER") && it.key() != k("CHAOS")) || !it.value().isString() || !policies.contains(it.value().toString())) return invalid(k("invalid_team_policy"));
    // Explicit candidate arrays define the searched side; otherwise an explicit
    // team policy overrides that side's global scenario policy.
    if (!request.contains(k("policies")) && teamPolicies.contains(objectiveTeam)) requestedPolicies = {teamPolicies.value(objectiveTeam).toString()};
    // A policy search holds the opponent policy fixed. A single-policy scenario
    // defaults both teams to that policy, preserving a simple all-hold case.
    const auto enemyPolicyKey = request.contains(k("opponent_policy")) ? k("opponent_policy") : k("enemy_policy");
    auto opponentPolicy = request.value(enemyPolicyKey).toString(request.contains(k("policies")) ? k("focus_damage") : requestedPolicies.first());
    if (request.contains(enemyPolicyKey) && (!request.value(enemyPolicyKey).isString() || !policies.contains(opponentPolicy))) return invalid(k("invalid_opponent_policy"));
    const auto enemyTeam = objectiveTeam == k("ORDER") ? k("CHAOS") : k("ORDER");
    if (teamPolicies.contains(enemyTeam)) opponentPolicy = teamPolicies.value(enemyTeam).toString();
    if ((request.contains(k("opponent_policy")) && !request.value(k("opponent_policy")).isString()) || !policies.contains(opponentPolicy)) return invalid(k("invalid_opponent_policy"));
    Objective weights;
    if (request.contains(k("objective")) && !request.value(k("objective")).isObject()) return invalid(k("invalid_objective"));
    const auto objective = request.value(k("objective")).toObject();
    for (const auto &key : {k("damage_weight"), k("survival_weight"), k("death_penalty"), k("uncertainty_penalty")})
        if (objective.contains(key) && !finiteNumber(objective.value(key), 0, 100)) return invalid(k("invalid_objective_weight"));
    weights.damage = objective.value(k("damage_weight")).toDouble(1); weights.survival = objective.value(k("survival_weight")).toDouble(1);
    weights.death = objective.value(k("death_penalty")).toDouble(2); weights.uncertainty = objective.value(k("uncertainty_penalty")).toDouble(.1);
    QVector<Fighter> players; QMap<QString, int> teams; QJsonArray assumptions;
    for (const auto &v : request.value(k("players")).toArray()) {
        if (!v.isObject()) return invalid(k("player_must_be_object"));
        const auto row = v.toObject(); Fighter p; p.team = row.value(k("team")).toString();
        if ((p.team != k("ORDER") && p.team != k("CHAOS")) || !integer(row.value(k("champion_id")), 1, std::numeric_limits<int>::max(), p.champion) || !db.champions.contains(p.champion) || !integer(row.value(k("level")), 1, 18, p.level)) return invalid(k("invalid_player_champion_team_or_level"));
        ++teams[p.team];
        const auto augmentValue = row.contains(k("augments")) ? row.value(k("augments")) : row.contains(k("augment_ids")) ? row.value(k("augment_ids")) : QJsonValue(QJsonArray{});
        if (!readIds(row.value(k("items")), db.items, p.items, false) || !readIds(augmentValue, db.augments, p.augments, true)) return invalid(k("unknown_or_invalid_loadout_ids"));
        for (int id : p.augments) if (db.augments[id].requiredLevel > p.level) return invalid(k("augment_required_level_not_reached"));
        if (row.contains(k("items_complete")) && (!row.value(k("items_complete")).isBool() || !row.value(k("items_complete")).toBool())) return invalid(k("complete_inventory_required"));
        if (row.contains(k("augments_complete")) && !row.value(k("augments_complete")).isBool()) return invalid(k("invalid_augments_completeness"));
        p.augmentsComplete = row.value(k("augments_complete")).toBool(false);
        if (p.augmentsComplete && !row.contains(k("augments")) && !row.contains(k("augment_ids"))) return invalid(k("complete_augments_require_explicit_array"));
        if (!p.augmentsComplete) p.unknowns.insert(k("unobserved_augments_may_be_missing"));
        p.context = db.context(p.champion, p.level); p.context.discreteSkillCasts = true;
        p.context.ranks = referenceCombatRanks(db, p.champion, p.level);
        const bool suppliedRanks = row.contains(k("ranks")) || row.contains(k("abilities"));
        const auto rankValue = row.contains(k("ranks")) ? row.value(k("ranks")) : row.value(k("abilities"));
        if (suppliedRanks && !rankValue.isObject()) return invalid(k("skill_ranks_must_be_object"));
        const auto supplied = rankValue.toObject(); p.referenceRanks = !suppliedRanks || supplied.size() < 4;
        for (auto it = supplied.begin(); it != supplied.end(); ++it) {
            if (!QStringList{k("Q"), k("W"), k("E"), k("R")}.contains(it.key())) return invalid(k("invalid_skill_slot"));
            const auto raw = it.value().isObject() ? it.value().toObject().value(k("abilityLevel")) : it.value(); int rank = 0, maximum = 6;
            for (const auto &spell : db.champions.value(p.champion).value(k("spells")).toArray()) if (spell.toObject().value(k("slot")) == it.key()) maximum = spell.toObject().value(k("max_rank")).toInt(6);
            if (!integer(raw, 0, maximum, rank)) return invalid(k("invalid_skill_rank")); p.context.ranks[it.key()] = rank;
        }
        QStringList skillLegalityUnknowns;
        if (!sandboxSkillRanksLegal(db, p.champion, p.level, p.context.ranks, &skillLegalityUnknowns)) return invalid(k("skill_rank_points_or_unlock_level_invalid"));
        for (const auto &x : skillLegalityUnknowns) p.unknowns.insert(x);
        if (p.referenceRanks) p.unknowns.insert(k("unobserved_skill_ranks_use_explicit_QWE_priority_reference"));
        if (row.contains(k("execution")) && !row.value(k("execution")).isObject()) return invalid(k("invalid_execution"));
        const auto execution = row.value(k("execution")).toObject();
        for (auto it = execution.begin(); it != execution.end(); ++it)
            if (!QStringList{k("output_factor"), k("damage_taken_factor"), k("support_factor")}.contains(it.key()) || !finiteNumber(it.value(), 0, it.key() == k("damage_taken_factor") ? 2 : 1)) return invalid(k("invalid_execution_factor"));
        p.outputFactor = execution.value(k("output_factor")).toDouble(1); p.takenFactor = execution.value(k("damage_taken_factor")).toDouble(1); p.supportFactor = execution.value(k("support_factor")).toDouble(1);
        p.context.horizon = horizon;
        const auto baseline = evaluateCombat(db, p.context, p.items, p.augments);
        if (!baseline.valid || !std::isfinite(baseline.stats[Stat::Hp]) || baseline.stats[Stat::Hp] <= 0 || !std::isfinite(damage(baseline)) || damage(baseline) < 0) return invalid(k("loadout_numeric_model_unavailable_or_incompatible"));
        p.stats = baseline.stats; p.maximumHp = p.stats[Stat::Hp]; p.threat = damage(baseline) * p.outputFactor;
        for (const auto &x : baseline.unknowns) p.unknowns.insert(x);
        if (row.contains(k("is_dead")) && !row.value(k("is_dead")).isBool()) return invalid(k("invalid_life_state"));
        const bool deadFlag = row.value(k("is_dead")).toBool(false);
        if (row.contains(k("current_health")) && !finiteNumber(row.value(k("current_health")), 0, p.maximumHp)) return invalid(k("invalid_current_health"));
        if (deadFlag && row.contains(k("current_health")) && row.value(k("current_health")).toDouble() > 0) return invalid(k("contradictory_dead_and_positive_health"));
        p.initialHealthAssumed = !row.contains(k("current_health")) && !deadFlag;
        p.hp = p.initialHp = deadFlag ? 0 : p.initialHealthAssumed ? p.maximumHp : row.value(k("current_health")).toDouble();
        p.initiallyDead = p.hp <= 1e-9;
        if (p.initialHealthAssumed) p.unknowns.insert(k("initial_full_health_is_explicit_scenario"));
        QJsonObject canonical{{k("champion"), p.champion}, {k("team"), p.team}, {k("level"), p.level}, {k("hp"), p.initialHp}, {k("execution"), execution}, {k("ranks"), supplied}};
        QJsonArray itemIds, augmentIds; for (int id : p.items) itemIds.append(id); for (int id : p.augments) augmentIds.append(id);
        canonical[k("items")] = itemIds; canonical[k("augments")] = augmentIds;
        p.canonical = QJsonDocument(canonical).toJson(QJsonDocument::Compact); players.append(p);
    }
    if (teams.value(k("ORDER")) != 5 || teams.value(k("CHAOS")) != 5) return invalid(k("exactly_five_players_per_team_required"));
    std::sort(players.begin(), players.end(), [](const Fighter &a, const Fighter &b) { return a.canonical < b.canonical; });
    QMap<QString, int> occurrences;
    for (auto &p : players) {
        // Unique champion/team identities remain stable across gear and
        // execution counterfactuals. Equal duplicate champions are anonymous.
        const auto prefix = p.team + k(":") + QString::number(p.champion);
        p.key = prefix + k(":") + QString::number(++occurrences[prefix]);
        assumptions.append(QJsonObject{{k("player_key"), p.key}, {k("output_factor"), p.outputFactor}, {k("damage_taken_factor"), p.takenFactor},
            {k("support_factor"), p.supportFactor}, {k("support_factor_applied"), false}, {k("source"), k("explicit_request_or_scenario_default")},
            {k("initial_full_health_assumed"), p.initialHealthAssumed}, {k("all_skills_ready_at_origin"), true}});
    }
    QVector<SandboxSupportActor> supportActors;
    for (const auto &p : players) supportActors.append({p.key, p.team, p.champion, p.context, p.stats, p.maximumHp, p.hp, p.supportFactor, p.threat, p.augments});
    SandboxSupportTimeline support; QString supportError;
    if (!support.load(db, request.value(k("support_events")), supportActors, horizon, &supportError)) return invalid(supportError);
    QJsonArray results; QJsonObject best; double bestValue = -std::numeric_limits<double>::infinity();
    for (const auto &policy : requestedPolicies) {
        auto result = simulate(db, players, policy, opponentPolicy, horizon, step, weights, objectiveTeam, support);
        const double value = result.value(k("objective_value")).toDouble();
        if (value > bestValue + 1e-12) { bestValue = value; best = result; }
        results.append(result);
    }
    QJsonObject out = best;
    out[k("schema_version")] = 1; out[k("available")] = true; out[k("status")] = k("conditional_numeric_sandbox");
    out[k("patch")] = catalog.patch; out[k("combat_revision")] = db.revision; out[k("scenario_label")] = request.value(k("scenario_label")).toString();
    out[k("best_policy")] = best.value(k("policy")); out[k("policy_results")] = results;
    out[k("policy_search_scope")] = k("enumerated_abstract_own_team_policies_with_fixed_opponent_policy_not_global_game_optimum");
    out[k("horizon_seconds")] = horizon; out[k("step_seconds")] = step; out[k("deterministic")] = true; out[k("seed_used")] = false;
    out[k("actual_win_probability")] = QJsonValue::Null; out[k("globally_optimal_gameplay")] = false;
    const auto supportResults = best.value(k("support")).toObject().value(k("event_results")).toArray();
    for (int i = 0; i < assumptions.size(); ++i) {
        auto assumption = assumptions[i].toObject(); bool applied = false;
        for (const auto &raw : supportResults) { const auto event = raw.toObject(); applied |= event.value(k("accepted")).toBool() && event.value(k("player_key")) == assumption.value(k("player_key")); }
        assumption[k("support_factor_applied")] = applied; assumptions[i] = assumption;
    }
    out[k("subjective_assumptions")] = assumptions;
    out[k("objective_formula")] = k("J = damage_weight * clamp(max(0,enemy_initial_HP-enemy_remaining_HP-enemy_health_cost)/enemy_maximum_HP,0,1) + survival_weight * clamp(remaining_own_HP/own_maximum_HP,0,1) - death_penalty * new_own_deaths/5 - uncertainty_penalty * uncertain_own_players/5");
    out[k("objective_weights")] = QJsonObject{{k("damage_weight"), weights.damage}, {k("survival_weight"), weights.survival}, {k("death_penalty"), weights.death}, {k("uncertainty_penalty"), weights.uncertainty}};
    out[k("numeric_formula_source")] = k("existing evaluateCombat: champion level stats + legal equipment + modeled augments + skill ranks; cumulative shared-origin differences, with target mitigation");
    out[k("timing_contract")] = k("[0,horizon) shared skill/item clock; explicit support/expiry splits intervals and resolves first; simultaneous interval-end damage settlement with shields before HP; overkill removed proportionally; dead actors stop next interval; target switch never resets casts");
    return out;
}
