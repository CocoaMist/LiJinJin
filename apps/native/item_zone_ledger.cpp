#include "item_zone_ledger.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>

namespace {
QString k(const char *s) { return QString::fromLatin1(s); }
constexpr double eps = 1e-8;
bool finite(double n, double lo = 0, double hi = 1e12) { return std::isfinite(n) && n >= lo && n <= hi; }
bool fail(QString *error, const char *reason) { if (error) *error = k(reason); return false; }
QString sha(const QJsonObject &o) { return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(o).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex()); }
QString identity(const QString &a, const QString &b) { return sha({{k("a"), a}, {k("b"), b}}); }
QJsonArray sorted(const QSet<QString> &s) { auto values = s.values(); values.sort(); return QJsonArray::fromStringList(values); }
QJsonObject constant(double n) { return {{k("op"), k("const")}, {k("value"), n}}; }
QJsonObject add(const QJsonArray &a) { return {{k("op"), k("add")}, {k("args"), a}}; }
QJsonObject mul(const QJsonArray &a) { return {{k("op"), k("mul")}, {k("args"), a}}; }
QJsonObject receiptJson(const ItemZoneLedger::UltimateDamageReceipt &r) {
    return {{k("receipt_id"), r.receiptId}, {k("owner_key"), r.ownerKey}, {k("target_key"), r.targetKey},
        {k("cast_instance_id"), r.castInstanceId}, {k("source_slot"), r.sourceSlot}, {k("time"), r.time},
        {k("actual_damage"), r.actualDamage}, {k("origin"), int(r.origin)}, {k("owner_currently_has_item"), r.ownerCurrentlyHasItem}};
}
}

QString ItemZoneLedger::zoneIdForReceipt(const QString &owner, const QString &receiptId) {
    return k("item3118:") + identity(owner, receiptId);
}

bool ItemZoneLedger::load(const CombatDatabase &db, const QVector<Participant> &participants, double start, QString *error) {
    if (!finite(start) || db.patch != k("16.21") || db.revision.isEmpty() || !db.items.contains(3118))
        return fail(error, "item_zone_source_scope_unverified");
    const auto &item = db.items[3118];
    // Exact reviewed compiled projection. A later patch or formula change must
    // receive a new admission; matching a few sampled outputs is insufficient.
    const double ratio = 0.02800000086426735;
    const QJsonObject dps = add({constant(40), mul({QJsonObject{{k("op"), k("stat")}, {k("stat"), k("ap")}, {k("part"), 0}}, constant(ratio)})});
    const QJsonArray existingEffects{QJsonObject{{k("op"), k("add")}, {k("stat"), k("r_haste")}, {k("value"), k("UltimateHaste")}}};
    if (item.id != 3118 || item.values.value(k("BaseDamage")) != 40 ||
        item.values.value(k("APRatio")) != ratio || item.values.value(k("GroundDuration")) != 3 ||
        item.calculations.value(k("GroundBurnDamagePerTickTooltipOnly")) != dps ||
        item.calculations.value(k("MagicResistanceShred")) != add({constant(10)}) ||
        item.calculations.value(k("{8e8f7a34}")) != mul({dps, constant(.25)}) ||
        item.effects != existingEffects || !item.temporalBindingsV4.isEmpty() ||
        !item.actions.isEmpty() || !item.eventRules.isEmpty())
        return fail(error, "item_zone_source_projection_changed");
    ItemZoneLedger fresh;
    QJsonArray roster;
    for (const auto &p : participants) {
        if (p.key.isEmpty() || p.team.isEmpty() || fresh.participants_.contains(p.key))
            return fail(error, "item_zone_invalid_participant");
        fresh.participants_[p.key] = p;
    }
    if (fresh.participants_.isEmpty()) return fail(error, "item_zone_empty_roster");
    for (const auto &p : fresh.participants_) roster.append(QJsonObject{{k("key"), p.key}, {k("team"), p.team}});
    fresh.sourceHash_ = sha({{k("item_id"), 3118}, {k("values"), item.values}, {k("calculations"), item.calculations},
        {k("effects"), item.effects}, {k("temporal_bindings_v4"), item.temporalBindingsV4},
        {k("actions"), item.actions}, {k("event_rules"), item.eventRules}});
    fresh.rosterHash_ = sha({{k("participants"), roster}});
    fresh.revision_ = db.revision; fresh.patch_ = db.patch;
    fresh.baseDps_ = 40; fresh.apRatio_ = ratio; fresh.duration_ = 3; fresh.mrReduction_ = 10;
    fresh.start_ = fresh.time_ = start; fresh.loaded_ = true;
    *this = fresh;
    return true;
}

ItemZoneLedger::TriggerResult ItemZoneLedger::trigger(const UltimateDamageReceipt &r) {
    return triggerInPlace(r, true);
}

ItemZoneLedger::TriggerResult ItemZoneLedger::triggerInPlace(const UltimateDamageReceipt &r, bool record) {
    TriggerResult out;
    auto reject = [&](const char *reason) { out.reason = k(reason); return out; };
    if (!loaded_ || !finite(r.time) || r.time != time_) return reject("item_zone_trigger_time_mismatch");
    if (r.receiptId.isEmpty() || r.castInstanceId.isEmpty() || r.sourceSlot != k("R") ||
        r.origin != Origin::ChampionUltimate || !finite(r.actualDamage) || r.actualDamage <= 0)
        return reject("item_zone_not_qualifying_ultimate_damage");
    if (!participants_.contains(r.ownerKey) || !participants_.contains(r.targetKey) ||
        participants_[r.ownerKey].team == participants_[r.targetKey].team)
        return reject("item_zone_ultimate_target_not_enemy_champion");
    const auto receiptKey = identity(r.ownerKey, r.receiptId);
    if (damageReceipts_.contains(receiptKey)) return reject("item_zone_duplicate_damage_receipt");
    // Consume a genuinely qualifying receipt even when creation is on cooldown.
    // Replaying that same receipt later may never evade the original ICD.
    damageReceipts_.insert(receiptKey);
    out.opportunityOnly = !r.ownerCurrentlyHasItem;
    const auto targetKey = identity(r.ownerKey, r.targetKey) + (r.ownerCurrentlyHasItem ? k(":active") : k(":opportunity"));
    for (auto it = zones_.begin(); it != zones_.end(); ++it) {
        auto &zone = it.value();
        if (zone.owner == r.ownerKey && zone.activeItem == r.ownerCurrentlyHasItem && zone.expires > time_ && zone.contacts.contains(r.targetKey)) {
            zone.expires = std::max(zone.expires, r.time + duration_);
            zone.refreshReceipts.append(r.receiptId);
            out.accepted = out.refreshed = true; out.zoneId = zone.id; out.expiresAt = zone.expires;
            break;
        }
    }
    if (!out.accepted) {
        if (r.time + eps < creationReady_.value(targetKey, -1)) out.reason = k("item_zone_per_target_creation_icd");
        else {
            Zone zone; zone.id = zoneIdForReceipt(r.ownerKey, r.receiptId); zone.owner = r.ownerKey;
            zone.triggerTarget = r.targetKey; zone.triggerReceipt = r.receiptId; zone.cast = r.castInstanceId;
            zone.activeItem = r.ownerCurrentlyHasItem;
            zone.start = r.time; zone.expires = r.time + duration_;
            zones_[zone.id] = zone; creationReady_[targetKey] = zone.expires;
            out.accepted = out.created = true; out.zoneId = zone.id; out.expiresAt = zone.expires;
        }
    }
    if (record) journal_.append(QJsonObject{{k("op"), k("trigger")}, {k("receipt"), receiptJson(r)}});
    return out;
}

bool ItemZoneLedger::setContact(const Contact &c, QString *error) {
    return contactInPlace(c, true, error);
}

bool ItemZoneLedger::contactInPlace(const Contact &c, bool record, QString *error) {
    if (!loaded_ || !finite(c.time) || c.time != time_) return fail(error, "item_zone_contact_time_mismatch");
    if (c.eventId.isEmpty() || contactEvents_.contains(c.eventId)) return fail(error, "item_zone_duplicate_or_empty_contact_event");
    if (!zones_.contains(c.zoneId) || zones_[c.zoneId].owner != c.ownerKey)
        return fail(error, "item_zone_contact_not_owned");
    auto &zone = zones_[c.zoneId];
    if (!participants_.contains(c.targetKey) || participants_[c.targetKey].team == participants_[c.ownerKey].team)
        return fail(error, "item_zone_contact_target_not_enemy_champion");
    if (zone.expires <= c.time) return fail(error, "item_zone_contact_expired");
    if (c.inside) {
        for (const auto &other : zones_) if (zone.activeItem && other.activeItem && other.owner != c.ownerKey && other.expires > c.time && other.contacts.contains(c.targetKey))
            return fail(error, "item_zone_cross_owner_curse_overlap_unverified");
        if (zone.contacts.contains(c.targetKey)) return fail(error, "item_zone_contact_already_inside");
        zone.contacts.insert(c.targetKey);
    } else {
        if (!zone.contacts.contains(c.targetKey)) return fail(error, "item_zone_contact_not_inside");
        zone.contacts.remove(c.targetKey);
    }
    contactEvents_.insert(c.eventId);
    if (record) journal_.append(QJsonObject{{k("op"), k("contact")}, {k("event_id"), c.eventId},
        {k("owner_key"), c.ownerKey}, {k("zone_id"), c.zoneId}, {k("target_key"), c.targetKey},
        {k("time"), c.time}, {k("inside"), c.inside}});
    return true;
}

bool ItemZoneLedger::clearTargetContacts(const QString &target, double at, QString *error) {
    return clearInPlace(target, at, true, error);
}

bool ItemZoneLedger::clearInPlace(const QString &target, double at, bool record, QString *error) {
    if (!loaded_ || !finite(at) || at != time_ || !participants_.contains(target))
        return fail(error, "item_zone_clear_target_invalid");
    for (auto &zone : zones_) zone.contacts.remove(target);
    if (record) journal_.append(QJsonObject{{k("op"), k("clear_target")}, {k("target_key"), target}, {k("time"), at}});
    return true;
}

bool ItemZoneLedger::advance(double until, const QMap<QString, double> &ap, QVector<DamageInterval> *intervals, QString *error) {
    auto staged = *this; QVector<DamageInterval> result;
    if (!staged.advanceInPlace(until, ap, &result, true, error)) return false;
    *this = staged;
    if (intervals) *intervals = result;
    return true;
}

bool ItemZoneLedger::advanceInPlace(double until, const QMap<QString, double> &ap, QVector<DamageInterval> *intervals, bool record, QString *error) {
    if (!loaded_ || !finite(until) || until < time_) return fail(error, "item_zone_nonmonotonic_time");
    for (auto it = ap.begin(); it != ap.end(); ++it) if (!participants_.contains(it.key()) || !finite(it.value(), -1e9, 1e9))
        return fail(error, "item_zone_invalid_owner_ap");
    QVector<DamageInterval> result;
    auto at = time_;
    while (at < until) {
        double next = until;
        for (const auto &zone : zones_) if (zone.expires > at) next = std::min(next, zone.expires);
        QMap<QString, DamageInterval> active;
        for (const auto &zone : zones_) if (zone.activeItem && zone.expires > at && !zone.contacts.isEmpty()) {
            if (!ap.contains(zone.owner)) return fail(error, "item_zone_owner_ap_required");
            for (const auto &target : zone.contacts) {
                const auto key = identity(zone.owner, target);
                auto &span = active[key]; span.ownerKey = zone.owner; span.targetKey = target;
                span.zoneIds.append(zone.id); span.from = at; span.until = next; span.flatMrReduction = mrReduction_;
            }
        }
        for (auto span : active) {
            span.zoneIds.sort();
            const double dps = baseDps_ + apRatio_ * ap.value(span.ownerKey);
            if (!finite(dps)) return fail(error, "item_zone_invalid_damage_rate");
            span.rawDamage = dps * (next - at); result.append(span);
        }
        at = next;
    }
    time_ = until;
    for (auto &zone : zones_) if (zone.expires <= time_) zone.contacts.clear();
    if (record) {
        QJsonObject values; for (auto it = ap.begin(); it != ap.end(); ++it) values[it.key()] = it.value();
        journal_.append(QJsonObject{{k("op"), k("advance")}, {k("until"), until}, {k("owner_ap"), values}});
    }
    if (intervals) *intervals = result;
    return true;
}

double ItemZoneLedger::nextBoundary(double at, double fallback) const {
    if (!loaded_ || at < time_ || !finite(at) || !finite(fallback) || fallback < at) return fallback;
    auto next = fallback;
    for (const auto &zone : zones_) if (!zone.contacts.isEmpty() && zone.expires > at) next = std::min(next, zone.expires);
    return next;
}

double ItemZoneLedger::flatMrReduction(const QString &target) const {
    for (const auto &zone : zones_) if (zone.activeItem && zone.expires > time_ && zone.contacts.contains(target)) return mrReduction_;
    return 0;
}

QJsonObject ItemZoneLedger::snapshot() const {
    QJsonArray zones;
    for (const auto &z : zones_) zones.append(QJsonObject{{k("zone_id"), z.id}, {k("owner_key"), z.owner},
        {k("trigger_target_key"), z.triggerTarget}, {k("trigger_receipt_id"), z.triggerReceipt}, {k("cast_instance_id"), z.cast},
        {k("start"), z.start}, {k("expires"), z.expires}, {k("active_item"), z.activeItem}, {k("contacts"), sorted(z.contacts)},
        {k("refresh_receipts"), QJsonArray::fromStringList(z.refreshReceipts)}});
    QJsonObject ready; for (auto it = creationReady_.begin(); it != creationReady_.end(); ++it) ready[it.key()] = it.value();
    return {{k("schema_version"), 1}, {k("item_id"), 3118}, {k("patch"), patch_}, {k("revision"), revision_},
        {k("source_projection_sha256"), sourceHash_}, {k("roster_sha256"), rosterHash_}, {k("start_time"), start_},
        {k("time"), time_}, {k("zones"), zones}, {k("creation_ready_at"), ready}, {k("seen_damage_receipts"), sorted(damageReceipts_)},
        {k("seen_contact_events"), sorted(contactEvents_)}, {k("journal"), journal_},
        {k("model"), k("conditional_explicit_contact_rate_integral")}, {k("live_game_verified"), false}};
}

bool ItemZoneLedger::restore(const QJsonObject &saved, QString *error) {
    if (!loaded_ || saved.value(k("schema_version")) != 1 || saved.value(k("item_id")) != 3118 ||
        saved.value(k("patch")) != patch_ || saved.value(k("revision")) != revision_ ||
        saved.value(k("source_projection_sha256")) != sourceHash_ || saved.value(k("roster_sha256")) != rosterHash_ ||
        !saved.value(k("start_time")).isDouble() || !finite(saved.value(k("start_time")).toDouble()) ||
        !saved.value(k("journal")).isArray() || saved.value(k("journal")).toArray().size() > 100000)
        return fail(error, "item_zone_checkpoint_binding_mismatch");
    auto staged = *this; staged.start_ = staged.time_ = saved.value(k("start_time")).toDouble();
    staged.zones_.clear(); staged.creationReady_.clear(); staged.damageReceipts_.clear(); staged.contactEvents_.clear(); staged.journal_ = {};
    for (const auto &raw : saved.value(k("journal")).toArray()) {
        if (!raw.isObject()) return fail(error, "item_zone_checkpoint_invalid_journal");
        const auto event = raw.toObject(); const auto op = event.value(k("op")).toString();
        if (op == k("trigger")) {
            const auto r = event.value(k("receipt")).toObject();
            if (!r.value(k("time")).isDouble() || !r.value(k("actual_damage")).isDouble() ||
                !r.value(k("origin")).isDouble() || r.value(k("origin")) != int(Origin::ChampionUltimate) ||
                !r.value(k("owner_currently_has_item")).isBool()) return fail(error, "item_zone_checkpoint_invalid_receipt");
            UltimateDamageReceipt receipt{r.value(k("receipt_id")).toString(), r.value(k("owner_key")).toString(), r.value(k("target_key")).toString(),
                r.value(k("cast_instance_id")).toString(), r.value(k("source_slot")).toString(), r.value(k("time")).toDouble(),
                r.value(k("actual_damage")).toDouble(), Origin::ChampionUltimate, r.value(k("owner_currently_has_item")).toBool()};
            const auto decision = staged.triggerInPlace(receipt, true);
            if (!decision.accepted && decision.reason != k("item_zone_per_target_creation_icd")) return fail(error, "item_zone_checkpoint_invalid_receipt");
        } else if (op == k("contact")) {
            if (!event.value(k("time")).isDouble() || !event.value(k("inside")).isBool()) return fail(error, "item_zone_checkpoint_invalid_contact");
            Contact contact{event.value(k("event_id")).toString(), event.value(k("owner_key")).toString(), event.value(k("zone_id")).toString(),
                event.value(k("target_key")).toString(), event.value(k("time")).toDouble(), event.value(k("inside")).toBool()};
            if (!staged.contactInPlace(contact, true, error)) return false;
        } else if (op == k("clear_target")) {
            if (!event.value(k("time")).isDouble() || !staged.clearInPlace(event.value(k("target_key")).toString(), event.value(k("time")).toDouble(), true, error)) return false;
        } else if (op == k("advance")) {
            if (!event.value(k("until")).isDouble() || !event.value(k("owner_ap")).isObject()) return fail(error, "item_zone_checkpoint_invalid_advance");
            QMap<QString, double> values;
            const auto object = event.value(k("owner_ap")).toObject();
            for (auto it = object.begin(); it != object.end(); ++it) {
                if (!it.value().isDouble()) return fail(error, "item_zone_checkpoint_invalid_advance");
                values[it.key()] = it.value().toDouble();
            }
            if (!staged.advanceInPlace(event.value(k("until")).toDouble(), values, nullptr, true, error)) return false;
        } else return fail(error, "item_zone_checkpoint_unknown_operation");
        if (staged.journal_.last() != raw) return fail(error, "item_zone_checkpoint_noncanonical_journal");
    }
    if (staged.snapshot() != saved) return fail(error, "item_zone_checkpoint_derived_state_mismatch");
    *this = staged;
    return true;
}
