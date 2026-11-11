#include "mode_context_rules.h"
#include "combat.h"
#include <QRegularExpression>
#include <cmath>

namespace {
QString k(const char *v) { return QString::fromLatin1(v); }
}
std::optional<QJsonObject> validatedCombatModeRules(const QJsonValue &value, const QString &patch) {
    if (value.isUndefined()) return QJsonObject{{k("status"), k("legacy_parameters_not_source_bound")}};
    if (!value.isObject()) return {};
    const auto rules = value.toObject();
    static const QRegularExpression sha(k("^[a-f0-9]{64}$"));
    const auto source = rules.value(k("source")).toObject();
    if (rules.value(k("schema_version")) != QJsonValue(1) || rules.value(k("patch")) != patch
        || patch != k("16.21") || rules.value(k("mode")) != k("KIWI")
        || rules.value(k("status")) != k("selected_parameters_bound_to_local_mode_resource")
        || rules.value(k("mode_entry")) != k("Maps/Shipping/Map12/Modes/KIWI")
        || rules.value(k("proof_sha256")) != k("6350cd556110a1e86ae458b27c84fb017856142f76076db0b8d78a09f68775f3")
        || source.value(k("path")) != k("research/2026-10-22/local-client/data/data/maps/shipping/map12/map12.bin.json")
        || source.value(k("sha256")) != k("096fa83c26b48af552927128864b667cfb9855c62dbf4e33820e82f1acdcfbe1")
        || !sha.match(source.value(k("manifest_sha256")).toString()).hasMatch()
        || rules.value(k("live_verified")) != QJsonValue(false)) return {};
    const auto parameters = rules.value(k("parameters")).toObject();
    const auto haste = parameters.value(k("ability_haste_cap")).toObject();
    const auto speed = parameters.value(k("attack_speed_cap")).toObject();
    if (haste.value(k("source_pointer")) != k("/{a6506f8a}/AbilityHasteMax")
        || haste.value(k("derivation")) != k("identity") || haste.value(k("source_value")) != QJsonValue(500.)
        || haste.value(k("value")) != QJsonValue(500.)
        || speed.value(k("source_pointer")) != k("/{64237f19}/mGroups/{136d1a0f}/mConstants/gcd_AttackMinDelay/mValue")
        || speed.value(k("derivation")) != k("round(1.0 / gcd_AttackMinDelay.mValue, 6)")
        || speed.value(k("source_value")) != QJsonValue(0.20000000298023224)
        || speed.value(k("value")) != QJsonValue(5.)) return {};
    // These are the only parameters this contract admits. Adaptive-force scale
    // evidence cannot silently authorize the separate overflow-crit coefficient.
    if (rules.value(k("runtime_parameter_keys")).toArray() != QJsonArray{k("attack_speed_cap"),k("ability_haste_cap")}
        || parameters.contains(k("overflow_crit_ad")) || parameters.contains(k("overflow_crit_ap"))) return {};
    return rules;
}
void applyCombatModeRules(CombatContext &context, const QJsonObject &rules) {
    if (rules.value(k("status")) != k("selected_parameters_bound_to_local_mode_resource")) return;
    const auto parameters = rules.value(k("parameters")).toObject();
    context.attackSpeedCap = parameters.value(k("attack_speed_cap")).toObject().value(k("value")).toDouble();
    context.abilityHasteCap = parameters.value(k("ability_haste_cap")).toObject().value(k("value")).toDouble();
}
