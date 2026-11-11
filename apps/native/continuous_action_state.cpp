#include "continuous_action_state.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>

namespace {
constexpr double eps = 1e-8, bound = 1e9;
QString k(const char *s) { return QString::fromLatin1(s); }
bool number(double value, double maximum = bound) { return std::isfinite(value) && value >= 0 && value <= maximum; }
bool jsonNumber(const QJsonValue &value, double *out, double maximum = bound) {
    if (!value.isDouble() || !number(value.toDouble(), maximum)) return false;
    *out = value.toDouble(); return true;
}
QJsonObject rejected(const QString &reason, double at) {
    QJsonObject out{{k("accepted"), false}, {k("reason"), reason}};
    if (number(at)) out[k("time")] = at;
    return out;
}
QString signature(const QJsonObject &rules) {
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(rules).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
bool ranked(const QJsonObject &rule, const char *field, int rank, double *value) {
    const auto array = rule.value(k(field)).toArray();
    return rank > 0 && rank < array.size() && jsonNumber(array.at(rank), value);
}
double hasteMultiplier(const CombatContext &context, const CombatStats &stats, const QString &slot) {
    double haste = stats[Stat::Haste];
    if (slot == k("Q")) haste += stats[Stat::QHaste];
    if (slot == k("W")) haste += stats[Stat::WHaste];
    if (slot == k("E")) haste += stats[Stat::EHaste];
    if (slot == k("R")) haste += stats[Stat::RHaste];
    return (1 - std::clamp(stats[Stat::CooldownReduction], 0., .99)) /
        (1 + std::clamp(haste, 0., std::max(0., context.abilityHasteCap)) / 100.);
}
bool validStats(const CombatStats &stats) {
    for (double value : stats.values) if (!std::isfinite(value) || std::abs(value) > bound) return false;
    return true;
}
bool validContext(const CombatContext &context, const QJsonObject &rules) {
    if (context.level < 1 || context.level > 18 || !number(context.abilityHasteCap, 100000)) return false;
    QMap<QString, int> maximums;
    auto collect = [&](const QJsonObject &rows) {
        for (auto it = rows.begin(); it != rows.end(); ++it) {
            const auto row = it.value().toObject();
            const auto rankSlot = row.value(k("rank_from_slot")).toString(it.key());
            maximums[rankSlot] = std::max(maximums.value(rankSlot, 0), row.value(k("max_rank")).toInt(it.key() == k("R") ? 3 : 5));
        }
    };
    collect(rules.value(k("slots")).toObject());
    const auto forms = rules.value(k("forms")).toObject();
    for (auto it = forms.begin(); it != forms.end(); ++it) collect(it.value().toObject().value(k("slots")).toObject());
    for (auto it = context.ranks.begin(); it != context.ranks.end(); ++it)
        if (!maximums.contains(it.key()) || it.value() < 0 || it.value() > maximums.value(it.key())) return false;
    return true;
}
bool resourceStats(const QJsonObject &rules, const QString &kind, const CombatContext &context,
                   const CombatStats &stats, double *maximum, double *regen) {
    const auto resource = rules.value(k("resource")).toObject();
    *maximum = kind == k("mana") ? stats[Stat::Mana] : 0;
    *regen = kind == k("mana") ? stats[Stat::ManaRegen] : 0;
    if (kind == k("energy")) {
        const double growth = (.7025 + .0175 * (context.level - 1)) * (context.level - 1);
        const double baseMax = resource.value(k("base_max")).toDouble(0), growthMax = resource.value(k("growth_max")).toDouble(0);
        const double baseRegen = resource.value(k("base_regen_per_second")).toDouble(0), growthRegen = resource.value(k("growth_regen_per_second")).toDouble(0);
        if (!number(baseMax) || !number(growthMax) || !number(baseRegen) || !number(growthRegen)) return false;
        *maximum = baseMax + growthMax * growth; *regen = baseRegen + growthRegen * growth;
    }
    // Segmented fury is measured in its own source units, never in mana.
    // Require both raw quantities; an omitted segment count is not evidence
    // for a default of one. Reviewed explicit maxima retain precedence.
    if (kind == k("fury") && !resource.contains(k("max")) && !resource.contains(k("maximum"))) {
        const auto raw = resource.value(k("raw_resource")).toObject();
        if (raw.contains(k("arIncrements")) && raw.contains(k("arMaxSegments"))) {
            double increment = 0, segments = 0;
            if (!jsonNumber(raw.value(k("arIncrements")), &increment) || increment <= 0 ||
                !jsonNumber(raw.value(k("arMaxSegments")), &segments, 10000) || segments < 1 || std::floor(segments) != segments)
                return false;
            *maximum = increment * segments;
        }
    }
    const auto maximumValue = resource.contains(k("max")) ? resource.value(k("max")) : resource.value(k("maximum"));
    if (!maximumValue.isUndefined() && !jsonNumber(maximumValue, maximum)) return false;
    if (resource.contains(k("regen_per_second")) && !jsonNumber(resource.value(k("regen_per_second")), regen)) return false;
    return number(*maximum) && number(*regen);
}
bool sourceResourceMaximumKnown(const QJsonObject &rules, const QString &kind) {
    const auto resource = rules.value(k("resource")).toObject();
    if (kind == k("mana") || kind == k("none") || resource.contains(k("max")) || resource.contains(k("maximum"))) return true;
    if (kind == k("energy") && resource.contains(k("base_max"))) return true;
    const auto raw = resource.value(k("raw_resource")).toObject();
    return kind == k("fury") && raw.contains(k("arIncrements")) && raw.contains(k("arMaxSegments"));
}
int ammoMaximum(const QJsonObject &ammo, int rank) {
    double value = 0;
    if (!jsonNumber(ammo.value(k("max")), &value, 10000)) {
        if (!ranked(ammo, "max_by_rank", rank, &value)) return -1;
    }
    return value >= 1 && value <= 10000 && std::floor(value) == value ? int(value) : -1;
}
bool ammoSourceCost(const QJsonObject &ammo, int rank, double *cost) {
    QJsonValue value = ammo.value(k("cost_per_cast"));
    if (ammo.contains(k("cost_per_cast_by_rank"))) {
        const auto values = ammo.value(k("cost_per_cast_by_rank")).toArray();
        if (rank < 1 || rank >= values.size()) return false;
        value = values.at(rank);
    }
    if (value.isUndefined()) { *cost = 1; return true; }
    if (!value.isDouble() || !std::isfinite(value.toDouble()) || std::abs(value.toDouble()) > 10000) return false;
    *cost = value.toDouble(); return true;
}
double logarithmicTravelTime(double distance, double slope, double coefficient) {
    if (distance == 0.) return 0.;
    const double ratio = coefficient * distance / slope;
    if (ratio < 1e-6) {
        const double correction = 1. - ratio / 2. + ratio * ratio / 3.;
        return distance / slope * correction;
    }
    if (std::isfinite(ratio)) return std::log1p(ratio) / coefficient;
    return (std::log(coefficient) + std::log(distance) - std::log(slope)) / coefficient;
}
double affineResource(double before, double inflow, double coefficient, double elapsed) {
    if (coefficient == 0.) return before + inflow * elapsed;
    const double x = coefficient * elapsed;
    const double response = x < 1e-6 ? elapsed * (1. - x / 2. + x * x / 6.) :
        -std::expm1(-x) / coefficient;
    return before + (inflow - coefficient * before) * response;
}
}

QString ContinuousActionState::skillKey(const QString &slot) const {
    const auto rule = slotRule(slot);
    const QString group = rule.value(k("cooldown_group")).toString();
    if (!group.isEmpty()) return k("shared:") + group;
    return m_form.isEmpty() ? slot : m_form + k(":") + slot;
}

QJsonObject ContinuousActionState::slotRule(const QString &slot) const {
    const auto forms = m_rules.value(k("forms")).toObject();
    if (!m_form.isEmpty()) {
        const auto slotRows = forms.value(m_form).toObject().value(k("slots")).toObject();
        if (slotRows.contains(slot)) return slotRows.value(slot).toObject();
    }
    return m_rules.value(k("slots")).toObject().value(slot).toObject();
}

bool ContinuousActionState::load(const QJsonObject &rules, const CombatContext &context,
                                const CombatStats &stats, const QJsonObject &initial,
                                double start, QString *error) {
    auto fail = [&](const char *why) { if (error) *error = k(why); return false; };
    if (!number(start) || !validStats(stats) || !validContext(context, rules)) return fail("invalid_start_or_stats");
    ContinuousActionState state;
    state.m_rules = rules; state.m_context = context; state.m_stats = stats;
    state.m_time = start; state.m_attackReadyAt = start; state.m_castReadyAt = start;
    state.m_alive = initial.value(k("alive")).toBool(true);
    const auto resource = rules.value(k("resource")).toObject();
    state.m_resourceKind = resource.value(k("kind")).toString(rules.value(k("resource_kind")).toString());
    if (state.m_resourceKind.isEmpty() || state.m_resourceKind.size() > 64) return fail("unsupported_or_unknown_resource_kind");
    if (!resourceStats(rules, state.m_resourceKind, context, stats, &state.m_resourceMaximum, &state.m_regen)) return fail("invalid_resource_growth_or_stats");
    if (initial.contains(k("resource_maximum")) || initial.contains(k("verified_resource_maximum"))) {
        if (state.m_resourceKind == k("mana") || state.m_resourceKind == k("none") || sourceResourceMaximumKnown(rules, state.m_resourceKind))
            return fail("initial_resource_maximum_cannot_override_source_capacity");
        if (initial.value(k("verified_resource_maximum")) != QJsonValue(true) ||
            !jsonNumber(initial.value(k("resource_maximum")), &state.m_resourceMaximum) || state.m_resourceMaximum <= 0)
            return fail("unverified_or_invalid_initial_resource_maximum");
        state.m_resourceMaximumObserved = true;
    }
    state.m_regenWhileDead = resource.value(k("regen_while_dead")).toBool(false);
    if (initial.contains(k("resource_current"))) {
        if (!jsonNumber(initial.value(k("resource_current")), &state.m_resource) || state.m_resource > state.m_resourceMaximum + eps) return fail("invalid_initial_resource");
        state.m_resourceKnown = true;
    } else if (state.m_resourceKind == k("none")) {
        state.m_resource = 0; state.m_resourceKnown = true;
    }
    const auto forms = rules.value(k("forms")).toObject();
    state.m_form = initial.value(k("initial_form")).toString(rules.value(k("initial_form")).toString());
    if (!forms.isEmpty() && (state.m_form.isEmpty() || !forms.contains(state.m_form))) return fail("initial_form_required");
    if (forms.isEmpty() && !state.m_form.isEmpty() && state.m_form != k("default")) return fail("unknown_initial_form");
    const auto charges = initial.value(k("charges")).toObject();
    const auto ready = initial.value(k("ready_at")).toObject();
    const auto remaining = initial.value(k("cooldown_remaining")).toObject();
    auto initialize = [&](const QString &form) {
        state.m_form = form;
        QJsonObject slotRows = rules.value(k("slots")).toObject();
        const auto specific = forms.value(form).toObject().value(k("slots")).toObject();
        for (auto it = specific.begin(); it != specific.end(); ++it) slotRows[it.key()] = it.value();
        for (auto it = slotRows.begin(); it != slotRows.end(); ++it) {
            const QString slot = it.key(), key = state.skillKey(slot);
            if (state.m_skills.contains(key)) continue;
            SkillState skill; skill.readyAt = start;
            const double readyIn = context.skillReadyIn.value(slot, 0);
            if (!number(readyIn)) return false;
            skill.readyAt += readyIn;
            const auto readyValue = ready.contains(key) ? ready.value(key) : (form == initial.value(k("initial_form")).toString(rules.value(k("initial_form")).toString()) ? ready.value(slot) : QJsonValue(QJsonValue::Undefined));
            if (!readyValue.isUndefined() && !jsonNumber(readyValue, &skill.readyAt)) return false;
            const auto remainingValue = remaining.contains(key) ? remaining.value(key) : remaining.value(slot);
            if (!remainingValue.isUndefined()) { double seconds; if (!jsonNumber(remainingValue, &seconds) || !number(start + seconds)) return false; skill.readyAt = start + seconds; }
            const auto spec = it.value().toObject();
            const int skillRank = context.ranks.value(spec.value(k("rank_from_slot")).toString(slot), 0);
            const auto ammo = spec.value(k("ammo")).toObject();
            if (!ammo.isEmpty()) {
                const int maximum = ammoMaximum(ammo, std::max(1, skillRank));
                if (maximum < 1) return false;
                auto count = charges.value(key);
                if (count.isUndefined() && form == initial.value(k("initial_form")).toString(rules.value(k("initial_form")).toString())) count = charges.value(slot);
                double value = 0;
                if (skillRank < 1) {
                    // An unlearned charge spell does not prevent entering the
                    // match, and rank-zero recharge data never grants ammo.
                    if (!count.isUndefined() && (!jsonNumber(count, &value, maximum) || value != 0)) return false;
                    skill.charges = 0; skill.chargesKnown = true;
                    state.m_skills.insert(key, skill); continue;
                }
                if (!count.isUndefined()) {
                    if (!jsonNumber(count, &value, maximum) || std::floor(value) != value) return false;
                    skill.charges = int(value); skill.chargesKnown = true;
                } else if (!ammo.value(k("initial_unknown")).toBool(true)) {
                    skill.charges = maximum; skill.chargesKnown = true;
                }
                if (skill.chargesKnown && skill.charges < maximum) {
                    if (!ammo.value(k("recharge_known")).toBool(true)) skill.rechargeAwaitingObservation = true;
                    else {
                        double recharge;
                        if (skillRank < 1 || !ranked(ammo, "recharge_by_rank", skillRank, &recharge) || recharge <= 0) return false;
                        if (ammo.value(k("haste_applies")).toBool(false)) recharge *= hasteMultiplier(context, stats, slot);
                        skill.rechargeDuration = recharge; skill.rechargeAt = start + recharge;
                    }
                }
            }
            state.m_skills.insert(key, skill);
        }
        return true;
    };
    if (forms.isEmpty()) { if (!initialize(state.m_form)) return fail("invalid_initial_skill_state"); }
    else for (auto it = forms.begin(); it != forms.end(); ++it) if (!initialize(it.key())) return fail("invalid_initial_skill_state");
    state.m_form = initial.value(k("initial_form")).toString(rules.value(k("initial_form")).toString());
    const auto attack = rules.value(k("attack")).toObject();
    if (attack.contains(k("ammo_max"))) {
        double maximum;
        if (!jsonNumber(attack.value(k("ammo_max")), &maximum, 10000) || maximum < 1 || std::floor(maximum) != maximum ||
            !jsonNumber(attack.value(k("reload_seconds")), &state.m_attackReloadSeconds) || state.m_attackReloadSeconds <= 0) return fail("invalid_attack_ammo_rule");
        state.m_attackAmmoMaximum = int(maximum); state.m_attackAmmo = -1;
        if (initial.contains(k("attack_ammo"))) {
            double count;
            if (!jsonNumber(initial.value(k("attack_ammo")), &count, maximum) || std::floor(count) != count) return fail("invalid_initial_attack_ammo");
            state.m_attackAmmo = int(count);
            if (!state.m_attackAmmo) state.m_attackReloadAt = start + state.m_attackReloadSeconds;
        }
    }
    state.m_loaded = true;
    if (initial.contains(k("checkpoint")) && !state.restoreCheckpoint(initial.value(k("checkpoint")).toObject(), start, error)) return false;
    *this = state; if (error) error->clear(); return true;
}

bool ContinuousActionState::advanceInPlace(double at, bool alive) {
    if (!m_loaded || !number(at) || at + eps < m_time) return false;
    at = std::max(at, m_time);
    if (m_resourceKnown && (m_alive || m_regenWhileDead)) {
        double paidUntil = at;
        if (m_upkeepActive && m_alive) {
            const double loss = m_upkeepRate - m_regen;
            if (m_resource <= eps || (loss > 0 && m_resource - (at - m_time) * loss <= eps)) {
                paidUntil = m_resource <= eps ? m_time : std::min(at, m_time + m_resource / loss);
                m_upkeepSpent += (paidUntil - m_time) * m_upkeepRate;
                m_resource = 0;
                stopUpkeep(paidUntil, k("resource_exhausted"));
                m_resource = std::min(m_resourceMaximum, (at - paidUntil) * m_regen);
            } else {
                m_upkeepSpent += (at - m_time) * m_upkeepRate;
                m_resource = std::clamp(m_resource - (at - m_time) * loss, 0., m_resourceMaximum);
            }
        } else m_resource = std::min(m_resourceMaximum, m_resource + (at - m_time) * m_regen);
    }
    const QString original = m_form;
    const auto forms = m_rules.value(k("forms")).toObject();
    const QStringList formNames = forms.isEmpty() ? QStringList{m_form} : forms.keys();
    QSet<QString> visited;
    for (const auto &form : formNames) {
        m_form = form;
        QJsonObject slotRows = m_rules.value(k("slots")).toObject();
        const auto specific = forms.value(form).toObject().value(k("slots")).toObject();
        for (auto it = specific.begin(); it != specific.end(); ++it) slotRows[it.key()] = it.value();
        for (auto it = slotRows.begin(); it != slotRows.end(); ++it) {
            const auto key = skillKey(it.key()); if (visited.contains(key)) continue; visited.insert(key);
            auto &skill = m_skills[key];
            const auto ammo = it.value().toObject().value(k("ammo")).toObject();
            if (!ammo.value(k("recharge_known")).toBool(true)) continue;
            if (!skill.chargesKnown || ammo.isEmpty() || skill.rechargeAt < 0 || skill.rechargeDuration <= 0) continue;
            const int skillRank = m_context.ranks.value(it.value().toObject().value(k("rank_from_slot")).toString(it.key()), 0);
            if (skillRank < 1) continue;
            const int maximum = ammoMaximum(ammo, skillRank);
            if (maximum < 1) { m_form = original; return false; }
            if (skill.rechargeAt <= at + eps) {
                // The first completion date is already committed. Only cycles
                // beginning after that completion use current rank/haste.
                double nextDuration = 0;
                if (!ranked(ammo, "recharge_by_rank", skillRank, &nextDuration) || nextDuration <= 0) { m_form = original; return false; }
                if (ammo.value(k("haste_applies")).toBool(false)) nextDuration *= hasteMultiplier(m_context, m_stats, it.key());
                if (!number(nextDuration) || nextDuration <= 0) { m_form = original; return false; }
                const double elapsed = std::max(0., at - skill.rechargeAt);
                const int added = int(std::min(double(maximum - skill.charges), 1 + std::floor((elapsed + eps) / nextDuration)));
                skill.charges += std::max(0, added);
                if (skill.charges >= maximum) skill.rechargeAt = -1;
                else skill.rechargeAt += added * nextDuration;
                skill.rechargeDuration = nextDuration;
            }
        }
    }
    m_form = original;
    if (m_attackReloadAt >= 0 && m_attackReloadAt <= at + eps) { m_attackAmmo = m_attackAmmoMaximum; m_attackReloadAt = -1; }
    for (auto it = m_skills.begin(); it != m_skills.end(); ++it) {
        auto &skill = it.value();
        if (skill.burstExpiresAt >= 0 && skill.burstExpiresAt <= at + eps) {
            skill.burstRemaining = 0; skill.burstExpiresAt = -1; skill.burstReadyAt = -1;
            skill.burstInstanceId.clear(); skill.burstChannelled = false;
        }
    }
    if (m_channelExpiresAt >= 0 && m_channelExpiresAt <= at + eps) {
        m_channelSlot.clear(); m_channelInstanceId.clear(); m_channelExpiresAt = -1;
    }
    if (m_alive && !alive) {
        if (m_upkeepActive) stopUpkeep(at, k("source_death"));
        m_castReadyAt = at;
        m_channelSlot.clear(); m_channelInstanceId.clear(); m_channelExpiresAt = -1;
        for (auto it = m_skills.begin(); it != m_skills.end(); ++it) {
            auto &skill = it.value();
            skill.tokenExpiresAt = -1; skill.tokenTarget.clear(); skill.tokenPhase.clear();
            skill.burstRemaining = 0; skill.burstExpiresAt = -1; skill.burstReadyAt = -1;
            skill.burstInstanceId.clear(); skill.burstChannelled = false;
        }
    }
    m_time = at; m_alive = alive; return true;
}

bool ContinuousActionState::advance(double at, bool alive) {
    ContinuousActionState candidate = *this;
    if (!candidate.advanceInPlace(at, alive)) return false;
    *this = candidate; return true;
}

bool ContinuousActionState::updateStats(const CombatStats &stats, double at) {
    return updateContext(m_context, stats, at);
}

bool ContinuousActionState::updateContext(const CombatContext &context, const CombatStats &stats,
                                         double at, const QString &resourcePolicy) {
    if (!m_loaded || resourcePolicy != k("clamp_current") || !validStats(stats) || !validContext(context, m_rules) ||
        context.championId != m_context.championId || context.level < m_context.level) return false;
    for (auto it = m_context.ranks.begin(); it != m_context.ranks.end(); ++it)
        if (context.ranks.value(it.key(), 0) < it.value()) return false;
    ContinuousActionState state = *this;
    if (!state.advanceInPlace(at, m_alive)) return false;
    if (!resourceStats(m_rules, m_resourceKind, context, stats, &state.m_resourceMaximum, &state.m_regen)) return false;
    if (state.m_resourceMaximumObserved) {
        if (sourceResourceMaximumKnown(m_rules, m_resourceKind)) return false;
        state.m_resourceMaximum = m_resourceMaximum;
    }
    state.m_resource = std::min(state.m_resource, state.m_resourceMaximum);
    const QString originalForm = state.m_form;
    const auto forms = m_rules.value(k("forms")).toObject();
    const QStringList formNames = forms.isEmpty() ? QStringList{m_form} : forms.keys();
    QSet<QString> visited;
    for (const auto &form : formNames) {
        state.m_form = form;
        QJsonObject rows = m_rules.value(k("slots")).toObject();
        const auto specific = forms.value(form).toObject().value(k("slots")).toObject();
        for (auto it = specific.begin(); it != specific.end(); ++it) rows[it.key()] = it.value();
        for (auto it = rows.begin(); it != rows.end(); ++it) {
            const auto spec = it.value().toObject(), ammo = spec.value(k("ammo")).toObject();
            const QString key = state.skillKey(it.key());
            if (visited.contains(key)) continue; visited.insert(key);
            if (ammo.isEmpty()) continue;
            auto &skill = state.m_skills[key];
            const QString rankSlot = spec.value(k("rank_from_slot")).toString(it.key());
            const int oldRank = state.m_context.ranks.value(rankSlot, 0), newRank = context.ranks.value(rankSlot, 0);
            if (newRank < 1) continue;
            const int maximum = ammoMaximum(ammo, newRank);
            if (maximum < 1 || skill.charges > maximum) return false;
            if (!ammo.value(k("recharge_known")).toBool(true)) {
                skill.rechargeAwaitingObservation = skill.chargesKnown && skill.charges < maximum;
                continue;
            }
            // Learning a charge skill starts the source recharge clock at zero
            // charges. Existing recharge intervals keep their committed dates.
            if (skill.chargesKnown && skill.charges < maximum && skill.rechargeAt < 0 &&
                (oldRank == 0 || newRank > oldRank)) {
                double recharge = 0;
                if (!ranked(ammo, "recharge_by_rank", newRank, &recharge) || recharge <= 0) return false;
                if (ammo.value(k("haste_applies")).toBool(false)) recharge *= hasteMultiplier(context, stats, it.key());
                if (!number(at + recharge)) return false;
                skill.rechargeAt = at + recharge; skill.rechargeDuration = recharge;
            }
        }
    }
    state.m_form = originalForm; state.m_context = context; state.m_stats = stats;
    if (state.m_upkeepActive) {
        const auto spec = state.slotRule(state.m_upkeepSlot).value(k("resource_upkeep_v5")).toObject();
        const int rank = context.ranks.value(state.m_upkeepSlot, 0);
        if (!ranked(spec, "per_second_by_rank", rank, &state.m_upkeepRate) || state.m_upkeepRate <= 0) return false;
        if (state.m_resource <= eps) state.stopUpkeep(at, k("resource_exhausted"));
    }
    *this = state; return true;
}

bool ContinuousActionState::interruptCast(double at) {
    ContinuousActionState state = *this;
    if (!state.advanceInPlace(at, m_alive)) return false;
    state.m_castReadyAt = at;
    state.m_channelSlot.clear(); state.m_channelInstanceId.clear(); state.m_channelExpiresAt = -1;
    for (auto it = state.m_skills.begin(); it != state.m_skills.end(); ++it) if (it.value().burstChannelled) {
        it.value().burstRemaining = 0; it.value().burstExpiresAt = -1; it.value().burstReadyAt = -1;
        it.value().burstInstanceId.clear(); it.value().burstChannelled = false;
    }
    *this = state; return true;
}

bool ContinuousActionState::beginChannel(const QString &slot, double at, double until,
                                          const QString &instanceId, bool verified) {
    if (!verified || !number(until) || until <= at + eps || until - at > 3600 || instanceId.isEmpty() || instanceId.size() > 256) return false;
    ContinuousActionState state = *this;
    if (!state.advanceInPlace(at, m_alive) || !state.m_alive || at + eps < state.m_castReadyAt ||
        state.slotRule(slot).isEmpty() || state.m_context.ranks.value(state.slotRule(slot).value(k("rank_from_slot")).toString(slot), 0) < 1) return false;
    if (state.m_channelExpiresAt > at + eps) return false;
    state.m_channelSlot = slot; state.m_channelInstanceId = instanceId; state.m_channelExpiresAt = until;
    *this = state; return true;
}

QJsonObject ContinuousActionState::channelState() const {
    return {{k("active"), m_loaded && m_alive && m_channelExpiresAt > m_time + eps},
        {k("slot"), m_channelSlot}, {k("instance_id"), m_channelInstanceId}, {k("expires_at"), m_channelExpiresAt}};
}

QJsonObject ContinuousActionState::cast(const QString &slot, double at, const CombatStats &stats, const QJsonObject &facts) {
    return castInContext(slot, at, stats, facts, {}, {});
}

QJsonObject ContinuousActionState::castSourceUpkeep(const QString &slot, double at, const CombatStats &stats,
                                                   const QJsonObject &facts, const QString &bindingId,
                                                   const QString &programHash) {
    if (bindingId.isEmpty() || programHash.isEmpty()) return rejected(k("source_upkeep_program_identity_required"), at);
    return castInContext(slot, at, stats, facts, bindingId, programHash);
}

QJsonObject ContinuousActionState::castSourceHealthScheduleV9(const QString &slot, double at, const CombatStats &stats,
                                                            const QJsonObject &facts, const QString &profileId,
                                                            const QString &profileHash, const QString &bindingId,
                                                            const QString &programHash) {
    if (profileId.isEmpty() || profileHash.isEmpty() || bindingId.isEmpty() || programHash.isEmpty())
        return rejected(k("documented_health_schedule_identity_required"), at);
    return castInContext(slot, at, stats, facts, {}, {}, profileId, profileHash, bindingId, programHash);
}

QJsonObject ContinuousActionState::castInContext(const QString &slot, double at, const CombatStats &stats,
                                                const QJsonObject &facts, const QString &upkeepBindingId,
                                                 const QString &upkeepProgramHash, const QString &profileId,
                                                 const QString &profileHash, const QString &bindingId,
                                                 const QString &programHash) {
    ContinuousActionState state = *this;
    if (!validStats(stats) || !state.advanceInPlace(at, m_alive)) return rejected(k("invalid_or_nonmonotonic_time_or_stats"), at);
    if (!state.m_alive) return rejected(k("dead"), at);
    if (at + eps < state.m_castReadyAt) return rejected(k("cast_in_progress"), at);
    const auto primarySpec = state.slotRule(slot);
    if (primarySpec.isEmpty()) return rejected(k("unknown_slot_or_form"), at);
    if (!primarySpec.value(k("resource_upkeep_v5")).toObject().isEmpty()) return rejected(k("toggle_requires_explicit_state_transition"), at);
    QJsonObject spec = primarySpec;
    const auto documentedProfile = state.m_rules.value(k("documented_profile_v9")).toObject();
    const auto healthSchedule = documentedProfile.value(k("action_contract")).toObject();
    const bool documentedRequest = !profileId.isEmpty() || !profileHash.isEmpty() || !bindingId.isEmpty() || !programHash.isEmpty();
    const bool documentedSlot = !documentedProfile.isEmpty() && documentedProfile.value(k("program")).toObject().value(k("slot")).toString() == slot;
    if (documentedSlot && !documentedRequest) return rejected(k("documented_profile_requires_state_executor"), at);
    if (documentedRequest) {
        const auto digest = [](const QString &value) { if (value.size() != 64) return false; for (const auto ch : value) if (!((ch >= QLatin1Char('0') && ch <= QLatin1Char('9')) || (ch >= QLatin1Char('a') && ch <= QLatin1Char('f')))) return false; return true; };
        const auto program = documentedProfile.value(k("program")).toObject();
        if (state.m_context.championId != 8 || slot != k("E") || !documentedSlot ||
            documentedProfile.value(k("profile_id")).toString() != profileId || !digest(profileHash) ||
            documentedProfile.value(k("profile_sha256")).toString() != profileHash ||
            documentedProfile.value(k("execution_kind")).toString() != k("documented_model_v9") ||
            documentedProfile.value(k("server_mechanics_certified")) != QJsonValue(false) ||
            program.value(k("binding_id")).toString() != bindingId || bindingId != k("8:E:documented_v9") ||
            !digest(programHash) || program.value(k("program_sha256")).toString() != programHash ||
            healthSchedule.value(k("kind")).toString() != k("documented_health_schedule_v9") ||
            healthSchedule.value(k("startup_seconds")) != QJsonValue(0) ||
            healthSchedule.value(k("charge_clock_starts_at")).toString() != k("cast_complete_at") ||
            healthSchedule.value(k("maximum_charge_seconds")) != QJsonValue(1.5) ||
            healthSchedule.value(k("payment_offsets_seconds")).toArray() != QJsonArray{0,.25,.5,.75} ||
            healthSchedule.value(k("payment_maximum_health_fraction")) != QJsonValue(.02) ||
            healthSchedule.value(k("exemption_health_fraction")) != QJsonValue(.12) ||
            !QStringList{k("lt"),k("le")}.contains(healthSchedule.value(k("exemption_comparison")).toString()) ||
            !QStringList{k("payment_before_release"),k("release_before_payment")}.contains(healthSchedule.value(k("coincident_release_order")).toString()) ||
            healthSchedule.value(k("payment_health_basis")).toString() != k("live_maximum_health") ||
            primarySpec.value(k("source_pointer")).toString() != k("Characters/Vladimir/Spells/VladimirEAbility/VladimirE") ||
            primarySpec.value(k("cast_time_seconds")) != QJsonValue(-2) ||
            primarySpec.value(k("dynamic_charge_contract_v7")).toObject().value(k("preserves_raw_total_time_sentinel")) != QJsonValue(-8))
            return rejected(k("invalid_or_mismatched_documented_health_schedule"), at);
    }
    const QString phase = facts.value(k("phase")).toString(k("primary"));
    if (documentedRequest && phase != k("primary")) return rejected(k("documented_health_schedule_requires_primary_activation"), at);
    if ((!upkeepBindingId.isEmpty() || !upkeepProgramHash.isEmpty()) && phase != k("primary"))
        return rejected(k("source_upkeep_requires_primary_activation"), at);
    const bool isBurst = phase == k("burst");
    const auto burst = primarySpec.value(k("burst")).toObject();
    if (state.m_channelExpiresAt > at + eps && (!isBurst || state.m_channelSlot != slot)) return rejected(k("channel_in_progress"), at);
    const int primaryRank = state.m_context.ranks.value(primarySpec.value(k("rank_from_slot")).toString(slot), 0);
    if (primaryRank < 1) return rejected(k("unlearned_skill"), at);
    auto &primarySkill = state.m_skills[state.skillKey(slot)];
    if (isBurst) {
        if (burst.isEmpty() || primarySkill.burstInstanceId.isEmpty() || primarySkill.burstRemaining < 1 ||
            primarySkill.burstExpiresAt <= at + eps) return rejected(k("missing_or_expired_burst_instance"), at);
        if (at + eps < primarySkill.burstReadyAt) return rejected(k("burst_interval"), at);
        if (primarySkill.lastCastAt >= 0 && at <= primarySkill.lastCastAt + eps) return rejected(k("duplicate_same_time_cast"), at);
        double interval = 0;
        if (!ranked(burst, "recast_interval_by_rank", primaryRank, &interval) || interval <= 0 || !number(at + interval)) return rejected(k("unknown_burst_interval"), at);
        const QString instanceId = primarySkill.burstInstanceId;
        const bool channelShot = primarySkill.burstChannelled;
        --primarySkill.burstRemaining; ++primarySkill.burstEmitted;
        primarySkill.lastCastAt = at; primarySkill.burstReadyAt = at + interval;
        if (primarySkill.burstChannelled && primarySkill.burstRemaining == 0) {
            state.m_channelSlot.clear(); state.m_channelInstanceId.clear(); state.m_channelExpiresAt = -1;
            primarySkill.burstExpiresAt = -1; primarySkill.burstReadyAt = -1;
            primarySkill.burstInstanceId.clear(); primarySkill.burstChannelled = false;
        }
        QJsonObject out{{k("accepted"), true}, {k("reason"), k("accepted")}, {k("time"), at}, {k("slot"), slot}, {k("form"), state.m_form},
            {k("spec"), primarySpec}, {k("resource_cost"), 0}, {k("resource_remaining"), state.m_resource}, {k("resource_known"), state.m_resourceKnown},
            {k("cooldown_seconds"), 0}, {k("ready_at"), primarySkill.readyAt}, {k("cast_complete_at"), at}, {k("phase"), phase},
            {k("payload_emitted"), true}, {k("action_payload_kind"), k("burst_shot")}, {k("burst_instance_id"), instanceId},
            {k("shot_number"), primarySkill.burstEmitted}, {k("fourth_shot"), primarySkill.burstEmitted == 4},
            {k("burst_remaining"), primarySkill.burstRemaining}, {k("burst_expires_at"), primarySkill.burstExpiresAt}, {k("assumptions"), QJsonArray{}}};
        if (channelShot) {
            out[k("verified_jhin_r_shot")] = true; out[k("jhin_r_shot_number")] = primarySkill.burstEmitted;
            out[k("jhin_r_fourth_shot")] = primarySkill.burstEmitted == 4;
            out[k("conditions")] = QJsonArray{k("jhin_r_channel_active")};
        }
        *this = state; return out;
    }
    if (!burst.isEmpty() && primarySkill.burstExpiresAt > at + eps) return rejected(k("burst_instance_already_active"), at);
    const bool isRecast = phase != k("primary");
    if (isRecast) {
        spec = primarySpec.value(k("recasts")).toObject().value(phase).toObject();
        if (spec.isEmpty() || !spec.value(k("requires_recast_token")).toBool(false)) return rejected(k("unknown_recast_phase"), at);
    }
    if (!spec.value(k("active_cast")).toBool(true)) return rejected(k("passive_or_inactive_in_current_form"), at);
    if (!burst.isEmpty() && burst.value(k("requires_observed_activation")).toBool(false) &&
        !facts.value(k("verified_activation_eligibility")).toBool(false)) return rejected(k("unverified_burst_activation"), at);
    if (!documentedRequest && spec.value(k("requires_verified_health_cost")).toBool(spec.value(k("requires_observed_health_cost")).toBool(false))) {
        double healthCost = 0;
        if (!facts.value(k("verified_health_cost")).toBool(false) || !jsonNumber(facts.value(k("health_cost")), &healthCost)) return rejected(k("unverified_health_cost"), at);
    }
    if (spec.value(k("requires_observed_eligibility")).toBool(false) && !facts.value(k("verified_activation_eligibility")).toBool(false)) return rejected(k("unverified_activation_eligibility"), at);
    const int rank = state.m_context.ranks.value(spec.value(k("rank_from_slot")).toString(primarySpec.value(k("rank_from_slot")).toString(slot)), 0);
    if (rank < 1) return rejected(k("unlearned_skill"), at);
    auto &skill = state.m_skills[state.skillKey(slot)];
    if (skill.lastCastAt >= 0 && at <= skill.lastCastAt + eps) return rejected(k("duplicate_same_time_cast"), at);
    if (isRecast) {
        if (skill.tokenPhase != phase || skill.tokenExpiresAt < 0 || at + eps >= skill.tokenExpiresAt) return rejected(k("missing_or_expired_recast_token"), at);
        const bool requiresHit = slot == k("Q") || slot == k("E");
        if (requiresHit && !facts.value(k("verified_recast_hit")).toBool(false)) return rejected(k("unverified_recast_hit"), at);
        if (slot == k("Q") && (skill.tokenTarget.isEmpty() || facts.value(k("target_key")).toString() != skill.tokenTarget)) return rejected(k("recast_target_mismatch"), at);
    } else if (at + eps < skill.readyAt) return rejected(k("cooldown"), at);
    const auto ammo = spec.value(k("ammo")).toObject();
    if (!ammo.isEmpty() && !skill.chargesKnown) return rejected(k("unknown_initial_charges"), at);
    if (!ammo.isEmpty() && skill.charges < 1) return rejected(k("no_charges"), at);
    QJsonArray assumptions;
    double cost = 0, baseCooldown = 0;
    bool costKnown = spec.value(k("cost_known")).toBool(false) && ranked(spec, "cost_by_rank", rank, &cost);
    if (!costKnown && facts.value(k("verified_resource_cost")).toBool(false) && jsonNumber(facts.value(k("resource_cost")), &cost)) { costKnown = true; assumptions.append(k("verified_resource_cost_override")); }
    if (!costKnown) return rejected(k("unknown_resource_cost"), at);
    const bool sourceUpkeepRequest = !upkeepBindingId.isEmpty() || !upkeepProgramHash.isEmpty();
    const auto upkeepContract = spec.value(k("continuously_managed_resource_v7")).toObject();
    const auto sourcePointer = spec.value(k("source_pointer")).toString();
    const bool sourceUpkeepSkill = state.m_context.championId == 136 && slot == k("Q") &&
        sourcePointer == k("Characters/AurelionSol/Spells/AurelionSolQAbility/AurelionSolQ");
    QJsonObject sourceUpkeep;
    if (!sourceUpkeepRequest && (spec.contains(k("continuously_managed_resource_v7")) || sourceUpkeepSkill))
        return rejected(k("source_upkeep_requires_state_executor"), at);
    if (sourceUpkeepRequest) {
        const auto validDigest = [](const QString &digest) {
            if (digest.size() != 64) return false;
            for (const auto character : digest) if (!((character >= QLatin1Char('0') && character <= QLatin1Char('9')) ||
                (character >= QLatin1Char('a') && character <= QLatin1Char('f')))) return false;
            return true;
        };
        if (!sourceUpkeepSkill || isRecast || !burst.isEmpty() || !ammo.isEmpty() ||
            upkeepBindingId != k("136:Q:stateful_v7") || !validDigest(upkeepProgramHash) ||
            upkeepContract.value(k("schema_version")).toInt() != 7 ||
            upkeepContract.value(k("kind")).toString() != k("source_upkeep_only") ||
            upkeepContract.value(k("resource_kind")).toString() != k("mana") ||
            upkeepContract.value(k("basis")).toString() != k("flat") ||
            upkeepContract.value(k("binding_id")).toString() != upkeepBindingId ||
            upkeepContract.value(k("program_sha256")).toString() != upkeepProgramHash ||
            upkeepContract.value(k("source_pointer")).toString() != sourcePointer ||
            !spec.value(k("cost_known")).toBool(false) ||
            !upkeepContract.value(k("source_cost_by_rank")).isArray() ||
            upkeepContract.value(k("source_cost_by_rank")) != spec.value(k("cost_by_rank")))
            return rejected(k("invalid_or_mismatched_source_upkeep_contract"), at);
        const int maximumRank = spec.value(k("max_rank")).toInt(5);
        if (maximumRank < 1 || maximumRank > 6) return rejected(k("invalid_source_upkeep_rank_contract"), at);
        const auto rateExpression = upkeepContract.value(k("rate_expression")).toObject();
        for (int sourceRank = 1; sourceRank <= maximumRank; ++sourceRank) {
            double sourceCost = 0;
            const auto rate = compileCombatFormula(rateExpression, state.m_context, sourceRank).evaluate(state.m_context, stats);
            if (!ranked(spec, "cost_by_rank", sourceRank, &sourceCost) || !rate || !number(*rate) ||
                std::abs(*rate - sourceCost) > eps * std::max(1., sourceCost))
                return rejected(k("source_upkeep_rate_does_not_match_source_cost"), at);
        }
        if (state.m_resourceKind != k("mana")) return rejected(k("unsupported_source_upkeep_resource_kind"), at);
        if (!state.m_resourceKnown) return rejected(k("unknown_initial_resource"), at);
        sourceUpkeep = {{k("binding_id"), upkeepBindingId}, {k("program_sha256"), upkeepProgramHash},
            {k("source_pointer"), sourcePointer}, {k("resource_kind"), k("mana")}, {k("basis"), k("flat")},
            {k("rate_per_second"), cost}, {k("activation_resource_cost"), 0}, {k("continuous_executor_required"), true}};
        cost = 0;
        assumptions.append(k("source_bound_continuous_resource_cost"));
    }
    bool cooldownKnown = spec.value(k("cooldown_known")).toBool(false) && ranked(spec, "cooldown_by_rank", rank, &baseCooldown);
    if (isRecast && !spec.value(k("starts_primary_cooldown")).toBool(true)) { cooldownKnown = true; baseCooldown = 0; }
    bool observedCooldown = false;
    if (facts.value(k("verified_observed_cooldown")).toBool(false) && jsonNumber(facts.value(k("observed_cooldown")), &baseCooldown)) {
        cooldownKnown = true; observedCooldown = true; assumptions.append(k("verified_observed_cooldown_override"));
    }
    if (!cooldownKnown) return rejected(k("unknown_cooldown"), at);
    if (!isRecast && ammo.isEmpty() && baseCooldown <= eps && !facts.value(k("verified_activation_eligibility")).toBool(false)) return rejected(k("zero_cooldown_requires_observed_eligibility"), at);
    const QString resourceKind = spec.value(k("resource_kind")).toString(state.m_resourceKind);
    if (cost > 0 && resourceKind != state.m_resourceKind) return rejected(k("unsupported_skill_resource_kind"), at);
    if (cost > 0 && !state.m_resourceKnown) return rejected(k("unknown_initial_resource"), at);
    const double maximum = state.m_resourceKind == k("mana") ? std::max(0., stats[Stat::Mana]) : state.m_resourceMaximum;
    state.m_resourceMaximum = maximum; state.m_resource = std::min(state.m_resource, maximum);
    if (cost > state.m_resource + eps) return rejected(k("insufficient_resource"), at);
    if (state.m_resourceKind == k("mana")) state.m_regen = std::max(0., stats[Stat::ManaRegen]);
    state.m_stats = stats;
    double cooldown = baseCooldown;
    if (!isRecast && !spec.value(k("consumes_cooldown")).toBool(true) && !observedCooldown) return rejected(k("script_managed_cooldown_requires_observation"), at);
    if (!observedCooldown && spec.value(k("cooldown_haste_applies")).toBool(spec.value(k("haste_applies")).toBool(true))) cooldown *= hasteMultiplier(state.m_context, stats, slot);
    double castTime = 0;
    QJsonObject dynamicCharge;
    if (documentedRequest) {
        castTime = healthSchedule.value(k("startup_seconds")).toDouble();
        assumptions.append(k("explicit_documented_model_v9_not_server_certified"));
    } else if (spec.contains(k("cast_time_seconds")) && !jsonNumber(spec.value(k("cast_time_seconds")), &castTime, 3600)) {
        const auto contract = spec.value(k("dynamic_charge_contract_v7")).toObject();
        const auto sourcePointer = spec.value(k("source_pointer")).toString();
        auto strictTrue = [](const QJsonValue &value) { return value.isBool() && value.toBool(); };
        if (!spec.value(k("cast_time_seconds")).isDouble() || spec.value(k("cast_time_seconds")).toDouble() != -2. ||
            state.m_context.championId != 8 || slot != k("E") || isRecast ||
            sourcePointer != k("Characters/Vladimir/Spells/VladimirEAbility/VladimirE") ||
            contract.value(k("schema_version")).toInt() != 7 ||
            contract.value(k("kind")).toString() != k("movable_charge_start_then_release") ||
            contract.value(k("charge_clock_starts_at")).toString() != k("cast_complete_at") ||
            contract.value(k("preserves_raw_cast_time_sentinel")).toDouble(0) != -2. ||
            contract.value(k("preserves_raw_total_time_sentinel")).toDouble(0) != -8. ||
            !strictTrue(contract.value(k("source_use_charge_channeling"))) ||
            !strictTrue(contract.value(k("source_can_move_while_channeling"))) ||
            !strictTrue(contract.value(k("movement_allowed"))) ||
            contract.value(k("start_emits_damage")) != QJsonValue(false) ||
            !strictTrue(contract.value(k("release_emits_damage_once"))))
            return rejected(k("unknown_or_invalid_cast_time"), at);
        double minimumStartup = 0, maximumStartup = 0, minimumCharge = 0, maximumCharge = 0;
        if (!jsonNumber(contract.value(k("minimum_startup_seconds")), &minimumStartup, 3600) ||
            !jsonNumber(contract.value(k("maximum_startup_seconds")), &maximumStartup, 3600) ||
            !jsonNumber(contract.value(k("minimum_charge_seconds")), &minimumCharge, 3600) ||
            !jsonNumber(contract.value(k("maximum_charge_seconds")), &maximumCharge, 3600) ||
            minimumStartup != 0. || minimumCharge != 0. || maximumStartup != 1.5 || maximumCharge != 1.5)
            return rejected(k("invalid_dynamic_charge_contract"), at);
        const auto sourceMaximum = compileCombatFormula(contract.value(k("maximum_charge_expression")).toObject(), state.m_context, rank).evaluate(state.m_context, stats);
        const auto fullThreshold = compileCombatFormula(contract.value(k("full_damage_threshold_expression")).toObject(), state.m_context, rank).evaluate(state.m_context, stats);
        if (!sourceMaximum || !fullThreshold || !number(*sourceMaximum, 3600) || !number(*fullThreshold, 3600) ||
            *sourceMaximum != maximumCharge || *fullThreshold > maximumCharge)
            return rejected(k("invalid_dynamic_charge_contract"), at);
        double chargeDuration = 0;
        if (!strictTrue(facts.value(k("verified_charge_startup_time"))) ||
            !jsonNumber(facts.value(k("charge_startup_seconds")), &castTime, 3600) ||
            castTime < minimumStartup || castTime > maximumStartup ||
            !strictTrue(facts.value(k("verified_charge_duration"))) ||
            !jsonNumber(facts.value(k("charge_duration")), &chargeDuration, 3600) ||
            chargeDuration < minimumCharge || chargeDuration > maximumCharge)
            return rejected(k("unverified_or_invalid_dynamic_charge_timing"), at);
        double healthCost = 0;
        if (!strictTrue(facts.value(k("verified_health_cost"))) || !jsonNumber(facts.value(k("health_cost")), &healthCost))
            return rejected(k("unverified_health_cost"), at);
        const auto minimumHealthCost = compileCombatFormula(contract.value(k("health_cost_minimum_expression")).toObject(), state.m_context, rank).evaluate(state.m_context, stats);
        const auto maximumHealthCost = compileCombatFormula(contract.value(k("health_cost_maximum_expression")).toObject(), state.m_context, rank).evaluate(state.m_context, stats);
        double healthAbsoluteTolerance = 0, healthRelativeTolerance = 0;
        if (!minimumHealthCost || !maximumHealthCost || !number(*minimumHealthCost) || !number(*maximumHealthCost) ||
            *minimumHealthCost > *maximumHealthCost ||
            !jsonNumber(contract.value(k("health_cost_comparison_absolute_tolerance")), &healthAbsoluteTolerance, 1e-6) ||
            !jsonNumber(contract.value(k("health_cost_comparison_relative_tolerance")), &healthRelativeTolerance, 1e-6))
            return rejected(k("invalid_dynamic_charge_health_cost_contract"), at);
        const double minimumTolerance = healthAbsoluteTolerance + healthRelativeTolerance * std::max(1., std::abs(*minimumHealthCost));
        const double maximumTolerance = healthAbsoluteTolerance + healthRelativeTolerance * std::max(1., std::abs(*maximumHealthCost));
        if (healthCost < *minimumHealthCost - minimumTolerance || healthCost > *maximumHealthCost + maximumTolerance)
            return rejected(k("dynamic_charge_health_cost_outside_source_bounds"), at);
        if (strictTrue(facts.value(k("verified_full_charge_release"))) && chargeDuration < *fullThreshold)
            return rejected(k("full_charge_release_precedes_source_threshold"), at);
        if (!number(at + castTime + chargeDuration)) return rejected(k("time_overflow"), at);
        assumptions.append(k("verified_dynamic_charge_startup_time"));
        assumptions.append(k("verified_dynamic_charge_duration"));
        dynamicCharge = {{k("source_pointer"), sourcePointer}, {k("source_cast_time_sentinel"), -2},
            {k("source_total_time_sentinel"), -8}, {k("startup_seconds"), castTime},
            {k("charge_duration"), chargeDuration}, {k("charge_starts_at"), at + castTime},
            {k("charge_release_at"), at + castTime + chargeDuration},
            {k("maximum_charge_seconds"), *sourceMaximum}, {k("full_damage_threshold_seconds"), *fullThreshold},
            {k("observed_health_cost"), healthCost}, {k("health_cost_minimum"), *minimumHealthCost}, {k("health_cost_maximum"), *maximumHealthCost},
            {k("movement_allowed"), true}, {k("start_emits_damage"), false}, {k("release_emits_damage_once"), true}};
    }
    if (!number(at + cooldown) || !number(at + castTime)) return rejected(k("time_overflow"), at);
    int burstMaximum = 0;
    double burstInterval = 0, burstDuration = 0;
    const bool burstChannelled = !burst.isEmpty() && burst.value(k("activation_cost_only")).toBool(false);
    if (!burst.isEmpty()) {
        double maximumCasts = 0;
        if (!jsonNumber(burst.value(k("max_casts")), &maximumCasts, 100) && !ranked(burst, "max_casts_by_rank", rank, &maximumCasts)) return rejected(k("unknown_burst_cast_count"), at);
        if (maximumCasts < 1 || maximumCasts > 100 || std::floor(maximumCasts) != maximumCasts) return rejected(k("invalid_burst_cast_count"), at);
        burstMaximum = int(maximumCasts);
        if (!ranked(burst, "recast_interval_by_rank", rank, &burstInterval) || burstInterval <= 0) return rejected(k("unknown_burst_interval"), at);
        if (!ranked(burst, "recast_window_by_rank", rank, &burstDuration)) {
            if (!burstChannelled || !facts.value(k("verified_channel_duration")).toBool(false) ||
                !jsonNumber(facts.value(k("channel_duration")), &burstDuration, 3600)) return rejected(k("unknown_channel_duration"), at);
            assumptions.append(k("verified_observed_channel_duration_override"));
        }
        if (burstDuration <= std::max(castTime, burstInterval) || !number(at + burstDuration) || !number(at + burstInterval)) return rejected(k("invalid_burst_duration"), at);
        if (!burst.contains(k("cooldown_starts_on"))) assumptions.append(k("burst_primary_cooldown_start_assumption"));
        else if (burst.value(k("cooldown_starts_on")).toString() != k("primary")) return rejected(k("unsupported_burst_cooldown_start"), at);
    }
    double recharge = 0;
    int chargeCost = 1;
    bool observedAmmoCost = false, unknownAmmoRecharge = false;
    if (!ammo.isEmpty()) {
        double chargeCostValue = 1;
        if (!ammoSourceCost(ammo, rank, &chargeCostValue)) return rejected(k("invalid_ammo_cost"), at);
        if (chargeCostValue <= 0) {
            if (facts.value(k("verified_ammo_cost")) != QJsonValue(true) || !jsonNumber(facts.value(k("ammo_cost")), &chargeCostValue, 10000) ||
                chargeCostValue < 1 || std::floor(chargeCostValue) != chargeCostValue || chargeCostValue > ammoMaximum(ammo, rank))
                return rejected(k("verified_scenario_ammo_cost_required"), at);
            observedAmmoCost = true; assumptions.append(k("verified_scenario_ammo_cost_not_source_repair"));
        } else {
            if (facts.contains(k("verified_ammo_cost")) || facts.contains(k("ammo_cost"))) return rejected(k("known_source_ammo_cost_cannot_override"), at);
            if (chargeCostValue < 1 || std::floor(chargeCostValue) != chargeCostValue) return rejected(k("invalid_ammo_cost"), at);
        }
        chargeCost = int(chargeCostValue);
        if (skill.charges < chargeCost) return rejected(k("no_charges"), at);
        if (!ammo.value(k("sequential")).toBool(true) || ammo.value(k("recharge_mode")).toString(k("sequential")) != k("sequential")) return rejected(k("unsupported_parallel_ammo_recharge"), at);
        unknownAmmoRecharge = !ammo.value(k("recharge_known")).toBool(true);
        if (unknownAmmoRecharge) assumptions.append(k("source_ammo_recharge_unknown_no_automatic_refill"));
        else {
            if (!ranked(ammo, "recharge_by_rank", rank, &recharge) || recharge <= 0) return rejected(k("unknown_ammo_recharge"), at);
            if (ammo.value(k("haste_applies")).toBool(false)) recharge *= hasteMultiplier(state.m_context, stats, slot);
            if (!number(at + recharge)) return rejected(k("time_overflow"), at);
        }
    }
    state.m_resource = std::max(0., state.m_resource - cost);
    if (state.m_upkeepActive && state.m_resource <= eps) state.stopUpkeep(at, k("resource_exhausted"));
    skill.lastCastAt = at;
    if (!isRecast || spec.value(k("starts_primary_cooldown")).toBool(true)) skill.readyAt = at + cooldown;
    if (!ammo.isEmpty()) {
        skill.charges -= chargeCost;
        skill.lastAmmoCost = chargeCost; skill.lastAmmoCostObserved = observedAmmoCost;
        if (unknownAmmoRecharge) { skill.rechargeAwaitingObservation = true; skill.rechargeAt = -1; skill.rechargeDuration = 0; }
        else if (skill.rechargeAt < 0) { skill.rechargeAt = at + recharge; skill.rechargeDuration = recharge; }
    }
    state.m_castReadyAt = at + castTime;
    if (!burst.isEmpty()) {
        skill.burstMaximum = burstMaximum; skill.burstChannelled = burstChannelled;
        skill.burstEmitted = burstChannelled ? 0 : 1;
        skill.burstRemaining = burstMaximum - skill.burstEmitted;
        skill.burstExpiresAt = at + burstDuration; skill.burstReadyAt = at + burstInterval;
        skill.burstInstanceId = QString::number(state.m_context.championId) + k(":") + state.skillKey(slot) + k(":") + QString::number(at, 'g', 17);
        if (burstChannelled) {
            state.m_channelSlot = slot; state.m_channelInstanceId = skill.burstInstanceId;
            state.m_channelExpiresAt = skill.burstExpiresAt;
        }
    }
    if (isRecast) {
        skill.tokenExpiresAt = -1; skill.tokenTarget.clear(); skill.tokenPhase.clear();
    } else {
        // Source-pinned second casts are finite capabilities, never another
        // ordinary cast awarded because the primary slot is on cooldown.
        const auto second = primarySpec.value(k("recasts")).toObject().value(k("second")).toObject();
        double window = 0;
        const bool hitConfirmed = facts.value(k("confirmed_hit")).toBool(facts.value(k("first_hit_confirmed")).toBool(false));
        const QString target = facts.value(k("target_key")).toString();
        if (!second.isEmpty() && ranked(second, "token_window_by_rank", rank, &window) && window > 0 &&
            (slot != k("Q") || (hitConfirmed && !target.isEmpty())) && (slot != k("E") || hitConfirmed)) {
            skill.tokenPhase = k("second"); skill.tokenExpiresAt = at + window; skill.tokenTarget = target;
        }
        const auto refund = primarySpec.value(k("on_champion_target_cooldown_recovery")).toObject();
        double fraction = 0;
        if (!refund.isEmpty() && facts.value(k("verified_w_champion_target")).toBool(false) && ranked(refund, "fraction_by_rank", rank, &fraction) && fraction <= 1)
            skill.readyAt = at + cooldown * (1 - fraction);
    }
    QJsonObject out{{k("accepted"), true}, {k("reason"), k("accepted")}, {k("time"), at}, {k("slot"), slot}, {k("form"), state.m_form},
        {k("spec"), spec}, {k("resource_cost"), cost}, {k("resource_remaining"), state.m_resource}, {k("resource_known"), state.m_resourceKnown},
        {k("cooldown_seconds"), isRecast ? 0 : skill.readyAt - at}, {k("ready_at"), skill.readyAt}, {k("cast_complete_at"), state.m_castReadyAt}, {k("phase"), phase},
        {k("recast_token_expires_at"), skill.tokenExpiresAt},
        {k("assumptions"), assumptions}};
    out[k("payload_emitted")] = !burstChannelled && dynamicCharge.isEmpty() && sourceUpkeep.isEmpty() && !documentedRequest;
    if (documentedRequest) {
        out[k("action_payload_kind")] = k("documented_health_schedule_start_v9");
        out[k("documented_profile_id_v9")] = profileId; out[k("documented_profile_sha256_v9")] = profileHash;
        out[k("health_schedule_v9")] = healthSchedule;
    }
    if (!sourceUpkeep.isEmpty()) {
        out[k("action_payload_kind")] = k("source_upkeep_start");
        out[k("source_upkeep_v7")] = sourceUpkeep;
    }
    if (!dynamicCharge.isEmpty()) {
        out[k("action_payload_kind")] = k("dynamic_charge_start");
        out[k("dynamic_charge_v7")] = dynamicCharge;
    }
    if (!burst.isEmpty()) {
        out[k("action_payload_kind")] = burstChannelled ? k("channel_start") : k("burst_shot");
        out[k("burst_instance_id")] = skill.burstInstanceId; out[k("shot_number")] = skill.burstEmitted;
        out[k("fourth_shot")] = false; out[k("burst_remaining")] = skill.burstRemaining;
        out[k("burst_expires_at")] = skill.burstExpiresAt;
    }
    if (!ammo.isEmpty()) {
        out[k("charges_remaining")] = skill.charges; out[k("recharge_at")] = skill.rechargeAt;
        out[k("ammo_cost")] = chargeCost; out[k("ammo_cost_observed")] = observedAmmoCost;
        out[k("recharge_awaiting_observation")] = skill.rechargeAwaitingObservation;
    }
    const QString switchesTo = spec.value(k("switches_to")).toString();
    if (!switchesTo.isEmpty()) {
        if (!state.m_rules.value(k("forms")).toObject().contains(switchesTo)) return rejected(k("unknown_transform_destination"), at);
        state.m_form = switchesTo; out[k("transformed_to")] = switchesTo;
    }
    *this = state; return out;
}

QJsonObject ContinuousActionState::attack(double at, double attackSpeed, bool alive) {
    ContinuousActionState state = *this;
    if (!number(attackSpeed, 100) || attackSpeed <= 0 || !state.advanceInPlace(at, m_alive)) return rejected(k("invalid_attack_speed_or_time"), at);
    if (!alive || !state.m_alive) return rejected(k("dead"), at);
    if (state.m_channelExpiresAt > at + eps) return rejected(k("channel_in_progress"), at);
    if (at + eps < state.m_castReadyAt) return rejected(k("cast_in_progress"), at);
    if (at + eps < state.m_attackReadyAt) return rejected(k("attack_cooldown"), at);
    if (state.m_attackReloadAt >= 0) return rejected(k("reloading"), at);
    if (state.m_attackAmmoMaximum && state.m_attackAmmo < 0) return rejected(k("unknown_initial_attack_ammo"), at);
    const double interval = 1 / attackSpeed;
    if (!number(at + interval)) return rejected(k("time_overflow"), at);
    const int shot = state.m_attackAmmoMaximum ? state.m_attackAmmoMaximum - state.m_attackAmmo + 1 : 0;
    state.m_attackReadyAt = at + interval;
    if (state.m_attackAmmoMaximum) {
        --state.m_attackAmmo;
        if (!state.m_attackAmmo) state.m_attackReloadAt = at + state.m_attackReloadSeconds;
    }
    QJsonObject out{{k("accepted"), true}, {k("reason"), k("accepted")}, {k("time"), at}, {k("ready_at"), state.m_attackReadyAt}, {k("shot_number"), shot}};
    if (state.m_attackAmmoMaximum) { out[k("ammo_remaining")] = state.m_attackAmmo; out[k("reload_at")] = state.m_attackReloadAt; }
    *this = state; return out;
}

QJsonObject ContinuousActionState::transform(const QString &form, double at, const QJsonObject &facts) {
    ContinuousActionState state = *this;
    if (!state.advanceInPlace(at, m_alive)) return rejected(k("invalid_or_nonmonotonic_time"), at);
    if (!state.m_alive) return rejected(k("dead"), at);
    if (state.m_channelExpiresAt > at + eps) return rejected(k("channel_in_progress"), at);
    if (at + eps < state.m_castReadyAt) return rejected(k("cast_in_progress"), at);
    const auto forms = state.m_rules.value(k("forms")).toObject();
    if (!forms.contains(form)) return rejected(k("unknown_form"), at);
    if (form == state.m_form) return rejected(k("already_in_form"), at);
    const QString oldForm = state.m_form;
    QJsonObject transformCast;
    if (state.slotRule(k("R")).value(k("switches_to")).toString() == form) {
        // Nidalee/Jayce transformation is a real R cast with the one source
        // cooldown group shared by both forms, not a free condition switch.
        transformCast = state.cast(k("R"), at, state.m_stats, facts);
        if (!transformCast.value(k("accepted")).toBool(false)) return transformCast;
    } else {
        // Resource-triggered changes (for example Gnar) need an actual
        // observed transition. The ledger does not invent fury or eligibility.
        if (!facts.value(k("verified_observed_form_transition")).toBool(false)) return rejected(k("unverified_form_transition"), at);
        state.m_form = form;
    }
    *this = state;
    return {{k("accepted"), true}, {k("reason"), k("accepted")}, {k("time"), at}, {k("previous_form"), oldForm}, {k("form"), form}, {k("cooldowns_preserved"), true}, {k("transform_cast"), transformCast}};
}

bool ContinuousActionState::reduceCooldown(const QString &slot, double seconds, double at) {
    if (!number(seconds)) return false;
    ContinuousActionState state = *this;
    if (!state.advanceInPlace(at, m_alive) || state.slotRule(slot).isEmpty()) return false;
    auto &skill = state.m_skills[state.skillKey(slot)];
    skill.readyAt = std::max(at, skill.readyAt - seconds);
    *this = state; return true;
}

QJsonObject ContinuousActionState::applyCooldownRefunds(const QJsonArray &effects, double at) {
    auto staged = *this;
    if (!staged.advanceInPlace(at, staged.m_alive) || !staged.m_alive || effects.isEmpty())
        return rejected(k("cooldown_refund_requires_living_loaded_actor_and_effects"), at);
    QJsonArray receipts;
    for (const auto &raw : effects) {
        const auto effect = raw.toObject();
        const auto slot = effect.value(k("key")).toString();
        double amount = 0;
        if (effect.value(k("op")).toString() != k("refund") ||
            !QStringList{k("Q"),k("W"),k("E"),k("R")}.contains(slot) ||
            effect.value(k("target")).toString() != k("self") ||
            effect.value(k("source")).toString().isEmpty() ||
            !jsonNumber(effect.value(k("value")), &amount, 3600) ||
            !effect.value(k("start")).isDouble() || std::abs(effect.value(k("start")).toDouble()-at)>eps ||
            staged.slotRule(slot).isEmpty())
            return rejected(k("invalid_source_cooldown_refund"), at);
        const auto rule = staged.slotRule(slot);
        if (!rule.value(k("ammo")).toObject().isEmpty())
            return rejected(k("cooldown_refund_charge_recharge_policy_unverified"), at);
        const auto rankSlot = rule.value(k("rank_from_slot")).toString(slot);
        if (staged.m_context.ranks.value(rankSlot) <= 0) {
            receipts.append(QJsonObject{{k("slot"),slot},{k("applied"),false},{k("reason"),k("skill_unlearned")}});
            continue;
        }
        auto &skill = staged.m_skills[staged.skillKey(slot)];
        const double before = skill.readyAt, remaining = std::max(0.,before-at);
        skill.readyAt = std::max(at,before-amount);
        receipts.append(QJsonObject{{k("slot"),slot},{k("applied"),true},{k("source"),effect.value(k("source"))},
            {k("ready_at_before"),before},{k("ready_at_after"),skill.readyAt},
            {k("remaining_before"),remaining},{k("seconds_refunded"),std::min(remaining,amount)}});
    }
    *this = std::move(staged);
    return {{k("accepted"),true},{k("time"),at},{k("refunds"),receipts},
        {k("minimum_cooldown_inferred_from_configure"),false}};
}

bool ContinuousActionState::restoreResource(double amount, double at, bool verified) {
    if (!verified || !number(amount) || amount > m_resourceMaximum + eps) return false;
    ContinuousActionState state = *this;
    if (!state.advanceInPlace(at, m_alive)) return false;
    state.m_resource = amount; state.m_resourceKnown = true; *this = state; return true;
}

QJsonObject ContinuousActionState::payResource(double amount, double at, bool verified) {
    if (!verified) return rejected(k("verified_resource_payment_required"), at);
    if (!number(amount)) return rejected(k("invalid_resource_payment"), at);
    ContinuousActionState state = *this;
    if (!state.advanceInPlace(at, m_alive)) return rejected(k("invalid_or_nonmonotonic_resource_payment_time"), at);
    if (!state.m_alive) return rejected(k("dead"), at);
    if (!state.m_resourceKnown) return rejected(k("unknown_initial_resource"), at);
    if (amount > state.m_resource) return rejected(k("insufficient_resource"), at);
    const double before = state.m_resource;
    state.m_resource = std::max(0., state.m_resource - amount);
    if (state.m_upkeepActive && state.m_resource <= eps) state.stopUpkeep(at, k("resource_exhausted"));
    QJsonObject result{{k("accepted"), true}, {k("reason"), k("accepted")}, {k("time"), state.m_time},
        {k("resource_kind"), state.m_resourceKind}, {k("resource_before"), before},
        {k("paid"), amount}, {k("resource_cost"), amount}, {k("resource_remaining"), state.m_resource}};
    *this = state; return result;
}

QJsonObject ContinuousActionState::integrateResourceUpkeep(double rate, const QString &basis,
                                                          double from, double to, bool verified) {
    if (!number(rate)) return rejected(k("invalid_resource_upkeep_rate"), to);
    if (basis == k("flat")) return integrateResourceUpkeep(rate, 0., from, to, verified);
    if (basis == k("current_fraction")) return integrateResourceUpkeep(0., rate, from, to, verified);
    if (basis == k("max_fraction")) {
        const double flat = rate * m_resourceMaximum;
        if (!number(flat)) return rejected(k("invalid_resource_upkeep_rate"), to);
        return integrateResourceUpkeep(flat, 0., from, to, verified);
    }
    return rejected(k("unsupported_resource_upkeep_basis"), to);
}

QJsonObject ContinuousActionState::integrateResourceUpkeep(double flatRate, double currentFractionRate,
                                                          double from, double to, bool verified) {
    if (!verified) return rejected(k("verified_resource_upkeep_required"), to);
    if (!m_loaded || !number(flatRate) || !number(currentFractionRate) ||
        !number(from) || !number(to) || std::abs(from - m_time) > eps || to + eps < m_time)
        return rejected(k("invalid_resource_upkeep_interval_or_rate"), to);
    if (!m_alive) return rejected(k("dead"), to);
    if (!m_resourceKnown) return rejected(k("unknown_initial_resource"), to);
    // The legacy toggle owns its integration in advanceInPlace(). Combining it
    // here would pay twice or count regeneration twice. The caller must merge
    // source effects into one affine request, never chain independent calls.
    if (m_upkeepActive) return rejected(k("resource_upkeep_conflicts_with_legacy_toggle"), to);
    from = m_time; to = std::max(to, from);
    const double elapsed = to - from, before = m_resource;
    const double q = m_regen - flatRate, b = currentFractionRate;
    double active = elapsed, depletion = -1., capAt = -1.;
    if (b == 0.) {
        if (q < 0.) depletion = before / -q;
        else if (q > 0.) capAt = std::max(0., (m_resourceMaximum - before) / q);
    } else {
        // A current-fraction cost with nonnegative net inflow approaches zero
        // asymptotically; an epsilon must not invent a finite exhaustion time.
        if (q < 0.) depletion = logarithmicTravelTime(before, -q, b);
        if (q - b * m_resourceMaximum > 0.)
            capAt = logarithmicTravelTime(m_resourceMaximum - before, q - b * m_resourceMaximum, b);
        else if (before >= m_resourceMaximum && q - b * before >= 0.) capAt = 0.;
    }
    const bool exhausted = depletion >= 0. && depletion <= elapsed;
    if (exhausted) active = depletion;
    const double freeFlow = capAt >= 0. ? std::min(active, capAt) : active;
    double remaining = affineResource(before, q, b, freeFlow);
    remaining = std::clamp(remaining, 0., m_resourceMaximum);
    double paid = std::max(0., before + m_regen * freeFlow - remaining);
    if (capAt >= 0. && active >= capAt) {
        remaining = m_resourceMaximum;
        paid += (flatRate + b * m_resourceMaximum) * (active - capAt);
    }
    if (exhausted) {
        // Use conservation at the exact depletion boundary, then ordinary
        // regeneration after the owner stops this effect.
        paid = before + m_regen * active;
        remaining = std::min(m_resourceMaximum, m_regen * (elapsed - active));
    }
    if (!number(paid) || !number(remaining) || !number(from + active))
        return rejected(k("resource_upkeep_numeric_overflow"), to);
    ContinuousActionState state = *this;
    if (!state.advanceInPlace(to, m_alive)) return rejected(k("resource_upkeep_clock_advance_failed"), to);
    state.m_resource = remaining;
    QJsonObject result{{k("accepted"), true}, {k("reason"), k("accepted")}, {k("from"), from}, {k("to"), to},
        {k("resource_kind"), m_resourceKind}, {k("resource_before"), before}, {k("resource_remaining"), remaining},
        {k("paid"), paid}, {k("flat_per_second"), flatRate}, {k("current_fraction_per_second"), b},
        {k("active_until"), from + active}, {k("exhausted"), exhausted},
        {k("exhausted_at"), exhausted ? QJsonValue(from + active) : QJsonValue(QJsonValue::Null)}};
    *this = state; return result;
}

QSet<QString> ContinuousActionState::formConditions() const {
    QSet<QString> out;
    const auto conditions = m_rules.value(k("forms")).toObject().value(m_form).toObject().value(k("conditions")).toArray();
    for (const auto &condition : conditions) if (condition.isString()) out.insert(condition.toString());
    if (out.contains(k("jayce_hammer_form"))) out.insert(k("jayce_hammer"));
    if (out.contains(k("jayce_cannon_form"))) out.insert(k("jayce_cannon"));
    return out;
}

double ContinuousActionState::nextBoundary(double at, double fallback) const {
    if (!m_loaded || !number(at) || !number(fallback) || fallback < at) return fallback;
    double next = fallback;
    auto consider = [&](double candidate) { if (candidate > at + eps && candidate < next) next = candidate; };
    consider(m_attackReadyAt); consider(m_attackReloadAt); consider(m_castReadyAt);
    consider(m_channelExpiresAt);
    consider(m_toggleReadyAt);
    if (m_upkeepActive && m_alive && m_resourceKnown && m_upkeepRate > m_regen)
        consider(m_time + m_resource / (m_upkeepRate - m_regen));
        for (auto it = m_skills.begin(); it != m_skills.end(); ++it) { consider(it.value().readyAt); consider(it.value().rechargeAt); consider(it.value().tokenExpiresAt); consider(it.value().burstReadyAt); consider(it.value().burstExpiresAt); }
    return next;
}

void ContinuousActionState::stopUpkeep(double at, const QString &reason) {
    m_upkeepActive = false; m_upkeepStoppedAt = at; m_upkeepStopReason = reason;
}

QJsonObject ContinuousActionState::upkeepState() const {
    return {{k("active"), m_upkeepActive}, {k("slot"), m_upkeepSlot},
        {k("instance_id"), m_upkeepInstanceId}, {k("per_second"), m_upkeepRate},
        {k("started_at"), m_upkeepStartedAt}, {k("stopped_at"), m_upkeepStoppedAt},
        {k("toggle_ready_at"), m_toggleReadyAt}, {k("stop_reason"), m_upkeepStopReason},
        {k("resource_spent"), m_upkeepSpent},
        {k("integration_policy"), k("continuous_net_regen_explicit_scenario_not_server_payment_phase")}};
}

QJsonObject ContinuousActionState::toggle(const QString &slot, bool enabled, double at,
                                         const CombatStats &stats, const QJsonObject &facts) {
    ContinuousActionState state = *this;
    if (!validStats(stats) || !state.advanceInPlace(at, m_alive)) return rejected(k("invalid_toggle_time_or_stats"), at);
    if (!state.m_alive) return rejected(k("dead"), at);
    const auto rule = state.slotRule(slot), spec = rule.value(k("resource_upkeep_v5")).toObject();
    if (spec.isEmpty() || spec.value(k("kind")).toString() != k("mana_per_second_toggle") || state.m_resourceKind != k("mana"))
        return rejected(k("source_pinned_toggle_rule_required"), at);
    const int rank = state.m_context.ranks.value(rule.value(k("rank_from_slot")).toString(slot), 0);
    if (rank < 1) return rejected(k("unlearned_skill"), at);
    if (!facts.value(k("verified_toggle_transition")).toBool() ||
        !facts.value(k("verified_toggle_activation_eligibility")).toBool() ||
        !facts.value(k("verified_instant_toggle_transition")).toBool())
        return rejected(k("verified_toggle_transition_required"), at);
    if (enabled == state.m_upkeepActive) return rejected(k("toggle_state_unchanged"), at);
    if (!state.m_upkeepSlot.isEmpty() && state.m_upkeepSlot != slot) return rejected(k("unsupported_simultaneous_resource_toggle"), at);
    if (at + eps < state.m_toggleReadyAt) return rejected(k("toggle_cooldown"), at);
    double rate = 0, cooldown = 0;
    if (!ranked(spec, "per_second_by_rank", rank, &rate) || rate <= 0 ||
        !ranked(spec, "toggle_cooldown_by_rank", rank, &cooldown) || cooldown <= 0 || !number(at + cooldown))
        return rejected(k("unknown_toggle_rate_or_cooldown"), at);
    const auto id = facts.value(k("toggle_instance_id")).toString();
    if (enabled && (id.isEmpty() || id.size() > 256)) return rejected(k("explicit_toggle_instance_required"), at);
    if (enabled && (!state.m_resourceKnown || state.m_resource <= eps)) return rejected(k("insufficient_known_toggle_resource"), at);
    if (!enabled && facts.value(k("source_toggle_instance_id")).toString() != state.m_upkeepInstanceId)
        return rejected(k("toggle_source_instance_mismatch"), at);
    if (enabled && (state.m_channelExpiresAt > at + eps || state.m_castReadyAt > at + eps)) return rejected(k("cast_in_progress"), at);
    if (!resourceStats(state.m_rules, state.m_resourceKind, state.m_context, stats, &state.m_resourceMaximum, &state.m_regen))
        return rejected(k("invalid_toggle_resource_stats"), at);
    state.m_resource = std::min(state.m_resource, state.m_resourceMaximum); state.m_stats = stats;
    if (enabled && state.m_resource <= eps) return rejected(k("insufficient_known_toggle_resource"), at);
    state.m_upkeepSlot = slot; state.m_upkeepRate = rate; state.m_toggleReadyAt = at + cooldown;
    if (enabled) {
        state.m_upkeepActive = true; state.m_upkeepStartedAt = at; state.m_upkeepStoppedAt = -1;
        state.m_upkeepStopReason.clear(); state.m_upkeepInstanceId = id;
    } else state.stopUpkeep(at, k("explicit_toggle_off"));
    *this = state;
    return {{k("accepted"), true}, {k("reason"), k("accepted")}, {k("time"), at},
        {k("resource_cost"), 0}, {k("payload_emitted"), false}, {k("upkeep"), upkeepState()}};
}

QJsonObject ContinuousActionState::snapshot() const {
    QJsonObject skills;
    for (auto it = m_skills.begin(); it != m_skills.end(); ++it) skills[it.key()] = QJsonObject{
        {k("ready_at"), it.value().readyAt}, {k("charges"), it.value().charges}, {k("charges_known"), it.value().chargesKnown},
        {k("recharge_at"), it.value().rechargeAt}, {k("recharge_duration"), it.value().rechargeDuration},
        {k("recharge_awaiting_observation"), it.value().rechargeAwaitingObservation},
        {k("last_ammo_cost"), it.value().lastAmmoCost}, {k("last_ammo_cost_observed"), it.value().lastAmmoCostObserved},
        {k("recast_token_expires_at"), it.value().tokenExpiresAt}, {k("recast_token_target"), it.value().tokenTarget}, {k("recast_token_phase"), it.value().tokenPhase}, {k("last_cast_at"), it.value().lastCastAt},
        {k("burst_remaining"), it.value().burstRemaining}, {k("burst_emitted"), it.value().burstEmitted}, {k("burst_maximum"), it.value().burstMaximum},
        {k("burst_expires_at"), it.value().burstExpiresAt}, {k("burst_ready_at"), it.value().burstReadyAt},
        {k("burst_channelled"), it.value().burstChannelled}, {k("burst_instance_id"), it.value().burstInstanceId}};
    QJsonObject ranks;
    for (auto it = m_context.ranks.begin(); it != m_context.ranks.end(); ++it) ranks[it.key()] = it.value();
    const bool maximumKnown = sourceResourceMaximumKnown(m_rules, m_resourceKind);
    QJsonArray capacityUnknowns;
    if (!maximumKnown) capacityUnknowns.append(k("source_resource_maximum_unknown"));
    if (m_resourceMaximumObserved) capacityUnknowns.append(k("resource_maximum_is_verified_scenario_observation_not_source_repair"));
    return {{k("schema"), k("continuous_action_state_v3")}, {k("mechanics_version"), 7}, {k("rules_sha256"), signature(m_rules)},
        {k("champion_id"), m_context.championId},
        {k("context_level"), m_context.level}, {k("context_ranks"), ranks}, {k("resource_update_policy"), k("clamp_current")},
        {k("time"), m_time}, {k("alive"), m_alive}, {k("form"), m_form}, {k("resource_kind"), m_resourceKind},
        {k("resource_current"), m_resource}, {k("resource_maximum"), m_resourceMaximum}, {k("resource_known"), m_resourceKnown},
        {k("resource_maximum_source_known"), maximumKnown}, {k("resource_maximum_observed"), m_resourceMaximumObserved},
        {k("verified_resource_maximum"), m_resourceMaximumObserved}, {k("resource_capacity_unknowns"), capacityUnknowns},
        {k("resource_maximum_observation"), m_resourceMaximumObserved ? QJsonValue(QJsonObject{
            {k("maximum"), m_resourceMaximum}, {k("verified"), true}, {k("resource_kind"), m_resourceKind},
            {k("rules_sha256"), signature(m_rules)}, {k("source_capacity_known"), false}}) : QJsonValue(QJsonValue::Null)},
        {k("resource_maximum_origin"), m_resourceMaximumObserved ? k("verified_scenario_observation") : maximumKnown ? k("source_known") : k("source_unknown")},
        {k("regen_per_second"), m_regen}, {k("regen_while_dead"), m_regenWhileDead}, {k("skills"), skills},
        {k("attack_ready_at"), m_attackReadyAt}, {k("attack_reload_at"), m_attackReloadAt},
        {k("attack_ammo"), m_attackAmmo}, {k("cast_ready_at"), m_castReadyAt}, {k("loaded"), m_loaded},
        {k("channel_slot"), m_channelSlot}, {k("channel_instance_id"), m_channelInstanceId}, {k("channel_expires_at"), m_channelExpiresAt},
        {k("resource_upkeep"), upkeepState()}};
}

bool ContinuousActionState::restoreCheckpoint(const QJsonObject &checkpoint, double start, QString *error) {
    auto fail = [&](const char *why) { if (error) *error = k(why); return false; };
    if (checkpoint.value(k("schema")).toString() != k("continuous_action_state_v3") || checkpoint.value(k("rules_sha256")).toString() != signature(m_rules)) return fail("checkpoint_schema_or_rules_mismatch");
    if (!checkpoint.value(k("loaded")).toBool(false) || checkpoint.value(k("champion_id")).toInt(-1) != m_context.championId) return fail("checkpoint_actor_mismatch");
    const int mechanicsVersion = checkpoint.value(k("mechanics_version")).toInt(3);
    if (mechanicsVersion < 3 || mechanicsVersion > 7) return fail("checkpoint_unknown_mechanics_version");
    if (mechanicsVersion >= 4) {
        QJsonObject ranks;
        for (auto it = m_context.ranks.begin(); it != m_context.ranks.end(); ++it) ranks[it.key()] = it.value();
        if (checkpoint.value(k("context_level")).toInt(-1) != m_context.level || checkpoint.value(k("context_ranks")).toObject() != ranks ||
            checkpoint.value(k("resource_update_policy")).toString() != k("clamp_current")) return fail("checkpoint_progression_context_mismatch");
    }
    double at;
    if (!jsonNumber(checkpoint.value(k("time")), &at) || std::abs(at - start) > eps) return fail("checkpoint_start_mismatch");
    const QString form = checkpoint.value(k("form")).toString();
    const auto forms = m_rules.value(k("forms")).toObject();
    if ((!forms.isEmpty() && !forms.contains(form)) || (forms.isEmpty() && !form.isEmpty() && form != k("default"))) return fail("checkpoint_unknown_form");
    if (checkpoint.value(k("resource_kind")).toString() != m_resourceKind) return fail("checkpoint_resource_kind_mismatch");
    if (mechanicsVersion >= 6) {
        const bool maximumKnown = sourceResourceMaximumKnown(m_rules, m_resourceKind);
        double checkpointMaximum = 0;
        if (!checkpoint.value(k("resource_maximum_observed")).isBool() ||
            checkpoint.value(k("resource_maximum_source_known")) != QJsonValue(maximumKnown) ||
            !jsonNumber(checkpoint.value(k("resource_maximum")), &checkpointMaximum))
            return fail("invalid_checkpoint_resource_maximum_origin");
        const bool observed = checkpoint.value(k("resource_maximum_observed")).toBool();
        const auto expectedOrigin = observed ? k("verified_scenario_observation") : maximumKnown ? k("source_known") : k("source_unknown");
        if (checkpoint.value(k("resource_maximum_origin")).toString() != expectedOrigin ||
            checkpoint.value(k("verified_resource_maximum")) != QJsonValue(observed)) return fail("invalid_checkpoint_resource_maximum_origin");
        if (observed) {
            double observedMaximum = 0;
            const auto observation = checkpoint.value(k("resource_maximum_observation")).toObject();
            if (maximumKnown || m_resourceKind == k("mana") || m_resourceKind == k("none") ||
                !jsonNumber(checkpoint.value(k("resource_maximum")), &observedMaximum) || observedMaximum <= 0 ||
                observation.value(k("maximum")) != QJsonValue(observedMaximum) || observation.value(k("verified")) != QJsonValue(true) ||
                observation.value(k("resource_kind")).toString() != m_resourceKind ||
                observation.value(k("rules_sha256")).toString() != signature(m_rules) ||
                observation.value(k("source_capacity_known")) != QJsonValue(false) ||
                (m_resourceMaximumObserved && observedMaximum != m_resourceMaximum))
                return fail("checkpoint_resource_maximum_observation_mismatch");
            m_resourceMaximum = observedMaximum; m_resourceMaximumObserved = true;
        } else if (!checkpoint.value(k("resource_maximum_observation")).isNull() || m_resourceMaximumObserved || (!maximumKnown && checkpointMaximum != m_resourceMaximum))
            return fail("checkpoint_resource_maximum_observation_mismatch");
    } else if (m_resourceMaximumObserved) return fail("checkpoint_resource_maximum_observation_requires_version_six");
    double resource;
    if (!jsonNumber(checkpoint.value(k("resource_current")), &resource)) return fail("invalid_checkpoint_resource");
    if (m_resourceMaximumObserved && resource > m_resourceMaximum + eps) return fail("checkpoint_observed_resource_exceeds_capacity");
    auto timer = [&](const char *field, double *out, bool negativeOne) {
        const auto value = checkpoint.value(k(field));
        if (negativeOne && value.isDouble() && value.toDouble() == -1) { *out = -1; return true; }
        return jsonNumber(value, out);
    };
    if (!timer("attack_ready_at", &m_attackReadyAt, false) || !timer("attack_reload_at", &m_attackReloadAt, true) || !timer("cast_ready_at", &m_castReadyAt, false)) return fail("invalid_checkpoint_timer");
    if (mechanicsVersion >= 4) {
        if (!timer("channel_expires_at", &m_channelExpiresAt, true)) return fail("invalid_checkpoint_channel_timer");
        m_channelSlot = checkpoint.value(k("channel_slot")).toString(); m_channelInstanceId = checkpoint.value(k("channel_instance_id")).toString();
        if (m_channelInstanceId.size() > 256 || ((m_channelExpiresAt >= 0) != (!m_channelSlot.isEmpty() && !m_channelInstanceId.isEmpty())) ||
            (m_channelExpiresAt >= 0 && m_channelExpiresAt <= at + eps)) return fail("invalid_checkpoint_channel_state");
    }
    const auto skillStates = checkpoint.value(k("skills")).toObject();
    const auto sourceKeys = skillStates.keys(), destinationKeys = m_skills.keys();
    if (QSet<QString>(sourceKeys.begin(), sourceKeys.end()) != QSet<QString>(destinationKeys.begin(), destinationKeys.end())) return fail("checkpoint_skill_keys_mismatch");
    for (auto it = m_skills.begin(); it != m_skills.end(); ++it) {
        const auto source = skillStates.value(it.key()).toObject(); double charges = 0;
        if (!jsonNumber(source.value(k("ready_at")), &it.value().readyAt) || !jsonNumber(source.value(k("charges")), &charges, 10000) || std::floor(charges) != charges ||
            !jsonNumber(source.value(k("recharge_duration")), &it.value().rechargeDuration)) return fail("invalid_checkpoint_skill_state");
        const auto recharge = source.value(k("recharge_at"));
        if (recharge.isDouble() && recharge.toDouble() == -1) it.value().rechargeAt = -1;
        else if (!jsonNumber(recharge, &it.value().rechargeAt)) return fail("invalid_checkpoint_recharge_timer");
        if (it.value().rechargeAt >= 0 && it.value().rechargeDuration <= 0) return fail("invalid_checkpoint_recharge_duration");
        it.value().charges = int(charges); it.value().chargesKnown = source.value(k("charges_known")).toBool(false);
        if (mechanicsVersion >= 7) {
            double lastCost = 0;
            if (!source.value(k("recharge_awaiting_observation")).isBool() || !source.value(k("last_ammo_cost_observed")).isBool() ||
                !jsonNumber(source.value(k("last_ammo_cost")), &lastCost, 10000) || std::floor(lastCost) != lastCost)
                return fail("invalid_checkpoint_ammo_observation");
            it.value().rechargeAwaitingObservation = source.value(k("recharge_awaiting_observation")).toBool();
            it.value().lastAmmoCostObserved = source.value(k("last_ammo_cost_observed")).toBool();
            it.value().lastAmmoCost = int(lastCost);
        }
        const auto expires = source.value(k("recast_token_expires_at"));
        if (expires.isDouble() && expires.toDouble() == -1) it.value().tokenExpiresAt = -1;
        else if (!jsonNumber(expires, &it.value().tokenExpiresAt)) return fail("invalid_checkpoint_recast_expiry");
        it.value().tokenTarget = source.value(k("recast_token_target")).toString(); it.value().tokenPhase = source.value(k("recast_token_phase")).toString();
        const auto lastCast = source.value(k("last_cast_at"));
        if (lastCast.isDouble() && lastCast.toDouble() == -1) it.value().lastCastAt = -1;
        else if (!jsonNumber(lastCast, &it.value().lastCastAt) || it.value().lastCastAt > at + eps) return fail("invalid_checkpoint_last_cast_time");
        if (mechanicsVersion >= 4) {
            auto readCount = [&](const char *field, int *destination) {
                double value;
                if (!jsonNumber(source.value(k(field)), &value, 100) || std::floor(value) != value) return false;
                *destination = int(value); return true;
            };
            auto readTimer = [&](const char *field, double *destination) {
                const auto value = source.value(k(field));
                if (value.isDouble() && value.toDouble() == -1) { *destination = -1; return true; }
                return jsonNumber(value, destination);
            };
            auto &skill = it.value();
            if (!readCount("burst_remaining", &skill.burstRemaining) || !readCount("burst_emitted", &skill.burstEmitted) ||
                !readCount("burst_maximum", &skill.burstMaximum) || !readTimer("burst_expires_at", &skill.burstExpiresAt) ||
                !readTimer("burst_ready_at", &skill.burstReadyAt)) return fail("invalid_checkpoint_burst_fields");
            skill.burstChannelled = source.value(k("burst_channelled")).toBool(false);
            skill.burstInstanceId = source.value(k("burst_instance_id")).toString();
            const bool active = !skill.burstInstanceId.isEmpty();
            if (skill.burstInstanceId.size() > 256 || skill.burstRemaining + skill.burstEmitted > skill.burstMaximum ||
                (active && (skill.burstExpiresAt <= at + eps || skill.burstReadyAt < 0 || skill.burstMaximum < 1 ||
                    skill.burstRemaining + skill.burstEmitted != skill.burstMaximum)) ||
                (!active && (skill.burstRemaining != 0 || skill.burstExpiresAt != -1 || skill.burstReadyAt != -1 || skill.burstChannelled))) return fail("invalid_checkpoint_burst_state");
        }
    }
    double attackAmmo;
    const auto ammoValue = checkpoint.value(k("attack_ammo"));
    if (ammoValue.isDouble() && ammoValue.toDouble() == -1) attackAmmo = -1;
    else if (!jsonNumber(ammoValue, &attackAmmo, m_attackAmmoMaximum)) return fail("invalid_checkpoint_attack_ammo");
    if (std::floor(attackAmmo) != attackAmmo) return fail("invalid_checkpoint_attack_ammo");
    m_attackAmmo = int(attackAmmo); m_resource = std::min(resource, m_resourceMaximum);
    m_resourceKnown = checkpoint.value(k("resource_known")).toBool(false); m_form = form;
    m_time = at; m_alive = checkpoint.value(k("alive")).toBool(true);
    if (!m_alive && m_channelExpiresAt >= 0) return fail("checkpoint_dead_channel");
    const QStringList formNames = forms.isEmpty() ? QStringList{m_form} : forms.keys();
    QSet<QString> verifiedKeys;
    for (const auto &formName : formNames) {
        m_form = formName;
        QJsonObject slotRows = m_rules.value(k("slots")).toObject();
        const auto specific = forms.value(formName).toObject().value(k("slots")).toObject();
        for (auto it = specific.begin(); it != specific.end(); ++it) slotRows[it.key()] = it.value();
        for (auto it = slotRows.begin(); it != slotRows.end(); ++it) {
            const QString key = skillKey(it.key()); if (verifiedKeys.contains(key)) continue; verifiedKeys.insert(key);
            const auto &skill = m_skills[key]; const auto spec = it.value().toObject(); const auto ammo = spec.value(k("ammo")).toObject();
            if (!ammo.isEmpty()) {
                const int rank = m_context.ranks.value(spec.value(k("rank_from_slot")).toString(it.key()), 0);
                const int maximum = ammoMaximum(ammo, std::max(1, rank));
                if (rank < 1) {
                    if (skill.charges != 0 || skill.rechargeAt != -1 || skill.rechargeAwaitingObservation || skill.lastAmmoCost != 0 || skill.lastAmmoCostObserved) return fail("checkpoint_unlearned_skill_has_ammo");
                    continue;
                }
                const bool unknownRecharge = !ammo.value(k("recharge_known")).toBool(true);
                if (maximum < 1 || skill.charges > maximum ||
                    (!unknownRecharge && (skill.rechargeAwaitingObservation || (skill.chargesKnown && skill.charges < maximum && skill.rechargeAt < at - eps))) ||
                    (unknownRecharge && (skill.rechargeAt != -1 || skill.rechargeDuration != 0 || skill.rechargeAwaitingObservation != (skill.chargesKnown && skill.charges < maximum))) ||
                    (skill.chargesKnown && skill.charges >= maximum && skill.rechargeAt != -1)) return fail("checkpoint_charge_state_out_of_bounds");
                double sourceCost = 0;
                if (!ammoSourceCost(ammo, rank, &sourceCost) || skill.lastAmmoCost > maximum ||
                    (skill.lastAmmoCostObserved && (sourceCost > 0 || skill.lastAmmoCost < 1 || skill.lastCastAt < 0)) ||
                    (!skill.lastAmmoCostObserved && skill.lastAmmoCost > 0 && skill.lastAmmoCost != sourceCost) ||
                    (mechanicsVersion >= 7 && sourceCost <= 0 && skill.lastCastAt >= 0 && !skill.lastAmmoCostObserved))
                    return fail("checkpoint_ammo_cost_observation_mismatch");
            } else if (skill.charges || skill.chargesKnown || skill.rechargeAt != -1 || skill.rechargeAwaitingObservation || skill.lastAmmoCost != 0 || skill.lastAmmoCostObserved) return fail("checkpoint_non_ammo_charge_state");
            if (!skill.tokenPhase.isEmpty() && !spec.value(k("recasts")).toObject().contains(skill.tokenPhase)) return fail("checkpoint_unknown_recast_phase");
            if (!skill.burstInstanceId.isEmpty()) {
                const auto burst = spec.value(k("burst")).toObject();
                if (burst.isEmpty()) return fail("checkpoint_unknown_burst");
                const bool sourceChannelled = burst.value(k("activation_cost_only")).toBool(false);
                if (sourceChannelled != skill.burstChannelled || !m_alive) return fail("checkpoint_invalid_burst_kind");
                if (skill.burstChannelled && (m_channelSlot != it.key() || m_channelInstanceId != skill.burstInstanceId ||
                    std::abs(m_channelExpiresAt - skill.burstExpiresAt) > eps || skill.burstRemaining < 1)) return fail("checkpoint_burst_channel_mismatch");
                double sourceMaximum = 0;
                const int rank = m_context.ranks.value(spec.value(k("rank_from_slot")).toString(it.key()), 0);
                if (rank < 1 || (!jsonNumber(burst.value(k("max_casts")), &sourceMaximum, 100) && !ranked(burst, "max_casts_by_rank", rank, &sourceMaximum)) ||
                    skill.burstMaximum > sourceMaximum) return fail("checkpoint_burst_count_out_of_bounds");
            }
        }
    }
    m_form = form;
    if (m_channelExpiresAt >= 0 && (slotRule(m_channelSlot).isEmpty() || m_context.ranks.value(slotRule(m_channelSlot).value(k("rank_from_slot")).toString(m_channelSlot), 0) < 1)) return fail("checkpoint_unknown_channel_slot");
    if (mechanicsVersion >= 5) {
        if (!checkpoint.value(k("resource_upkeep")).isObject()) return fail("checkpoint_missing_resource_upkeep");
        const auto upkeep = checkpoint.value(k("resource_upkeep")).toObject();
        if (!upkeep.value(k("active")).isBool() || !jsonNumber(upkeep.value(k("per_second")), &m_upkeepRate) ||
            !jsonNumber(upkeep.value(k("toggle_ready_at")), &m_toggleReadyAt) ||
            !jsonNumber(upkeep.value(k("resource_spent")), &m_upkeepSpent)) return fail("invalid_checkpoint_resource_upkeep");
        auto pastTimer = [&](const char *field, double *out) {
            const auto value = upkeep.value(k(field));
            if (value.isDouble() && value.toDouble() == -1) { *out = -1; return true; }
            return jsonNumber(value, out) && *out <= at + eps;
        };
        if (!pastTimer("started_at", &m_upkeepStartedAt) || !pastTimer("stopped_at", &m_upkeepStoppedAt)) return fail("invalid_checkpoint_upkeep_time");
        m_upkeepActive = upkeep.value(k("active")).toBool(); m_upkeepSlot = upkeep.value(k("slot")).toString();
        m_upkeepInstanceId = upkeep.value(k("instance_id")).toString(); m_upkeepStopReason = upkeep.value(k("stop_reason")).toString();
        if (upkeep.value(k("integration_policy")).toString() != k("continuous_net_regen_explicit_scenario_not_server_payment_phase")) return fail("checkpoint_upkeep_policy_mismatch");
        if (m_upkeepSlot.isEmpty()) {
            if (m_upkeepActive || !m_upkeepInstanceId.isEmpty() || m_upkeepRate || m_upkeepSpent || m_toggleReadyAt ||
                m_upkeepStartedAt != -1 || m_upkeepStoppedAt != -1 || !m_upkeepStopReason.isEmpty()) return fail("checkpoint_spurious_upkeep");
        } else {
            const auto spec = slotRule(m_upkeepSlot).value(k("resource_upkeep_v5")).toObject();
            double rate = 0;
            if (spec.value(k("kind")).toString() != k("mana_per_second_toggle") || m_resourceKind != k("mana") ||
                !ranked(spec, "per_second_by_rank", m_context.ranks.value(m_upkeepSlot), &rate) || rate <= 0 ||
                std::abs(rate - m_upkeepRate) > eps || m_upkeepInstanceId.isEmpty() || m_upkeepInstanceId.size() > 256 ||
                m_upkeepStartedAt < 0) return fail("checkpoint_upkeep_source_mismatch");
            if (m_upkeepActive) {
                if (!m_alive || !m_resourceKnown || m_resource <= eps || m_upkeepStoppedAt != -1 || !m_upkeepStopReason.isEmpty()) return fail("checkpoint_invalid_active_upkeep");
            } else if (m_upkeepStoppedAt < m_upkeepStartedAt || !QStringList{k("explicit_toggle_off"),k("resource_exhausted"),k("source_death")}.contains(m_upkeepStopReason))
                return fail("checkpoint_invalid_stopped_upkeep");
        }
    }
    return true;
}
