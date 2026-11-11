#include "continuous_control_state.h"
#include <algorithm>
#include <cmath>

namespace {
QString k(const char *s) { return QString::fromLatin1(s); }
constexpr double maximumTime = 86400;
constexpr double maximumDuration = 3600;
constexpr int maximumEvents = 8192;
bool bounded(double value, double minimum, double maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}
bool number(const QJsonValue &value, double minimum, double maximum) {
    return value.isDouble() && bounded(value.toDouble(), minimum, maximum);
}
bool identifier(const QJsonValue &value) {
    return value.isString() && !value.toString().trimmed().isEmpty() && value.toString().size() <= 256;
}
bool blocksCast(const QString &type) {
    return type == k("stun") || type == k("knockup") || type == k("airborne") || type == k("knockback") || type == k("suppression") || type == k("silence") || type == k("charm") || type == k("fear") || type == k("taunt");
}
bool blocksAttack(const QString &type) {
    return type == k("stun") || type == k("knockup") || type == k("airborne") || type == k("knockback") || type == k("suppression") || type == k("disarm") || type == k("charm") || type == k("fear");
}
bool blocksMove(const QString &type) {
    return type == k("stun") || type == k("knockup") || type == k("airborne") || type == k("knockback") || type == k("suppression") || type == k("root") || type == k("charm") || type == k("fear") || type == k("taunt");
}
bool hasTenacity(const QString &type) {
    return type != k("knockup") && type != k("airborne") && type != k("knockback") && type != k("suppression");
}
const QSet<QString> &supportedTypes() {
    static const QSet<QString> result{k("stun"), k("knockup"), k("airborne"), k("knockback"), k("suppression"), k("silence"), k("disarm"), k("root"), k("slow"), k("charm"), k("fear"), k("taunt"), k("nearsight")};
    return result;
}
}

bool ContinuousControlState::apply(const QJsonObject &event, const CombatStats &stats, QString *error) {
    const auto fail = [&](const char *reason) { if (error) *error = k(reason); return false; };
    if (!identifier(event.value(k("event_id"))) || !identifier(event.value(k("source"))) || !identifier(event.value(k("target")))) return fail("invalid_control_event_identity");
    const auto id = event.value(k("event_id")).toString();
    const auto target = event.value(k("target")).toString();
    if (eventIds_.contains(id)) return fail("duplicate_control_event_id");
    if (entries_.size() >= maximumEvents) return fail("control_event_limit_exceeded");
    if (!target_.isEmpty() && target_ != target) return fail("control_target_does_not_match_actor");
    if (!event.value(k("type")).isString()) return fail("invalid_control_type");
    const auto type = event.value(k("type")).toString();
    if (!supportedTypes().contains(type)) return fail("unsupported_control_type");
    if (type == k("taunt") && event.value(k("source")).toString() == target) return fail("taunt_source_matches_target");
    if (!number(event.value(k("time")), 0, maximumTime) || !number(event.value(k("duration")), 0.000001, maximumDuration)) return fail("invalid_control_time_or_duration");
    const double start = event.value(k("time")).toDouble(), duration = event.value(k("duration")).toDouble();
    if (start < time_) return fail("control_event_before_current_time");
    if (start + duration > maximumTime) return fail("control_end_time_exceeds_limit");
    const double tenacity = stats[Stat::Tenacity], slowResist = stats[Stat::SlowResist];
    if (!bounded(tenacity, 0, 1) || !bounded(slowResist, 0, 1)) return fail("invalid_control_resistance_stat");
    double floor = 0;
    if (event.contains(k("duration_floor_seconds"))) {
        if (!number(event.value(k("duration_floor_seconds")), 0, maximumDuration)) return fail("invalid_control_duration_floor");
        floor = event.value(k("duration_floor_seconds")).toDouble();
    }
    double slowFraction = 0;
    if (type == k("slow")) {
        if (!number(event.value(k("slow_fraction")), 0, 1)) return fail("missing_or_invalid_slow_fraction");
        slowFraction = event.value(k("slow_fraction")).toDouble() * (1 - slowResist);
    } else if (event.contains(k("slow_fraction"))) return fail("slow_fraction_on_non_slow_control");
    Entry entry; entry.event = event; entry.id = id; entry.target = target; entry.source = event.value(k("source")).toString(); entry.type = type;
    entry.start = start; entry.originalDuration = duration; entry.tenacity = tenacity; entry.slowResist = slowResist;
    entry.tenacityApplied = hasTenacity(type); entry.slowFraction = slowFraction;
    // The floor may prevent resistance shortening, but never lengthens the
    // original control. Airborne and suppression ignore both tenacity/floor.
    const double effectiveDuration = entry.tenacityApplied ? std::min(duration, std::max(floor, duration * (1 - tenacity))) : duration;
    entry.end = start + effectiveDuration;
    // Validation above is transactional: a rejected event never consumes an ID.
    entries_.append(entry); eventIds_.insert(id); target_ = target;
    if (error) error->clear();
    return true;
}

bool ContinuousControlState::advanceTo(double time, QString *error) {
    if (!bounded(time, 0, maximumTime) || time < time_) {
        if (error) *error = k("non_monotonic_or_invalid_control_time");
        return false;
    }
    time_ = time; if (error) error->clear(); return true;
}

double ContinuousControlState::effectiveEnd(const Entry &entry) const { return entry.deathAt < 0 ? entry.end : std::min(entry.end, entry.deathAt); }
bool ContinuousControlState::active(const Entry &entry, double time) const { return time >= entry.start && time < effectiveEnd(entry); }

bool ContinuousControlState::notifyActorDeath(const QString &actor, double time, bool verified, QString *error) {
    const auto fail = [&](const char *reason) { if (error) *error = k(reason); return false; };
    if (!verified) return fail("unverified_control_actor_death");
    if (!identifier(QJsonValue(actor)) || !bounded(time, 0, maximumTime) || time < time_)
        return fail("invalid_or_nonmonotonic_control_actor_death");
    for (auto &entry : entries_) if (effectiveEnd(entry) > time &&
        (entry.target == actor || (entry.type == k("taunt") && entry.source == actor))) {
        entry.deathAt = time; entry.deadActor = actor;
    }
    time_ = time; if (error) error->clear(); return true;
}

bool ContinuousControlState::canCast(double time) const {
    if (!bounded(time, 0, maximumTime)) return false;
    for (const auto &entry : entries_) if (active(entry, time) && blocksCast(entry.type)) return false;
    return true;
}

bool ContinuousControlState::canAttack(double time) const {
    if (!bounded(time, 0, maximumTime)) return false;
    for (const auto &entry : entries_) if (active(entry, time) && blocksAttack(entry.type)) return false;
    return true;
}

QString ContinuousControlState::forcedAttackTarget(double time) const {
    if (!bounded(time, 0, maximumTime)) return {};
    const Entry *selected = nullptr;
    // Explicit deterministic model priority; this is not an observed claim
    // about the server's tie-breaking order for simultaneous taunts.
    for (const auto &entry : entries_) if (entry.type == k("taunt") && active(entry, time) &&
        (!selected || entry.start > selected->start || (entry.start == selected->start && entry.id > selected->id))) selected = &entry;
    return selected ? selected->source : QString();
}

bool ContinuousControlState::canAttackTarget(double time, const QString &target) const {
    if (!identifier(QJsonValue(target)) || !canAttack(time)) return false;
    const auto forced = forcedAttackTarget(time);
    return forced.isEmpty() || forced == target;
}

bool ContinuousControlState::canMove(double time) const {
    if (!bounded(time, 0, maximumTime)) return false;
    for (const auto &entry : entries_) if (active(entry, time) && blocksMove(entry.type)) return false;
    return true;
}

bool ContinuousControlState::interruptsCast(double time) const {
    if (!bounded(time, 0, maximumTime)) return false;
    // This is the ordinary interruptible-cast/channel contract. An ability
    // declared uninterruptible by verified cast data must be handled by caller.
    for (const auto &entry : entries_) if (active(entry, time) && blocksCast(entry.type)) return true;
    return false;
}

bool ContinuousControlState::nearsighted(double time) const {
    if (!bounded(time, 0, maximumTime)) return false;
    for (const auto &entry : entries_) if (entry.type == k("nearsight") && active(entry, time)) return true;
    return false;
}

double ContinuousControlState::movementMultiplier(double time) const {
    if (!canMove(time)) return 0;
    double strongest = 0;
    for (const auto &entry : entries_) if (active(entry, time) && entry.type == k("slow")) strongest = std::max(strongest, entry.slowFraction);
    return 1 - strongest;
}

double ContinuousControlState::nextBoundary(double at, double fallback) const {
    if (!bounded(at, 0, maximumTime) || !bounded(fallback, at, maximumTime)) return at;
    double next = fallback;
    for (const auto &entry : entries_) {
        const double end = effectiveEnd(entry);
        if (end <= entry.start) continue;
        if (entry.start > at) next = std::min(next, entry.start);
        if (end > at) next = std::min(next, end);
    }
    return next;
}

double ContinuousControlState::nextExpiry(double at, double fallback) const {
    if (!bounded(at, 0, maximumTime) || !bounded(fallback, at, maximumTime)) return at;
    double next = fallback;
    for (const auto &entry : entries_) if (effectiveEnd(entry) > at && effectiveEnd(entry) > entry.start) next = std::min(next, effectiveEnd(entry));
    return next;
}

QJsonObject ContinuousControlState::snapshot(double at) const {
    if (!bounded(at, 0, maximumTime)) return {{k("valid"), false}, {k("error"), k("invalid_control_snapshot_time")}};
    QJsonArray activeEntries;
    for (const auto &entry : entries_) if (active(entry, at)) {
        activeEntries.append(QJsonObject{{k("event_id"), entry.id}, {k("source"), entry.source}, {k("target"), entry.target}, {k("type"), entry.type},
            {k("start"), entry.start}, {k("end"), effectiveEnd(entry)}, {k("original_duration"), entry.originalDuration}, {k("effective_duration"), effectiveEnd(entry) - entry.start},
            {k("tenacity_applied"), entry.tenacityApplied}, {k("tenacity"), entry.tenacity}, {k("slow_resist"), entry.slowResist}, {k("effective_slow_fraction"), entry.slowFraction}});
    }
    auto result = checkpoint();
    const QJsonObject view{{k("valid"), true}, {k("time"), at}, {k("target"), target_}, {k("can_cast"), canCast(at)}, {k("can_attack"), canAttack(at)}, {k("can_move"), canMove(at)},
        {k("interrupts_cast"), interruptsCast(at)}, {k("movement_multiplier"), movementMultiplier(at)}, {k("active_controls"), activeEntries},
        {k("forced_attack_target"), forcedAttackTarget(at)}, {k("forced_attack_target_requires_world_eligibility"), true},
        {k("taunt_priority_contract"), k("latest_active_start_then_lexicographically_greatest_event_id_model_assumption")},
        {k("taunt_priority_server_verified"), false}, {k("taunt_death_contract"), k("verified_target_death_clears_existing_controls_source_death_ends_its_existing_taunts")},
        {k("nearsighted"), nearsighted(at)}, {k("visibility_contract"), k("external_target_visibility_observation_required_nearsight_does_not_imply_all_targets_hidden")},
        {k("duration_floor_default_seconds"), 0}, {k("duration_floor_contract"), k("explicit_scenario_floor_default_zero_not_inferred_game_minimum")},
        {k("overlap_contract"), k("union_for_action_locks_strongest_slow_no_duration_or_magnitude_addition")},
        {k("forced_movement_simulated"), false}, {k("displacement_simulated"), false}, {k("cast_interrupt_contract"), k("ordinary_interruptible_cast_or_channel_only")}};
    for (auto it = view.begin(); it != view.end(); ++it) result.insert(it.key(), it.value());
    return result;
}

QJsonObject ContinuousControlState::checkpoint() const {
    QJsonArray records;
    for (const auto &entry : entries_) {
        QJsonObject row{{k("event"), entry.event}, {k("tenacity"), entry.tenacity}, {k("slow_resist"), entry.slowResist}};
        if (entry.deathAt >= 0) row[k("actor_death")] = QJsonObject{{k("actor"), entry.deadActor}, {k("time"), entry.deathAt}, {k("verified"), true}};
        records.append(row);
    }
    return {{k("schema"), k("continuous_control_v4")}, {k("time"), time_}, {k("target"), target_}, {k("records"), records}};
}

bool ContinuousControlState::restore(const QJsonObject &checkpoint, QString *error) {
    const auto fail = [&](const char *reason) { if (error) *error = k(reason); return false; };
    const auto schema = checkpoint.value(k("schema")).toString();
    if ((schema != k("continuous_control_v3") && schema != k("continuous_control_v4")) || !number(checkpoint.value(k("time")), 0, maximumTime) || !checkpoint.value(k("records")).isArray()) return fail("invalid_control_checkpoint");
    if (!checkpoint.value(k("target")).isString()) return fail("invalid_control_checkpoint_target");
    const auto records = checkpoint.value(k("records")).toArray();
    if (records.size() > maximumEvents) return fail("control_event_limit_exceeded");
    ContinuousControlState candidate;
    for (const auto &record : records) {
        if (!record.isObject()) return fail("invalid_control_checkpoint_record");
        const auto row = record.toObject();
        if (!row.value(k("event")).isObject() || !number(row.value(k("tenacity")), 0, 1) || !number(row.value(k("slow_resist")), 0, 1)) return fail("invalid_control_checkpoint_record");
        CombatStats stats; stats[Stat::Tenacity] = row.value(k("tenacity")).toDouble(); stats[Stat::SlowResist] = row.value(k("slow_resist")).toDouble();
        QString reason;
        if (!candidate.apply(row.value(k("event")).toObject(), stats, &reason)) { if (error) *error = k("invalid_control_checkpoint_event:") + reason; return false; }
        if (row.contains(k("actor_death"))) {
            if (schema != k("continuous_control_v4") || !row.value(k("actor_death")).isObject()) return fail("invalid_control_checkpoint_actor_death");
            const auto death = row.value(k("actor_death")).toObject();
            if (!identifier(death.value(k("actor"))) || !number(death.value(k("time")), 0, checkpoint.value(k("time")).toDouble()) ||
                death.value(k("verified")) != QJsonValue(true)) return fail("invalid_control_checkpoint_actor_death");
            auto &entry = candidate.entries_.last();
            const auto actor = death.value(k("actor")).toString(); const double at = death.value(k("time")).toDouble();
            if (at >= entry.end || (actor != entry.target && (entry.type != k("taunt") || actor != entry.source)))
                return fail("invalid_control_checkpoint_actor_death_scope");
            entry.deathAt = at; entry.deadActor = actor;
        }
    }
    if (candidate.target_ != checkpoint.value(k("target")).toString()) return fail("control_checkpoint_target_mismatch");
    candidate.time_ = checkpoint.value(k("time")).toDouble();
    *this = candidate; if (error) error->clear(); return true;
}
