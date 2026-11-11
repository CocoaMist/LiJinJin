#include "decision_context.h"
#include "combat_sandbox.h"
#include "growth_objective.h"
#include "teamfight_sandbox.h"
#include "role_guidance.h"
#include <QCache>
#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QString key(const char *s) { return QString::fromLatin1(s); }
QJsonValue canonicalIds(const QJsonValue &v) {
    if (!v.isArray()) return v;
    QList<QJsonValue> values; for (const auto &id : v.toArray()) values.append(id);
    std::sort(values.begin(), values.end(), [](const QJsonValue &a, const QJsonValue &b) {
        return QJsonDocument(QJsonArray{a}).toJson(QJsonDocument::Compact) < QJsonDocument(QJsonArray{b}).toJson(QJsonDocument::Compact);
    });
    QJsonArray out; for (const auto &id : values) out.append(id); return out;
}
double objectiveOffense(const CombatEvaluation &v, const QString &style) {
    Q_UNUSED(style);
    return std::max(1.,v.attackDamage+v.skillDamage+v.procDamage);
}
bool boundedInt(const QJsonValue &v, int lo, int hi) {
    return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() == std::floor(v.toDouble()) && v.toDouble() >= lo && v.toDouble() <= hi;
}
bool unboundTargetCondition(const QString &condition) {
    // Reviewed damage-safety contracts include champion-prefixed identifiers,
    // not just target_* keys. These facts describe a particular recipient/hit,
    // so a player-only row cannot establish them for the neutral model target
    // or each of five opponents. Actor forms, ready buffs and other global
    // conditions are intentionally not excluded by this set.
    static const QSet<QString> targetConditions{
        key("galio_r_landed"), key("xinzhao_w_thrust_hits"), key("leblanc_q_mark_triggered"), key("leblanc_e_tether_completed"),
        key("soraka_e_expiration_hit"), key("nunu_e_root_hits"), key("anivia_q_explosion_hits"), key("shaco_box_hits"), key("shaco_clone_explodes"),
        key("trundle_r_full_drain_completed_for_each_modeled_cast"), key("champion_48_q_empowered_attack_each_modeled_cast_hits"),
        key("champion_53_e_empowered_attack_each_modeled_cast_hits"), key("skarner_wall_pin"), key("actual_q_hit"),
        key("champion_86_q_empowered_attack_each_modeled_cast_hits"), key("leona_q_empowered_attack_hit"), key("leona_e_last_champion_hit"),
        key("leona_r_center_hit"), key("leona_r_outer_hit"), key("talon_w_return_hits"), key("ahri_q_return_hits"), key("lulu_e_enemy_cast"),
        key("draven_q_one_empowered_attack_each_modeled_cast_hits"), key("champion_122_w_empowered_attack_each_modeled_cast_hits"),
        key("yasuo_r_airborne_target_verified"), key("velkoz_w_second_phase_hits"), key("akshan_swing_hit"), key("briar_e_release_hits"),
        key("samira_r_style_ready_single_projectile_hits"), key("reksai_q_one_empowered_attack_each_modeled_cast_hits"),
        key("kalista_e_one_embedded_spear_verified"), key("rakan_r_contact"), key("xayah_e_one_returning_feather_verified"),
        key("sylas_e_chain_hits"), key("neeko_q_second_bloom_hits"), key("rell_e_empowered_attack_explosion_each_modeled_cast_hits"),
        key("sett_q_one_empowered_attack_each_modeled_cast_hits"), key("lillia_q_outer_hits"), key("ksante_w_charge_and_impact_verified"),
        key("smolder_w_explosion_hits"), key("milio_r_allies_in_range"), key("zaahen_w_pull_hits"), key("naafiri_e_second_slash_hits")};
    return condition.startsWith(key("target_")) || condition.contains(key("_target_")) || condition.endsWith(key("_target"))
        || targetConditions.contains(condition);
}
}

PlayerCombatContext playerCombatContext(const CombatDatabase &db, const QJsonObject &player) {
    PlayerCombatContext out;
    out.report = {{key("available"), false}, {key("rank_source"), key("unavailable")},
        {key("provided_rank_slots"), QJsonArray{}}, {key("unknowns"), QJsonArray{}},
        {key("current_action_timeline_observed"), false}};
    auto invalid = [&](const char *reason) { out.report[key("reason")] = key(reason); return out; };
    if (!boundedInt(player.value(key("champion_id")), 1, 10000) || !boundedInt(player.value(key("level")), 1, 18)
        || !db.champions.contains(player.value(key("champion_id")).toInt())) return invalid("player_champion_or_level_invalid");
    out.context = db.context(player.value(key("champion_id")).toInt(), player.value(key("level")).toInt());
    const QStringList abilitySlots{key("Q"), key("W"), key("E"), key("R")};
    QMap<QString, int> observed;
    if (player.contains(key("ranks"))) {
        if (!player.value(key("ranks")).isObject()) return invalid("player_ranks_invalid");
        const auto ranks = player.value(key("ranks")).toObject();
        for (auto it = ranks.begin(); it != ranks.end(); ++it) {
            if (!abilitySlots.contains(it.key()) || !boundedInt(it.value(), 0, 6)) return invalid("player_ranks_invalid");
            observed[it.key()] = it.value().toInt();
        }
    }
    if (player.contains(key("abilities"))) {
        if (!player.value(key("abilities")).isObject()) return invalid("player_abilities_invalid");
        const auto abilities = player.value(key("abilities")).toObject();
        for (const auto &slot : abilitySlots) if (abilities.contains(slot)) {
            const auto ability = abilities.value(slot);
            const auto rank = ability.toObject().value(key("abilityLevel"));
            if (!ability.isObject() || !boundedInt(rank, 0, 6)) return invalid("player_ability_rank_invalid");
            if (observed.contains(slot) && observed.value(slot) != rank.toInt()) return invalid("player_rank_sources_conflict");
            observed[slot] = rank.toInt();
        }
    }
    QStringList unknowns;
    if (!sandboxSkillRanksLegal(db, out.context.championId, out.context.level, observed, &unknowns)) return invalid("player_skill_rank_points_or_unlock_invalid");
    // Fill only unobserved slots. An observed W/E/R allocation can consume the
    // budget otherwise assumed by Q-priority; never overwrite that observation.
    const auto reference = referenceCombatRanks(db, out.context.championId, out.context.level);
    out.context.ranks = observed;
    for (const auto &slot : abilitySlots) if (!observed.contains(slot)) {
        for (int rank = reference.value(slot); rank >= 0; --rank) {
            auto candidate = out.context.ranks; candidate[slot] = rank;
            if (sandboxSkillRanksLegal(db, out.context.championId, out.context.level, candidate)) { out.context.ranks = candidate; break; }
        }
    }
    const auto rankSource = observed.size() == 4 ? key("observed") : observed.isEmpty() ? key("reference") : key("mixed_observed_and_reference");
    if (observed.size() != 4) unknowns.append(key("player_skill_ranks_use_disclosed_reference"));
    auto factKey = [](const QString &name) {
        if (name.isEmpty() || name.size() > 128 || name != name.trimmed()) return false;
        for (const auto c : name) if (c.unicode() < 32 || c.unicode() == 127) return false;
        return true;
    };
    if (player.contains(key("effect_counters"))) {
        if (!player.value(key("effect_counters")).isObject() || player.value(key("effect_counters")).toObject().size() > 256) return invalid("player_effect_counters_invalid");
        const auto counters = player.value(key("effect_counters")).toObject();
        for (auto it = counters.begin(); it != counters.end(); ++it) {
            if (!factKey(it.key()) || !it.value().isDouble() || !std::isfinite(it.value().toDouble()) || std::abs(it.value().toDouble()) > 1e15) return invalid("player_effect_counter_invalid");
            if (it.key().startsWith(key("target_"))) { unknowns.append(key("player_target_counters_not_bound_to_model_target")); continue; }
            out.context.counters[it.key()] = it.value().toDouble();
        }
    } else unknowns.append(key("player_effect_counters_unobserved"));
    QJsonArray isolatedConditions;
    if (player.contains(key("effect_conditions"))) {
        if (!player.value(key("effect_conditions")).isArray() || player.value(key("effect_conditions")).toArray().size() > 256) return invalid("player_effect_conditions_invalid");
        for (const auto &value : player.value(key("effect_conditions")).toArray()) {
            if (!value.isString() || !factKey(value.toString())) return invalid("player_effect_condition_invalid");
            if (unboundTargetCondition(value.toString())) {
                isolatedConditions.append(value);
                unknowns.append(key("player_target_conditions_not_bound_to_model_target"));
                continue;
            }
            if (value.toString() == key("continuous_temporal_verified") || value.toString() == key("continuous_action_verified")) {
                isolatedConditions.append(value);
                unknowns.append(key("player_condition_requires_verified_action_ledger"));
                continue;
            }
            out.context.conditions.insert(value.toString());
        }
    } else unknowns.append(key("player_effect_conditions_unobserved"));
    QJsonArray provided; QJsonObject resolvedRanks;
    for (const auto &slot : abilitySlots) { if (observed.contains(slot)) provided.append(slot); resolvedRanks[slot] = out.context.ranks.value(slot); }
    unknowns.removeDuplicates(); unknowns.sort();
    out.available = true; out.report[key("available")] = true; out.report[key("rank_source")] = rankSource;
    out.report[key("provided_rank_slots")] = provided; out.report[key("resolved_ranks")] = resolvedRanks;
    out.report[key("counter_source")] = player.contains(key("effect_counters")) ? key("provided_current_row") : key("unobserved");
    out.report[key("condition_source")] = player.contains(key("effect_conditions")) ? key("provided_current_row") : key("unobserved");
    out.report[key("isolated_conditions")] = isolatedConditions;
    out.report[key("unknowns")] = QJsonArray::fromStringList(unknowns);
    return out;
}

PlayerCombatContext selfSkillRankContext(const CombatDatabase &db, const GameState &state) {
    auto result = playerCombatContext(db, QJsonObject{{key("champion_id"),state.championId},{key("level"),state.level},
        {key("abilities"),state.abilities},{key("effect_counters"),QJsonObject{}},{key("effect_conditions"),QJsonArray{}}});
    QJsonArray missing;
    for (const auto &slot : {key("Q"),key("W"),key("E"),key("R")}) if (!state.abilities.contains(slot)) missing.append(slot);
    result.report[key("source")] = key("active_player_abilities");
    result.report[key("missing_slots")] = missing;
    result.report[key("complete")] = result.available && result.report.value(key("rank_source")) == key("observed");
    auto unknowns = result.report.value(key("unknowns")).toArray();
    for (int i=0;i<unknowns.size();++i) if (unknowns[i] == key("player_skill_ranks_use_disclosed_reference"))
        unknowns[i] = key("self_skill_ranks_use_partial_qwe_priority_reference");
    result.report[key("unknowns")] = unknowns;
    return result;
}

QJsonArray liveLoadoutBasis(const GameState &state) {
    QList<QJsonObject> rows;
    for (const auto &v : state.players) {
        QJsonObject row; const auto p = v.toObject();
        row[key("row_is_object")] = v.isObject();
        for (const auto &name : {key("champion_id"), key("team"), key("self"), key("level"), key("items"), key("items_complete"), key("augment_ids"), key("augments_complete"),
                 key("abilities"), key("ranks"), key("effect_counters"), key("effect_conditions")}) {
            const auto value = p.value(name);
            row[name] = QJsonObject{{key("present"), p.contains(name)}, {key("value"), name == key("items") || name == key("augment_ids") || name == key("effect_conditions") ? canonicalIds(value) : value}};
        }
        rows.append(row);
    }
    std::sort(rows.begin(), rows.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return QJsonDocument(a).toJson(QJsonDocument::Compact) < QJsonDocument(b).toJson(QJsonDocument::Compact);
    });
    QJsonArray out; for (const auto &row : rows) out.append(row); return out;
}

TeammateCombatContext evaluateTeammateContext(const Catalog &catalog, const GameState &state, const CombatContext &target, const QString &style, bool useCache) {
    TeammateCombatContext out;
    out.report = {{key("available"), false}, {key("modeled_teammates"), 0}, {key("scope"), key("conditional_sum_of_four_other_ally_loadouts")},
        {key("seconds"), target.horizon}, {key("actual_teamfight_prediction"), false}, {key("unknowns"), QJsonArray{}}};
    auto unavailable = [&](const char *reason) { out.report[key("reason")] = key(reason); return out; };
    if (!catalog.combat || !state.fresh() || !modelRulesUsable(catalog, state) || state.gameMode != key("KIWI")) return unavailable("fresh_same_patch_kiwi_required");
    QString ownTeam; int selfCount = 0, allies = 0, enemies = 0;
    for (const auto &v : state.players) if (v.toObject().value(key("self")).toBool()) { ++selfCount; ownTeam = v.toObject().value(key("team")).toString(); }
    if (selfCount != 1 || (ownTeam != key("ORDER") && ownTeam != key("CHAOS")) || state.players.size() != 10) return unavailable("verified_ten_player_team_mapping_required");
    for (const auto &v : state.players) {
        const auto side = v.toObject().value(key("team")).toString();
        if (side == ownTeam) ++allies;
        else if (side == key("ORDER") || side == key("CHAOS")) ++enemies;
    }
    if (allies != 5 || enemies != 5) return unavailable("verified_five_vs_five_required");
    const auto &db = *catalog.combat;
    const QJsonObject basis{{key("loadouts"), liveLoadoutBasis(state)}, {key("context"), state.matchContext.value(key("context_id"))},
        {key("database"), QString::number(quintptr(&db), 16)}, {key("revision"), db.revision}, {key("style"), style},
        {key("seconds"), target.horizon}, {key("discrete_casts"),target.discreteSkillCasts}, {key("armor"), target.targetArmor}, {key("mr"), target.targetMr}, {key("hp"), target.targetHp}, {key("ms"), target.targetMs}};
    const auto bytes = QJsonDocument(basis).toJson(QJsonDocument::Compact);
    static thread_local QCache<QByteArray, TeammateCombatContext> cache(128);
    if (useCache) if (const auto *found = cache.object(bytes)) return *found;
    QJsonArray rows; QStringList unknowns{key("full_health_reference_without_complete_action_timelines"), key("ally_auras_controls_targeting_and_heal_sharing_not_simulated")};
    int missingAugments = 0;
    for (const auto &v : state.players) {
        const auto p = v.toObject(); if (p.value(key("team")) != ownTeam || p.value(key("self")).toBool()) continue;
        if (!boundedInt(p.value(key("champion_id")), 1, 10000) || !boundedInt(p.value(key("level")), 1, 18)
            || !db.champions.contains(p.value(key("champion_id")).toInt()) || !p.value(key("items_complete")).toBool()
            || !p.value(key("items")).isArray() || p.value(key("items")).toArray().size() > 6) return unavailable("teammate_loadout_incomplete");
        QList<int> items, augments;
        for (const auto &id : p.value(key("items")).toArray()) {
            if (!boundedInt(id, 1, 1000000) || !db.items.contains(id.toInt())) return unavailable("teammate_item_unknown");
            items.append(id.toInt());
        }
        if (p.contains(key("augment_ids")) && (!p.value(key("augment_ids")).isArray() || p.value(key("augment_ids")).toArray().size() > 12)) return unavailable("teammate_augment_list_invalid");
        for (const auto &id : p.value(key("augment_ids")).toArray()) {
            if (!boundedInt(id, 1, 1000000) || !db.augments.contains(id.toInt()) || augments.contains(id.toInt())) return unavailable("teammate_augment_unknown_or_duplicate");
            augments.append(id.toInt());
        }
        if (!p.value(key("augments_complete")).toBool() || !p.value(key("augment_ids")).isArray()) ++missingAugments;
        const auto playerContext = playerCombatContext(db, p);
        if (!playerContext.available) { out.report[key("invalid_player_context")] = playerContext.report; return unavailable("teammate_current_context_invalid"); }
        auto ctx = playerContext.context; ctx.horizon = target.horizon;
        for (const auto &value : playerContext.report.value(key("unknowns")).toArray()) unknowns.append(value.toString());
        ctx.discreteSkillCasts=target.discreteSkillCasts;
        ctx.targetArmor = target.targetArmor; ctx.targetMr = target.targetMr; ctx.targetHp = target.targetHp; ctx.targetMs = target.targetMs;
        const auto result = evaluateCombat(db, ctx, items, augments, false, style);
        if (!result.valid || !std::isfinite(result.survival) || result.survival <= 0) return unavailable("teammate_model_unavailable");
        // Allies contribute their complete modeled output, regardless of self's
        // preferred attack/spell build. A tank's preference must not erase ADCs.
        const double allyOutput=std::max(1.,result.attackDamage+result.skillDamage+result.procDamage);
        out.teammates.append(result); out.offense += allyOutput; out.survival += result.survival;
        if (!std::isfinite(out.offense) || !std::isfinite(out.survival)) return unavailable("invalid_team_numeric_result");
        unknowns.append(result.unknowns);
        rows.append(QJsonObject{{key("champion_id"), ctx.championId}, {key("level"), ctx.level}, {key("objective_output"), allyOutput}, {key("reference_effective_hp"), result.survival},
            {key("combat_context"), playerContext.report}});
    }
    if (out.teammates.size() != 4) return unavailable("four_other_allies_required");
    out.available = true; if (missingAugments) unknowns.append(key("teammate_augments_unobserved_or_incomplete"));
    // Roles remain descriptive data elsewhere; neither a tank label nor a
    // former style control may change the candidate comparison weights.
    out.survivalWeight=-1;
    out.report[key("role_weight_policy")]=key("disabled_shared_frozen_growth_objective_metrics");
    out.report[key("role_weight_is_learned")]=false;
    out.report[key("role_matching_enabled")]=false;out.report[key("fixed_direction_matching_enabled")]=false;
    out.report[key("purchase_life_state_policy")]=key("next_full_team_fight; temporary_deaths_do_not_change_purchase_weights");
    out.report[key("survival_weight")]=QJsonValue(QJsonValue::Null);
    unknowns.removeDuplicates(); unknowns.sort();
    out.report[key("available")] = true; out.report[key("modeled_teammates")] = 4;
    out.report[key("unobserved_augment_players")] = missingAugments; out.report[key("unknowns")] = QJsonArray::fromStringList(unknowns);
    out.report[key("teammates")] = rows; out.report[key("other_ally_objective_output")] = out.offense; out.report[key("other_ally_reference_effective_hp")] = out.survival;
    if (useCache) cache.insert(bytes, new TeammateCombatContext(out)); return out;
}

QJsonObject compareTeamObjective(const TeammateCombatContext &team, const CombatEvaluation &before, const CombatEvaluation &after,
    const QString &style, double beforeHealth, double afterHealth, double otherHealth) {
    QJsonObject out{{key("available"), false}, {key("score_gain"), QJsonValue(QJsonValue::Null)},
        {key("objective"), key("marginal_team_output_and_durability")}, {key("win_probability"), false}};
    if (!team.available || !before.valid || !after.valid || before.score <= 0 || after.score <= 0 || before.survival <= 0 || after.survival <= 0) return out;
    const double a = objectiveOffense(before, style), b = objectiveOffense(after, style);
    const double weight=growthObjectiveMetricWeight(before,key("mixed_effective_hp")),offenseWeight=growthObjectiveMetricWeight(before,key("modeled_output"));
    const double ah = beforeHealth < 0 ? before.survival : beforeHealth, bh = afterHealth < 0 ? after.survival : afterHealth, oh = otherHealth < 0 ? team.survival : otherHealth;
    if (!std::isfinite(ah) || !std::isfinite(bh) || !std::isfinite(oh) || ah <= 0 || bh <= 0 || oh < 0) return out;
    const double offenseRatio = (team.offense + b) / (team.offense + a), healthRatio = (oh + bh) / (oh + ah);
    // Retain the core's small movement/sustain preference without counting its
    // individual output and durability for a second time.
    const double preferenceRatio = (after.score / before.score) / (std::pow(b / a,offenseWeight) * std::pow(after.survival / before.survival,weight));
    const double score = std::pow(offenseRatio,offenseWeight) * std::pow(healthRatio,weight) * preferenceRatio - 1;
    if (!std::isfinite(score)) return out;
    out[key("available")] = true; out[key("score_gain")] = score;
    out[key("team_output_gain")] = offenseRatio - 1; out[key("team_durability_gain")] = healthRatio - 1;
    out[key("self_output_share")] = a / (team.offense + a); out[key("self_durability_share")] = ah / (oh + ah);
    out[key("survival_weight")] = weight; out[key("offense_weight")] = offenseWeight;
    out[key("skill_cycle_weight")]=growthObjectiveMetricWeight(before,key("skill_cycle_capacity"));
    out[key("role_matching_enabled")]=false;out[key("fixed_direction_matching_enabled")]=false;out[key("role_adjustment")]=0;
    out[key("weights_frozen_before_candidate")]=true;
    return out;
}

QJsonObject compareHistoricalTeamOutputObjective(const TeammateCombatContext &team,
    const CombatEvaluation &before,const CombatEvaluation &after,const QString &style,
    double selfOutputShare,double beforeHealth,double afterHealth,double otherHealth) {
    auto out=compareTeamObjective(team,before,after,style,beforeHealth,afterHealth,otherHealth);
    if(!out.value(key("available")).toBool()||!std::isfinite(selfOutputShare)||selfOutputShare<0||selfOutputShare>1)
        return {{key("available"),false},{key("score_gain"),QJsonValue(QJsonValue::Null)}};
    const double beforeRatio=out.value(key("team_output_gain")).toDouble()+1;
    const double candidateRatio=objectiveOffense(after,style)/objectiveOffense(before,style);
    const double ratio=1+selfOutputShare*(candidateRatio-1),weight=out.value(key("offense_weight")).toDouble();
    const double score=(out.value(key("score_gain")).toDouble()+1)*std::pow(ratio/beforeRatio,weight)-1;
    if(!std::isfinite(score)||ratio<=0||beforeRatio<=0)
        return {{key("available"),false},{key("score_gain"),QJsonValue(QJsonValue::Null)}};
    out[key("score_gain")]=score;out[key("team_output_gain")]=ratio-1;
    out[key("modeled_self_output_share")]=out.value(key("self_output_share"));
    out[key("self_output_share")]=selfOutputShare;
    out[key("historical_output_share_sensitivity")]=true;
    out[key("individual_output_replaced")]=false;out[key("damage_taken_is_current_hp")]=false;
    out[key("current_output_share_observed")]=false;out[key("causal_effect_known")]=false;
    return out;
}

namespace {
QJsonObject windowScenario(const QJsonObject &detail,const QString &name,
                          const QString &scoreField,const QString &countField,bool fiveTargets) {
    QJsonObject out{{key("scenario"),name},{key("available"),false}};
    const auto windows=detail.value(key("windows")).toArray();
    if(windows.size()!=3){out[key("reason")]=key("three_windows_required");return out;}
    QMap<int,double> values;double sum=0;
    for(const auto &v:windows){
        const auto row=v.toObject();const auto seconds=row.value(key("seconds")),score=row.value(scoreField);
        const int duration=seconds.toInt(-1);
        const auto count=row.value(countField);
        if(!seconds.isDouble()||seconds.toDouble()!=duration||(duration!=3&&duration!=8&&duration!=20)
            ||values.contains(duration)||!score.isDouble()||!std::isfinite(score.toDouble())
            ||(fiveTargets&&(!count.isDouble()||count.toDouble()!=5.))){
            out[key("reason")]=key("invalid_or_incomplete_window");return out;
        }
        values.insert(duration,score.toDouble());sum+=score.toDouble()/3.;
    }
    if(!std::isfinite(sum)){out[key("reason")]=key("non_finite_aggregate");return out;}
    QJsonArray cells;double low=std::numeric_limits<double>::infinity(),high=-low;
    for(auto it=values.cbegin();it!=values.cend();++it){
        cells.append(QJsonObject{{key("seconds"),it.key()},{key("score_gain"),it.value()}});
        low=std::min(low,it.value());high=std::max(high,it.value());
    }
    out[key("available")]=true;out[key("score_gain")]=sum;out[key("cells")]=cells;
    out[key("minimum_window_gain")]=low;out[key("maximum_window_gain")]=high;
    out[key("gain_sign_reversal")]=low<0&&high>0;
    return out;
}
}

QJsonObject summarizeCombatScenarios(const QJsonObject &detail,bool fiveTargets) {
    QJsonObject out{{key("schema_version"),1},{key("available"),false},{key("complete"),false},
        {key("score_gain"),QJsonValue(QJsonValue::Null)},{key("scenarios"),QJsonArray{}},
        {key("aggregation"),key("minimum_complete_three_window_scenario_means")},
        {key("conditional_model_only"),true},{key("scenario_probabilities_known"),false},
        {key("five_targets_required"),fiveTargets}};
    QJsonArray scenarios,missing;bool complete=detail.value(key("available")).isBool()&&detail.value(key("available")).toBool();
    const QString currentField=fiveTargets?key("team_score_gain"):key("score_gain");
    auto current=windowScenario(detail,key("current_model"),currentField,key("team_supported_targets"),fiveTargets);
    // Single-actor offline sandboxes can still provide the team objective.
    if(!fiveTargets){
        bool team=true;for(const auto &v:detail.value(key("windows")).toArray())team&=v.toObject().value(key("team_score_gain")).isDouble();
        if(team)current=windowScenario(detail,key("current_model"),key("team_score_gain"),key("team_supported_targets"),false);
    }
    if(current.value(key("available")).toBool())scenarios.append(current);
    else {complete=false;missing.append(key("current_model"));}
    const QList<QStringList> fields{
        {key("enemy_pressure"),key("historical_pressure_scenario"),key("historical_pressure_team_score_gain"),key("historical_pressure_supported_targets")},
        {key("ally_output"),key("historical_ally_output_scenario"),key("historical_ally_output_team_score_gain"),key("historical_ally_output_supported_targets")},
        {key("joint_roster"),key("historical_roster_scenario"),key("historical_joint_team_score_gain"),key("historical_joint_supported_targets")}};
    for(const auto &f:fields){
        const auto report=detail.value(f[1]).toObject();
        if(!report.value(key("available")).toBool()||(f[0]==key("joint_roster")&&!report.value(key("combined_ten_player_sensitivity")).toBool()))continue;
        const auto scenario=windowScenario(detail,f[0],f[2],f[3],true);
        if(scenario.value(key("available")).toBool())scenarios.append(scenario);
        else {complete=false;missing.append(f[0]);}
    }
    out[key("scenarios")]=scenarios;out[key("incomplete_scenarios")]=missing;
    out[key("available")]=current.value(key("available"));out[key("complete")]=complete;
    if(complete){
        double worst=std::numeric_limits<double>::infinity();QString selected;
        for(const auto &v:scenarios){const auto scenario=v.toObject();const double score=scenario.value(key("score_gain")).toDouble();
            if(score<worst){worst=score;selected=scenario.value(key("scenario")).toString();}}
        out[key("score_gain")]=worst;out[key("selected_scenario")]=selected;
    }
    return out;
}

QJsonObject compareCombatScenarioCandidates(const QJsonArray &candidates) {
    QJsonObject out{{key("schema_version"),1},{key("available"),false},{key("ranking_override"),false},
        {key("conditional_model_only"),true},{key("scenario_probabilities_known"),false},
        {key("regret_unit"),key("model_score_gain_percentage_points")}};
    const auto fail=[&](const char *reason){out[key("reason")]=key(reason);return out;};
    if(candidates.size()<2||candidates.size()>12)return fail("candidate_count_out_of_bounds");
    QMap<int,QMap<QString,double>> matrix;QStringList axes;
    for(const auto &v:candidates){
        const auto candidate=v.toObject();const auto id=candidate.value(key("candidate_id"));
        if(!id.isDouble()||id.toDouble()!=id.toInt()||id.toInt()<=0||matrix.contains(id.toInt()))return fail("candidate_identity_invalid");
        const auto summary=candidate.value(key("scenario_summary")).toObject();
        if(!candidate.value(key("comparison_eligible")).toBool()||!summary.value(key("complete")).toBool())return fail("candidate_comparison_incomplete");
        QMap<QString,double> scores;QSet<QString> names;
        const auto scenarios=summary.value(key("scenarios")).toArray();
        if(scenarios.isEmpty()||scenarios.size()>4)return fail("scenario_count_invalid");
        for(const auto &s:scenarios){const auto scenario=s.toObject();const auto name=scenario.value(key("scenario")).toString();
            if(!QSet<QString>{key("current_model"),key("enemy_pressure"),key("ally_output"),key("joint_roster")}.contains(name)||names.contains(name))return fail("scenario_identity_invalid");
            names.insert(name);QSet<int> durations;const auto cells=scenario.value(key("cells")).toArray();
            if(cells.size()!=3)return fail("scenario_cells_incomplete");
            for(const auto &cell:cells){const auto row=cell.toObject();const auto seconds=row.value(key("seconds")),gain=row.value(key("score_gain"));
                const int duration=seconds.toInt(-1);
                if(!seconds.isDouble()||seconds.toDouble()!=duration||(duration!=3&&duration!=8&&duration!=20)||durations.contains(duration)
                    ||!gain.isDouble()||!std::isfinite(gain.toDouble())||std::abs(gain.toDouble())>1e6)return fail("scenario_cell_invalid");
                durations.insert(duration);scores.insert(name+QLatin1Char(':')+QString::number(duration),gain.toDouble());
            }
        }
        if(!names.contains(key("current_model")))return fail("current_scenario_required");
        if(matrix.isEmpty())axes=scores.keys();else if(axes!=scores.keys())return fail("candidate_scenario_axes_mismatch");
        matrix.insert(id.toInt(),scores);
    }
    QMap<QString,double> best;for(const auto &axis:axes){double value=-std::numeric_limits<double>::infinity();
        for(const auto &scores:matrix)value=std::max(value,scores.value(axis));best.insert(axis,value);}
    constexpr double displayTolerance=.0005;
    const QString shortAxis=key("current_model:3"),longAxis=key("current_model:20");
    bool hasSharedLeader=false;
    for(const auto &scores:matrix){
        hasSharedLeader|=best.value(shortAxis)-scores.value(shortAxis)<=displayTolerance
            &&best.value(longAxis)-scores.value(longAxis)<=displayTolerance;
    }
    const bool durationTradeoff=matrix.size()==3&&!hasSharedLeader;
    QJsonArray rows,pareto,minimax;double leastRegret=std::numeric_limits<double>::infinity();
    constexpr double tolerance=1e-9;
    for(auto it=matrix.cbegin();it!=matrix.cend();++it){
        QJsonArray dominatedBy;double regret=0;QString worstAxis;
        for(const auto &axis:axes){const double delta=100*(best.value(axis)-it.value().value(axis));if(delta>regret){regret=delta;worstAxis=axis;}}
        for(auto other=matrix.cbegin();other!=matrix.cend();++other){if(other.key()==it.key())continue;bool all=true,strict=false;
            for(const auto &axis:axes){const double delta=other.value().value(axis)-it.value().value(axis);all&=delta>=0;strict|=delta>tolerance;}
            if(all&&strict)dominatedBy.append(other.key());}
        if(dominatedBy.isEmpty())pareto.append(it.key());
        if(regret<leastRegret-tolerance){leastRegret=regret;minimax=QJsonArray{it.key()};}
        else if(std::abs(regret-leastRegret)<=tolerance)minimax.append(it.key());
        QJsonObject row{{key("candidate_id"),it.key()},{key("dominated_by"),dominatedBy},
            {key("maximum_regret_points"),regret},{key("worst_regret_axis"),worstAxis}};
        if(matrix.size()==3){
            QJsonObject retained;QJsonArray leads;
            for(const auto &axis:axes){
                double kept=-std::numeric_limits<double>::infinity();
                for(auto other=matrix.cbegin();other!=matrix.cend();++other){
                    if(other.key()!=it.key())kept=std::max(kept,other.value().value(axis));
                }
                retained[axis]=QJsonObject{{key("current_best_points"),100*best.value(axis)},
                    {key("retained_best_points"),100*kept},{key("floor_delta_points"),100*(kept-best.value(axis))}};
            }
            for(const int seconds:{3,8,20}){
                const auto axis=key("current_model:")+QString::number(seconds);
                if(best.value(axis)-it.value().value(axis)<=displayTolerance)leads.append(seconds);
            }
            row[key("current_model_leading_seconds")]=leads;
            row[key("single_slot_retained_by_axis")]=retained;
        }
        rows.append(row);
    }
    out[key("available")]=true;out[key("candidate_count")]=matrix.size();out[key("axis_count")]=axes.size();
    out[key("candidates")]=rows;out[key("pareto_candidate_ids")]=pareto;out[key("minimax_regret_candidate_ids")]=minimax;
    out[key("minimum_maximum_regret_points")]=leastRegret;
    out[key("tradeoff_present")]=pareto.size()>1;
    out[key("current_model_duration_tradeoff")]=durationTradeoff;
    return out;
}

QJsonObject summarizeRosterHistoryScenarios(const QJsonObject &detail) {
    QJsonObject out{{key("available"),false},{key("score_gain"),QJsonValue(QJsonValue::Null)},
        {key("scenarios"),QJsonArray{}},{key("aggregation"),key("minimum_complete_three_window_scenario_means")}};
    const auto windows=detail.value(key("windows")).toArray();if(windows.size()!=3)return out;
    QJsonArray summaries;double worst=std::numeric_limits<double>::infinity();QString selected;
    const QList<QStringList> fields{
        {key("enemy_pressure"),key("historical_pressure_scenario"),key("historical_pressure_team_score_gain"),key("historical_pressure_supported_targets")},
        {key("ally_output"),key("historical_ally_output_scenario"),key("historical_ally_output_team_score_gain"),key("historical_ally_output_supported_targets")},
        {key("joint_roster"),key("historical_roster_scenario"),key("historical_joint_team_score_gain"),key("historical_joint_supported_targets")}};
    for(const auto &f:fields){
        const auto report=detail.value(f[1]).toObject();
        if(!report.value(key("available")).toBool()||(f[0]==key("joint_roster")&&!report.value(key("combined_ten_player_sensitivity")).toBool()))continue;
        const auto scenario=windowScenario(detail,f[0],f[2],f[3],true);
        if(!scenario.value(key("available")).toBool())continue;
        const double mean=scenario.value(key("score_gain")).toDouble();
        summaries.append(QJsonObject{{key("scenario"),f[0]},{key("score_gain"),mean},{key("windows"),3},{key("supported_target_window_pairs"),15}});
        if(mean<worst){worst=mean;selected=f[0];}
    }
    out[key("scenarios")]=summaries;
    if(!selected.isEmpty()){out[key("available")]=true;out[key("score_gain")]=worst;out[key("selected_scenario")]=selected;}
    return out;
}

QString resolvedCombatPreference(const Catalog &catalog,const GameState &state,const QString &style) {
    Q_UNUSED(catalog);Q_UNUSED(state);Q_UNUSED(style);
    return growthCombatPreference();
}

QJsonObject applyOpponentLoadoutContext(const Catalog &catalog,const GameState &state,CombatContext &context) {
    QJsonObject out{{QStringLiteral("source"),QStringLiteral("reference_target")},{QStringLiteral("available"),false},
        {QStringLiteral("exact_live_resistances"),false},{QStringLiteral("target_current_health_known"),false}};
    if(!catalog.combat||!state.fresh()||!modelRulesUsable(catalog,state))return out;
    QString team;int selfCount=0;
    for(const auto &value:state.players)if(value.toObject().value(QStringLiteral("self")).toBool()){++selfCount;team=value.toObject().value(QStringLiteral("team")).toString();}
    if(selfCount!=1||(team!=QStringLiteral("ORDER")&&team!=QStringLiteral("CHAOS")))return out;
    const auto &db=*catalog.combat;double armor=0,mr=0,hp=0;int known=0,unknown=0,unobservedAugments=0;QJsonArray contexts;
    for(const auto &value:state.players){const auto row=value.toObject();const auto side=row.value(QStringLiteral("team")).toString();
        if(side==team)continue;if(side!=QStringLiteral("ORDER")&&side!=QStringLiteral("CHAOS")){++unknown;continue;}
        const int cid=row.value(QStringLiteral("champion_id")).toInt(),level=row.value(QStringLiteral("level")).toInt();
        if(!db.champions.contains(cid)||level<1||level>18||!row.value(QStringLiteral("items")).isArray()||!row.value(QStringLiteral("items_complete")).toBool()){++unknown;continue;}
        QList<int> items,augments;bool valid=true;
        for(const auto &id:row.value(QStringLiteral("items")).toArray()){if(!id.isDouble()||id.toDouble()!=id.toInt()||!db.items.contains(id.toInt())){valid=false;break;}items.append(id.toInt());}
        for(const auto &id:row.value(QStringLiteral("augment_ids")).toArray()){if(!id.isDouble()||id.toDouble()!=id.toInt()||!db.augments.contains(id.toInt())){valid=false;break;}augments.append(id.toInt());}
        if(!valid){++unknown;continue;}
        const auto playerContext=playerCombatContext(db,row);
        contexts.append(QJsonObject{{key("champion_id"),cid},{key("combat_context"),playerContext.report}});
        if(!playerContext.available){++unknown;continue;}
        const auto result=evaluateCombat(db,playerContext.context,items,augments);
        if(!result.valid||!std::isfinite(result.stats[Stat::Armor])||!std::isfinite(result.stats[Stat::Mr])||!std::isfinite(result.stats[Stat::Hp])){++unknown;continue;}
        armor+=result.stats[Stat::Armor];mr+=result.stats[Stat::Mr];hp+=result.stats[Stat::Hp];++known;
        if(!row.value(QStringLiteral("augments_complete")).toBool())++unobservedAugments;
    }
    if(known==5&&unknown==0){context.targetArmor=armor/known;context.targetMr=mr/known;context.targetHp=hp/known;
        out[QStringLiteral("source")]=QStringLiteral("observed_opponent_loadouts_estimate");out[QStringLiteral("available")]=true;}
    out[QStringLiteral("known_enemy_builds")]=known;out[QStringLiteral("unknown_enemy_builds")]=unknown;
    out[QStringLiteral("unobserved_enemy_augment_lists")]=unobservedAugments;
    out[QStringLiteral("roster_complete")]=known==5&&unknown==0;
    out[QStringLiteral("partial_observation_not_applied")]=known>0&&(known!=5||unknown>0);
    out[QStringLiteral("enemy_contexts")]=contexts;
    out[QStringLiteral("armor")]=context.targetArmor;out[QStringLiteral("magic_resist")]=context.targetMr;out[QStringLiteral("max_health")]=context.targetHp;
    return out;
}
