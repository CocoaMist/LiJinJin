#include "local_choice_evidence.h"

#include "analysis.h"
#include "combat.h"
#include "combat_unknown_policy.h"
#include "decision_context.h"
#include "growth_objective.h"
#include "headliner.h"
#include "local_choice_v2.h"
#include "mechanic_actions.h"
#include "purchase_planner.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>

namespace {
constexpr int MaxCandidates = 12;
constexpr int MaxId = 10000000;
constexpr qint64 SnapshotMs = 4000;
constexpr qint64 ExtraMs = 120;
constexpr double Window = 8;
constexpr double NumericBound = 1e9;

QString k(const char *s) { return QString::fromLatin1(s); }
QJsonValue nil() { return QJsonValue(QJsonValue::Null); }
QString schema() { return k("lixin-native-local-choice-evidence-v1"); }
QString scenario() { return k("eight_second_continuous_cast_reference_same_snapshot"); }
bool number(double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; }
bool number(const QJsonValue &v, double lo, double hi) { return v.isDouble() && number(v.toDouble(), lo, hi); }
int entityId(const QJsonValue &v) {
    return number(v, 1, MaxId) && std::floor(v.toDouble()) == v.toDouble() ? v.toInt() : 0;
}
bool yes(const QJsonObject &o, const char *field) { return o.value(k(field)) == QJsonValue(true); }

QJsonArray ids(const QList<int> &values) {
    QJsonArray result;
    for (const int id : values) result.append(id);
    return result;
}

bool sameIds(QList<int> a, QList<int> b) {
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return a == b;
}

template <class Entries>
bool knownIds(const QJsonValue &value, const Entries &known, int maximum, bool unique, QList<int> &out) {
    if (!value.isArray() || value.toArray().size() > maximum) return false;
    for (const auto &entry : value.toArray()) {
        const int id = entityId(entry);
        if (!id || !known.contains(id) || (unique && out.contains(id))) return false;
        out.append(id);
    }
    return true;
}

bool token(const QString &value, int limit) {
    if (value.isEmpty() || value.size() > limit) return false;
    for (const QChar c : value) {
        const auto u = c.unicode();
        if (u < 33 || u > 126) return false;
    }
    return true;
}

QString fingerprint(const QJsonObject &basis) {
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(basis).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}

bool digest(const QJsonValue &value) {
    const QString hash = value.toString();
    if (hash.size() != 64) return false;
    for (const QChar c : hash)
        if (!(c >= QLatin1Char('0') && c <= QLatin1Char('9')) && !(c >= QLatin1Char('a') && c <= QLatin1Char('f'))) return false;
    return true;
}

bool freshAt(const GameState &state, qint64 nowMs) {
    if (state.source != GameState::Source::Live || !state.received.isValid()) return false;
    const qint64 received = state.received.toMSecsSinceEpoch();
    return received > 0 && nowMs >= received && nowMs - received < SnapshotMs;
}

bool goldKnown(const GameState &state) { return state.goldKnown && number(state.gold, 0, NumericBound); }

// The selected and owned lists are read only after their observed completeness
// and exact actor binding have been verified. A missing list cannot stand for [].
QString bindScope(const Catalog &catalog, const GameState &state, bool forAugments,
                  qint64 nowMs, QJsonObject &scope, QList<int> &selected) {
    if (!freshAt(state, nowMs)) return k("fresh_live_snapshot_required");
    if (!modelPatchMatches(catalog, state) || !token(catalog.patch, 24)
        || !digest(catalog.combat->revision) || state.gameMode != k("KIWI")) return k("same_patch_combat_revision_required");
    const auto &db = *catalog.combat;
    const QString contextId = state.matchContext.value(k("context_id")).toString();
    if (!yes(state.matchContext, "active") || state.matchContext.value(k("stage")) != k("in_game")
        || !token(contextId, 160) || !number(state.gameTime, 0, 86400)
        || state.championId <= 0 || state.championId > MaxId || state.level < 1 || state.level > 18
        || !db.champions.contains(state.championId)) return k("active_actor_context_required");
    if (state.players.size() > 10) return k("self_identity_unverified");
    QJsonObject self;
    for (const auto &value : state.players) {
        const auto row = value.toObject();
        if (!yes(row, "self")) continue;
        if (!self.isEmpty()) return k("self_identity_unverified");
        self = row;
    }
    if (entityId(self.value(k("champion_id"))) != state.championId
        || entityId(self.value(k("level"))) != state.level) return k("self_identity_unverified");
    QList<int> observedOwned, observedSelected;
    if (!yes(self, "items_complete") || !knownIds(self.value(k("items")), db.items, 6, false, observedOwned)
        || !sameIds(observedOwned, state.owned)) return k("self_inventory_unverified");
    if (!yes(self, "augments_complete") || !knownIds(self.value(k("augment_ids")), db.augments, 12, true, observedSelected)
        || !sameIds(observedSelected, state.selected)) return k("selected_augments_unverified");
    selected = effectiveSelectedAugments(catalog, state);
    if (!sameIds(observedSelected, selected)) return k("effective_augments_differ_from_observation");
    QJsonArray offers;
    if (forAugments) {
        QList<int> offered;
        offers = ids(state.offers);
        if (offers.size() != 3 || !knownIds(offers, catalog.augments, 3, true, offered))
            return k("exact_three_current_offers_required");
    }
    scope = {{k("context_id"), contextId}, {k("patch"), catalog.patch}, {k("combat_revision"), db.revision},
        {k("champion_id"), state.championId}, {k("level"), state.level}, {k("sampled_game_time"), state.gameTime},
        {k("source_received_at_ms"), double(state.received.toMSecsSinceEpoch())},
        {k("owned_items"), ids(state.owned)}, {k("selected_augments"), ids(selected)},
        {k("offer_ids"), forAugments ? QJsonValue(offers) : nil()},
        {k("gold"), !forAugments && goldKnown(state) ? QJsonValue(state.gold) : nil()},
        {k("window_seconds"), Window}, {k("scenario"), scenario()}};
    return {};
}

QString candidateId(int id, bool forAugments) { return k(forAugments ? "a:" : "i:") + QString::number(id); }

QJsonObject candidateRow(int id, bool forAugments, const char *source) {
    return {{k("candidate_id"), candidateId(id, forAugments)}, {k(forAugments ? "augment_id" : "item_id"), id},
        {k("candidate_source"), k(source)}, {k("status"), k("native_reference_only")}, {k("available"), false},
        {k("native_purchase_verified"), forAugments ? nil() : QJsonValue(false)},
        {k("affordable"), nil()}, {k("remaining_cost"), nil()}, {k("predicted_components"), nil()},
        {k("predicted_log_component_ratios"), nil()}, {k("reasons"), QJsonArray{}}};
}

void rowReason(QJsonObject &row, const char *code) { row[k("reasons")] = QJsonArray{k(code)}; }

struct Candidate {
    QJsonObject evidence;
    QList<int> items, augments;
    int id = 0;
    bool comparable = false;
};

QJsonObject components(const CombatEvaluation &evaluation) {
    return {{k("attackDamage"), evaluation.attackDamage}, {k("skillDamage"), evaluation.skillDamage},
        {k("survival"), evaluation.survival}};
}

bool componentValues(const QJsonObject &object, double minimum, double maximum) {
    return object.size() == 3 && number(object.value(k("attackDamage")), minimum, maximum)
        && number(object.value(k("skillDamage")), minimum, maximum) && number(object.value(k("survival")), minimum, maximum);
}

bool finiteStats(const CombatStats &stats) {
    return std::all_of(stats.values.cbegin(), stats.values.cend(), [](double v) { return number(v, -NumericBound, NumericBound); });
}

bool usableEvaluation(const CombatEvaluation &value) {
    return value.valid && finiteStats(value.stats) && componentValues(components(value), 0, NumericBound)
        && combatCriticalUnknowns(value.unknowns).isEmpty();
}

QJsonObject difference(const QJsonObject &after, const QJsonObject &before) {
    QJsonObject result;
    for (auto it = after.constBegin(); it != after.constEnd(); ++it) result[it.key()] = it.value().toDouble() - before.value(it.key()).toDouble();
    return result;
}

bool closeProbeStats(const QJsonObject &probes, const char *joint, const char *first, const char *second) {
    const auto stats = [&](const char *name) { return probes.value(k(name)).toObject().value(k("final_stats_delta")).toObject(); };
    const auto a = stats(first), b = stats(second), combined = stats(joint);
    if (a.size() != static_cast<int>(Stat::Count) || a.size() != b.size() || a.size() != combined.size()) return false;
    for (auto it = a.constBegin(); it != a.constEnd(); ++it) {
        const auto bv = b.value(it.key()), cv = combined.value(it.key());
        if (!bv.isDouble() || !cv.isDouble()) return false;
        const double expected = it.value().toDouble() + bv.toDouble();
        if (std::abs(cv.toDouble() - expected) > 1e-7 * (1 + std::abs(expected))) return false;
    }
    return true;
}

QJsonObject skillRanks(const CombatContext &ctx) {
    QJsonObject ranks;
    for (const char *slot : {"Q", "W", "E", "R"}) {
        if (!ctx.ranks.contains(k(slot))) return {};
        ranks[k(slot)] = ctx.ranks.value(k(slot));
    }
    return ranks;
}

QJsonObject modelContext(const CombatContext &ctx) {
    const auto ranks = skillRanks(ctx);
    if (ranks.isEmpty()) return {};
    return {{k("level"), ctx.level}, {k("ranged"), ctx.ranged}, {k("uses_mana"), ctx.usesMana},
        {k("target_armor"), ctx.targetArmor}, {k("target_mr"), ctx.targetMr}, {k("target_hp"), ctx.targetHp},
        {k("target_ms"), ctx.targetMs}, {k("horizon"), ctx.horizon}, {k("base_as"), ctx.baseAs},
        {k("as_ratio"), ctx.asRatio}, {k("base_stats"), ctx.base.json()}, {k("ranks"), ranks},
        {k("skills_ready_at_zero_scenario_assumption"), true}};
}

bool trainingContext(const CombatContext &ctx) {
    return ctx.level >= 6 && ctx.level <= 18 && ctx.horizon == Window
        && number(ctx.targetArmor, 20, 240) && number(ctx.targetMr, 20, 180)
        && number(ctx.targetHp, 1100, 4500) && number(ctx.targetMs, 310, 420)
        && number(ctx.baseAs, 0.1, 5) && number(ctx.asRatio, 0, 5) && finiteStats(ctx.base);
}

bool prospectiveState(const CombatContext &ctx, int id, bool augment) {
    const QString prefix = k(augment ? "augment:" : "item:") + QString::number(id) + QLatin1Char(':');
    for (auto it = ctx.counters.cbegin(); it != ctx.counters.cend(); ++it)
        if (it.key().startsWith(prefix)) return true;
    for (const QString &condition : ctx.conditions)
        if (condition.startsWith(prefix)) return true;
    return false;
}
}

QJsonObject buildLocalChoiceEvidence(const Catalog &catalog, const GameState &state,
    const QJsonObject &nativeResult, bool forAugments, const RecommendationWorkPtr &work) {
    QElapsedTimer elapsed;
    elapsed.start();
    QJsonObject scope;
    QList<int> selected;
    QList<Candidate> candidates;
    const auto finish = [&](const QString &reason, const QJsonValue &baseline = QJsonValue(QJsonValue::Null)) {
        QJsonArray rows, candidateIds;
        bool available = false;
        for (const auto &candidate : candidates) {
            rows.append(candidate.evidence);
            candidateIds.append(candidate.evidence.value(k("candidate_id")));
            available |= yes(candidate.evidence, "available");
        }
        if (!scope.isEmpty()) {
            scope[k("candidate_ids")] = candidateIds;
            if (!scope.contains(k("scope_fingerprint"))) scope[k("scope_fingerprint")] = fingerprint(scope);
        }
        return QJsonObject{{k("schema"), schema()}, {k("analysis_only"), true},
            {k("decision_ranking_authority"), false}, {k("execution_eligible"), false},
            {k("live_certified"), false}, {k("available"), available}, {k("scope"), scope},
            {k("status"), available ? k("conditional_partial_estimates") : rows.isEmpty() ? k("unavailable") : k("native_reference_only")},
            {k("candidates"), rows}, {k("baseline_components"), available ? baseline : nil()},
            {k("reasons"), reason.isEmpty() ? QJsonArray{} : QJsonArray{reason}}};
    };
    const auto stopReason = [&]() -> QString {
        if (!freshAt(state, QDateTime::currentMSecsSinceEpoch())) return k("snapshot_expired");
        if (work && work->stopped()) return k("native_work_stopped");
        if (elapsed.elapsed() >= ExtraMs) return k("local_evidence_budget_exceeded");
        return {};
    };
    const auto scopeError = bindScope(catalog, state, forAugments, QDateTime::currentMSecsSinceEpoch(), scope, selected);
    if (!scopeError.isEmpty()) return finish(scopeError);
    const auto &db = *catalog.combat;
    const auto purchase = nativeResult.value(k("plan")).toObject().value(k("purchase_sandbox")).toObject();
    const auto &native = forAugments ? nativeResult : purchase;
    const auto health = native.value(k("health_basis")).toObject();
    if (native.value(k("patch")) != catalog.patch || !yes(nativeResult, "computation_complete")
        || health.value(k("context_id")) != scope.value(k("context_id"))
        || health.value(k("source_received_at_ms")) != scope.value(k("source_received_at_ms"))
        || health.value(k("champion_id")) != scope.value(k("champion_id"))
        || health.value(k("patch")) != scope.value(k("patch")) || !yes(health, "snapshot_bound")
        || (!forAugments && (native.value(k("context_id")) != scope.value(k("context_id"))
            || nativeResult.value(k("model_game_time")) != QJsonValue(state.gameTime))))
        return finish(k("native_candidate_basis_unverified"));
    if (const auto stop = stopReason(); !stop.isEmpty()) return finish(stop);

    QSet<int> seen;
    const auto nativeRows = native.value(k(forAugments ? "offers" : "candidates")).toArray();
    const auto conditionalRows = forAugments ? QJsonArray{} : native.value(k("conditional_purchase_candidates")).toArray();
    if (nativeRows.size() + conditionalRows.size() > MaxCandidates || (forAugments && nativeRows.size() != 3))
        return finish(k("native_candidate_set_out_of_bounds"));
    for (const auto &rows : {nativeRows, conditionalRows}) for (const auto &entry : rows) {
        const int id = entityId(entry.toObject().value(k(forAugments ? "augment_id" : "item_id")));
        const bool known = forAugments ? state.offers.contains(id) : catalog.items.contains(id) && db.items.contains(id);
        if (!id || !known || seen.contains(id)) return finish(k("native_candidate_identity_mismatch"));
        seen.insert(id);
    }
    const bool purchaseScope = !forAugments && yes(purchase, "enumeration_complete")
        && state.explicitPurchaseChain.isEmpty() && purchase.value(k("policy")) == k("budgeted_sandbox_v1");
    for (const auto &entry : nativeRows) {
        const auto row = entry.toObject();
        const int id = row.value(k(forAugments ? "augment_id" : "item_id")).toInt();
        Candidate c{candidateRow(id, forAugments, forAugments ? "native_current_offer" : "native_purchase_candidate"),
            state.owned, selected, id};
        if (forAugments) {
            c.comparable = yes(row, "ordinary_offer_eligible") && yes(row, "available")
                && yes(row, "marginal_comparable") && yes(row, "decision_marginal_comparable")
                && yes(row, "decision_numeric_available") && number(row.value(k("current_gain_percent")), -NumericBound, NumericBound)
                && number(row.value(k("decision_gain_percent")), -NumericBound, NumericBound)
                && row.value(k("critical_unknowns")).toArray().isEmpty()
                && row.value(k("decision_blocking_unknowns")).toArray().isEmpty()
                && catalog.ordinaryOfferEligible(id) && db.augments.contains(id) && !selected.contains(id)
                && db.augments[id].requiredLevel <= state.level;
            if (c.comparable) c.augments.append(id);
            else rowReason(c.evidence, "native_offer_comparison_incomplete");
        } else if (!purchaseScope) {
            rowReason(c.evidence, "native_purchase_comparison_incomplete");
        } else {
            if (const auto stop = stopReason(); !stop.isEmpty()) return finish(stop);
            const auto quote = planInventoryPurchase(catalog, state.championId, state.level, state.owned, id);
            QList<int> after;
            const int cost = entityId(quote.value(k("remaining_cost")));
            if (!yes(quote, "legal") || !cost || row.value(k("remaining_cost")) != QJsonValue(cost)
                || !knownIds(quote.value(k("inventory_after")), db.items, 6, false, after) || !after.contains(id)) {
                rowReason(c.evidence, "native_purchase_quote_unverified");
            } else {
                const bool budget = goldKnown(state) && yes(purchase, "budget_known");
                const bool affordable = budget && state.gold >= cost;
                const auto nativeAffordability = row.value(k("affordable"));
                c.evidence[k("native_purchase_verified")] = true;
                c.evidence[k("remaining_cost")] = cost;
                if (budget && nativeAffordability == QJsonValue(affordable)) c.evidence[k("affordable")] = affordable;
                c.comparable = affordable && nativeAffordability == QJsonValue(true)
                    && yes(row, "pair_ranking_eligible") && yes(row, "numeric_comparison_complete")
                    && row.value(k("critical_unknowns")).toArray().isEmpty();
                c.items = after;
                if (!c.comparable) rowReason(c.evidence, !budget ? "budget_unknown" : !affordable ? "budget_insufficient" : "native_purchase_comparison_incomplete");
            }
        }
        candidates.append(c);
    }
    for (const auto &entry : conditionalRows) {
        const int id = entry.toObject().value(k("item_id")).toInt();
        Candidate c{candidateRow(id, false, "native_conditional_purchase_candidate"), {}, {}, id};
        rowReason(c.evidence, "purchase_conditions_unverified");
        candidates.append(c);
    }
    if (std::none_of(candidates.cbegin(), candidates.cend(), [](const Candidate &c) { return c.comparable; }))
        return finish(k("no_bound_numeric_candidate"));
    if (const auto stop = stopReason(); !stop.isEmpty()) return finish(stop);

    // One frozen, disclosed continuous-cast reference, matching the teacher's
    // window convention. Native ranking keeps its own cast schedule/objective.
    const auto ranks = selfSkillRankContext(db, state);
    if (!ranks.available || !ranks.report.value(k("missing_slots")).toArray().isEmpty()) return finish(k("observed_skill_ranks_required"));
    if (state.effectCounters.size() > 256 || state.effectConditions.size() > 256 || state.mechanicObservations.size() > 512
        || state.liveStats.size() > 128 || !number(state.liveStats.value(k("maxHealth")), 1, NumericBound))
        return finish(k("numeric_snapshot_unverified"));
    auto ctx = db.context(state.championId, state.level);
    ctx.horizon = Window;
    ctx.discreteSkillCasts = false;
    ctx.ranks = ranks.context.ranks;
    ctx.counters = state.effectCounters;
    ctx.conditions = state.effectConditions;
    for (auto it = ctx.counters.cbegin(); it != ctx.counters.cend(); ++it)
        if (!token(it.key(), 160) || !number(it.value(), -NumericBound, NumericBound)) return finish(k("numeric_snapshot_unverified"));
    for (const auto &condition : ctx.conditions)
        if (!token(condition, 160)) return finish(k("numeric_snapshot_unverified"));
    const auto target = native.value(k("target_context")).toObject();
    if (!number(target.value(k("armor")), 20, 240) || !number(target.value(k("magic_resist")), 20, 180)
        || !number(target.value(k("max_health")), 1100, 4500)) return finish(k("native_target_outside_numeric_bounds"));
    ctx.targetArmor = target.value(k("armor")).toDouble();
    ctx.targetMr = target.value(k("magic_resist")).toDouble();
    ctx.targetHp = target.value(k("max_health")).toDouble();
    const auto context = modelContext(ctx);
    if (!trainingContext(ctx) || context.isEmpty()) return finish(k("scenario_outside_numeric_bounds"));
    scope[k("skill_ranks")] = context.value(k("ranks"));
    scope[k("target_source")] = target.value(k("source")) == k("observed_opponent_loadouts_estimate")
        ? k("observed_opponent_loadouts_estimate") : k("reference_target");
    const auto preference = resolvedCombatPreference(catalog, state, QStringLiteral("成长联动"));
    if (const auto stop = stopReason(); !stop.isEmpty()) return finish(stop);
    if (!forAugments) {
        ctx.counters[k("observed_game_time")] = state.gameTime;
        if (number(state.mechanicBaselineTime, 0, state.gameTime)) ctx.counters[k("mechanic_baseline_time")] = state.mechanicBaselineTime;
        const auto ledger = replayCombatMechanics(db, ctx, state.owned, selected, state.mechanicObservations);
        applyMechanicLedgerContext(ledger, state.gameTime, ctx, true);
    }
    if (const auto stop = stopReason(); !stop.isEmpty()) return finish(stop);
    anchorObservedCombatStats(db, ctx, state.owned, selected, state.liveStats, preference);
    if (const auto stop = stopReason(); !stop.isEmpty()) return finish(stop);
    bindGrowthObjective(db, ctx, state.owned, selected);
    const auto baseline = evaluateCombat(db, ctx, state.owned, selected, false, preference);
    if (!usableEvaluation(baseline)) return finish(k("native_baseline_incomplete"));
    const auto baselineStats = baseline.stats.json(), baselineComponents = components(baseline);
    const auto offsets = localChoiceV2ProbeOffsets();
    if (offsets.size() != 27) return finish(k("native_probe_contract_unavailable"));
    QJsonObject probes;
    for (auto probe = offsets.constBegin(); probe != offsets.constEnd(); ++probe) {
        if (const auto stop = stopReason(); !stop.isEmpty()) return finish(stop);
        const auto offset = probe.value().toObject();
        if (offset.isEmpty() || offset.size() > 2) return finish(k("native_probe_contract_unavailable"));
        auto changed = ctx;
        for (auto stat = offset.constBegin(); stat != offset.constEnd(); ++stat) {
            const auto key = combatStat(stat.key());
            if (!key || !number(stat.value(), 0, 1000)) return finish(k("native_probe_contract_unavailable"));
            changed.observedOffset[*key] += stat.value().toDouble();
        }
        const auto value = evaluateCombat(db, changed, state.owned, selected, false, preference);
        if (!usableEvaluation(value)) return finish(k("native_response_probe_incomplete"));
        probes[probe.key()] = QJsonObject{{k("component_delta"), difference(components(value), baselineComponents)},
            {k("final_stats_delta"), difference(value.stats.json(), baselineStats)},
            {k("unknowns"), QJsonArray::fromStringList(value.unknowns)}};
    }
    if (!closeProbeStats(probes, "ap_haste", "ap", "haste") || !closeProbeStats(probes, "ad_haste", "ad", "haste")
        || !closeProbeStats(probes, "hp_haste", "hp", "haste") || !closeProbeStats(probes, "hp_double", "hp", "hp"))
        return finish(k("native_response_probe_closure_failed"));

    QJsonArray packetCandidates;
    for (auto &c : candidates) {
        if (!c.comparable) continue;
        if (const auto stop = stopReason(); !stop.isEmpty()) return finish(stop);
        if ((forAugments || !state.owned.contains(c.id)) && prospectiveState(ctx, c.id, forAugments)) {
            rowReason(c.evidence, "prospective_candidate_state_unverified");
            continue;
        }
        const auto value = evaluateCombat(db, ctx, c.items, c.augments, false, preference);
        if (!usableEvaluation(value)) { rowReason(c.evidence, "native_candidate_stats_incomplete"); continue; }
        packetCandidates.append(QJsonObject{{k("candidate_id"), c.evidence.value(k("candidate_id"))},
            {k("native_valid"), true}, {k("stats"), value.stats.json()}, {k("unknowns"), QJsonArray::fromStringList(value.unknowns)}});
    }
    if (packetCandidates.isEmpty()) return finish(k("native_candidate_stats_incomplete"));
    if (const auto stop = stopReason(); !stop.isEmpty()) return finish(stop);
    const QJsonObject packet{{k("task"), forAugments ? 1 : 0}, {k("context"), context}, {k("baseline_stats"), baselineStats},
        {k("reference_components"), baselineComponents}, {k("reference_unknowns"), QJsonArray::fromStringList(baseline.unknowns)},
        {k("context_probe"), probes}, {k("candidates"), packetCandidates}};
    QJsonArray candidateIds;
    for (const auto &c : candidates) candidateIds.append(c.evidence.value(k("candidate_id")));
    scope[k("candidate_ids")] = candidateIds;
    scope[k("scope_fingerprint")] = fingerprint(QJsonObject{{k("scope"), scope}, {k("model_input"), packet}});
    if (const auto stop = stopReason(); !stop.isEmpty()) return finish(stop);
    const auto output = evaluateLocalChoiceV2(packet);
    if (const auto stop = stopReason(); !stop.isEmpty()) return finish(stop);
    const auto resultRows = output.value(k("candidates")).toArray();
    if (output.value(k("schema")) != k("lixin-component-comparison-v2") || resultRows.size() != packetCandidates.size())
        return finish(k("local_model_output_unavailable"));
    for (qsizetype i = 0; i < resultRows.size(); ++i)
        if (resultRows.at(i).toObject().value(k("candidate_id")) != packetCandidates.at(i).toObject().value(k("candidate_id")))
            return finish(k("local_model_output_identity_mismatch"));
    for (auto &c : candidates) {
        for (const auto &entry : resultRows) {
            const auto result = entry.toObject();
            if (result.value(k("candidate_id")) != c.evidence.value(k("candidate_id"))) continue;
            const auto estimate = result.value(k("predicted_components")).toObject();
            const auto ratios = result.value(k("predicted_log_component_ratios")).toObject();
            if (!yes(result, "available") || !componentValues(estimate, 0, NumericBound) || !componentValues(ratios, -20, 20)) {
                rowReason(c.evidence, "local_model_output_unavailable");
                continue;
            }
            c.evidence[k("status")] = k("conditional_partial_estimate");
            c.evidence[k("available")] = true;
            c.evidence[k("predicted_components")] = estimate;
            c.evidence[k("predicted_log_component_ratios")] = ratios;
        }
    }
    return finish(yes(output, "available") ? QString() : k("local_model_output_unavailable"), baselineComponents);
}

bool localChoiceEvidenceCurrent(const Catalog &catalog, const GameState &state, const QJsonObject &evidence,
                                bool forAugments, qint64 nowMs) {
    if (nowMs < 0) nowMs = QDateTime::currentMSecsSinceEpoch();
    if (evidence.value(k("schema")) != schema() || !yes(evidence, "analysis_only")
        || evidence.value(k("execution_eligible")) != QJsonValue(false)
        || evidence.value(k("live_certified")) != QJsonValue(false)
        || evidence.value(k("decision_ranking_authority")) != QJsonValue(false)) return false;
    const auto status = evidence.value(k("status"));
    if (status != k("native_reference_only") && status != k("conditional_partial_estimates")) return false;
    QJsonObject current;
    QList<int> selected;
    if (!bindScope(catalog, state, forAugments, nowMs, current, selected).isEmpty()) return false;
    const auto scope = evidence.value(k("scope")).toObject();
    for (const char *key : {"context_id", "patch", "combat_revision", "champion_id", "level", "offer_ids", "window_seconds", "scenario"})
        if (scope.value(k(key)) != current.value(k(key))) return false;
    if (status == k("conditional_partial_estimates")) {
        const auto ranks = selfSkillRankContext(*catalog.combat, state);
        if (!ranks.available || !ranks.report.value(k("missing_slots")).toArray().isEmpty()
            || QJsonValue(skillRanks(ranks.context)) != scope.value(k("skill_ranks"))) return false;
    }
    QList<int> owned, chosen;
    if (!knownIds(scope.value(k("owned_items")), catalog.combat->items, 6, false, owned)
        || !sameIds(owned, state.owned) || !knownIds(scope.value(k("selected_augments")), catalog.combat->augments, 12, true, chosen)
        || !sameIds(chosen, selected)) return false;
    const auto received = scope.value(k("source_received_at_ms"));
    const auto sampled = scope.value(k("sampled_game_time"));
    if (!number(received, 1, double(nowMs)) || std::floor(received.toDouble()) != received.toDouble()
        || double(nowMs) - received.toDouble() >= SnapshotMs || received.toDouble() > current.value(k("source_received_at_ms")).toDouble()
        || !number(sampled, 0, state.gameTime) || state.gameTime - sampled.toDouble() >= double(SnapshotMs) / 1000
        || !digest(scope.value(k("scope_fingerprint")))) return false;
    const auto rows = evidence.value(k("candidates")).toArray(), boundIds = scope.value(k("candidate_ids")).toArray();
    if (rows.isEmpty() || rows.size() > MaxCandidates || rows.size() != boundIds.size() || (forAugments && rows.size() != 3)) return false;
    const auto capturedGold = scope.value(k("gold"));
    const bool hasCurrentBudget = !forAugments && goldKnown(state);
    if (hasCurrentBudget ? !number(capturedGold, 0, NumericBound) : !capturedGold.isNull()) return false;
    QSet<int> seen;
    bool available = false;
    for (qsizetype i = 0; i < rows.size(); ++i) {
        const auto row = rows.at(i).toObject();
        const int id = entityId(row.value(k(forAugments ? "augment_id" : "item_id")));
        const bool known = forAugments ? state.offers.contains(id) : catalog.items.contains(id) && catalog.combat->items.contains(id);
        if (!id || !known || seen.contains(id) || row.value(k("candidate_id")) != candidateId(id, forAugments)
            || row.value(k("candidate_id")) != boundIds.at(i) || !row.value(k("available")).isBool()) return false;
        seen.insert(id);
        const bool rowAvailable = yes(row, "available");
        const auto predicted = row.value(k("predicted_components")), ratios = row.value(k("predicted_log_component_ratios"));
        if (rowAvailable) {
            if (row.value(k("status")) != k("conditional_partial_estimate") || !componentValues(predicted.toObject(), 0, NumericBound)
                || !componentValues(ratios.toObject(), -20, 20)) return false;
        } else if (row.value(k("status")) != k("native_reference_only") || !predicted.isNull() || !ratios.isNull()) return false;
        const auto source = row.value(k("candidate_source"));
        const auto affordability = row.value(k("affordable")), cost = row.value(k("remaining_cost"));
        if (forAugments) {
            if (source != k("native_current_offer") || !cost.isNull() || !affordability.isNull()
                || !row.value(k("native_purchase_verified")).isNull() || (rowAvailable && !catalog.ordinaryOfferEligible(id))) return false;
        } else if (source == k("native_purchase_candidate") && yes(row, "native_purchase_verified")) {
            if (!entityId(cost) || (rowAvailable && affordability != QJsonValue(true))) return false;
            if (affordability.isBool() && (!goldKnown(state) || affordability.toBool() != (state.gold >= cost.toDouble())
                || affordability.toBool() != (capturedGold.toDouble() >= cost.toDouble()))) return false;
            if (!affordability.isBool() && !affordability.isNull()) return false;
        } else {
            if ((source != k("native_purchase_candidate") && source != k("native_conditional_purchase_candidate"))
                || rowAvailable || !cost.isNull() || !affordability.isNull()
                || row.value(k("native_purchase_verified")) != QJsonValue(false)) return false;
        }
        available |= rowAvailable;
    }
    return evidence.value(k("available")) == QJsonValue(available)
        && (status == k("conditional_partial_estimates")) == available
        && (available ? componentValues(evidence.value(k("baseline_components")).toObject(), 0, NumericBound)
                      : evidence.value(k("baseline_components")).isNull());
}
