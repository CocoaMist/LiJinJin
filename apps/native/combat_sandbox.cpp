#include "combat_sandbox.h"
#include "combat_unknown_policy.h"
#include "combat_observation_scope.h"
#include "combat.h"
#include "decision_context.h"
#include "growth.h"
#include "growth_objective.h"
#include "headliner.h"
#include "mechanic_actions.h"
#include "mechanic_triggers.h"
#include <QDataStream>
#include <QIODevice>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QString k(const char *s) { return QString::fromLatin1(s); }
QJsonValue nil() { return QJsonValue(QJsonValue::Null); }
QJsonValue number(double n) { return std::isfinite(n) ? QJsonValue(n) : nil(); }
QJsonArray ids(const QList<int> &values) { QJsonArray a; for (int id : values) a.append(id); return a; }
QJsonArray codes(QStringList values) { values.removeDuplicates(); values.sort(); return QJsonArray::fromStringList(values); }
double output(const CombatEvaluation &e) { return e.attackDamage + e.skillDamage + e.procDamage; }
QJsonValue gain(double before, double after, bool valid) {
    return valid && std::isfinite(before) && before > 0 && std::isfinite(after) ? number(after / before - 1) : nil();
}
bool integer(const QJsonValue &v, int lo, int hi, int *out = nullptr) {
    if (!v.isDouble() || !std::isfinite(v.toDouble()) || v.toDouble() < lo || v.toDouble() > hi
        || std::floor(v.toDouble()) != v.toDouble()) return false;
    if (out) *out = int(v.toDouble());
    return true;
}
bool finiteTree(const QJsonValue &v, int depth = 0) {
    if (depth > 12 || v.isNull() || v.isUndefined()) return false;
    if (v.isDouble()) return std::isfinite(v.toDouble());
    if (v.isObject()) { const auto o = v.toObject(); for (auto it = o.begin(); it != o.end(); ++it) if (!finiteTree(it.value(), depth + 1)) return false; }
    if (v.isArray()) for (const auto &x : v.toArray()) if (!finiteTree(x, depth + 1)) return false;
    return true;
}
QMap<QString, int> referenceRanks(const CombatDatabase &db, int champion, int level) {
    QMap<QString, int> maximum, ranks;
    for (const auto &v : db.champions.value(champion).value(k("spells")).toArray()) {
        const auto o = v.toObject(); maximum[o.value(k("slot")).toString()] = o.value(k("max_rank")).toInt();
    }
    const int r = std::min(maximum.value(k("R")), level >= 16 ? 3 : level >= 11 ? 2 : level >= 6 ? 1 : 0);
    int remaining = level - r;
    for (const auto &slot : {k("Q"), k("W"), k("E")}) {
        ranks[slot] = std::max(0, std::min({maximum.value(slot), remaining, (level + 1) / 2}));
        remaining -= ranks[slot];
    }
    ranks[k("R")] = r; return ranks;
}
QJsonObject ranksJson(const QMap<QString, int> &ranks) { QJsonObject o; for (auto it = ranks.begin(); it != ranks.end(); ++it) o[it.key()] = it.value(); return o; }
bool readIds(const QJsonValue &raw, const QMap<int, CombatEntry> &entries, QList<int> &out, int limit) {
    if (!raw.isArray() || raw.toArray().size() > limit) return false;
    for (const auto &v : raw.toArray()) { int id = 0; if (!integer(v, 1, std::numeric_limits<int>::max(), &id) || !entries.contains(id)) return false; out.append(id); }
    return true;
}
bool hasActions(const CombatEntry &entry) {
    if (!entry.actions.isEmpty()) return true;
    for (const auto &v : entry.eventRules) if (!v.toObject().value(k("actions")).toArray().isEmpty()) return true;
    return false;
}
bool hasCopyTrue(const CombatDatabase &db, const QList<int> &items, const QList<int> &augments) {
    for (int pass = 0; pass < 2; ++pass) {
        const auto &entries = pass ? db.augments : db.items; const auto &list = pass ? augments : items;
        for (int id : list) for (const auto &v : entries.value(id).effects) if (v.toObject().value(k("op")) == k("extra_true")) return true;
    }
    return false;
}
// These two temporary analytical adapters only suppress a damage type. They
// are not candidate equipment, observations, or claims about any game mechanic.
int addTypeProbe(CombatDatabase &db, const QString &type, int start) {
    int id = start; while (db.augments.contains(id)) --id;
    CombatEntry probe; probe.id = id; probe.coverage = k("modeled_numeric_effects");
    probe.effects.append(QJsonObject{{k("op"), k("damage_type_amp")}, {k("stat"), type}, {k("value"), -1}});
    db.augments.insert(id, probe); return id;
}
struct Enemy {
    int index = 0, champion = 0, level = 0;
    bool valid = false, itemsComplete = false, augmentsComplete = false, sharesKnown = false;
    QList<int> items, augments;
    CombatContext context;
    QJsonObject contextReport;
    CombatEvaluation reference;
    double physical = 0, magic = 0, pure = 0;
    QStringList unknowns;
};
void writeJson(QDataStream &stream, const QJsonObject &value) { stream << QJsonDocument(value).toJson(QJsonDocument::Compact); }
void writeJson(QDataStream &stream, const QJsonArray &value) { stream << QJsonDocument(value).toJson(QJsonDocument::Compact); }
QStringList ordered(const QSet<QString> &values) { auto list = values.values(); list.sort(); return list; }
void writeStats(QDataStream &stream, const CombatStats &stats) { for (double value : stats.values) stream << value; }
void writeEntry(QDataStream &stream, const CombatEntry &entry) {
    stream << entry.id << entry.price << entry.requiredLevel << entry.eligible << entry.boots << entry.finished;
    writeStats(stream, entry.stats);
    writeJson(stream, entry.values); writeJson(stream, entry.calculations); writeJson(stream, entry.triggerContract);
    writeJson(stream, entry.effects); writeJson(stream, entry.actions); writeJson(stream, entry.eventRules);
    writeJson(stream, entry.growthStackInputs); writeJson(stream, entry.temporalBindingsV4);
    stream << entry.coverage << entry.groups << entry.from;
}
// Exact, work-local bytes, not a digest or the intentionally coarse UI
// fingerprint. Preserve row order, duplicate IDs, absent JSON fields, observed
// clocks, event facts and all historical/tactical evidence. Catalog records
// read by the observed baseline are included as complete records as well.
QByteArray snapshotBasis(const Catalog &catalog, const GameState &state, const QList<int> &augments, const QString &style) {
    QByteArray bytes; QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream << qint32(1) << qint32(state.source) << state.phase << state.message << state.gameMode << state.observedPatch
        << state.queueId << state.championId << state.level << state.gameTime << state.gold << state.ap << state.ad << state.hp
        << state.goldKnown << state.effectCounters << ordered(state.effectConditions) << state.mechanicBaselineTime
        << state.owned << state.bench << state.offers << state.selected << state.received << style << augments;
    writeJson(stream, state.abilities); writeJson(stream, state.liveStats); writeJson(stream, state.mechanicObservations);
    writeJson(stream, state.headlinerContext); writeJson(stream, state.matchContext); writeJson(stream, state.recommendationEvidence);
    writeJson(stream, state.tacticalHistory); writeJson(stream, state.players);
    const auto &db = *catalog.combat;
    stream << quint64(quintptr(&catalog)) << quint64(quintptr(&db)) << db.revision << db.patch
        << catalog.root << catalog.patch << catalog.catalogPath;
    writeJson(stream, catalog.runtimeManifest);
    QSet<int> champions{state.championId}, items, selected;
    for (int id : state.owned) items.insert(id);
    for (int id : augments) selected.insert(id);
    for (const auto &value : state.players) {
        const auto row = value.toObject(); champions.insert(row.value(k("champion_id")).toInt());
        for (const auto &id : row.value(k("items")).toArray()) items.insert(id.toInt());
        for (const auto &id : row.value(k("augment_ids")).toArray()) selected.insert(id.toInt());
    }
    auto heroIds = champions.values(), itemIds = items.values(), augmentIds = selected.values();
    std::sort(heroIds.begin(), heroIds.end()); std::sort(itemIds.begin(), itemIds.end()); std::sort(augmentIds.begin(), augmentIds.end());
    stream << heroIds << itemIds << augmentIds;
    for (int id : heroIds) { stream << db.champions.contains(id) << catalog.champions.contains(id); writeJson(stream, db.champions.value(id)); writeJson(stream, catalog.champions.value(id)); }
    for (int id : itemIds) { stream << db.items.contains(id); if (db.items.contains(id)) writeEntry(stream, db.items.value(id)); }
    for (int id : augmentIds) { stream << db.augments.contains(id); if (db.augments.contains(id)) writeEntry(stream, db.augments.value(id)); writeJson(stream, catalog.augments.value(id)); }
    return bytes;
}
QByteArray contextBasis(const CombatContext &ctx) {
    QByteArray bytes; QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream << ctx.championId << ctx.level << ctx.ranged << ctx.usesMana;
    for (const auto *stats : {&ctx.base, &ctx.observedOffset, &ctx.observedInputOffset, &ctx.eventStats, &ctx.permanentEventStats}) writeStats(stream, *stats);
    writeJson(stream, ctx.formStatTransforms);
    stream << ctx.baseAs << ctx.asRatio << ctx.ranks << ctx.counters << ordered(ctx.conditions)
        << ctx.targetArmor << ctx.targetMr << ctx.targetHp << ctx.targetMs << ctx.horizon
        << ctx.discreteSkillCasts << ctx.skillReadyIn << ctx.explicitActionCounts << ctx.explicitAttackCount
        << ctx.explicitCastCounts << ctx.explicitActiveSeconds << ctx.explicitProcCounts << ordered(ctx.explicitComponentKeys)
        << ctx.restrictExplicitComponents << ctx.explicitComponentScales << ctx.attackSpeedCap << ctx.abilityHasteCap
        << ctx.overflowCritAd << ctx.overflowCritAp;
    return bytes;
}
double mitigation(double resistance, double percent, double flat, double shred) {
    const double effective = resistance > 0 ? std::max(0., resistance * (1 - std::clamp(shred, 0., 1.)) * (1 - std::clamp(percent, 0., 1.)) - flat) : resistance;
    return resistanceMultiplier(effective);
}
QJsonValue weightedHealth(const CombatEvaluation &self, const Enemy &enemy) {
    if (!self.valid || !enemy.sharesKnown) return nil();
    const auto &s = self.stats; const auto &a = enemy.reference.stats;
    const double pm = mitigation(s[Stat::Armor], a[Stat::ArmorPenPercent], a[Stat::ArmorPen] + a[Stat::Lethality], 1 - (1 - a[Stat::Shred]) * (1 - a[Stat::ArmorShred]));
    const double mm = mitigation(s[Stat::Mr], a[Stat::MagicPenPercent], a[Stat::MagicPen], 1 - (1 - a[Stat::Shred]) * (1 - a[Stat::MagicShred]));
    const double divisor = enemy.physical * pm + enemy.magic * mm + enemy.pure;
    const double health = s[Stat::Hp] + s[Stat::Shield] * (1 + s[Stat::HealShield]);
    // Preserve the core's other conditional defensive modifier, rather than
    // accidentally removing it when replacing its reference 50/50 mixture.
    const double referenceMitigation = .5 * resistanceMultiplier(s[Stat::Armor]) + .5 * resistanceMultiplier(s[Stat::Mr]);
    const double otherDefense = health > 0 ? self.survival * referenceMitigation / health : 0;
    return divisor > 0 && health > 0 && otherDefense > 0 ? number(health / divisor * otherDefense) : nil();
}
QJsonValue incomingTeamHealth(const CombatEvaluation &self,const QVector<Enemy> &enemies,const QMap<int,double> *historicalWeights=nullptr) {
    // Harmonic EHP combines incoming damage, not five equally likely attackers.
    // Each enemy retains its own penetration and conditional damage mixture.
    if(enemies.size()!=5)return nil();double weight=0,pressure=0;
    for(const auto &enemy:enemies){const auto hp=weightedHealth(self,enemy);const double damage=historicalWeights?historicalWeights->value(enemy.index,-1):output(enemy.reference);
        if(!enemy.valid||!hp.isDouble()||hp.toDouble()<=0||!std::isfinite(damage)||damage<0)return nil();
        weight+=damage;pressure+=damage/hp.toDouble();
    }
    return weight>0&&pressure>0?number(weight/pressure):nil();
}
struct HistoricalPressure {bool available=false;QMap<int,double> weights;QJsonObject report;};
HistoricalPressure historicalPressure(const Catalog &catalog,const GameState &state,const QVector<Enemy> &enemies) {
    HistoricalPressure result;const auto evidence=state.recommendationEvidence;
    const auto input=evidence.value(k("historical_pressure_scenario")).toObject();
    result.report={{k("available"),false},{k("source"),k("historical_player_dpm_pressure_sensitivity")},
        {k("weights"),QJsonArray{}},{k("reason"),k("historical_pressure_unavailable")},
        {k("current_enemy_damage_type_and_penetration_held_fixed"),true},{k("current_model_pressure_replaced"),false},
        {k("actual_current_damage_or_focus_fire_known"),false},{k("offense_multiplier_applied"),false},
        {k("scope"),k("observational_historical_damage_share_sensitivity_not_causal_or_current_damage")}};
    const auto fail=[&](const char *reason){result.report[k("reason")]=k(reason);return result;};
    if(!evidence.value(k("available")).toBool()||!input.value(k("available")).toBool())return result;
    const auto context=state.matchContext.value(k("context_id")).toString();const auto asOf=evidence.value(k("as_of_ms"));
    if(context.isEmpty()||evidence.value(k("context_id")).toString()!=context||state.observedPatch.isEmpty()
        ||evidence.value(k("patch")).toString()!=state.observedPatch||state.observedPatch!=catalog.patch
        ||!asOf.isDouble()||!std::isfinite(asOf.toDouble())||asOf.toDouble()<=0||asOf.toDouble()>QDateTime::currentMSecsSinceEpoch())return fail("historical_pressure_evidence_scope_invalid");
    int known=0;const auto rows=input.value(k("weights"));
    if(!integer(input.value(k("known_enemy_count")),5,5,&known)||!rows.isArray()||rows.toArray().size()!=5||enemies.size()!=5)return fail("historical_pressure_requires_five_enemies");
    QSet<int> observed;QJsonArray validated;double sum=0;
    for(const auto &value:rows.toArray()){
        const auto row=value.toObject();int index=0,champion=0,samples=0;const auto weight=row.value(k("weight"));
        if(!value.isObject()||!integer(row.value(k("player_index")),0,9,&index)||observed.contains(index)
            ||!integer(row.value(k("champion_id")),1,std::numeric_limits<int>::max(),&champion)
            ||!integer(row.value(k("samples")),5,std::numeric_limits<int>::max(),&samples)
            ||!weight.isDouble()||!std::isfinite(weight.toDouble())||weight.toDouble()<0||weight.toDouble()>1)return fail("historical_pressure_weight_invalid");
        bool match=false;for(const auto &enemy:enemies)if(enemy.index==index&&enemy.champion==champion&&enemy.valid&&enemy.sharesKnown)match=true;
        if(!match)return fail("historical_pressure_enemy_mapping_invalid");
        observed.insert(index);result.weights[index]=weight.toDouble();sum+=weight.toDouble();
        validated.append(QJsonObject{{k("player_index"),index},{k("champion_id"),champion},{k("weight"),weight},{k("samples"),samples},{k("scope"),row.value(k("scope")).toString().left(64)}});
    }
    if(!std::isfinite(sum)||std::abs(sum-1)>1e-6)return fail("historical_pressure_weights_not_normalized");
    result.available=true;result.report[k("available")]=true;result.report[k("reason")]=k("five_enemy_historical_dpm_weights_validated");
    result.report[k("known_enemy_count")]=known;result.report[k("weights")]=validated;result.report[k("as_of_ms")]=asOf;
    return result;
}
struct HistoricalAllyOutput {bool available=false;double selfShare=0;QJsonObject report;};
HistoricalAllyOutput historicalAllyOutput(const Catalog &catalog,const GameState &state) {
    HistoricalAllyOutput result;const auto evidence=state.recommendationEvidence;
    const auto input=evidence.value(k("historical_ally_output_scenario")).toObject();
    result.report={{k("available"),false},{k("source"),k("historical_five_ally_output_share_sensitivity")},
        {k("weights"),QJsonArray{}},{k("reason"),k("historical_ally_output_unavailable")},
        {k("current_dps_known"),false},{k("skill_multiplier"),false},{k("causal_effect_known"),false},
        {k("individual_output_replaced"),false},{k("damage_taken_is_current_hp"),false}};
    const auto fail=[&](const char *reason){result.report[k("reason")]=k(reason);return result;};
    if(!evidence.value(k("available")).toBool()||!input.value(k("available")).toBool())return result;
    const auto context=state.matchContext.value(k("context_id")).toString();const auto asOf=evidence.value(k("as_of_ms"));
    if(context.isEmpty()||evidence.value(k("context_id")).toString()!=context||state.observedPatch.isEmpty()
        ||evidence.value(k("patch")).toString()!=state.observedPatch||state.observedPatch!=catalog.patch
        ||!asOf.isDouble()||!std::isfinite(asOf.toDouble())||asOf.toDouble()<=0||asOf.toDouble()>QDateTime::currentMSecsSinceEpoch()
        ||(input.contains(k("as_of_ms"))&&input.value(k("as_of_ms"))!=asOf))return fail("historical_ally_evidence_scope_invalid");
    QString own;int selves=0,allyCount=0;
    for(const auto &v:state.players){const auto p=v.toObject();if(p.value(k("self")).toBool()){own=p.value(k("team")).toString();++selves;}}
    if(state.players.size()!=10||selves!=1||(own!=k("ORDER")&&own!=k("CHAOS")))return fail("historical_ally_mapping_invalid");
    for(const auto &v:state.players)allyCount+=v.toObject().value(k("team")).toString()==own;
    const auto rows=input.value(k("weights"));
    if(allyCount!=5||input.value(k("known_ally_count")).toInt()!=5||!rows.isArray()||rows.toArray().size()!=5
        ||input.value(k("scope"))!=k("historical_ally_output_share_sensitivity_scenario"))return fail("historical_ally_requires_five_same_patch_references");
    QSet<int> observed;QJsonArray validated;double sum=0,dpmTotal=0;int selfRows=0;
    for(const auto &v:rows.toArray()){
        const auto row=v.toObject();int index=0,champion=0,samples=0;const auto weight=row.value(k("weight")),dpm=row.value(k("damage_per_minute"));
        const auto scope=row.value(k("scope")).toString();
        if(!v.isObject()||!integer(row.value(k("player_index")),0,9,&index)||observed.contains(index)
            ||!integer(row.value(k("champion_id")),1,std::numeric_limits<int>::max(),&champion)
            ||!integer(row.value(k("samples")),5,std::numeric_limits<int>::max(),&samples)
            ||!row.value(k("self")).isBool()||(scope!=k("same_patch_champion")&&scope!=k("same_patch"))
            ||!weight.isDouble()||!std::isfinite(weight.toDouble())||weight.toDouble()<0||weight.toDouble()>1
            ||!dpm.isDouble()||!std::isfinite(dpm.toDouble())||dpm.toDouble()<0)return fail("historical_ally_weight_invalid");
        const auto p=state.players[index].toObject();
        if(p.value(k("team")).toString()!=own||p.value(k("champion_id")).toInt()!=champion||p.value(k("self"))!=row.value(k("self")))return fail("historical_ally_mapping_invalid");
        observed.insert(index);sum+=weight.toDouble();dpmTotal+=dpm.toDouble();
        if(row.value(k("self")).toBool()){++selfRows;result.selfShare=weight.toDouble();}
        validated.append(QJsonObject{{k("player_index"),index},{k("champion_id"),champion},{k("self"),row.value(k("self"))},
            {k("weight"),weight},{k("damage_per_minute"),dpm},{k("samples"),samples},{k("scope"),scope}});
    }
    if(selfRows!=1||!std::isfinite(sum)||std::abs(sum-1)>1e-6||!std::isfinite(dpmTotal)||dpmTotal<=0)return fail("historical_ally_weights_not_normalized");
    for(const auto &v:validated){const auto row=v.toObject();if(std::abs(row.value(k("weight")).toDouble()-row.value(k("damage_per_minute")).toDouble()/dpmTotal)>1e-6)return fail("historical_ally_weights_do_not_match_reference");}
    result.available=true;result.report[k("available")]=true;result.report[k("reason")]=k("five_ally_historical_output_shares_validated");
    result.report[k("known_ally_count")]=5;result.report[k("weights")]=validated;result.report[k("as_of_ms")]=asOf;
    result.report[k("historical_self_output_share")]=result.selfShare;
    return result;
}
QJsonObject defense(const CombatEvaluation &before, const CombatEvaluation &after, const Enemy &enemy) {
    const auto a = weightedHealth(before, enemy), b = weightedHealth(after, enemy);
    return {{k("available"), a.isDouble() && b.isDouble()}, {k("physical_share"), enemy.sharesKnown ? number(enemy.physical) : nil()},
        {k("magic_share"), enemy.sharesKnown ? number(enemy.magic) : nil()}, {k("true_share"), enemy.sharesKnown ? number(enemy.pure) : nil()},
        {k("type_share_reference_seconds"), 8}, {k("type_share_source"), k("visible_loadout_neutral_resistance_model")},
        {k("current_weighted_effective_hp"), a}, {k("candidate_weighted_effective_hp"), b},
        {k("relative_gain"), a.isDouble() && b.isDouble() ? gain(a.toDouble(), b.toDouble(), true) : nil()},
        {k("actual_damage_shares_known"), false}, {k("control_and_focus_fire_known"), false},
        {k("scope"), k("conditional_resists_penetration_shields_and_core_defense_modifier")}};
}
QJsonValue contextualGain(const CombatEvaluation &before, const CombatEvaluation &after, const QJsonObject &d, const QString &style) {
    Q_UNUSED(style);
    const auto raw = gain(before.score, after.score, before.valid && after.valid);
    const auto a = d.value(k("current_weighted_effective_hp")), b = d.value(k("candidate_weighted_effective_hp"));
    if (!raw.isDouble() || !a.isDouble() || !b.isDouble() || a.toDouble() <= 0 || before.survival <= 0 || after.survival <= 0) return raw;
    const double weightedRatio = b.toDouble() / a.toDouble(), referenceRatio = after.survival / before.survival;
    return number((1 + raw.toDouble()) * std::pow(weightedRatio / referenceRatio, growthObjectiveMetricWeight(before,k("mixed_effective_hp"))) - 1);
}
QJsonObject evaluated(const CombatEvaluation &e) {
    return {{k("raw_eval"), e.json()}, {k("modeled_output"), e.valid ? number(output(e)) : nil()}, {k("actual_damage_known"), false}};
}
struct BaselineEvaluation {
    CombatEvaluation combat;
    QJsonObject report;
    const QJsonObject &rendered() {
        if (report.isEmpty()) report = evaluated(combat);
        return report;
    }
};
void collectUnknowns(QStringList &all, const CombatEvaluation &e) { all.append(e.unknowns); if (!e.valid) all.append(k("invalid_combat_evaluation")); }
QJsonObject publicReplay(const MechanicLedger &ledger, double time, const QJsonObject &raw) {
    QJsonObject totals;
    const auto rawTotals = raw.value(k("raw_event_totals")).toObject();
    for (auto it = rawTotals.begin(); it != rawTotals.end(); ++it) {
        if (!it.value().isDouble() || !std::isfinite(it.value().toDouble())) continue;
        const auto op = it.key().section(QChar(u':'), 0, 0), type = it.key().section(QChar(u':'), 1);
        if (!QStringList{k("damage"), k("heal"), k("health_cost"), k("shield"), k("execute")}.contains(op)) continue;
        const auto name = op == k("damage") && QStringList{k("physical"), k("magic"), k("true")}.contains(type) ? op + QChar(u':') + type : op + k(":unclassified");
        totals[name] = totals.value(name).toDouble() + it.value().toDouble();
    }
    return {{k("self_stat_deltas"), ledger.statsAt(time).json()}, {k("confirmed_permanent_stats"), ledger.statsAt(time, true).json()},
        {k("raw_event_totals"), totals}, {k("actions_applied"), ledger.consumed.size()}, {k("action_decisions"), ledger.decisions.size()},
        {k("active_effects_count"), raw.value(k("active_effects")).toArray().size()}, {k("scheduled_effects_count"), raw.value(k("scheduled_effects")).toArray().size()},
        {k("state_values_count"), raw.value(k("values")).toObject().size()}, {k("scope"), k("explicit_observations_raw_totals_not_actual_or_predicted_damage")}};
}

// Only validated, timestamped events from equipment actually owned at the
// snapshot are replayed. Proposed purchases never acquire an invented past.
QJsonArray observations(const CombatDatabase &db, const GameState &state, const QList<int> &augments, QJsonObject &coverage, QStringList &unknowns) {
    QVector<QJsonObject> rows; QSet<QByteArray> seen;
    int rejected = 0, duplicates = 0, future = 0, noActions = 0;
    const int start = std::max(0, int(state.mechanicObservations.size()) - 512);
    if (start) unknowns.append(k("mechanic_observation_input_truncated"));
    for (int i = start; i < state.mechanicObservations.size(); ++i) {
        const auto raw = state.mechanicObservations[i]; const auto o = raw.toObject();
        int id = 0; const auto t = o.value(k("time")); const auto source = o.value(k("source")).toString();
        if (!raw.isObject() || !integer(o.value(k("id")), 1, std::numeric_limits<int>::max(), &id) || !t.isDouble() || !std::isfinite(t.toDouble()) || t.toDouble() < 0
            || !o.value(k("event")).isString() || o.value(k("event")).toString().isEmpty() || (o.contains(k("facts")) && (!o.value(k("facts")).isObject() || !finiteTree(o.value(k("facts")))))) { ++rejected; continue; }
        if (t.toDouble() > state.gameTime) { ++future; continue; }
        const CombatEntry *entry = nullptr;
        if (source == k("item") && state.owned.contains(id)) { const auto it = db.items.constFind(id); if (it != db.items.end()) entry = &*it; }
        if (source == k("augment") && augments.contains(id)) { const auto it = db.augments.constFind(id); if (it != db.augments.end()) entry = &*it; }
        if (!entry) { ++rejected; continue; }
        const auto key = QJsonDocument(o).toJson(QJsonDocument::Compact); if (seen.contains(key)) { ++duplicates; continue; } seen.insert(key);
        if (!hasActions(*entry)) ++noActions;
        rows.append(o);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const QJsonObject &a, const QJsonObject &b) { return a.value(k("time")).toDouble() < b.value(k("time")).toDouble(); });
    QJsonArray out; for (const auto &o : rows) out.append(o);
    coverage[k("observations_received")] = state.mechanicObservations.size(); coverage[k("observations_validated")] = out.size();
    coverage[k("observations_rejected")] = rejected; coverage[k("observations_future_excluded")] = future; coverage[k("observations_duplicates")] = duplicates;
    coverage[k("observations_without_event_actions")] = noActions; coverage[k("observations_truncated")] = start > 0;
    if (rejected) unknowns.append(k("invalid_or_unowned_mechanic_observation"));
    if (noActions) unknowns.append(k("some_observations_have_no_executable_event_actions"));
    return out;
}
void eventCounters(const CombatDatabase &db, CombatContext &ctx, const GameState &state, const QList<int> &augments, const QJsonArray &events) {
    QMap<QString, double> clocks;
    for (const auto &v : events) {
        const auto o = v.toObject(); const bool augment = o.value(k("source")) == k("augment"); const int id = o.value(k("id")).toInt();
        const auto &entries = augment ? db.augments : db.items; auto entry = entries.value(id);
        if (o.value(k("event")) != entry.triggerContract.value(k("event"))) for (const auto &r : entry.eventRules) if (r.toObject().value(k("event")) == o.value(k("event"))) { entry.triggerContract = r.toObject(); break; }
        const auto decision = evaluateMechanicTrigger(entry, ctx, ctx.base, o, clocks);
        const auto count = o.value(k("facts")).toObject().value(k("numeric_effect_count"));
        const double ago = state.gameTime - o.value(k("time")).toDouble();
        if (!decision.matched || !count.isDouble() || !std::isfinite(count.toDouble()) || count.toDouble() < 0 || ago < 0 || ago >= ctx.horizon) continue;
        const auto prefix = (augment ? k("augment:") : k("item:")) + QString::number(id) + QChar(u':');
        for (const auto &ef : entry.effects) {
            const auto e = ef.toObject(); const auto counter = e.value(k("counter")).toString(), op = e.value(k("op")).toString();
            if ((op == k("observed_damage") || op == k("observed_heal")) && counter.startsWith(prefix) && !state.effectCounters.contains(counter)) ctx.counters[counter] += count.toDouble();
        }
    }
    Q_UNUSED(augments);
}
// Only immutable observed-snapshot preparation lives here. Candidate inventory,
// proposed cards and explicit scenario inputs must never enter this object.
struct PreparedSnapshot {
    CombatContext context;
    QJsonArray events;
    QJsonObject coverage, skillRankReport, replay;
    QStringList unknowns;
    bool ranksObserved = false;
};
}

struct CombatSandboxReuse::Data {
    QByteArray snapshot;
    const Catalog *batchCatalog = nullptr;
    const GameState *batchState = nullptr;
    bool batchBound = false;
    QList<int> batchAugments;
    QString batchStyle;
    QMap<QByteArray, BaselineEvaluation> baselines;
    QMap<QByteArray, TeammateCombatContext> teammates;
    QMap<QByteArray, QJsonObject> growth;
    PreparedSnapshot prepared;
    bool preparedReady = false;
    QVector<Enemy> enemies;
    bool enemiesReady = false;
    int enemyRows = 0, omitted = 0, validEnemies = 0, shareModels = 0, typeProbeCalls = 0;
    qint64 snapshots = 0, invalidations = 0, baselineEvaluations = 0, baselineHits = 0;
    qint64 enemyProbeEvaluations = 0, enemyHits = 0, teammateEvaluations = 0, teammateHits = 0, growthEvaluations = 0, growthHits = 0;
    qint64 preparedBuilds = 0, preparedHits = 0;
    void bind(const Catalog &catalog, const GameState &state, const QList<int> &augments, const QString &style) {
        const bool batchInput = &catalog == batchCatalog && &state == batchState;
        if (batchInput && batchBound && batchAugments == augments && batchStyle == style) return;
        batchBound = batchInput;
        if (batchBound) { batchAugments = augments; batchStyle = style; }
        const auto basis = snapshotBasis(catalog, state, augments, style);
        if (snapshot == basis) return;
        if (!snapshot.isEmpty()) ++invalidations;
        ++snapshots; snapshot = basis; baselines.clear(); teammates.clear(); growth.clear(); enemies.clear(); enemiesReady = false;
        prepared = {}; preparedReady = false;
    }
};

CombatSandboxReuse::CombatSandboxReuse() : data(std::make_unique<Data>()) {}
CombatSandboxReuse::CombatSandboxReuse(const Catalog &catalog, const GameState &state) : CombatSandboxReuse() {
    data->batchCatalog = &catalog;
    data->batchState = &state;
}
CombatSandboxReuse::~CombatSandboxReuse() = default;
void CombatSandboxReuse::clear() { data = std::make_unique<Data>(); }
QJsonObject CombatSandboxReuse::stats() const {
    return {{k("scope"), k("one_read_only_recommendation_job_exact_snapshot_and_context_bytes")},
        {k("snapshots"), double(data->snapshots)}, {k("invalidations"), double(data->invalidations)},
        {k("baseline_evaluations"), double(data->baselineEvaluations)}, {k("baseline_cache_hits"), double(data->baselineHits)},
        {k("enemy_probe_evaluations"), double(data->enemyProbeEvaluations)}, {k("enemy_model_cache_hits"), double(data->enemyHits)},
        {k("teammate_context_evaluations"), double(data->teammateEvaluations)}, {k("teammate_context_cache_hits"), double(data->teammateHits)},
        {k("growth_baseline_evaluations"), double(data->growthEvaluations)}, {k("growth_baseline_cache_hits"), double(data->growthHits)},
        {k("prepared_snapshot_builds"), double(data->preparedBuilds)}, {k("prepared_snapshot_hits"), double(data->preparedHits)},
        {k("retained_snapshots"), data->snapshot.isEmpty() ? 0 : 1},
        {k("retained_entries"), data->baselines.size() + data->teammates.size() + data->growth.size() + (data->enemiesReady ? 1 : 0) + (data->preparedReady ? 1 : 0)},
        {k("maximum_retained_entries"), 34}, {k("candidate_results_cached"), false}};
}

QMap<QString, int> referenceCombatRanks(const CombatDatabase &db, int champion, int level) {
    if (!db.champions.contains(champion) || level < 1 || level > 18) return {};
    return referenceRanks(db, champion, level);
}

QJsonObject evaluateCombatSandbox(const Catalog &catalog, const GameState &state, const QList<int> &candidateInventory, const CombatSandboxOptions &options) {
    QJsonObject coverage{{k("complete_combat_simulation"), false}, {k("actual_damage_available"), false}, {k("real_win_rate_available"), false},
        {k("maximum_targets"), 5}, {k("maximum_windows"), 3}, {k("maximum_target_window_pairs"), 15}, {k("control_simulation_available"), false}};
    QStringList unknowns{k("champion_passives_mode_balance_and_hit_conditions_not_fully_simulated"), k("enemy_complete_action_timelines_and_hidden_stacks_not_observed"),
        k("future_hits_controls_targeting_and_position_unknown"), k("damage_windows_are_comparison_objectives_not_hit_timelines")};
    QJsonObject out{{k("schema_version"), 1}, {k("available"), false}, {k("status"), k("unavailable")}, {k("current"), QJsonObject{}},
        {k("candidate"), QJsonObject{}}, {k("windows"), QJsonArray{}}, {k("growth"), QJsonObject{}}, {k("match_win_rate"), nil()}};
    const auto health=combatHealthContract(catalog,state);
    unknowns.append(combatHealthUnknowns(health));
    out[k("health_basis")]=health;
    QJsonArray unknownSources;QSet<QString> unknownSourceKeys;bool allySourcesRecorded=false;
    auto recordUnknownSource=[&](const QString &scope,int champion,const QStringList &values){
        if(unknownSourceKeys.contains(scope))return;unknownSourceKeys.insert(scope);
        const auto critical=combatCriticalUnknowns(values);if(critical.isEmpty())return;
        unknownSources.append(QJsonObject{{k("scope"),scope},{k("champion_id"),champion},
            {k("critical_unknowns"),QJsonArray::fromStringList(critical)}});
    };
    auto finish = [&](const QString &status) {
        auto windows=out.value(k("windows")).toArray();
        const auto finiteGain=[](const QJsonValue &value){return value.isDouble()&&std::isfinite(value.toDouble());};
        const auto completeWindow=[&](const QJsonObject &row){
            return row.value(k("targets")).toArray().size()==5&&row.value(k("supported_targets")).toInt()==5
                &&finiteGain(row.value(k("score_gain")))&&(!options.teamObjective
                    ||(row.value(k("team_supported_targets")).toInt()==5&&finiteGain(row.value(k("team_score_gain")))));
        };
        bool complete=windows.size()==3;
        for(const auto &raw:windows)complete&=completeWindow(raw.toObject());
        const bool numericAvailable=out.value(k("available")).toBool()
            &&out.value(k("current")).toObject().value(k("raw_eval")).toObject().value(k("valid")).toBool()
            &&out.value(k("candidate")).toObject().value(k("raw_eval")).toObject().value(k("valid")).toBool();
        // An isolated replay may intentionally compare one reference target.
        // Once a roster is supplied (or the caller is live), a partial roster
        // average cannot certify the requested multi-opponent comparison.
        const bool rosterRequested=state.source==GameState::Source::Live||state.players.size()>1;
        if(out.value(k("available")).toBool()&&rosterRequested&&!complete)unknowns.append(k("combat_target_comparison_incomplete"));
        coverage[k("target_comparison_complete")]=complete;
        coverage[k("target_comparison_required_for_ranking")]=rosterRequested;
        out[k("status")] = status; out[k("coverage")] = coverage; out[k("unknowns")] = codes(unknowns);
        out[k("critical_unknown_sources")]=unknownSources;
        const auto boundary=combatComparisonBoundary(unknowns,numericAvailable);
        out[k("comparison_boundary")]=boundary;
        for(auto it=boundary.begin();it!=boundary.end();++it)out[it.key()]=it.value();
        out[k("ranking_scope")]=rosterRequested?k("all_requested_target_windows"):k("reference_target_only");
        for(int i=0;i<windows.size();++i){auto row=windows[i].toObject();auto windowUnknowns=unknowns;
            if(!completeWindow(row))windowUnknowns.append(k("combat_target_comparison_incomplete"));
            const auto windowBoundary=combatComparisonBoundary(windowUnknowns,finiteGain(row.value(k("score_gain"))));
            row[k("comparison_boundary")]=windowBoundary;
            for(auto it=windowBoundary.begin();it!=windowBoundary.end();++it)row[it.key()]=it.value();windows[i]=row;}
        out[k("windows")]=windows;
        return out;
    };
    QString workStage = k("validation");
    int completedTargetPairs = 0, completedComparisons = 0, completedEnemyProbes = 0;
    const auto stopped = [&](const char *stage) { workStage = k(stage); return options.work && options.work->stopped(); };
    const auto interrupted = [&]() {
        // A completed prefix is diagnostic progress, never a comparison of the
        // candidate. Discard all scores, scenarios, windows and growth output.
        auto interruptedCoverage = coverage;
        interruptedCoverage[k("target_window_pairs_completed")] = completedTargetPairs;
        interruptedCoverage[k("target_comparison_evaluations_completed")] = completedComparisons;
        interruptedCoverage[k("enemy_probe_evaluations_completed")] = completedEnemyProbes;
        auto interruptedUnknowns = unknowns; interruptedUnknowns.append(k("comparison_work_interrupted"));
        const auto reason = options.work ? options.work->stopReason() : QString{};
        return QJsonObject{{k("schema_version"), 1}, {k("available"), false}, {k("status"), k("interrupted")},
            {k("stop_reason"), reason.isEmpty() ? k("cancelled") : reason}, {k("interrupted_stage"), workStage},
            {k("current"), QJsonObject{}}, {k("candidate"), QJsonObject{}}, {k("windows"), QJsonArray{}},
            {k("growth"), QJsonObject{}}, {k("match_win_rate"), nil()}, {k("coverage"), interruptedCoverage},
            {k("unknowns"), codes(interruptedUnknowns)}};
    };
    if (stopped("validation")) return interrupted();
    if (!catalog.combat || !catalog.combat->champions.contains(state.championId)) { unknowns.append(k("champion_combat_model_unavailable")); return finish(k("model_unavailable")); }
    if (!modelRulesUsable(catalog, state)) { unknowns.append(k("catalog_patch_mismatch")); return finish(k("patch_mismatch")); }
    if (!state.fresh() || state.level < 1 || state.level > 18 || !std::isfinite(state.gameTime) || state.gameTime < 0) { unknowns.append(k("fresh_valid_snapshot_required")); return finish(k("snapshot_unavailable")); }
    const auto &db = *catalog.combat; const auto augments = effectiveSelectedAugments(catalog, state);
    auto candidateAugments = augments;QSet<int> newAugments;
    out[k("comparison_kind")]=options.compareCandidateAugments?k("next_augment_same_inventory"):k("inventory_same_augments");
    out[k("baseline_augment_ids")]=ids(augments);
    if(options.compareCandidateAugments){
        candidateAugments=options.candidateAugments;
        out[k("candidate_augment_ids")]=ids(candidateAugments);
        auto originalInventory=state.owned, proposedInventory=candidateInventory;
        std::sort(originalInventory.begin(),originalInventory.end());std::sort(proposedInventory.begin(),proposedInventory.end());
        if(originalInventory!=proposedInventory){unknowns.append(k("augment_comparison_inventory_must_remain_unchanged"));return finish(k("invalid_candidate_augments"));}
        QSet<int> baselineSet,candidateSet;int added=0;
        for(int id:augments){if(baselineSet.contains(id)){unknowns.append(k("duplicate_baseline_augment"));return finish(k("invalid_candidate_augments"));}baselineSet.insert(id);}
        if(candidateAugments.size()>12){unknowns.append(k("candidate_augment_input_limit_exceeded"));return finish(k("invalid_candidate_augments"));}
        for(int id:candidateAugments){
            if(id<=0||candidateSet.contains(id)){unknowns.append(k("invalid_or_duplicate_candidate_augment"));return finish(k("invalid_candidate_augments"));}candidateSet.insert(id);
            const auto entry=db.augments.constFind(id);
            if(entry==db.augments.cend()){unknowns.append(k("unknown_candidate_augment_id"));return finish(k("invalid_candidate_augments"));}
            if(!baselineSet.contains(id)){
                ++added;newAugments.insert(id);
                if(!catalog.ordinaryOfferEligible(id)){unknowns.append(k("candidate_augment_not_ordinary_offer"));return finish(k("invalid_candidate_augments"));}
                if(entry->requiredLevel>state.level){unknowns.append(k("candidate_augment_level_requirement_not_met"));return finish(k("invalid_candidate_augments"));}
            }
        }
        for(int id:augments)if(!candidateSet.contains(id)){unknowns.append(k("candidate_augment_drops_baseline_selection"));return finish(k("invalid_candidate_augments"));}
        if(added!=1){unknowns.append(k("candidate_augment_requires_one_new_offer"));return finish(k("invalid_candidate_augments"));}
    }else out[k("candidate_augment_ids")]=ids(candidateAugments);
    if (state.owned.size() > 6 || candidateInventory.size() > 6 || augments.size() > 12) { unknowns.append(k("loadout_input_limit_exceeded")); return finish(k("invalid_loadout")); }
    bool idsKnown = true;
    for (int id : state.owned) if (!db.items.contains(id)) { idsKnown = false; unknowns.append(k("unknown_current_item_id")); }
    for (int id : candidateInventory) if (!db.items.contains(id)) { idsKnown = false; unknowns.append(k("unknown_candidate_item_id")); }
    for (int id : augments) if (!db.augments.contains(id)) { idsKnown = false; unknowns.append(k("unknown_selected_augment_id")); }
    if (!idsKnown) return finish(k("invalid_loadout"));
    const auto style = resolvedCombatPreference(catalog, state, options.style);
    auto *reuse = options.reuse ? options.reuse->data.get() : nullptr;
    if (stopped("snapshot_binding")) return interrupted();
    if (reuse) reuse->bind(catalog, state, augments, style);
    if (stopped("snapshot_binding")) return interrupted();
    const auto baselineFor = [&](const CombatContext &context, const QByteArray &key, BaselineEvaluation &local) -> BaselineEvaluation & {
        if (reuse) {
            const auto found = reuse->baselines.find(key);
            if (found != reuse->baselines.end()) { ++reuse->baselineHits; return *found; }
        }
        local.combat = evaluateCombat(db, context, state.owned, augments, false, style);
        if (reuse) {
            ++reuse->baselineEvaluations;
            if (reuse->baselines.size() < 16) return *reuse->baselines.insert(key, local);
        }
        return local;
    };
    const auto evaluateTeammates = [&](const CombatContext &context, const QByteArray &key) {
        if (reuse) {
            const auto found = reuse->teammates.constFind(key);
            if (found != reuse->teammates.cend()) { ++reuse->teammateHits; return *found; }
        }
        // A work-local cache owns the complete snapshot basis. Bypass the
        // legacy global teammate cache rather than nest a narrower key in it.
        const auto result = evaluateTeammateContext(catalog, state, context, style, reuse == nullptr);
        if (reuse) { ++reuse->teammateEvaluations; if (reuse->teammates.size() < 15) reuse->teammates.insert(key, result); }
        return result;
    };
    out[k("objective")] = QJsonObject{{k("role_matching_enabled"),false},{k("fixed_direction_matching_enabled"),false},
        {k("offense_source"),k("all_current_modeled_output_without_direction_filter")},
        {k("score_gain_unit"), k("candidate_to_current_ratio_minus_one")}, {k("win_probability"), false}};
    out[k("timing_policy")]=QJsonObject{{k("skill_casts"),k("integer_recasts_on_one_shared_origin_end_exclusive")},
        {k("initial_readiness"),k("all_ready_hypothesis_not_live_cooldown_observation")},{k("attack_and_proc_timing"),k("partial_rate_model")},
        {k("windows_overlap"),true},{k("sum_cumulative_windows_permitted"),false}};
    PreparedSnapshot prepared;
    if (reuse && reuse->preparedReady) {
        // QJson/Qt containers copy on write: every candidate receives its own
        // context value before excluding proposed-card past state below.
        prepared = reuse->prepared; ++reuse->preparedHits;
        if (stopped("observed_mechanic_ledger")) return interrupted();
        if (stopped("observed_panel_anchor")) return interrupted();
    } else {
        auto &context = prepared.context;
        auto &preparedCoverage = prepared.coverage;
        auto &preparedUnknowns = prepared.unknowns;
        context = db.context(state.championId, state.level); context.discreteSkillCasts = true;
        const auto skillRanks = selfSkillRankContext(db,state);
        out[k("skill_rank_observation")] = skillRanks.report;
        if (!skillRanks.available) { unknowns.append(k("self_skill_ranks_invalid")); return finish(k("invalid_self_skill_ranks")); }
        prepared.skillRankReport = skillRanks.report;
        prepared.ranksObserved = skillRanks.report.value(k("complete")).toBool();
        context.ranks = skillRanks.context.ranks;
        for (const auto &value : skillRanks.report.value(k("unknowns")).toArray()) preparedUnknowns.append(value.toString());
        int counterCount = 0;
        for (auto it = state.effectCounters.begin(); it != state.effectCounters.end(); ++it) {
            if (++counterCount <= 256 && it.key().size() <= 128 && std::isfinite(it.value())) context.counters[it.key()] = it.value(); else preparedUnknowns.append(k("invalid_or_excess_observed_counter"));
        }
        for (const auto &condition : state.effectConditions) if (condition.size() <= 128 && context.conditions.size() < 256) context.conditions.insert(condition);
        context.counters[k("observed_game_time")] = state.gameTime;
        if (std::isfinite(state.mechanicBaselineTime) && state.mechanicBaselineTime >= 0 && state.mechanicBaselineTime <= state.gameTime) {
            context.counters[k("mechanic_baseline_time")] = state.mechanicBaselineTime;
            preparedCoverage[k("mechanic_baseline_time")] = state.mechanicBaselineTime;
        } else {
            context.counters.remove(k("mechanic_baseline_time"));
            preparedCoverage[k("mechanic_baseline_time")] = nil();
            if (state.mechanicBaselineTime != -1) preparedUnknowns.append(k("invalid_mechanic_baseline_time"));
        }
        if (stopped("observed_mechanic_ledger")) return interrupted();
        prepared.events = observations(db, state, augments, preparedCoverage, preparedUnknowns);
        const auto ledger = replayCombatMechanics(db, context, state.owned, augments, prepared.events);
        if (stopped("observed_mechanic_ledger")) return interrupted();
        applyMechanicLedgerContext(ledger,state.gameTime,context,true);
        const auto ledgerJson = ledger.json(state.gameTime);
        preparedCoverage[k("observed_actions_applied")] = ledger.consumed.size(); preparedCoverage[k("observed_action_decisions")] = ledger.decisions.size();
        int rejectedActions = 0;
        for (const auto &v : ledger.decisions) if (!v.toObject().value(k("applied")).toBool()) ++rejectedActions;
        preparedCoverage[k("observed_actions_rejected_or_unresolved")] = rejectedActions;
        if (rejectedActions) preparedUnknowns.append(k("some_observed_actions_rejected_or_unresolved"));
        preparedCoverage[k("observed_actions_numeric_scope")] = k("self_stat_buffs_consumed_event_totals_reported_separately");
        if (!ledgerJson.value(k("values")).toObject().isEmpty()) preparedUnknowns.append(k("mechanic_state_values_not_all_consumed_by_numeric_model"));
        if (!ledgerJson.value(k("raw_event_totals")).toObject().isEmpty()) preparedUnknowns.append(k("event_damage_heal_shield_totals_not_actual_or_future_damage"));
        QJsonObject anchor;
        if (stopped("observed_panel_anchor")) return interrupted();
        if (!state.liveStats.isEmpty()) { anchor = anchorObservedCombatStats(db, context, state.owned, augments, state.liveStats, style); preparedUnknowns.append(k("observed_panel_residual_origin_and_future_persistence_unknown")); }
        bindGrowthObjective(db,context,state.owned,augments);
        prepared.replay = publicReplay(ledger, state.gameTime, ledgerJson);
        if (stopped("observed_panel_anchor")) return interrupted();
        preparedCoverage[k("observed_anchor")] = anchor; preparedCoverage[k("self_skill_ranks_observed")] = prepared.ranksObserved;
        // Publish only a complete preparation. An interrupted build can never
        // supply a partial ledger or anchor to the next candidate.
        if (reuse) { reuse->prepared = prepared; reuse->preparedReady = true; ++reuse->preparedBuilds; }
    }
    auto ctx = prepared.context;
    const auto &events = prepared.events;
    const bool ranksObserved = prepared.ranksObserved;
    out[k("skill_rank_observation")] = prepared.skillRankReport;
    for (auto it = prepared.coverage.begin(); it != prepared.coverage.end(); ++it) coverage[it.key()] = it.value();
    unknowns.append(prepared.unknowns);
    out[k("growth_objective")]=ctx.growthObjectiveBasis;
    {auto objective=out.value(k("objective")).toObject();const auto weights=ctx.growthObjectiveBasis.value(k("metric_weights")).toObject();objective[k("metric_weights")]=weights;
        objective[k("survival_weight")]=weights.value(k("mixed_effective_hp"));objective[k("offense_weight")]=weights.value(k("modeled_output"));objective[k("skill_cycle_weight")]=weights.value(k("skill_cycle_capacity"));
        objective[k("basis_frozen_before_candidates")]=true;out[k("objective")]=objective;}
    // A proposed card has no observed past. Its own event counters/active
    // flags can enter only the separately labeled explicit scenario below.
    int excludedCandidateCounters=0,excludedCandidateConditions=0;
    const auto candidateContext=[&](CombatContext context){
        for(int id:newAugments){const auto prefix=k("augment:")+QString::number(id)+QChar(u':');
            for(auto it=context.counters.begin();it!=context.counters.end();)if(it.key().startsWith(prefix))it=context.counters.erase(it);else ++it;
            for(auto it=context.conditions.begin();it!=context.conditions.end();)if(it->startsWith(prefix))it=context.conditions.erase(it);else ++it;
        }return context;
    };
    for(int id:newAugments){const auto prefix=k("augment:")+QString::number(id)+QChar(u':');for(auto it=ctx.counters.cbegin();it!=ctx.counters.cend();++it)excludedCandidateCounters+=it.key().startsWith(prefix);for(const auto &condition:ctx.conditions)excludedCandidateConditions+=condition.startsWith(prefix);}
    coverage[k("new_augment_past_counters_excluded")]=excludedCandidateCounters;coverage[k("new_augment_past_conditions_excluded")]=excludedCandidateConditions;
    if(excludedCandidateCounters||excludedCandidateConditions)unknowns.append(k("proposed_augment_past_state_not_observed_as_owned"));
    out[k("mechanic_replay")] = prepared.replay;
    out[k("skill_rank_policy")] = ranksObserved ? k("observed_current_ranks_fixed_for_growth") : k("partial_observed_plus_qwe_priority_reference_fixed_for_growth");
    out[k("skill_ranks")] = ranksJson(ctx.ranks);
    out[k("data_revision")] = db.revision; out[k("catalog_patch")] = catalog.patch;

    // Reference-target baseline is intentionally identified as such. Real
    // opponent-specific armor/HP are supplied separately below, never averaged.
    auto base = ctx; eventCounters(db, base, state, augments, events);
    if (stopped("reference_baseline")) return interrupted();
    BaselineEvaluation referenceLocal;
    auto &referenceBaseline = baselineFor(base, reuse ? contextBasis(base) : QByteArray{}, referenceLocal);
    const auto current = referenceBaseline.combat;
    if (stopped("reference_baseline")) return interrupted();
    if (stopped("reference_candidate")) return interrupted();
    const auto candidate = evaluateCombat(db, candidateContext(base), candidateInventory, candidateAugments, false, style);
    if (stopped("reference_candidate")) return interrupted();
    out[k("current")] = referenceBaseline.rendered(); out[k("candidate")] = evaluated(candidate);
    out[k("reference_target")] = QJsonObject{{k("observed"), false}, {k("armor"), ctx.targetArmor}, {k("magic_resist"), ctx.targetMr}, {k("max_health"), ctx.targetHp}, {k("seconds"), 8}};
    collectUnknowns(unknowns, current); collectUnknowns(unknowns, candidate);
    recordUnknownSource(k("self_current_inventory"),state.championId,current.unknowns);
    recordUnknownSource(k("self_candidate_inventory"),state.championId,candidate.unknowns);
    if (!current.valid || !candidate.valid) return finish(k("invalid_loadout"));

    // Scenario data is applied only to a second branch. Even an overwrite of a
    // real counter remains explicit and does not change current/candidate above.
    auto scenario = candidateContext(ctx); QJsonObject scenarioCounters; QJsonArray scenarioConditions; int counterCount = 0;
    for (auto it = options.scenarioCounters.begin(); it != options.scenarioCounters.end(); ++it) {
        if (++counterCount <= 256 && it.key().size() <= 128 && std::isfinite(it.value()) && it.key() != k("observed_game_time")) { scenario.counters[it.key()] = it.value(); scenarioCounters[it.key()] = it.value(); }
        else unknowns.append(k("invalid_or_excess_scenario_counter"));
    }
    auto conditionList = options.scenarioConditions.values(); conditionList.sort();
    for (const auto &condition : conditionList) if (condition.size() <= 128 && scenarioConditions.size() < 256) { scenario.conditions.insert(condition); scenarioConditions.append(condition); }
    const bool hasScenario = !scenarioCounters.isEmpty() || !scenarioConditions.isEmpty();
    if (hasScenario) {
        if (stopped("explicit_scenario_baseline")) return interrupted();
        const auto sc = evaluateCombat(db, scenario, state.owned, augments, false, style);
        if (stopped("explicit_scenario_baseline")) return interrupted();
        if (stopped("explicit_scenario_candidate")) return interrupted();
        const auto sn = evaluateCombat(db, scenario, candidateInventory, candidateAugments, false, style);
        if (stopped("explicit_scenario_candidate")) return interrupted();
        out[k("scenario")] = QJsonObject{{k("explicit_assumption"), true}, {k("observed"), false}, {k("counters"), scenarioCounters}, {k("conditions"), scenarioConditions},
            {k("current"), evaluated(sc)}, {k("candidate"), evaluated(sn)}, {k("score_gain"), gain(sc.score, sn.score, sc.valid && sn.valid)}};
        collectUnknowns(unknowns, sc); collectUnknowns(unknowns, sn); unknowns.append(k("scenario_inputs_not_observed_or_forecast"));
    }
    Q_UNUSED(options.scenarioLabel);

    QString selfTeam; int selves = 0, selfRows = 0;
    for (const auto &v : state.players) if (v.toObject().value(k("self")).toBool()) { ++selves; selfTeam = v.toObject().value(k("team")).toString(); if (v.toObject().value(k("items_complete")).toBool()) ++selfRows; }
    coverage[k("self_inventory_complete_observed")] = selves == 1 && selfRows == 1;
    const bool teamKnown = selves == 1 && (selfTeam == k("ORDER") || selfTeam == k("CHAOS"));
    QVector<Enemy> enemies; int omitted = 0, enemyRows = 0, validEnemies = 0, shareModels = 0, typeProbeCalls = 0;
    if (stopped("enemy_models")) return interrupted();
    if (reuse && reuse->enemiesReady) {
        enemies = reuse->enemies; omitted = reuse->omitted; enemyRows = reuse->enemyRows;
        validEnemies = reuse->validEnemies; shareModels = reuse->shareModels; typeProbeCalls = reuse->typeProbeCalls;
        ++reuse->enemyHits;
    } else {
    CombatDatabase probeDb = db;
    const int physicalProbe = addTypeProbe(probeDb, k("physical"), std::numeric_limits<int>::max() - 1);
    const int magicProbe = addTypeProbe(probeDb, k("magic"), std::numeric_limits<int>::max() - 2);
    if (teamKnown && state.players.size() <= 10) for (int i = 0; i < state.players.size(); ++i) {
        if (stopped("enemy_models")) return interrupted();
        const auto row = state.players[i].toObject(); const auto team = row.value(k("team")).toString();
        if (team == selfTeam || (team != k("ORDER") && team != k("CHAOS"))) continue;
        ++enemyRows; if (enemies.size() == 5) { ++omitted; continue; }
        Enemy enemy; enemy.index = i; enemy.itemsComplete = row.value(k("items_complete")).toBool(); enemy.augmentsComplete = row.value(k("augments_complete")).toBool();
        bool ok = integer(row.value(k("champion_id")), 1, std::numeric_limits<int>::max(), &enemy.champion) && db.champions.contains(enemy.champion)
            && integer(row.value(k("level")), 1, 18, &enemy.level) && readIds(row.value(k("items")), db.items, enemy.items, 6);
        if (!row.value(k("augment_ids")).isUndefined()) ok = readIds(row.value(k("augment_ids")), db.augments, enemy.augments, 12) && ok;
        else enemy.augmentsComplete = false;
        if (!enemy.itemsComplete) enemy.unknowns.append(k("enemy_inventory_incomplete"));
        if (!enemy.augmentsComplete) enemy.unknowns.append(k("enemy_augments_incomplete"));
        if (ok && enemy.itemsComplete) {
            const auto observedContext = playerCombatContext(db, row);
            enemy.contextReport = observedContext.report;
            for (const auto &value : observedContext.report.value(k("unknowns")).toArray()) enemy.unknowns.append(value.toString());
            if (!observedContext.available) { ok = false; enemy.unknowns.append(k("enemy_current_context_invalid")); }
            else enemy.context = observedContext.context;
        }
        if (ok && enemy.itemsComplete) {
            enemy.context.discreteSkillCasts=true;
            enemy.context.targetArmor = 0; enemy.context.targetMr = 0; enemy.context.targetHp = current.stats[Stat::Hp]; enemy.context.targetMs = current.stats[Stat::Ms];
            if (stopped("enemy_reference_probe")) return interrupted();
            enemy.reference = evaluateCombat(db, enemy.context, enemy.items, enemy.augments, false, style); ++typeProbeCalls; ++completedEnemyProbes;
            if (stopped("enemy_reference_probe")) return interrupted();
            enemy.valid = enemy.reference.valid;
            enemy.unknowns.append(enemy.reference.unknowns);
            if (enemy.valid) {
                ++validEnemies;
                if (!hasCopyTrue(db, enemy.items, enemy.augments)) {
                    if (stopped("enemy_physical_probe")) return interrupted();
                    auto list = enemy.augments; list.append(physicalProbe); const auto noPhysical = evaluateCombat(probeDb, enemy.context, enemy.items, list, false, style); ++typeProbeCalls; ++completedEnemyProbes;
                    if (stopped("enemy_physical_probe")) return interrupted();
                    if (stopped("enemy_magic_probe")) return interrupted();
                    list = enemy.augments; list.append(magicProbe); const auto noMagic = evaluateCombat(probeDb, enemy.context, enemy.items, list, false, style); ++typeProbeCalls; ++completedEnemyProbes;
                    if (stopped("enemy_magic_probe")) return interrupted();
                    const double total = output(enemy.reference), p = total - output(noPhysical), m = total - output(noMagic), t = total - p - m;
                    const double tolerance = std::max(1e-6, std::abs(total) * 1e-8);
                    if (noPhysical.valid && noMagic.valid && std::isfinite(total) && total > 0 && p >= -tolerance && m >= -tolerance && t >= -tolerance) {
                        const double sum = std::max(0., p) + std::max(0., m) + std::max(0., t);
                        enemy.physical = std::max(0., p) / sum; enemy.magic = std::max(0., m) / sum; enemy.pure = std::max(0., t) / sum; enemy.sharesKnown = true; ++shareModels;
                    } else enemy.unknowns.append(k("damage_type_decomposition_failed"));
                } else enemy.unknowns.append(k("copied_true_damage_type_decomposition_unknown"));
            }
        } else enemy.unknowns.append(k("enemy_loadout_model_unavailable"));
        enemies.append(enemy);
    }
    if (stopped("enemy_models")) return interrupted();
    if (reuse) {
        reuse->enemies = enemies; reuse->omitted = omitted; reuse->enemyRows = enemyRows;
        reuse->validEnemies = validEnemies; reuse->shareModels = shareModels; reuse->typeProbeCalls = typeProbeCalls;
        reuse->enemiesReady = true; reuse->enemyProbeEvaluations += typeProbeCalls;
    }
    }
    for (const auto &enemy : enemies){unknowns.append(enemy.unknowns);
        recordUnknownSource(k("enemy:")+QString::number(enemy.index),enemy.champion,enemy.unknowns);}
    if (!teamKnown || state.players.size() > 10) unknowns.append(k("verified_self_team_mapping_unavailable"));
    if (enemyRows != 5 || validEnemies != 5 || omitted) unknowns.append(k("five_enemy_loadouts_not_fully_available"));
    coverage[k("visible_enemy_rows")] = enemyRows; coverage[k("modeled_enemy_builds")] = validEnemies; coverage[k("modeled_enemy_damage_shares")] = shareModels;
    coverage[k("enemy_rows_omitted")] = omitted; coverage[k("enemy_type_probe_evaluations")] = typeProbeCalls;
    coverage[k("roster_ten_people_observed")] = teamKnown && state.players.size() == 10 && enemyRows == 5;
    coverage[k("enemy_damage_shares_are_conditional_model")] = true;
    QJsonArray threats;double totalThreat=0;for(const auto &enemy:enemies)if(enemy.valid)totalThreat+=output(enemy.reference);
    for(const auto &enemy:enemies)threats.append(QJsonObject{{k("champion_id"),enemy.champion},{k("target_index"),enemy.index},
        {k("conditional_damage_weight"),totalThreat>0&&enemy.valid?number(output(enemy.reference)/totalThreat):nil()}, {k("combat_context"),enemy.contextReport}});
    out[k("incoming_pressure")]=QJsonObject{{k("source"),k("visible_loadouts_neutral_target_8s_model")},{k("enemies"),threats},
        {k("actual_damage_shares_known"),false},{k("focus_fire_known"),false},{k("health_pool_policy"),k("harmonic_effective_hp_weighted_by_conditional_enemy_output")}};
    auto historical=historicalPressure(catalog,state,enemies);
    historical.report[k("team_objective_requested")]=options.teamObjective;
    auto allyHistorical=historicalAllyOutput(catalog,state);
    allyHistorical.report[k("team_objective_requested")]=options.teamObjective;
    int unboundTargetCounters = 0;
    for (auto it = ctx.counters.cbegin(); it != ctx.counters.cend(); ++it) if (it.key().startsWith(k("target_"))) ++unboundTargetCounters;
    coverage[k("unbound_target_counters_excluded_per_opponent")] = unboundTargetCounters;
    const auto unboundTargetCode = k("generic_target_observations_not_bound_to_each_opponent");
    if (unboundTargetCounters) unknowns.append(unboundTargetCode);
    QJsonArray windows; int targetPairs = 0, comparisonCalls = 0,historicalPairs=0,allyHistoricalPairs=0,jointHistoricalPairs=0;
    for (double seconds : {3., 8., 20.}) {
        if (stopped("target_window")) return interrupted();
        QJsonArray targets; double sumGain = 0, sumTeamGain = 0,sumHistoricalGain=0,sumAllyHistoricalGain=0,sumJointHistoricalGain=0;
        int gains = 0, teamGains = 0,historicalGains=0,allyHistoricalGains=0,jointHistoricalGains=0;
        for (const auto &enemy : enemies) {
            if (stopped("target_window")) return interrupted();
            ++targetPairs;
            auto targetUnknowns = enemy.unknowns;
            if (unboundTargetCounters) targetUnknowns.append(unboundTargetCode);
            QJsonObject row{{k("target_index"), enemy.index}, {k("champion_id"), enemy.champion}, {k("level"), enemy.level}, {k("available"), enemy.valid},
                {k("items_complete"), enemy.itemsComplete}, {k("augments_complete"), enemy.augmentsComplete}, {k("unknowns"), codes(targetUnknowns)},
                {k("combat_context"), enemy.contextReport},
                {k("score_gain"), nil()}, {k("output_gain"), nil()},{k("historical_pressure_team_score_gain"),nil()},
                {k("historical_ally_output_team_score_gain"),nil()},{k("historical_joint_team_score_gain"),nil()}};
            if (enemy.valid) {
                auto c = ctx; c.horizon = seconds; c.targetArmor = enemy.reference.stats[Stat::Armor]; c.targetMr = enemy.reference.stats[Stat::Mr];
                c.targetHp = enemy.reference.stats[Stat::Hp]; c.targetMs = enemy.reference.stats[Stat::Ms];
                // No generic target_* input has an opponent identity binding.
                // Keep all of them only in the reference/scenario branches;
                // even crit modifiers cannot be spread across all five foes.
                for (auto it = c.counters.begin(); it != c.counters.end();) {
                    if (it.key().startsWith(k("target_"))) it = c.counters.erase(it);
                    else ++it;
                }
                eventCounters(db, c, state, augments, events);
                if (stopped("target_baseline")) return interrupted();
                const auto contextKey = reuse ? contextBasis(c) : QByteArray{};
                BaselineEvaluation targetLocal;
                auto &targetBaseline = baselineFor(c, contextKey, targetLocal);
                const auto &a = targetBaseline.combat;
                ++comparisonCalls; ++completedComparisons;
                if (stopped("target_baseline")) return interrupted();
                if (stopped("target_candidate")) return interrupted();
                const auto b = evaluateCombat(db, candidateContext(c), candidateInventory, candidateAugments, false, style); ++comparisonCalls; ++completedComparisons;
                if (stopped("target_candidate")) return interrupted();
                row[k("available")] = a.valid && b.valid; row[k("current")] = targetBaseline.rendered(); row[k("candidate")] = evaluated(b);
                auto pairUnknowns=a.unknowns;pairUnknowns.append(b.unknowns);pairUnknowns.append(targetUnknowns);pairUnknowns.append(combatHealthUnknowns(health));
                const auto pairBoundary=combatComparisonBoundary(pairUnknowns,a.valid&&b.valid);
                row[k("comparison_boundary")]=pairBoundary;
                for(auto it=pairBoundary.begin();it!=pairBoundary.end();++it)row[it.key()]=it.value();
                const auto d = defense(a, b, enemy);
                row[k("raw_score_gain")] = gain(a.score, b.score, a.valid && b.valid);
                row[k("score_gain")] = contextualGain(a, b, d, style); row[k("output_gain")] = gain(output(a), output(b), a.valid && b.valid);
                if (options.teamObjective) {
                    if (stopped("target_teammate_context")) return interrupted();
                    const auto teammates = evaluateTeammates(c, contextKey);
                    if (stopped("target_teammate_context")) return interrupted();
                    const auto incomingA=incomingTeamHealth(a,enemies),incomingB=incomingTeamHealth(b,enemies);
                    double otherHealth = 0; bool weighted = incomingA.isDouble()&&incomingB.isDouble();
                    for (const auto &ally : teammates.teammates) {
                        const auto hp = incomingTeamHealth(ally,enemies);
                        if (!hp.isDouble()) { weighted = false; break; }
                        otherHealth += hp.toDouble();
                    }
                    const auto teamGain = compareTeamObjective(teammates, a, b, style,
                        weighted ? incomingA.toDouble() : -1,
                        weighted ? incomingB.toDouble() : -1,
                        weighted ? otherHealth : -1);
                    row[k("team_defense_basis")]=weighted?k("all_five_enemy_output_weighted_pressure"):k("reference_50_50_incomplete_damage_mixture_fallback");
                    row[k("team_incoming_effective_hp")]=QJsonObject{{k("current"),weighted?incomingA:nil()},{k("candidate"),weighted?incomingB:nil()}};
                    row[k("team_context")] = teammates.report;
                    row[k("team_objective")] = teamGain;
                    row[k("team_score_gain")] = teamGain.value(k("score_gain"));
                    if (teamGain.value(k("available")).toBool()) { sumTeamGain += teamGain.value(k("score_gain")).toDouble(); ++teamGains; }
                    if(allyHistorical.available){
                        const auto allyGain=compareHistoricalTeamOutputObjective(teammates,a,b,style,allyHistorical.selfShare,
                            weighted?incomingA.toDouble():-1,weighted?incomingB.toDouble():-1,weighted?otherHealth:-1);
                        const auto score=allyGain.value(k("score_gain"));
                        if(allyGain.value(k("available")).toBool()&&score.isDouble()&&std::isfinite(score.toDouble())){
                            row[k("historical_ally_output_team_score_gain")]=score;row[k("historical_ally_output_objective")]=allyGain;
                            sumAllyHistoricalGain+=score.toDouble();++allyHistoricalGains;++allyHistoricalPairs;
                        }
                    }
                    if(historical.available){
                        const auto historicalA=incomingTeamHealth(a,enemies,&historical.weights),historicalB=incomingTeamHealth(b,enemies,&historical.weights);
                        double historicalOther=0;bool historicalKnown=historicalA.isDouble()&&historicalB.isDouble()&&teammates.available;
                        for(const auto &ally:teammates.teammates){const auto hp=incomingTeamHealth(ally,enemies,&historical.weights);if(!hp.isDouble()){historicalKnown=false;break;}historicalOther+=hp.toDouble();}
                        if(historicalKnown){const auto historicalGain=compareTeamObjective(teammates,a,b,style,historicalA.toDouble(),historicalB.toDouble(),historicalOther);
                            const auto score=historicalGain.value(k("score_gain"));
                            if(historicalGain.value(k("available")).toBool()&&score.isDouble()&&std::isfinite(score.toDouble())){
                                row[k("historical_pressure_team_score_gain")]=score;sumHistoricalGain+=score.toDouble();++historicalGains;++historicalPairs;
                                row[k("historical_pressure_incoming_effective_hp")]=QJsonObject{{k("current"),historicalA},{k("candidate"),historicalB}};
                            }
                            if(allyHistorical.available){
                                const auto jointGain=compareHistoricalTeamOutputObjective(teammates,a,b,style,allyHistorical.selfShare,historicalA.toDouble(),historicalB.toDouble(),historicalOther);
                                const auto jointScore=jointGain.value(k("score_gain"));
                                if(jointGain.value(k("available")).toBool()&&jointScore.isDouble()&&std::isfinite(jointScore.toDouble())){
                                    row[k("historical_joint_team_score_gain")]=jointScore;
                                    sumJointHistoricalGain+=jointScore.toDouble();++jointHistoricalGains;++jointHistoricalPairs;
                                }
                            }
                        }
                    }
                    for (const auto &u : teammates.report.value(k("unknowns")).toArray()) unknowns.append(u.toString());
                    if(!allySourcesRecorded){allySourcesRecorded=true;const auto allyRows=teammates.report.value(k("teammates")).toArray();
                        for(int i=0;i<teammates.teammates.size()&&i<allyRows.size();++i)
                            recordUnknownSource(k("ally:")+QString::number(i),allyRows[i].toObject().value(k("champion_id")).toInt(),teammates.teammates[i].unknowns);}
                    if (!teammates.available) unknowns.append(k("incomplete_teammates_individual_objective_fallback"));
                    if (!weighted) unknowns.append(k("incomplete_enemy_pressure_reference_50_50_team_defense_fallback"));
                }
                row[k("defense_score_source")] = d.value(k("available")).toBool() ? k("visible_enemy_damage_share_model") : k("reference_50_50_fallback");
                row[k("target_estimate")] = QJsonObject{{k("armor"), c.targetArmor}, {k("magic_resist"), c.targetMr}, {k("max_health"), c.targetHp}, {k("exact_live_stats"), false}, {k("current_health_known"), false}};
                row[k("defense")] = d; collectUnknowns(unknowns, a); collectUnknowns(unknowns, b);
                if (row.value(k("score_gain")).isDouble()) { sumGain += row.value(k("score_gain")).toDouble(); ++gains; }
            } else { row[k("current")] = QJsonObject{{k("raw_eval"), nil()}}; row[k("candidate")] = QJsonObject{{k("raw_eval"), nil()}}; row[k("defense")] = defense(current, candidate, enemy); }
            targets.append(row);
            ++completedTargetPairs;
        }
        if (stopped("target_window")) return interrupted();
        windows.append(QJsonObject{{k("seconds"), seconds}, {k("targets"), targets}, {k("score_gain"), gains ? number(sumGain / gains) : nil()},
            {k("team_score_gain"), gains && teamGains == gains ? number(sumTeamGain / teamGains) : nil()}, {k("team_supported_targets"), teamGains},
            {k("historical_pressure_team_score_gain"),historicalGains==5?number(sumHistoricalGain/historicalGains):nil()},{k("historical_pressure_supported_targets"),historicalGains},
            {k("historical_ally_output_team_score_gain"),allyHistoricalGains==5?number(sumAllyHistoricalGain/allyHistoricalGains):nil()},
            {k("historical_ally_output_supported_targets"),allyHistoricalGains},
            {k("historical_joint_team_score_gain"),jointHistoricalGains==5?number(sumJointHistoricalGain/jointHistoricalGains):nil()},
            {k("historical_joint_supported_targets"),jointHistoricalGains},
            {k("supported_targets"), gains}, {k("aggregate"), k("equal_weight_available_target_relative_gains")}});
    }
    out[k("windows")] = windows;
    historical.report[k("validated_weights_available")]=historical.available;
    historical.report[k("scored_target_window_pairs")]=historicalPairs;
    historical.report[k("available")]=historical.available&&options.teamObjective&&historicalPairs==15;
    if(historical.available&&historicalPairs!=15)historical.report[k("reason")]=options.teamObjective?k("historical_pressure_team_context_incomplete"):k("historical_pressure_team_objective_not_requested");
    out[k("historical_pressure_scenario")]=historical.report;
    allyHistorical.report[k("validated_weights_available")]=allyHistorical.available;
    allyHistorical.report[k("scored_target_window_pairs")]=allyHistoricalPairs;
    allyHistorical.report[k("available")]=allyHistorical.available&&options.teamObjective&&allyHistoricalPairs==15;
    if(allyHistorical.available&&allyHistoricalPairs!=15)allyHistorical.report[k("reason")]=options.teamObjective?k("historical_ally_team_context_incomplete"):k("historical_ally_team_objective_not_requested");
    out[k("historical_ally_output_scenario")]=allyHistorical.report;
    const bool historicalReady=historical.report.value(k("available")).toBool(),allyReady=allyHistorical.report.value(k("available")).toBool();
    out[k("historical_roster_scenario")]=QJsonObject{{k("available"),historicalReady||allyReady},
        {k("enemy_pressure_available"),historicalReady},{k("ally_output_available"),allyReady},
        {k("combined_ten_player_sensitivity"),historicalReady&&allyReady&&jointHistoricalPairs==15},
        {k("scored_target_window_pairs"),std::max({historicalPairs,allyHistoricalPairs,jointHistoricalPairs})},
        {k("joint_target_window_pairs"),jointHistoricalPairs},{k("current_baseline_replaced"),false},
        {k("scope"),k("historical_output_allocation_and_incoming_pressure_sensitivity")},
        {k("causal_effect_known"),false},{k("support_or_control_multiplier_invented"),false}};
    out[k("historical_roster_summary")]=summarizeRosterHistoryScenarios(out);
    // These intervals partition the SAME cumulative scenario. Never add the
    // 3/8/20-second totals, which would award an opening cast three times.
    QJsonArray intervals;
    for(int targetIndex=0;targetIndex<enemies.size();++targetIndex){double previousSeconds=0,previousA=0,previousB=0;bool supported=true;QJsonArray segments;
        for(const auto &v:windows){const auto w=v.toObject();const auto row=w.value(k("targets")).toArray().at(targetIndex).toObject();
            const auto a=row.value(k("current")).toObject().value(k("modeled_output")),b=row.value(k("candidate")).toObject().value(k("modeled_output"));
            supported &= a.isDouble()&&b.isDouble()&&a.toDouble()>=previousA&&b.toDouble()>=previousB;
            segments.append(QJsonObject{{k("start_seconds"),previousSeconds},{k("end_seconds"),w.value(k("seconds"))},
                {k("current_output"),supported?number(a.toDouble()-previousA):nil()},{k("candidate_output"),supported?number(b.toDouble()-previousB):nil()}});
            previousSeconds=w.value(k("seconds")).toDouble();previousA=a.toDouble();previousB=b.toDouble();
        }
        intervals.append(QJsonObject{{k("champion_id"),enemies[targetIndex].champion},{k("target_index"),enemies[targetIndex].index},
            {k("available"),supported},{k("segments"),segments},{k("scope"),k("nonoverlapping_differences_of_one_conditional_cumulative_model_not_observed_hits")}});
    }
    out[k("continuous_output_intervals")]=intervals;
    if (options.includeGrowthProjection) {
        const int endLevel = std::min(18, state.level + 2);
        if (stopped("growth_baseline")) return interrupted();
        QJsonObject currentGrowth;
        const auto growthKey = reuse ? contextBasis(ctx) : QByteArray{};
        if (reuse && reuse->growth.contains(growthKey)) { currentGrowth = reuse->growth.value(growthKey); ++reuse->growthHits; }
        else {
            currentGrowth = predictCombatGrowth(db, ctx, state.owned, augments, endLevel);
            if (reuse) { ++reuse->growthEvaluations; if (reuse->growth.isEmpty()) reuse->growth.insert(growthKey, currentGrowth); }
        }
        if (stopped("growth_baseline")) return interrupted();
        if (stopped("growth_candidate")) return interrupted();
        const auto candidateGrowth = predictCombatGrowth(db, candidateContext(ctx), candidateInventory, candidateAugments, endLevel);
        if (stopped("growth_candidate")) return interrupted();
        out[k("growth")] = QJsonObject{{k("start_level"), state.level}, {k("end_level"), endLevel}, {k("skill_rank_policy"), k("current_or_reference_ranks_held_fixed_no_extra_skill_points_inferred")},
            {k("additional_future_stacks_predicted"), false}, {k("current"), currentGrowth},
            {k("candidate"), candidateGrowth}};
        if (hasScenario) {
            if (stopped("growth_scenario_candidate")) return interrupted();
            auto growth = out.value(k("growth")).toObject(); growth[k("scenario_candidate")] = predictCombatGrowth(db, scenario, candidateInventory, candidateAugments, endLevel);
            if (stopped("growth_scenario_candidate")) return interrupted();
            growth[k("scenario_is_observed")] = false; out[k("growth")] = growth;
        }
    }
    coverage[k("target_window_pairs")] = targetPairs; coverage[k("target_comparison_evaluations")] = comparisonCalls;
    coverage[k("all_equipment_and_augments_use_shared_combat_rules")] = true;
    coverage[k("candidate_actions_do_not_receive_historical_events")] = true;
    if (stopped("complete")) return interrupted();
    out[k("available")] = true;
    return finish(k("partial_deterministic_model"));
}
