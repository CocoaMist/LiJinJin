#include "shop_recommendation_projection.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>
#include <algorithm>
#include <cmath>

namespace ShopRecommendations {
namespace {
bool integer(const QJsonValue &value, int minimum = 1, int maximum = 1000000) {
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() == std::floor(value.toDouble())
        && value.toDouble() >= minimum && value.toDouble() <= maximum;
}
bool label(const QJsonValue &value, int maximum) {
    if (!value.isString() || value.toString().trimmed().isEmpty() || value.toString().size() > maximum) return false;
    for (QChar ch : value.toString()) if (ch.category() == QChar::Other_Control) return false;
    return true;
}
bool contextValid(const Context &value) {
    static const QRegularExpression key(QStringLiteral("^[A-Za-z][A-Za-z0-9]{0,63}$"));
    return value.championId > 0 && value.championId <= 100000 && label(value.sessionId, 256)
        && key.match(value.championKey).hasMatch() && label(value.sourceVersion, 80);
}
bool sourceUrl(const QJsonValue &value) {
    if (!label(value, 2048)) return false;
    const QUrl url(value.toString(), QUrl::StrictMode);
    return url.isValid() && (url.scheme() == "https" || url.scheme() == "http") && !url.host().isEmpty()
        && url.userName().isEmpty() && url.password().isEmpty();
}
bool hash(const QJsonValue &value) {
    static const QRegularExpression expression(QStringLiteral("^[0-9a-f]{64}$"));
    return value.isString() && expression.match(value.toString()).hasMatch();
}
QJsonObject matchedSource(const QJsonObject &row, const QJsonArray &sources, const QString &patch) {
    if (!sourceUrl(row.value("source_url")) || !hash(row.value("source_sha256"))
        || !label(row.value("id"), 256) || row.value("patch") != patch) return {};
    for (const auto &entry : sources) {
        if (!entry.isObject()) continue;
        const auto source = entry.toObject();
        if (source.value("url") == row.value("source_url") && source.value("sha256") == row.value("source_sha256")
            && source.value("patch") == patch && source.value("mode") == row.value("mode")
            && source.value("region") == row.value("region") && source.value("source_kind") == row.value("source_kind")) return source;
    }
    return {};
}
QString digest(const QJsonObject &value) {
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(value).toJson(QJsonDocument::Compact),
                                                      QCryptographicHash::Sha256).toHex());
}
}

Request projectRecommendation(const Context &context, const QSet<int> &allowed,
                              const QJsonObject &reference, const QJsonObject &purchase) {
    Request output; output.context = context; output.verifiedItemIds = allowed;
    if (!contextValid(context) || allowed.isEmpty()) return output;
    bool current = false, publicReference = false;
    const auto target = purchase.value("item_id");
    const bool purchaseBound = (!purchase.contains("context_id") || purchase.value("context_id") == context.sessionId)
        && (!purchase.contains("champion_id") || (integer(purchase.value("champion_id"), 1, 100000)
            && purchase.value("champion_id").toInt() == context.championId))
        && (!purchase.contains("source_version") || purchase.value("source_version") == context.sourceVersion)
        && (!purchase.contains("legal") || purchase.value("legal") == QJsonValue(true));
    if (purchaseBound && integer(target) && allowed.contains(target.toInt())) {
        Group group; group.title = QStringLiteral("当前待购（目标与组件，商店核对）");
        group.items.append({target.toInt(), 1});
        QSet<int> included{target.toInt()};
        const auto components = purchase.value("buyable_components").toArray();
        for (int i = 0; i < std::min(64, int(components.size())) && group.items.size() < 32; ++i) {
            if (!components[i].isObject()) continue;
            const auto component = components[i].toObject(); const auto id = component.value("item_id");
            if (!integer(id) || !allowed.contains(id.toInt()) || included.contains(id.toInt())
                || !integer(component.value("remaining_cost"), 1, 100000000)
                || (component.contains("legal") && component.value("legal") != QJsonValue(true))
                || (component.contains("affordable") && component.value("affordable") != QJsonValue(true))) continue;
            included.insert(id.toInt()); group.items.append({id.toInt(), 1});
        }
        group.provenance = {{"kind", "current_purchase"}, {"source", "upstream_gated_current_purchase"},
            {"source_version", context.sourceVersion}, {"champion_id", context.championId},
            {"component_scope", "upstream_gated_buyable_components_only"}, {"future_plan_consumed", false},
            {"actual_shop_prices_verified", false}, {"automatic_purchase", false}};
        if (purchase.value("affordable").isBool()) group.provenance["target_affordable"] = purchase.value("affordable");
        output.groups.append(group); current = true;
    }

    const auto rows = reference.value("records").toArray(), sources = reference.value("sources").toArray();
    const auto champion = reference.value("champion_id");
    const bool referenceBound = reference.value("available") == QJsonValue(true)
        && reference.value("reference_only") == QJsonValue(true)
        && reference.value("numeric_override") != QJsonValue(true) && reference.value("ranking_override") != QJsonValue(true)
        && integer(champion, 1, 100000) && champion.toInt() == context.championId
        && label(reference.value("patch"), 80) && rows.size() <= 128 && sources.size() <= 512;
    QSet<QString> builds;
    if (referenceBound) for (const auto &entry : rows) {
        if (output.groups.size() >= 3) break;
        if (!entry.isObject()) continue;
        const auto row = entry.toObject(); const auto rowChampion = row.value("champion_id");
        if (row.value("kind") != "item_build" || !integer(rowChampion, 1, 100000)
            || rowChampion.toInt() != context.championId || !row.value("data").isObject()) continue;
        const QString patch = reference.value("patch").toString();
        if (matchedSource(row, sources, patch).isEmpty()) continue;
        const auto data = row.value("data").toObject(); const auto raw = data.value("item_ids");
        if (!raw.isArray() || raw.toArray().isEmpty() || raw.toArray().size() > 32) continue;
        QList<int> ids; QSet<int> seen; bool valid = true;
        for (const auto &value : raw.toArray()) {
            if (!integer(value) || !allowed.contains(value.toInt())) { valid = false; break; }
            if (!seen.contains(value.toInt())) { seen.insert(value.toInt()); ids.append(value.toInt()); }
        }
        if (!valid || ids.isEmpty() || ids.size() > 6) continue;
        const bool ordered = data.value("ordered") == QJsonValue(true);
        QList<int> identity = ids; if (!ordered) std::sort(identity.begin(), identity.end());
        QStringList signature{ordered ? QStringLiteral("ordered") : QStringLiteral("unordered")};
        for (int id : identity) signature.append(QString::number(id));
        if (builds.contains(signature.join(','))) continue;
        builds.insert(signature.join(','));
        Group group;
        group.title = (ordered ? QStringLiteral("资料参考出装（来源有序） · %1")
                               : QStringLiteral("参考成装（不代表购买顺序） · %1")).arg(patch);
        for (int id : ids) group.items.append({id, 1});
        group.provenance = {{"kind", "public_item_build_reference"}, {"reference_only", true},
            {"ranking_override", false}, {"numeric_override", false}, {"record_id", row.value("id")},
            {"source_url", row.value("source_url")}, {"source_sha256", row.value("source_sha256")},
            {"source_patch", patch}, {"source_mode", row.value("mode")}, {"source_region", row.value("region")},
            {"source_kind", row.value("source_kind")}, {"champion_id", context.championId},
            {"ordered_by_source", ordered}, {"purchase_order_inferred", false}};
        if (label(reference.value("revision"), 256)) group.provenance["bundle_revision"] = reference.value("revision");
        output.groups.append(group); publicReference = true;
    }
    if (output.groups.isEmpty()) return output;
    output.sourceId = current && publicReference ? QStringLiteral("lixin_current_purchase+public_equipment_reference")
        : current ? QStringLiteral("lixin_current_purchase") : QStringLiteral("public_equipment_reference");
    QJsonArray evidence;
    for (const auto &group : output.groups) {
        QJsonArray ids; for (const auto &item : group.items) ids.append(item.id);
        evidence.append(QJsonObject{{"items", ids}, {"provenance", group.provenance}});
    }
    output.evidenceId = QStringLiteral("projection-sha256:") + digest(QJsonObject{{"champion_id", context.championId},
        {"source_version", context.sourceVersion}, {"groups", evidence}});
    output.confirmed = true;
    return output;
}
} // namespace ShopRecommendations
