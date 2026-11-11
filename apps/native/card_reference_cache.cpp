#include "card_reference_cache.h"
#include "analysis.h"
#include "augment_advisory.h"
#include "augment_context_advice.h"
#include "coach_knowledge.h"
#include "combat.h"
#include "external_strategy_reference.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QSet>
#include <algorithm>

namespace {
QString k(const char *value) { return QString::fromLatin1(value); }
QJsonArray ids(const QList<int> &values) {
    QJsonArray out; for (int id : values) out.append(id); return out;
}
QJsonObject project(const QJsonObject &input, std::initializer_list<const char *> fields) {
    QJsonObject out;
    // Do not insert a missing value: missing, null, false, invalid types, and
    // a verified empty list have different meanings in the source functions.
    for (const auto *field : fields) if (input.contains(k(field))) out[k(field)] = input.value(k(field));
    return out;
}
bool admitted(const Catalog &catalog, const GameState &state) {
    if (!state.fresh() || !modelRulesUsable(catalog, state) || state.gameMode != k("KIWI")
        || (state.queueId > 0 && state.queueId != 2400) || !catalog.champions.contains(state.championId)
        || state.offers.size() != 3) return false;
    QSet<int> seen;
    for (int id : state.offers) {
        if (seen.contains(id) || !catalog.ordinaryOfferEligible(id)) return false;
        seen.insert(id);
    }
    return true;
}
Catalog pinned(const Catalog &catalog, const QJsonObject &evidence) {
    Catalog copy = catalog;
    copy.strategyEvidence = evidence;
    // currentExternalStrategyEvidence returns an explicitly pinned snapshot
    // immediately when runtime_reload_origin is absent (see that API). Thus
    // all three advisory scans use the very revision represented in the key,
    // even when the background pointer changes during this synchronous build.
    copy.strategyEvidence.remove(k("runtime_reload_origin"));
    return copy;
}
QJsonObject build(const Catalog &catalog, const GameState &state) {
    QJsonObject mechanisms;
    for (int id : state.offers) mechanisms[QString::number(id)] = augmentMechanismReference(catalog, state, id);
    return {{k("mechanism_references"), mechanisms}, {k("advisory"), buildAugmentAdvisory(catalog, state)}};
}
QJsonArray projectAugmentIdentities(const QMap<int, QJsonObject> &augments) {
    // Include collisions outside the current offers. Qt detaches a copied
    // map on mutation, so this projection can be reused on unchanged frames.
    QJsonArray identities;
    for (auto it = augments.cbegin(); it != augments.cend(); ++it)
        identities.append(QJsonObject{{k("id"), it.key()}, {k("identity"), project(it.value(), {"key", "identity"})}});
    return identities;
}
QJsonObject key(const Catalog &catalog, const GameState &state, const QString &binding,
                const QJsonObject &evidence, const QJsonArray &augmentIdentities) {
    auto owned = state.owned; std::sort(owned.begin(), owned.end());
    QJsonArray selfRows, augmentContent, itemContent, combatAugments, combatItems, contextualCompanions;
    // The reviewed interaction checks may inspect these cards even when they
    // are in the partial selected list rather than this round's three offers.
    for(int id:{1029,3049})contextualCompanions.append(QJsonObject{{k("id"),id},{k("content"),catalog.augments.value(id)}});
    for (const auto &value : state.players) {
        const auto row = value.toObject();
        if (row.value(k("self")) == QJsonValue(true))
            selfRows.append(project(row, {"self", "champion_id", "items", "items_complete", "augment_ids", "augments_complete"}));
    }
    for (int id : state.offers) {
        augmentContent.append(QJsonObject{{k("id"), id}, {k("content"), catalog.augments.value(id)}});
        if (catalog.combat) combatAugments.append(QJsonObject{{k("id"), id},
            {k("present"), catalog.combat->augments.contains(id)},
            {k("calculations"), catalog.combat->augments.value(id).calculations}});
    }
    QSet<int> relevantItems; for (int id : owned) relevantItems.insert(id); relevantItems.insert(1031);
    auto itemIds = relevantItems.values(); std::sort(itemIds.begin(), itemIds.end());
    for (int id : itemIds) {
        itemContent.append(QJsonObject{{k("id"), id}, {k("present"), catalog.items.contains(id)}});
        if (catalog.combat) combatItems.append(QJsonObject{{k("id"), id}, {k("present"), catalog.combat->items.contains(id)},
            {k("stats"), catalog.combat->items.value(id).stats.json()}});
    }
    return {{k("match_context"), state.matchContext}, {k("offer_binding_revision"), binding},
        {k("offers"), ids(state.offers)}, {k("champion_id"), state.championId}, {k("level"), state.level},
        {k("champion_identity"), project(catalog.champion(state.championId), {"key", "patch"})},
        {k("queue_id"), state.queueId}, {k("mode"), state.gameMode}, {k("source"), int(state.source)},
        {k("observed_patch"), state.observedPatch}, {k("owned_multiset"), ids(owned)}, {k("selected_ordered"), ids(state.selected)},
        {k("self_rows"), selfRows}, {k("context_advice_version"), augmentContextAdviceVersion()}, {k("context_companion_cards"),contextualCompanions}, {k("catalog_root"), catalog.root}, {k("catalog_path"), catalog.catalogPath},
        {k("catalog_patch"), catalog.patch}, {k("catalog_manifest"), catalog.runtimeManifest},
        {k("augment_content"), augmentContent}, {k("augment_identities"), augmentIdentities}, {k("owned_catalog_presence"), itemContent},
        {k("combat_present"), bool(catalog.combat)}, {k("combat_patch"), catalog.combat ? catalog.combat->patch : QString()},
        {k("combat_revision"), catalog.combat ? catalog.combat->revision : QString()}, {k("combat_augments"), combatAugments},
        {k("combat_items"), combatItems}, {k("strategy_revision"), project(evidence, {"available", "status", "reason", "revision", "patch", "mode", "generated_at"})}};
}
}

QJsonObject buildCardReferencesUncached(const Catalog &catalog, const GameState &state) {
    return build(pinned(catalog, currentExternalStrategyEvidence(catalog)), state);
}

QJsonObject CardReferenceCache::get(const Catalog &catalog, const GameState &state, const QString &binding) {
    QElapsedTimer elapsed; elapsed.start(); ++calls_;
    const bool allowed = admitted(catalog, state);
    if (!allowed) { clear(); ++rejected_; }
    const auto evidence = currentExternalStrategyEvidence(catalog);
    // Verified installed bundles have content-addressed revisions. An ad-hoc
    // available pack lacking one has no stable invalidation contract; calculate
    // it every time rather than risk retaining a mutated fixture/reference.
    const bool versioned = !evidence.value(k("available")).toBool() || !evidence.value(k("revision")).toString().isEmpty();
    if (!versioned) { clear(); ++unversioned_; }
    QJsonObject currentKey;
    if (allowed && versioned) {
        if (!identitySource_.isSharedWith(catalog.augments)) {
            identitySource_ = catalog.augments;
            augmentIdentities_ = projectAugmentIdentities(identitySource_);
        }
        currentKey = key(catalog, state, binding, evidence, augmentIdentities_);
    }
    if (allowed && versioned && valid_ && currentKey == key_ && state.fresh()) {
        ++hits_; elapsedNs_ += elapsed.nsecsElapsed(); return result_;
    }
    ++misses_; ++builds_; QElapsedTimer building; building.start();
    const auto result = build(pinned(catalog, evidence), state); buildNs_ += building.nsecsElapsed();
    if (allowed && versioned && state.fresh()) { key_ = currentKey; result_ = result; valid_ = true; }
    else clear();
    elapsedNs_ += elapsed.nsecsElapsed(); return result;
}

void CardReferenceCache::clear() {
    valid_ = false;
    key_ = {};
    result_ = {};
    identitySource_.clear();
    augmentIdentities_ = {};
}

QJsonObject CardReferenceCache::statistics() const {
    return {{k("capacity"), 1}, {k("entries"), valid_ ? 1 : 0}, {k("calls"), qint64(calls_)}, {k("hits"), qint64(hits_)},
        {k("misses"), qint64(misses_)}, {k("builds"), qint64(builds_)}, {k("admission_rejections"), qint64(rejected_)},
        {k("unversioned_source_bypasses"), qint64(unversioned_)}, {k("elapsed_ns"), elapsedNs_}, {k("build_elapsed_ns"), buildNs_},
        {k("scope"), k("owner_thread_card_reference_pair_only")}, {k("numeric_ranking_cached"), false}};
}
