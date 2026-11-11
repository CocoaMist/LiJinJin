#include "mechanic_delivery_v4.h"
#include "combat.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <cmath>

namespace {
QString k(const char *s) { return QString::fromLatin1(s); }
constexpr double epsilon = 1e-8;
bool finiteNumber(const QJsonValue &v, double low, double high = 1e12) {
    return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= low && v.toDouble() <= high;
}
bool finiteJson(const QJsonValue &v) {
    if (v.isDouble()) return std::isfinite(v.toDouble()) && std::abs(v.toDouble()) <= 1e12;
    if (v.isArray()) { for (const auto &part : v.toArray()) if (!finiteJson(part)) return false; }
    if (v.isObject()) { const auto object = v.toObject(); for (auto it = object.begin(); it != object.end(); ++it) if (!finiteJson(it.value())) return false; }
    return true;
}
QJsonObject rejected(const QString &reason, int index = -1) {
    return {{k("accepted"), false}, {k("error"), reason}, {k("effect_index"), index},
            {k("deliveries"), QJsonArray{}}, {k("state_only_ops"), QJsonArray{}}};
}
QString digest(const QJsonObject &value) {
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(value).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
}

QJsonObject planMechanicDeliveryV4(const QJsonArray &newEffects,
                                  const QJsonObject &observation,
                                  const QString &actorKey,
                                  const QJsonArray &players,
                                  double at) {
    if (!std::isfinite(at) || at < 0 || at > 1e7 || actorKey.isEmpty()) return rejected(k("invalid_delivery_time_or_actor"));
    QMap<QString, QJsonObject> roster;
    for (const auto &raw : players) {
        if (!raw.isObject()) return rejected(k("invalid_delivery_player"));
        const auto player = raw.toObject();
        const auto key = player.value(k("player_key")).toString(), team = player.value(k("team")).toString();
        if (key.isEmpty() || roster.contains(key) || team.isEmpty() || !player.value(k("alive")).isBool()
            || !finiteNumber(player.value(k("current_health")), 0) || !finiteNumber(player.value(k("maximum_health")), epsilon)
            || player.value(k("current_health")).toDouble() > player.value(k("maximum_health")).toDouble() + epsilon
            || player.value(k("alive")).toBool() != (player.value(k("current_health")).toDouble() > epsilon)) return rejected(k("invalid_delivery_player"));
        roster.insert(key, player);
    }
    if (!roster.contains(actorKey)) return rejected(k("delivery_actor_not_found"));
    const auto actor = roster.value(actorKey);
    const auto observed = observation.value(k("observation")).isObject() ? observation.value(k("observation")).toObject() : observation;
    const auto facts = observed.value(k("facts")).toObject();
    QString observationId = observation.value(k("event_id")).toString(observed.value(k("event_id")).toString());
    if (observationId.isEmpty()) observationId = digest(observed);
    if (newEffects.size() > 4096) return rejected(k("delivery_effect_limit"));
    // These operations have already changed the staged progression ledger.
    // Action-clock operations are deliberately absent: recording a refund in
    // a mechanic counter does not change ContinuousActionState's cooldown.
    const QSet<QString> stateOps{k("set"), k("configure"), k("add"), k("add_capped"),
        k("reset_source_cooldown"), k("expire_buff"), k("grant_item"), k("transform_items"),
        k("grant_list"), k("quest_item"), k("apply_anvil"), k("base_stats")};
    QJsonArray deliveries, stateOnly;
    QSet<QString> effectIds;
    double immediateHealthCost = 0;
    for (int index = 0; index < newEffects.size(); ++index) {
        if (!newEffects[index].isObject()) return rejected(k("invalid_delivery_effect"), index);
        const auto effect = newEffects[index].toObject();
        const auto op = effect.value(k("op")).toString(), key = effect.value(k("key")).toString();
        const auto target = effect.value(k("target")).toString(), source = effect.value(k("source")).toString();
        if (op.isEmpty() || key.isEmpty() || target.isEmpty() || source.isEmpty() || !effect.contains(k("value")) || !finiteJson(effect)) return rejected(k("invalid_delivery_effect"), index);
        if (!finiteNumber(effect.value(k("start")), 0, 1e7) || !finiteNumber(effect.value(k("expires")), -1, 1e7)) return rejected(k("invalid_delivery_lifetime"), index);
        const double start = effect.value(k("start")).toDouble(), expires = effect.value(k("expires")).toDouble();
        if ((expires < 0 && expires != -1) || (expires >= 0 && expires + epsilon < start)) return rejected(k("invalid_delivery_lifetime"), index);
        QString localId = effect.value(k("effect_id")).toString();
        if (localId.isEmpty()) localId = digest(effect);
        const auto effectId = actorKey + QChar(u':') + observationId + QChar(u':') + localId;
        if (effectIds.contains(effectId)) return rejected(k("duplicate_delivery_effect"), index);
        effectIds.insert(effectId);
        if (stateOps.contains(op) || op == k("buff")) {
            if (target != k("self")) return rejected(k("unsupported_progression_state_recipient"), index);
            if (op == k("buff") && (!combatStat(key) || !finiteNumber(effect.value(k("value")), -1e12))) return rejected(k("unsupported_progression_stat_buff"), index);
            if ((op == k("add") || op == k("add_capped")) && !finiteNumber(effect.value(k("value")), -1e12)) return rejected(k("invalid_progression_counter_delta"), index);
            if ((op == k("grant_item") || op == k("transform_items") || op == k("quest_item"))
                && (!finiteNumber(effect.value(k("value")), 1, 2147483647) || std::floor(effect.value(k("value")).toDouble()) != effect.value(k("value")).toDouble())) return rejected(k("invalid_progression_item_reward"), index);
            if ((op == k("grant_list") || op == k("apply_anvil")) && !effect.value(k("value")).isArray()) return rejected(k("invalid_progression_list_reward"), index);
            stateOnly.append(op);
            continue;
        }
        if (op != k("damage") && op != k("heal") && op != k("shield") && op != k("health_cost")) return rejected(k("unsupported_progression_recipient_effect:") + op, index);
        if (start + epsilon < at) return rejected(k("delivery_effect_already_started"), index);
        if (!finiteNumber(effect.value(k("value")), 0)) return rejected(k("invalid_delivery_amount"), index);
        const bool self = target == k("self");
        // A target id closes a single-recipient effect; it cannot supply a
        // missing list of tagged allies or enemies within an unknown area.
        const QSet<QString> singleTargets{k("target"), k("ally"), k("enemy"), k("lowest_health_ally"), k("chain_or_initial_ally")};
        if (!self && !singleTargets.contains(target)) return rejected(k("unsupported_delivery_recipient_set"), index);
        const auto recipientKey = self ? actorKey : effect.value(k("target_id")).toString();
        if (recipientKey.isEmpty() || !roster.contains(recipientKey)) return rejected(k("delivery_recipient_not_found"), index);
        const auto recipient = roster.value(recipientKey);
        if (!recipient.value(k("alive")).toBool()) return rejected(k("delivery_recipient_not_alive"), index);
        const bool sameTeam = recipient.value(k("team")) == actor.value(k("team"));
        QString damageType;
        if (op == k("damage")) {
            if (sameTeam) return rejected(k("delivery_damage_recipient_not_enemy"), index);
            if (key != k("physical") && key != k("magic") && key != k("true")) return rejected(k("delivery_damage_type_unknown"), index);
            damageType = key;
        } else {
            if (!sameTeam) return rejected(k("delivery_support_recipient_not_ally"), index);
            if (key != k("health")) return rejected(op == k("shield") ? k("typed_shield_not_supported") : k("delivery_health_key_unknown"), index);
        }
        const bool delayed = start > at + epsilon;
        bool cancelOnDeath = false;
        if (delayed) {
            const auto policyOwner = effect.contains(k("source_death_policy")) ? effect : facts;
            const auto policy = policyOwner.value(k("source_death_policy")).toString();
            if (!policyOwner.value(k("verified_source_death_policy")).toBool() || (policy != k("persist") && policy != k("cancel"))) return rejected(k("delayed_delivery_source_death_policy_unknown"), index);
            cancelOnDeath = policy == k("cancel");
        }
        if (op == k("health_cost")) {
            if (!self || delayed) return rejected(k("unsupported_delayed_or_other_recipient_health_cost"), index);
            immediateHealthCost += effect.value(k("value")).toDouble();
            // Match the execution ledger's living-health boundary. A cost
            // cannot be admitted with a staged reward if its eventual debit
            // would leave zero (or numerically dead) health.
            if (!std::isfinite(immediateHealthCost) || actor.value(k("current_health")).toDouble() - immediateHealthCost <= epsilon) return rejected(k("insufficient_delivery_health"), index);
        }
        deliveries.append(QJsonObject{{k("op"), op}, {k("recipient_key"), recipientKey}, {k("amount"), effect.value(k("value"))},
            {k("damage_type"), damageType}, {k("start_at"), start}, {k("expires_at"), expires},
            {k("source"), source}, {k("effect_id"), effectId}, {k("cancel_on_source_death"), cancelOnDeath},
            {k("cancel_on_interrupt"), false}, {k("recipient_alive_required"), true}, {k("defenses_at_delivery"), true}});
        if(effect.contains(k("causal_3116"))){auto packet=deliveries.last().toObject();packet[k("causal_3116")]=effect.value(k("causal_3116"));deliveries[deliveries.size()-1]=packet;}
    }
    return {{k("accepted"), true}, {k("error"), QJsonValue(QJsonValue::Null)},
        {k("deliveries"), deliveries}, {k("state_only_ops"), stateOnly}, {k("preflight_only"), true}};
}
