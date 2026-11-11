#include "live_combat_summary.h"
#include "tactical_context.h"
#include "analysis.h"
#include "combat.h"
#include "headliner.h"
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QString q(const char *text) { return QString::fromUtf8(text); }
QJsonValue nullValue() { return QJsonValue(QJsonValue::Null); }
bool integer(const QJsonValue &value, int minimum, int maximum, int *out = nullptr) {
    if (!value.isDouble() || !std::isfinite(value.toDouble())
        || value.toDouble() < minimum || value.toDouble() > maximum
        || std::floor(value.toDouble()) != value.toDouble()) return false;
    if (out) *out = int(value.toDouble());
    return true;
}
QJsonValue numberOrNull(const QJsonValue &value, int minimum = 0) {
    return integer(value, minimum, std::numeric_limits<int>::max()) ? value : nullValue();
}
QJsonArray jsonIds(const QList<int> &ids) {
    QJsonArray out;
    for (int id : ids) out.append(id);
    return out;
}
QList<int> readIds(const QJsonValue &input, bool &valid, int limit) {
    QList<int> ids;
    valid = input.isArray() && input.toArray().size() <= limit;
    for (const auto &value : input.toArray()) {
        int id = 0;
        if (integer(value, 1, std::numeric_limits<int>::max(), &id)) ids.append(id);
        else valid = false;
    }
    return ids;
}
QJsonObject scores(const QJsonObject &raw) {
    QJsonObject out;
    for (const auto &key : {q("kills"), q("deaths"), q("assists"), q("creepScore")})
        out[key] = numberOrNull(raw.value(key));
    return out;
}
QJsonObject observedPanel(const GameState &state) {
    QJsonObject out;
    // Current HP, resource balance, KDA and economy are deliberately excluded.
    for (const auto &key : {q("abilityPower"), q("attackDamage"), q("maxHealth"),
                           q("armor"), q("magicResist"), q("attackSpeed"), q("moveSpeed")}) {
        const auto value = state.liveStats.value(key);
        if (value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0)
            out[key] = value;
    }
    return out;
}
QMap<QString, int> referenceRanks(const CombatDatabase &db, int champion, int level) {
    QMap<QString, int> maximum, ranks;
    for (const auto &value : db.champions.value(champion).value(q("spells")).toArray()) {
        const auto spell = value.toObject();
        maximum[spell.value(q("slot")).toString()] = spell.value(q("max_rank")).toInt();
    }
    const int ultimate = std::min(maximum.value(q("R")), level >= 16 ? 3 : level >= 11 ? 2 : level >= 6 ? 1 : 0);
    int remaining = level - ultimate;
    for (const auto &slot : {q("Q"), q("W"), q("E")}) {
        const int rank = std::max(0, std::min({maximum.value(slot), remaining, (level + 1) / 2}));
        ranks[slot] = rank;
        remaining -= rank;
    }
    ranks[q("R")] = ultimate;
    return ranks;
}
QString relativeTeam(const QString &raw, const QString &own) {
    if ((own == q("ORDER") || own == q("CHAOS")) && (raw == q("ORDER") || raw == q("CHAOS")))
        return raw == own ? q("ALLY") : q("ENEMY");
    if ((own == q("ALLY") || own == q("ENEMY")) && (raw == q("ALLY") || raw == q("ENEMY")))
        return raw == own ? q("ALLY") : q("ENEMY");
    return q("UNKNOWN");
}
QJsonValue equipmentValue(const Catalog &catalog, const QList<int> &items, bool complete) {
    if (!complete) return nullValue();
    qint64 total = 0;
    for (int id : items) {
        const auto price = catalog.items.value(id).value(q("price_total"));
        if (!integer(price, 0, std::numeric_limits<int>::max())) return nullValue();
        total += price.toInt();
        if (total > std::numeric_limits<int>::max()) return nullValue();
    }
    return int(total);
}
struct Fighter {
    int champion = 0, level = 0;
    QString team;
    bool self = false, complete = false, augmentsComplete = false, selfContext = false, valid = false;
    QList<int> items, augments;
    QJsonObject basis, anchor;
    QStringList unknowns;
    CombatContext context;
    CombatEvaluation value;
};
QJsonObject emptyModel(const Fighter &fighter, const Catalog &catalog) {
    return {{q("output_8s_estimate"), nullValue()}, {q("effective_health_estimate"), nullValue()},
        {q("model_available"), false}, {q("model_partial"), true},
        {q("model_unknowns"), QJsonArray::fromStringList(fighter.unknowns)},
        {q("equipment_value"), equipmentValue(catalog, fighter.items, fighter.complete)},
        {q("output_components"), QJsonObject{}}, {q("target_context"), QJsonObject{}},
        {q("self_observed_anchor"), QJsonObject{{q("available"), false}}}};
}
QStringList orderedUnknowns(QStringList values) {
    values.removeDuplicates();
    values.sort();
    return values;
}
}

QJsonObject observedLiveCombatSummary(const Catalog &catalog, const GameState &state) {
    const auto now = QDateTime::currentDateTimeUtc();
    const bool replay = state.source == GameState::Source::Replay;
    const qint64 age = state.received.isValid() ? state.received.msecsTo(now) : -1;
    const bool fresh = replay || (state.source == GameState::Source::Live && age >= 0 && age < 4000);
    QString context = state.matchContext.value(q("context_id")).toString();
    static const QRegularExpression format(q("^[A-Za-z0-9_-]{1,96}$"));
    bool contextKnown = state.matchContext.value(q("active")).toBool() && format.match(context).hasMatch();
    if (replay && !contextKnown) { context = q("synthetic_replay"); contextKnown = true; }
    QJsonObject out{{q("schema_version"), 1}, {q("available"), false}, {q("fresh"), false},
        {q("current_context"), false}, {q("context_id"), nullValue()}, {q("game_time"), nullValue()},
        {q("source"), replay ? q("demo_or_replay") : state.source == GameState::Source::Live ? q("live_read_only") : q("unavailable")},
        {q("snapshot_scope"), replay ? q("synthetic_or_replay") : q("current_match_anonymous")},
        {q("actual_damage_source"), q("unavailable_live_source")}, {q("actual_damage_available"), false},
        {q("actual_damage_statistics"), false}, {q("match_win_rate"), nullValue()},
        {q("summary_period_ms"), 1000}, {q("freshness_limit_ms"), 4000},
        {q("snapshot_age_ms"), replay ? QJsonValue(0) : age >= 0 ? QJsonValue(age) : nullValue()},
        {q("horizon_seconds"), 8}, {q("players"), QJsonArray{}}, {q("cache_hit"), false},
        {q("model_available_players"), 0}, {q("model_kind"), q("observation_only_waiting_async_model")},
        {q("status"), q("waiting_snapshot")}};
    if (!fresh || state.source == GameState::Source::Offline) {
        out[q("status")] = state.source == GameState::Source::Offline ? q("offline") : q("stale_snapshot");
        return out;
    }
    if (!contextKnown || (state.phase != q("InProgress") && state.phase != q("Reconnect"))
        || !std::isfinite(state.gameTime) || state.gameTime < 0 || state.players.isEmpty() || state.players.size() > 10) {
        out[q("status")] = !contextKnown ? q("waiting_match_context") : q("waiting_live_roster");
        return out;
    }
    out[q("context_id")] = context; out[q("current_context")] = true; out[q("fresh")] = true;
    out[q("game_time")] = state.gameTime;
    out[q("observed_patch")] = state.observedPatch.isEmpty() ? nullValue() : QJsonValue(state.observedPatch);
    out[q("catalog_patch")] = catalog.patch;
    out[q("last_observed_ms")] = state.received.isValid() ? state.received.toMSecsSinceEpoch() : now.toMSecsSinceEpoch();
    int selfCount = 0; QString ownTeam;
    for (const auto &value : state.players) if (value.toObject().value(q("self")).toBool()) {
        ++selfCount; ownTeam = value.toObject().value(q("team")).toString();
    }
    if (selfCount != 1) ownTeam.clear();
    QJsonArray rows;
    for (int index = 0; index < state.players.size(); ++index) {
        const auto raw = state.players[index].toObject();
        Fighter fighter;
        integer(raw.value(q("champion_id")), 1, std::numeric_limits<int>::max(), &fighter.champion);
        integer(raw.value(q("level")), 1, std::numeric_limits<int>::max(), &fighter.level);
        fighter.team = relativeTeam(raw.value(q("team")).toString(), ownTeam);
        fighter.self = selfCount == 1 && raw.value(q("self")).toBool();
        bool validItems = false, validAugments = false;
        fighter.items = readIds(raw.value(q("items")), validItems, 6);
        fighter.augments = readIds(raw.value(q("augment_ids")), validAugments, 16);
        fighter.complete = validItems && raw.value(q("items_complete")).isBool() && raw.value(q("items_complete")).toBool();
        fighter.augmentsComplete = validAugments && raw.value(q("augments_complete")).isBool() && raw.value(q("augments_complete")).toBool();
        if (fighter.self && state.championId == fighter.champion && state.level == fighter.level)
            for (int id : effectiveSelectedAugments(catalog, state)) if (id > 0 && !fighter.augments.contains(id)) fighter.augments.append(id);
        std::sort(fighter.items.begin(), fighter.items.end()); std::sort(fighter.augments.begin(), fighter.augments.end());
        fighter.unknowns.append(q("async_model_not_ready_for_current_basis"));
        if (!fighter.complete) fighter.unknowns.append(q("inventory_incomplete"));
        if (!fighter.augmentsComplete) fighter.unknowns.append(q("augments_unobserved_or_incomplete"));
        auto row = emptyModel(fighter, catalog);
        const auto score = scores(raw.value(q("scores")).toObject());
        QJsonValue kda = nullValue();
        if (score.value(q("kills")).isDouble() && score.value(q("deaths")).isDouble() && score.value(q("assists")).isDouble())
            kda = (score.value(q("kills")).toDouble() + score.value(q("assists")).toDouble()) / std::max(1., score.value(q("deaths")).toDouble());
        const QJsonObject observed{{q("row_index"), index}, {q("champion_id"), fighter.champion > 0 ? QJsonValue(fighter.champion) : nullValue()},
            {q("team"), fighter.team}, {q("self"), fighter.self}, {q("level"), fighter.level > 0 ? QJsonValue(fighter.level) : nullValue()},
            {q("items"), jsonIds(fighter.items)}, {q("items_complete"), fighter.complete},
            {q("augment_ids"), jsonIds(fighter.augments)}, {q("augments_complete"), fighter.augmentsComplete},
            {q("scores"), score}, {q("cs"), score.value(q("creepScore"))}, {q("kda"), kda},
            {q("is_dead"), raw.value(q("is_dead")).isBool() ? raw.value(q("is_dead")) : nullValue()},
            {q("last_observed_ms"), out.value(q("last_observed_ms"))}, {q("last_observed_game_time"), state.gameTime},
            {q("damage_to_champions"), nullValue()}, {q("damage_taken"), nullValue()},
            {q("actual_damage_source"), q("unavailable_live_source")}, {q("tactical_role"), QJsonObject{}}};
        for (auto it = observed.begin(); it != observed.end(); ++it) row[it.key()] = it.value();
        rows.append(row);
    }
    out[q("players")] = rows; out[q("available")] = !rows.isEmpty(); out[q("status")] = q("observed_only");
    out[q("tactical_context")] = QJsonObject{{q("available"), false}, {q("reason"), q("async_model_pending")}, {q("players"), QJsonArray{}}};
    out[q("kda_definition")] = q("(kills+assists)/max(1,deaths); missing scores remain null");
    return out;
}

QByteArray liveCombatSummaryBasis(const Catalog &catalog, const GameState &state) {
    QJsonArray rows;
    for (const auto &value : state.players) {
        const auto raw = value.toObject(); QJsonObject row;
        for (const auto &name : {q("champion_id"), q("level"), q("team"), q("self"), q("items_complete"), q("augments_complete")})
            row[name] = QJsonObject{{q("present"), raw.contains(name)}, {q("value"), raw.value(name)}};
        for (const auto &name : {q("items"), q("augment_ids")}) {
            bool valid = false; auto values = readIds(raw.value(name), valid, name == q("items") ? 6 : 16);
            std::sort(values.begin(), values.end());
            row[name] = QJsonObject{{q("present"), raw.contains(name)}, {q("valid"), valid}, {q("values"), jsonIds(values)}};
        }
        rows.append(row);
    }
    QJsonObject counters, ranks;
    for (auto it = state.effectCounters.begin(); it != state.effectCounters.end(); ++it) if (std::isfinite(it.value())) counters[it.key()] = it.value();
    for (const auto &slot : {q("Q"), q("W"), q("E"), q("R")}) ranks[slot] = state.abilities.value(slot).toObject().value(q("abilityLevel"));
    auto conditions = state.effectConditions.values(); conditions.sort();
    const auto headliner = observedHeadliner(state);
    const QJsonObject basis{{q("context"), state.matchContext.value(q("context_id"))}, {q("active"), state.matchContext.value(q("active"))},
        {q("source"), int(state.source)}, {q("phase"), state.phase}, {q("champion"), state.championId}, {q("level"), state.level},
        {q("catalog_patch"), catalog.patch}, {q("observed_patch"), state.observedPatch}, {q("mode"), state.gameMode},
        {q("database"), QString::number(quintptr(catalog.combat.get()), 16)}, {q("revision"), catalog.combat ? catalog.combat->revision : QString()},
        {q("players"), rows}, {q("observed_panel"), observedPanel(state)}, {q("ranks"), ranks}, {q("counters"), counters},
        {q("conditions"), QJsonArray::fromStringList(conditions)}, {q("effective_augments"), jsonIds(effectiveSelectedAugments(catalog, state))},
        {q("headliner_rule"), headliner.value(q("rule_key"))}, {q("headliner_facts"), headliner.value(q("facts"))},
        {q("unprojected_events"), !state.mechanicObservations.isEmpty()}, {q("tactical_history"), state.tacticalHistory},
        {q("recommendation_evidence"), state.recommendationEvidence}};
    return QJsonDocument(basis).toJson(QJsonDocument::Compact);
}

void LiveCombatSummary::clearCache() {
    if (!modelBasis.isEmpty() || !activeContext.isEmpty()) ++clears;
    modelBasis.clear();
    modelRows = {};
    databaseIdentity = nullptr;
    activeContext.clear();
    lastGameTime = -1;
    modeledPlayers = 0;
}

QJsonObject LiveCombatSummary::update(const Catalog &catalog, const GameState &state, const RecommendationWorkPtr &work) {
    QElapsedTimer updateTimer;
    updateTimer.start();
    ++updates;
    const auto now = QDateTime::currentDateTimeUtc();
    const bool replay = state.source == GameState::Source::Replay;
    const auto age = state.received.isValid() ? state.received.msecsTo(now) : qint64(-1);
    const bool fresh = replay || (state.source == GameState::Source::Live && age >= 0 && age < 4000);
    const auto source = replay ? q("demo_or_replay") : state.source == GameState::Source::Live ? q("live_read_only") : q("unavailable");
    QString context = state.matchContext.value(q("context_id")).toString();
    static const QRegularExpression contextFormat(q("^[A-Za-z0-9_-]{1,96}$"));
    bool contextKnown = state.matchContext.value(q("active")).toBool() && contextFormat.match(context).hasMatch();
    if (replay && !contextKnown) { context = q("synthetic_replay"); contextKnown = true; }
    QJsonObject out{{q("schema_version"), 1}, {q("available"), false}, {q("fresh"), false},
        {q("current_context"), false}, {q("context_id"), nullValue()}, {q("game_time"), nullValue()},
        {q("source"), source}, {q("snapshot_scope"), replay ? q("synthetic_or_replay") : q("current_match_anonymous")},
        {q("actual_damage_source"), q("unavailable_live_source")}, {q("actual_damage_available"), false},
        {q("actual_damage_statistics"), false}, {q("match_win_rate"), nullValue()},
        {q("summary_period_ms"), 1000}, {q("freshness_limit_ms"), 4000},
        {q("snapshot_age_ms"), replay ? QJsonValue(0) : age >= 0 ? QJsonValue(age) : nullValue()},
        {q("horizon_seconds"), 8}, {q("players"), QJsonArray{}}, {q("cache_hit"), false}};
    const auto finish = [&](const QString &status) {
        out[q("rule_basis")]=modelRuleBasis(catalog,state);
        lastStatus = status;
        lastFresh = out.value(q("fresh")).toBool();
        lastUpdateMs = double(updateTimer.nsecsElapsed()) / 1e6;
        out[q("status")] = status;
        out[q("update_ms")] = lastUpdateMs;
        out[q("model_compute_ms")] = lastComputeMs;
        out[q("model_generation")] = generation;
        return out;
    };
    const auto interrupted = [&]() {
        clearCache();
        out = observedLiveCombatSummary(catalog, state);
        out[q("stop_reason")] = work ? work->stopReason() : q("interrupted");
        return finish(q("model_interrupted"));
    };
    if (work && work->stopped()) return interrupted();
    if (!fresh || state.source == GameState::Source::Offline) {
        clearCache();
        return finish(state.source == GameState::Source::Offline ? q("offline") : q("stale_snapshot"));
    }
    if (!contextKnown || (state.phase != q("InProgress") && state.phase != q("Reconnect"))
        || !std::isfinite(state.gameTime) || state.gameTime < 0 || state.players.isEmpty() || state.players.size() > 10) {
        clearCache();
        return finish(!contextKnown ? q("waiting_match_context") : q("waiting_live_roster"));
    }
    QString resetReason;
    if (context != activeContext) { resetReason = q("context_changed"); clearCache(); }
    else if (lastGameTime >= 0 && state.gameTime + 1e-6 < lastGameTime) {
        resetReason = q("game_time_regressed");
        clearCache();
    }
    activeContext = context;
    lastGameTime = state.gameTime;
    out[q("context_id")] = context;
    out[q("current_context")] = true;
    out[q("fresh")] = true;
    out[q("game_time")] = state.gameTime;
    out[q("cache_reset_reason")] = resetReason.isEmpty() ? nullValue() : QJsonValue(resetReason);
    out[q("observed_patch")] = state.observedPatch.isEmpty() ? nullValue() : QJsonValue(state.observedPatch);
    out[q("catalog_patch")] = catalog.patch;
    out[q("last_observed_ms")] = state.received.isValid() ? state.received.toMSecsSinceEpoch() : now.toMSecsSinceEpoch();

    int selfCount = 0;
    QString ownTeam;
    for (const auto &value : state.players) if (value.toObject().value(q("self")).toBool()) {
        ++selfCount;
        ownTeam = value.toObject().value(q("team")).toString();
    }
    if (selfCount != 1) ownTeam.clear();
    const auto *db = catalog.combat.get();
    const bool versionMatches = db && modelPatchMatches(catalog, state);
    const bool rulesUsable = modelRulesUsable(catalog,state);
    const bool modeMatches = state.gameMode == q("KIWI");
    const auto panel = observedPanel(state);
    const auto headliner = observedHeadliner(state);
    QVector<Fighter> fighters;
    QJsonArray bases;
    for (const auto &value : state.players) {
        const auto raw = value.toObject();
        Fighter fighter;
        integer(raw.value(q("champion_id")), 1, std::numeric_limits<int>::max(), &fighter.champion);
        integer(raw.value(q("level")), 1, std::numeric_limits<int>::max(), &fighter.level);
        fighter.team = relativeTeam(raw.value(q("team")).toString(), ownTeam);
        fighter.self = selfCount == 1 && raw.value(q("self")).toBool();
        fighter.selfContext = fighter.self && state.championId == fighter.champion && state.level == fighter.level;
        bool validItems = false, validAugments = false;
        fighter.items = readIds(raw.value(q("items")), validItems, 6);
        fighter.augments = readIds(raw.value(q("augment_ids")), validAugments, 16);
        fighter.complete = validItems && raw.value(q("items_complete")).isBool() && raw.value(q("items_complete")).toBool();
        fighter.augmentsComplete = validAugments && raw.value(q("augments_complete")).isBool() && raw.value(q("augments_complete")).toBool();
        if (fighter.selfContext) for (int id : effectiveSelectedAugments(catalog, state))
            if (id > 0 && !fighter.augments.contains(id)) fighter.augments.append(id);
        std::sort(fighter.items.begin(), fighter.items.end());
        std::sort(fighter.augments.begin(), fighter.augments.end());
        if (!fighter.complete) fighter.unknowns.append(q("inventory_incomplete"));
        if (!fighter.augmentsComplete) fighter.unknowns.append(q("augments_unobserved_or_incomplete"));
        if (!validAugments) fighter.unknowns.append(q("augment_list_not_observed"));
        if (fighter.team == q("UNKNOWN")) fighter.unknowns.append(q("own_team_unverified"));
        if (fighter.self && !fighter.selfContext) fighter.unknowns.append(q("self_panel_context_mismatch"));
        if (!versionMatches) fighter.unknowns.append(q("model_patch_unverified_or_mismatch"));
        if (!modeMatches) fighter.unknowns.append(q("kiwi_mode_unverified"));
        if (fighter.level < 1 || fighter.level > 18) fighter.unknowns.append(q("level_outside_model_1_18"));
        if (!db || !db->champions.contains(fighter.champion)) fighter.unknowns.append(q("champion_not_modeled"));
        bool itemsCovered = db != nullptr;
        if (db) for (int id : fighter.items) if (!db->items.contains(id)) {
            itemsCovered = false;
            fighter.unknowns.append(q("item_not_modeled:") + QString::number(id));
        }
        fighter.valid = rulesUsable && modeMatches && fighter.complete && itemsCovered
            && db->champions.contains(fighter.champion) && fighter.level >= 1 && fighter.level <= 18;
        fighter.basis = {{q("champion_id"), fighter.champion}, {q("level"), fighter.level},
            {q("team"), fighter.team}, {q("self"), fighter.self}, {q("self_context"), fighter.selfContext},
            {q("items"), jsonIds(fighter.items)}, {q("inventory_complete"), fighter.complete},
            {q("augment_ids"), jsonIds(fighter.augments)}, {q("augments_complete"), fighter.augmentsComplete},
            {q("valid_augment_list"), validAugments}};
        bases.append(fighter.basis);
        fighters.append(fighter);
    }
    QJsonObject counters, ranks;
    for (auto it = state.effectCounters.cbegin(); it != state.effectCounters.cend(); ++it)
        if (std::isfinite(it.value())) counters[it.key()] = it.value();
    QStringList conditions = state.effectConditions.values();
    conditions.sort();
    for (const auto &slot : {q("Q"), q("W"), q("E"), q("R")})
        ranks[slot] = state.abilities.value(slot).toObject().value(q("abilityLevel"));
    const QJsonObject basis{{q("context_id"), context}, {q("source"), source},
        {q("patch"), catalog.patch}, {q("observed_patch"), state.observedPatch},
        {q("revision"), db ? db->revision : QString()}, {q("mode"), state.gameMode},
        {q("players"), bases}, {q("observed_panel"), panel}, {q("ability_ranks"), ranks},
        {q("self_counters"), counters}, {q("self_conditions"), QJsonArray::fromStringList(conditions)},
        {q("headliner_rule"), headliner.value(q("rule_key"))}, {q("headliner_facts"), headliner.value(q("facts"))},
        {q("unprojected_mechanic_events"), !state.mechanicObservations.isEmpty()}};
    const auto bytes = QJsonDocument(basis).toJson(QJsonDocument::Compact);
    const bool hit = bytes == modelBasis && databaseIdentity == db && modelRows.size() == fighters.size();
    if (hit) ++hits;
    else {
        ++misses;
        ++generation;
        QElapsedTimer computeTimer;
        computeTimer.start();
        modelRows = {};
        modeledPlayers = 0;
        for (auto &fighter : fighters) {
            if (work && work->stopped()) return interrupted();
            if (!fighter.valid) continue;
            fighter.context = db->context(fighter.champion, fighter.level);
            fighter.context.horizon = 8;
            fighter.context.ranks = referenceRanks(*db, fighter.champion, fighter.level);
            bool allRanksObserved = fighter.selfContext;
            if (fighter.selfContext) {
                fighter.context.counters = state.effectCounters;
                fighter.context.conditions = state.effectConditions;
                for (const auto &slot : {q("Q"), q("W"), q("E"), q("R")}) {
                    int rank = 0;
                    if (integer(state.abilities.value(slot).toObject().value(q("abilityLevel")), 0, 6, &rank))
                        fighter.context.ranks[slot] = rank;
                    else allRanksObserved = false;
                }
            }
            if (!allRanksObserved) fighter.unknowns.append(q("reference_skill_allocation"));
            if (headliner.isEmpty()) fighter.unknowns.append(q("headliner_context_unobserved"));
            else fighter.unknowns.append(q("headliner_conditional_effects_partial"));
            if (fighter.selfContext && !state.mechanicObservations.isEmpty())
                fighter.unknowns.append(q("event_ledger_not_projected_in_lightweight_summary"));
            fighter.value = evaluateCombat(*db, fighter.context, fighter.items, fighter.augments);
            if (work && work->stopped()) return interrupted();
            ++evaluations;
            if (!fighter.value.valid) {
                fighter.valid = false;
                fighter.unknowns.append(q("combat_combination_unavailable"));
                continue;
            }
            if (fighter.selfContext && !panel.isEmpty()) {
                const auto beforeAnchor = fighter.context;
                fighter.anchor = anchorObservedCombatStats(*db, fighter.context, fighter.items, fighter.augments, panel);
                if (work && work->stopped()) return interrupted();
                evaluations += fighter.anchor.value(q("evaluations")).toInt();
                if (fighter.anchor.value(q("baseline_matched")).toBool()) {
                    fighter.value = evaluateCombat(*db, fighter.context, fighter.items, fighter.augments);
                    ++evaluations;
                    fighter.unknowns.append(q("observed_panel_residual_not_causal"));
                } else {
                    fighter.context = beforeAnchor;
                    fighter.unknowns.append(q("observed_panel_anchor_unmatched"));
                }
            }
            fighter.valid = fighter.value.valid && std::isfinite(fighter.value.stats[Stat::Armor])
                && std::isfinite(fighter.value.stats[Stat::Mr]) && std::isfinite(fighter.value.stats[Stat::Hp])
                && fighter.value.stats[Stat::Hp] > 0;
        }
        for (auto &fighter : fighters) {
            if (work && work->stopped()) return interrupted();
            if (!fighter.valid) { modelRows.append(emptyModel(fighter, catalog)); continue; }
            const auto opposing = fighter.team == q("ALLY") ? q("ENEMY") : fighter.team == q("ENEMY") ? q("ALLY") : q("UNKNOWN");
            int count = 0, visible = 0, missingAugments = 0;
            double armor = 0, mr = 0, hp = 0;
            bool opposingPartial = false;
            for (const auto &other : fighters) if (opposing != q("UNKNOWN") && other.team == opposing) {
                ++visible;
                if (!other.valid) continue;
                ++count;
                armor += other.value.stats[Stat::Armor];
                mr += other.value.stats[Stat::Mr];
                hp += other.value.stats[Stat::Hp];
                if (!other.augmentsComplete) ++missingAugments;
                opposingPartial |= !other.value.unknowns.isEmpty() || !other.unknowns.isEmpty();
            }
            const bool meanAvailable = count == 5 && visible == 5;
            if (meanAvailable) {
                fighter.context.targetArmor = armor / count;
                fighter.context.targetMr = mr / count;
                fighter.context.targetHp = hp / count;
                if (opposingPartial) fighter.unknowns.append(q("opponent_model_partial"));
                if (missingAugments) fighter.unknowns.append(q("opponent_augments_unobserved"));
            } else fighter.unknowns.append(q("opponent_loadouts_incomplete_reference_target"));
            const QJsonObject target{{q("source"), meanAvailable ? q("conditional_opposing_loadout_mean") : q("reference_target")},
                {q("available"), meanAvailable}, {q("exact_live_resistances"), false},
                {q("modeled_players"), count}, {q("observed_players"), visible},
                {q("unobserved_augment_players"), missingAugments},
                {q("armor"), fighter.context.targetArmor}, {q("magic_resist"), fighter.context.targetMr},
                {q("max_health"), fighter.context.targetHp}};
            const auto result = evaluateCombat(*db, fighter.context, fighter.items, fighter.augments);
            if (work && work->stopped()) return interrupted();
            ++evaluations;
            fighter.unknowns.append(result.unknowns);
            fighter.unknowns = orderedUnknowns(fighter.unknowns);
            auto model = emptyModel(fighter, catalog);
            model[q("target_context")] = target;
            model[q("self_observed_anchor")] = fighter.anchor.isEmpty() ? QJsonObject{{q("available"), false}} : fighter.anchor;
            // onhitDamage is a subset of procDamage; replacementAttackDamage is
            // a subset of skillDamage. Adding either again double counts it.
            const double output = result.attackDamage + result.skillDamage + result.procDamage;
            if (result.valid && std::isfinite(output) && output >= 0
                && std::isfinite(result.survival) && result.survival > 0) {
                model[q("output_8s_estimate")] = output;
                model[q("effective_health_estimate")] = result.survival;
                model[q("model_available")] = true;
                model[q("model_partial")] = !fighter.unknowns.isEmpty();
                model[q("output_components")] = QJsonObject{{q("attack"), result.attackDamage},
                    {q("skill"), result.skillDamage}, {q("proc"), result.procDamage},
                    {q("onhit_subset"), result.onhitDamage}, {q("replacement_subset"), result.replacementAttackDamage},
                    {q("sum_rule"), q("attack+skill+proc; subsets not added again")}};
                ++modeledPlayers;
            } else {
                fighter.unknowns.append(q("combat_evaluation_unavailable"));
                model[q("model_unknowns")] = QJsonArray::fromStringList(orderedUnknowns(fighter.unknowns));
            }
            modelRows.append(model);
        }
        modelBasis = bytes;
        databaseIdentity = db;
        lastComputeMs = double(computeTimer.nsecsElapsed()) / 1e6;
        maxComputeMs = std::max(maxComputeMs, lastComputeMs);
    }
    if (work && work->stopped()) return interrupted();
    const auto tactics=analyzeTacticalContext(catalog,state);
    if (work && work->stopped()) return interrupted();
    out[q("tactical_context")]=tactics;
    QJsonArray rows;
    for (int index = 0; index < fighters.size(); ++index) {
        const auto &fighter = fighters[index];
        const auto raw = state.players[index].toObject();
        const auto score = scores(raw.value(q("scores")).toObject());
        QJsonValue kda = nullValue();
        if (score.value(q("kills")).isDouble() && score.value(q("deaths")).isDouble() && score.value(q("assists")).isDouble())
            kda = (score.value(q("kills")).toDouble() + score.value(q("assists")).toDouble()) / std::max(1., score.value(q("deaths")).toDouble());
        auto row = modelRows[index].toObject();
        row[q("tactical_role")]=tacticalPlayer(tactics,index);
        const QJsonObject projection{{q("row_index"), index}, {q("champion_id"), fighter.champion > 0 ? QJsonValue(fighter.champion) : nullValue()},
            {q("team"), fighter.team}, {q("self"), fighter.self}, {q("level"), fighter.level > 0 ? QJsonValue(fighter.level) : nullValue()},
            {q("items"), jsonIds(fighter.items)}, {q("items_complete"), fighter.complete},
            {q("augment_ids"), jsonIds(fighter.augments)}, {q("augments_complete"), fighter.augmentsComplete},
            {q("scores"), score}, {q("cs"), score.value(q("creepScore"))}, {q("kda"), kda},
            {q("is_dead"), raw.value(q("is_dead")).isBool() ? raw.value(q("is_dead")) : nullValue()},
            {q("last_observed_ms"), out.value(q("last_observed_ms"))}, {q("last_observed_game_time"), state.gameTime},
            {q("damage_to_champions"), nullValue()}, {q("damage_taken"), nullValue()},
            {q("actual_damage_source"), q("unavailable_live_source")}};
        for (auto it = projection.constBegin(); it != projection.constEnd(); ++it) row[it.key()] = it.value();
        rows.append(row);
    }
    out[q("players")] = rows;
    out[q("available")] = !rows.isEmpty();
    out[q("model_available_players")] = modeledPlayers;
    out[q("cache_hit")] = hit;
    out[q("model_kind")] = q("conditional_partial_combat_estimate");
    out[q("kda_definition")] = q("(kills+assists)/max(1,deaths); missing scores remain null");
    out[q("assumptions")] = QJsonArray{q("8s continuous attack/cast comparison; not observed combat or a win rate"),
        q("Full modeled health; 50% physical/50% magic effective-health reference; no prediction of hit rate, positioning or control"),
        q("Other players use reference skill allocation; unknown augments and trigger conditions remain partial"),
        q("Opposing defense is a conditional loadout mean only with five modeled opponents, otherwise 80 armor/60 MR/2500 HP reference"),
        q("Self visible combat-panel residual may anchor stats without identifying the source of its bonuses"),
        q("Equipment value is catalog total price, not spent gold; kills/CS/deaths never become bonus modeled damage")};
    return finish(q("ready"));
}

QJsonObject LiveCombatSummary::report() const {
    return {{q("schema_version"), 1}, {q("status"), lastStatus}, {q("fresh"), lastFresh},
        {q("updates"), updates}, {q("model_cache_hits"), hits}, {q("model_cache_misses"), misses},
        {q("model_batches"), misses}, {q("model_evaluations"), evaluations}, {q("cache_clears"), clears},
        {q("model_generation"), generation}, {q("model_available_players"), modeledPlayers},
        {q("last_compute_ms"), lastComputeMs}, {q("max_compute_ms"), maxComputeMs}, {q("last_update_ms"), lastUpdateMs},
        {q("summary_period_ms"), 1000}, {q("freshness_limit_ms"), 4000},
        {q("actual_damage_source"), q("unavailable_live_source")}, {q("identity_fields_retained"), false}};
}
