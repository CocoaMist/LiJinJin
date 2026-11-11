#include "combat_units_v8.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>

namespace {
QString k(const char *value) { return QString::fromLatin1(value); }
constexpr double maxTime = 1e7 + 300, maxQuantity = 1e9, maxResistance = 1e6;
constexpr int maxUnits = 1024, maxEvents = 32768, maxPlayers = 64;
bool numeric(const QJsonValue &value, double minimum, double maximum) {
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= minimum && value.toDouble() <= maximum;
}
bool identifier(const QString &value) { return !value.isEmpty() && value == value.trimmed() && value.size() <= 256; }
QString digest(const QJsonObject &value) {
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(value).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
QJsonArray playerArray(const QSet<QString> &keys) {
    auto sorted = keys.values(); std::sort(sorted.begin(), sorted.end()); QJsonArray result;
    for (const auto &key : sorted) result.append(key);
    return result;
}
bool validPlayers(const QSet<QString> &keys) {
    if (keys.isEmpty() || keys.size() > maxPlayers) return false;
    for (const auto &key : keys) if (!identifier(key)) return false;
    return true;
}
QJsonArray boundaries() {
    return {k("npc_parameters_are_verified_synthetic_scenario_observations_not_game_source_repairs"),
        k("npc_position_ai_visibility_regeneration_gold_and_xp_not_simulated")};
}
}

bool CombatUnitsV8::loadObserved(const QJsonArray &observations, const QSet<QString> &playerKeys,
                                double start, QString *error) {
    const auto fail = [&](const char *reason) { if (error) *error = k(reason); return false; };
    if (!std::isfinite(start) || start < 0 || start > maxTime || !validPlayers(playerKeys)) return fail("invalid_unit_clock_or_player_roster");
    if (observations.size() > maxUnits) return fail("unit_observation_limit_exceeded");
    const QSet<QString> allowed{k("unit_id"), k("unit_class"), k("maximum_health"), k("current_health"), k("armor"), k("mr"),
        k("verified_unit_observation"), k("synthetic_observation"), k("current_shield"), k("verified_shield_observation"), k("movement_speed")};
    CombatUnitsV8 candidate; candidate.playerKeys_ = playerKeys; candidate.start_ = start; candidate.time_ = start;
    for (const auto &raw : observations) {
        if (!raw.isObject()) return fail("invalid_unit_observation");
        const auto row = raw.toObject();
        for (auto it = row.begin(); it != row.end(); ++it) if (!allowed.contains(it.key())) return fail("unsupported_unit_observation_field");
        const auto id = row.value(k("unit_id")).toString(), unitClass = row.value(k("unit_class")).toString();
        if (!identifier(id) || playerKeys.contains(id)) return fail("unit_id_invalid_or_collides_with_player");
        if (candidate.units_.contains(id)) return fail("duplicate_unit_id");
        if (unitClass != k("minion") && unitClass != k("large")) return fail("invalid_nonplayer_unit_class");
        if (row.value(k("verified_unit_observation")) != QJsonValue(true) || row.value(k("synthetic_observation")) != QJsonValue(true))
            return fail("verified_synthetic_unit_observation_required");
        if (!numeric(row.value(k("maximum_health")), 0, maxQuantity) || row.value(k("maximum_health")).toDouble() <= 0 ||
            !numeric(row.value(k("current_health")), 0, row.value(k("maximum_health")).toDouble()) || row.value(k("current_health")).toDouble() <= 0 ||
            !numeric(row.value(k("armor")), -maxResistance, maxResistance) || !numeric(row.value(k("mr")), -maxResistance, maxResistance))
            return fail("invalid_or_predead_unit_health_or_resistance");
        if (row.contains(k("movement_speed")) && !numeric(row.value(k("movement_speed")), 0, 1e5)) return fail("invalid_unit_movement_speed_observation");
        double shield = 0;
        if (row.contains(k("current_shield")) || row.contains(k("verified_shield_observation"))) {
            if (row.value(k("verified_shield_observation")) != QJsonValue(true) || !numeric(row.value(k("current_shield")), 0, maxQuantity))
                return fail("verified_unit_shield_observation_required");
            shield = row.value(k("current_shield")).toDouble();
        }
        Unit unit; unit.id = id; unit.unitClass = unitClass; unit.observation = row;
        unit.maximumHealth = row.value(k("maximum_health")).toDouble(); unit.health = unit.initialHealth = row.value(k("current_health")).toDouble();
        unit.armor = row.value(k("armor")).toDouble(); unit.mr = row.value(k("mr")).toDouble(); unit.shield = unit.initialShield = shield;
        candidate.units_.insert(id, unit);
    }
    candidate.loaded_ = true; *this = candidate; if (error) error->clear(); return true;
}

QJsonObject CombatUnitsV8::unit(const QString &unitId) const {
    const auto found = units_.constFind(unitId); if (found == units_.cend()) return {};
    const auto &unit = found.value();
    QJsonObject result{{k("unit_id"), unit.id}, {k("unit_class"), unit.unitClass}, {k("maximum_health"), unit.maximumHealth},
        {k("current_health"), unit.health}, {k("current_shield"), unit.shield}, {k("armor"), unit.armor}, {k("mr"), unit.mr},
        {k("initial_health"), unit.initialHealth}, {k("initial_shield"), unit.initialShield}, {k("alive"), unit.health > 0},
        {k("death_time"), unit.diedAt < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(unit.diedAt)}, {k("death_receipt_id"), unit.deathReceiptId},
        {k("movement_speed_known"), unit.observation.contains(k("movement_speed"))},
        {k("observed"), true}, {k("synthetic"), true}, {k("observation_origin"), k("verified_synthetic_scenario")}};
    if (unit.observation.contains(k("movement_speed"))) result[k("movement_speed")] = unit.observation.value(k("movement_speed"));
    return result;
}

bool CombatUnitsV8::advanceTo(double at, QString *error) {
    if (!loaded_ || !std::isfinite(at) || at < time_ || at > maxTime) {
        if (error) *error = k("invalid_or_nonmonotonic_unit_clock"); return false;
    }
    time_ = at; if (error) error->clear(); return true;
}

QJsonObject CombatUnitsV8::receiveDamage(const QString &unitId, const QString &attackerPlayerKey,
                                        const QString &eventId, double at, double mitigatedAmount, QString *error) {
    const auto fail = [&](const char *reason) {
        if (error) *error = k(reason);
        return QJsonObject{{k("accepted"), false}, {k("reason"), k(reason)}};
    };
    if (!loaded_ || !identifier(eventId) || !identifier(attackerPlayerKey) || !playerKeys_.contains(attackerPlayerKey)) return fail("invalid_unit_damage_identity_or_attacker");
    if (!std::isfinite(at) || at < time_ || at > maxTime || !std::isfinite(mitigatedAmount) || mitigatedAmount < 0 || mitigatedAmount > maxQuantity)
        return fail("invalid_unit_damage_time_or_amount");
    if (eventIds_.contains(eventId)) return fail("duplicate_unit_damage_event_id");
    if (damageEvents_.size() >= maxEvents) return fail("unit_damage_event_limit_exceeded");
    auto found = units_.find(unitId); if (found == units_.end()) return fail("unit_target_not_found");
    if (found->health <= 0) return fail("unit_target_already_dead");
    auto &target = found.value(); const double healthBefore = target.health, shieldBefore = target.shield;
    const double shieldDamage = std::min(shieldBefore, mitigatedAmount);
    const double actualDamage = std::min(healthBefore, mitigatedAmount - shieldDamage);
    const double healthAfter = healthBefore - actualDamage, shieldAfter = shieldBefore - shieldDamage;
    const bool died = healthBefore > 0 && healthAfter == 0;
    const auto identity = digest(QJsonObject{{k("event_id"), eventId}, {k("unit_id"), unitId}, {k("attacker_key"), attackerPlayerKey}});
    const auto receiptId = k("native-unit-damage-v8:") + identity;
    QJsonObject result{{k("accepted"), true}, {k("type"), k("native_unit_damage_receipt_v8")}, {k("native_unit_receipt"), true},
        {k("receipt_id"), receiptId}, {k("event_id"), eventId}, {k("time"), at}, {k("source_key"), attackerPlayerKey}, {k("attacker_key"), attackerPlayerKey},
        {k("target_key"), unitId}, {k("unit_id"), unitId}, {k("unit_class"), target.unitClass}, {k("target_type"), k("nonplayer_unit")},
        {k("requested_damage"), mitigatedAmount}, {k("actual_damage"), actualDamage}, {k("shield_damage"), shieldDamage},
        {k("health_before"), healthBefore}, {k("remaining_health"), healthAfter}, {k("shield_before"), shieldBefore}, {k("remaining_shield"), shieldAfter},
        {k("target_death"), died}, {k("observed"), true}, {k("synthetic"), true}, {k("observation_origin"), k("verified_synthetic_scenario")},
        {k("gold_xp_awarded"), false}, {k("unknowns"), boundaries()}};
    if (died) {
        const auto deathId = k("native-unit-death-v8:") + identity;
        const auto killClass = target.unitClass == k("minion") ? k("ordinary_non_champion") : k("large_non_champion");
        const QJsonObject death{{k("accepted"), true}, {k("type"), k("native_unit_death_receipt_v8")}, {k("native_unit_receipt"), true},
            {k("receipt_id"), deathId}, {k("damage_receipt_id"), receiptId}, {k("event_id"), eventId}, {k("time"), at},
            {k("source_key"), attackerPlayerKey}, {k("attacker_key"), attackerPlayerKey}, {k("target_key"), unitId},
            {k("unit_id"), unitId}, {k("unit_class"), target.unitClass}, {k("actual_damage"), actualDamage}, {k("shield_damage"), shieldDamage},
            {k("target_death"), true}, {k("kill_receipt_id"), deathId}, {k("kill_source_key"), attackerPlayerKey},
            {k("kill_target_key"), unitId}, {k("kill_target_type"), killClass}, {k("kill_receipt_time"), at},
            {k("kill_participant_keys"), QJsonArray{attackerPlayerKey}}, {k("observed"), true}, {k("synthetic"), true},
            {k("observation_origin"), k("verified_synthetic_scenario")}, {k("gold_xp_awarded"), false}, {k("unknowns"), boundaries()}};
        for (const auto &field : {k("kill_receipt_id"), k("kill_source_key"), k("kill_target_key"), k("kill_target_type"), k("kill_receipt_time"), k("kill_participant_keys")}) result[field] = death.value(field);
        result[k("death_receipt")] = death; target.deathReceiptId = deathId; target.diedAt = at; receipts_.insert(deathId, death);
    }
    target.health = healthAfter; target.shield = shieldAfter; time_ = at;
    eventIds_.insert(eventId); receipts_.insert(receiptId, result);
    damageEvents_.append(QJsonObject{{k("unit_id"), unitId}, {k("attacker_key"), attackerPlayerKey}, {k("event_id"), eventId}, {k("time"), at}, {k("mitigated_damage"), mitigatedAmount}});
    if (error) error->clear(); return result;
}

QJsonObject CombatUnitsV8::snapshot() const {
    QJsonArray observations, units, events, receipts;
    for (auto it = units_.cbegin(); it != units_.cend(); ++it) { observations.append(it.value().observation); units.append(unit(it.key())); }
    for (const auto &event : damageEvents_) events.append(event);
    for (auto it = receipts_.cbegin(); it != receipts_.cend(); ++it) receipts.append(it.value());
    QJsonObject result{{k("schema"), k("combat_units_v8")}, {k("loaded"), loaded_}, {k("start_time"), start_}, {k("time"), time_},
        {k("player_keys"), playerArray(playerKeys_)}, {k("observations"), observations}, {k("units"), units}, {k("damage_events"), events}, {k("receipts"), receipts},
        {k("damage_contract"), k("native_evaluated_mitigated_damage_shield_then_health_no_second_mitigation")},
        {k("observation_contract"), k("verified_synthetic_npc_parameters_never_player_alias_or_game_source_repair")},
        {k("omitted_shield_contract"), k("no_shield_in_this_explicit_synthetic_unit_not_a_claim_about_live_game")}, {k("unknowns"), boundaries()}};
    result[k("checkpoint_integrity_sha256")] = digest(result); return result;
}

bool CombatUnitsV8::restore(const QJsonObject &checkpoint, const QSet<QString> &playerKeys, QString *error) {
    const auto fail = [&](const char *reason) { if (error) *error = k(reason); return false; };
    if (checkpoint.value(k("schema")).toString() != k("combat_units_v8") || checkpoint.value(k("loaded")) != QJsonValue(true) ||
        !numeric(checkpoint.value(k("start_time")), 0, maxTime) || !numeric(checkpoint.value(k("time")), checkpoint.value(k("start_time")).toDouble(), maxTime) ||
        checkpoint.value(k("player_keys")) != QJsonValue(playerArray(playerKeys)) || !checkpoint.value(k("observations")).isArray() ||
        !checkpoint.value(k("damage_events")).isArray() || !checkpoint.value(k("units")).isArray() || !checkpoint.value(k("receipts")).isArray())
        return fail("invalid_unit_checkpoint");
    auto body = checkpoint; const auto expected = body.take(k("checkpoint_integrity_sha256")).toString();
    if (expected != digest(body)) return fail("unit_checkpoint_integrity_mismatch");
    const auto events = checkpoint.value(k("damage_events")).toArray();
    if (events.size() > maxEvents || checkpoint.value(k("units")).toArray().size() > maxUnits || checkpoint.value(k("receipts")).toArray().size() > 2 * maxEvents)
        return fail("unit_checkpoint_limit_exceeded");
    CombatUnitsV8 candidate; QString reason;
    if (!candidate.loadObserved(checkpoint.value(k("observations")).toArray(), playerKeys, checkpoint.value(k("start_time")).toDouble(), &reason)) {
        if (error) *error = k("unit_checkpoint_seed:") + reason; return false;
    }
    for (const auto &raw : events) {
        if (!raw.isObject()) return fail("invalid_unit_checkpoint_damage_event");
        const auto event = raw.toObject();
        if (event.size() != 5 || !event.value(k("unit_id")).isString() || !event.value(k("attacker_key")).isString() || !event.value(k("event_id")).isString() ||
            !numeric(event.value(k("time")), candidate.time(), maxTime) || !numeric(event.value(k("mitigated_damage")), 0, maxQuantity))
            return fail("invalid_unit_checkpoint_damage_event");
        if (!candidate.receiveDamage(event.value(k("unit_id")).toString(), event.value(k("attacker_key")).toString(), event.value(k("event_id")).toString(),
            event.value(k("time")).toDouble(), event.value(k("mitigated_damage")).toDouble(), &reason).value(k("accepted")).toBool()) {
            if (error) *error = k("unit_checkpoint_damage:") + reason; return false;
        }
    }
    if (!candidate.advanceTo(checkpoint.value(k("time")).toDouble(), &reason) || candidate.snapshot() != checkpoint) return fail("unit_checkpoint_replay_mismatch");
    *this = candidate; if (error) error->clear(); return true;
}
