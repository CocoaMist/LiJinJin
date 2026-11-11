#pragma once

#include "analysis.h"
#include <QDir>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <cmath>

namespace LoadoutOutcomeReference {
inline constexpr int maximumPairsPerChampion = 24;

inline bool integer(const QJsonValue &value, double minimum, double maximum) {
    const double number = value.toDouble(-1);
    return number >= minimum && number <= maximum && std::floor(number) == number;
}

inline QJsonObject comparisonArm(const QJsonObject &arm) {
    const auto matches = arm.value(QStringLiteral("matches"));
    if (!integer(matches, 5, 1000000)) return {};
    QJsonObject result{{QStringLiteral("matches"), matches}};
    const auto copyMetric = [&](const QString &sampleKey, const QString &metricKey) {
        const auto samples = arm.value(sampleKey);
        const auto metric = arm.value(metricKey);
        if (!integer(samples, 0, matches.toDouble())) return false;
        const double value = metric.toDouble(-1);
        if (samples.toInt() == 0 ? !metric.isNull() : (!std::isfinite(value) || value < 0 || value > 1e9))
            return false;
        result[sampleKey] = samples;
        result[metricKey] = metric;
        return true;
    };
    if (!copyMetric(QStringLiteral("kda_samples"), QStringLiteral("average_kda"))
        || !copyMetric(QStringLiteral("damage_samples"), QStringLiteral("average_damage_per_minute"))) return {};
    return result;
}

inline QJsonObject pinnedChampions(const Catalog &catalog, const QString &contextId, qint64 startedAtMs, QString &status) {
    struct PinnedBundle {
        QString root, patch, contextId;
        qint64 startedAtMs;
        QJsonObject champions;
    };
    static QMutex mutex;
    static QList<PinnedBundle> pinned;
    const QMutexLocker lock(&mutex);
    for (const auto &entry : pinned) {
        if (entry.root == catalog.root && entry.patch == catalog.patch && entry.contextId == contextId
            && entry.startedAtMs == startedAtMs) return entry.champions;
    }

    status = QStringLiteral("bundle_unavailable");
    const QDir root(catalog.root);
    constexpr qint64 maximumBytes = 2 * 1024 * 1024;
    for (const int schema : {2, 1}) {
        QFile file(root.filePath(QStringLiteral(".local/player-intelligence/loadout-outcomes.v%1.json").arg(schema)));
        if (!file.open(QIODevice::ReadOnly) || file.size() > maximumBytes) continue;
        const auto bytes = file.read(maximumBytes + 1);
        if (file.error() != QFileDevice::NoError || bytes.size() > maximumBytes) continue;
        const auto bundle = QJsonDocument::fromJson(bytes).object();
        const auto champions = bundle.value(QStringLiteral("champions")).toObject();
        if (bundle.value(QStringLiteral("schema_version")) != QJsonValue(schema)
            || bundle.value(QStringLiteral("patch")) != catalog.patch
            || champions.isEmpty() || champions.size() > 1000) continue;
        if (!integer(bundle.value(QStringLiteral("latest_ended_at_ms")), 1, startedAtMs - 1)) {
            status = QStringLiteral("outside_history_cutoff");
            continue;
        }

        // Both workers keep the same pre-match reference even when a new bundle arrives.
        if (pinned.size() == 8) pinned.removeFirst();
        pinned.append(PinnedBundle{catalog.root, catalog.patch, contextId, startedAtMs, champions});
        return champions;
    }
    return {};
}

inline QJsonObject project(const Catalog &catalog, const GameState &state) {
    QJsonObject result{{QStringLiteral("status"), QStringLiteral("waiting_live_context")},
        {QStringLiteral("available"), false}, {QStringLiteral("reference_only"), true},
        {QStringLiteral("causal_effect_known"), false}, {QStringLiteral("players"), QJsonArray{}}};
    const auto start = state.matchContext.value(QStringLiteral("started_at_ms"));
    const auto contextId = state.matchContext.value(QStringLiteral("context_id")).toString();
    if (state.source != GameState::Source::Live || !state.fresh()
        || state.gameMode != QStringLiteral("KIWI") || state.queueId != 2400
        || !modelPatchMatches(catalog, state) || catalog.root.isEmpty()
        || state.matchContext.value(QStringLiteral("active")) != QJsonValue(true)
        || contextId.isEmpty()
        || !integer(start, 1, state.received.toMSecsSinceEpoch())
        || state.players.isEmpty() || state.players.size() > 10) return result;

    QString bundleStatus;
    const auto champions = pinnedChampions(catalog, contextId, qint64(start.toDouble()), bundleStatus);
    if (champions.isEmpty()) {
        result[QStringLiteral("status")] = bundleStatus;
        return result;
    }

    QJsonArray players;
    int selves = 0;
    for (const auto &value : state.players) {
        if (value.toObject().value(QStringLiteral("self")) == QJsonValue(true)) ++selves;
    }
    constexpr int maximumObservedPairs = 6;
    for (qsizetype index = 0; index < state.players.size(); ++index) {
        const auto player = state.players.at(index).toObject();
        const auto team = player.value(QStringLiteral("team")).toString();
        const int champion = player.value(QStringLiteral("champion_id")).toInt();
        if (!catalog.champions.contains(champion)
            || (team != QStringLiteral("ORDER") && team != QStringLiteral("CHAOS")
                && team != QStringLiteral("ALLY") && team != QStringLiteral("ENEMY"))
            || player.value(QStringLiteral("visible")) == QJsonValue(false)
            || player.value(QStringLiteral("hidden")) == QJsonValue(true)
            || player.value(QStringLiteral("items_visible")) == QJsonValue(false)
            || player.value(QStringLiteral("augments_visible")) == QJsonValue(false)) continue;
        const auto items = player.value(QStringLiteral("items")).toArray();
        const auto augments = player.value(QStringLiteral("augment_ids")).toArray();
        const auto comparisons = champions.value(QString::number(champion)).toArray();
        if (items.isEmpty() || items.size() > 7 || augments.size() > 7
            || comparisons.size() > maximumPairsPerChampion) continue;

        const bool self = player.value(QStringLiteral("self")) == QJsonValue(true);
        const bool canMatchOffers = self && selves == 1 && champion == state.championId && state.offers.size() <= 3;
        QJsonArray pairs, offerPairs;
        QSet<QPair<int, int>> seen;
        QSet<int> offeredAugments;
        for (const auto &comparison : comparisons) {
            const auto row = comparison.toObject();
            const auto item = row.value(QStringLiteral("item_id"));
            const auto augment = row.value(QStringLiteral("augment_id"));
            const int itemId = item.toInt(), augmentId = augment.toInt();
            if (!catalog.items.contains(itemId) || !catalog.augments.contains(augmentId)
                || !items.contains(item)) continue;
            const bool observed = augments.contains(augment);
            if (observed ? pairs.size() >= maximumObservedPairs
                         : !canMatchOffers || !state.offers.contains(augmentId) || state.selected.contains(augmentId)
                             || !state.owned.contains(itemId) || offeredAugments.contains(augmentId)) continue;
            const auto exposed = comparisonArm(row.value(QStringLiteral("exposed")).toObject());
            const auto control = comparisonArm(row.value(QStringLiteral("control")).toObject());
            const QPair<int, int> key{itemId, augmentId};
            if (exposed.isEmpty() || control.isEmpty() || seen.contains(key)) continue;
            const auto knownOnBothSides = [&](const QString &metric) {
                return exposed.value(metric).isDouble() && control.value(metric).isDouble();
            };
            if (!knownOnBothSides(QStringLiteral("average_kda"))
                && !knownOnBothSides(QStringLiteral("average_damage_per_minute"))) continue;
            seen.insert(key);
            if (!observed) offeredAugments.insert(augmentId);
            (observed ? pairs : offerPairs).append(QJsonObject{{QStringLiteral("item_id"), item},
                {QStringLiteral("augment_id"), augment}, {QStringLiteral("exposed"), exposed},
                {QStringLiteral("control"), control}});
        }
        if (pairs.isEmpty() && offerPairs.isEmpty()) continue;
        QJsonObject entry{{QStringLiteral("player_index"), qint64(index)}, {QStringLiteral("champion_id"), champion},
            {QStringLiteral("team"), team}, {QStringLiteral("self"), self}, {QStringLiteral("pairs"), pairs}};
        if (!offerPairs.isEmpty()) entry[QStringLiteral("offered_augment_pairs")] = offerPairs;
        players.append(entry);
    }
    result[QStringLiteral("status")] = players.isEmpty() ? QStringLiteral("no_observed_pairs") : QStringLiteral("available");
    result[QStringLiteral("available")] = !players.isEmpty();
    result[QStringLiteral("players")] = players;
    return result;
}

inline QHash<int, QJsonObject> offeredPairs(const Catalog &catalog, const GameState &state) {
    QHash<int, QJsonObject> result;
    if (state.offers.isEmpty()) return result;
    const auto players = project(catalog, state).value(QStringLiteral("players")).toArray();
    for (const auto &value : players) {
        const auto player = value.toObject();
        for (const auto &entry : player.value(QStringLiteral("offered_augment_pairs")).toArray()) {
            const auto pair = entry.toObject();
            result.insert(pair.value(QStringLiteral("augment_id")).toInt(), pair);
        }
    }
    return result;
}
}
