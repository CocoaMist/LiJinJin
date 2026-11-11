#include "continuous_sandbox.h"
#include "continuous_action_state.h"
#include "continuous_control_state.h"
#include "progression_v4.h"
#include "form_stats_v4.h"
#include "mechanic_delivery_v4.h"
#include "combat_sustain.h"
#include "stateful_skill_v7.h"
#include "combat_units_v8.h"
#include "combat_sandbox.h"
#include "teamfight_sandbox.h"
#include "purchase_planner.h"
#include "world_attack_probe_v1.h"
#include "item_zone_ledger.h"
#include <QJsonDocument>
#include <QCryptographicHash>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace {
QString k(const char *s) { return QString::fromLatin1(s); }
constexpr double eps = 1e-8;
const QStringList policies{k("focus_damage"), k("focus_frontline"), k("spread"), k("protect_carry"), k("hold")};
bool number(const QJsonValue &v, double lo, double hi) { return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= lo && v.toDouble() <= hi; }
bool integer(const QJsonValue &v, int lo, int hi, int &out) { if (!number(v, lo, hi) || std::floor(v.toDouble()) != v.toDouble()) return false; out = int(v.toDouble()); return true; }
QJsonArray codes(const QSet<QString> &s) { auto a = s.values(); a.sort(); return QJsonArray::fromStringList(a); }
QJsonObject invalid(const QString &reason) { return {{k("available"), false}, {k("status"), k("invalid_request")}, {k("reason"), reason}, {k("actual_win_probability"), QJsonValue::Null}}; }
struct Shield { QString source, slot; double amount = 0, expires = 0; };
struct Pending {
    double time = 0;
    QJsonObject event;
    QString kind = QStringLiteral("cast_completion"), instance;
    bool locksActor = true, cancelOnInterrupt = true, cancelOnSourceDeath = true, cancelOnTargetDeath = false;
};
struct CastInstance {
    QString id, actor, slot, binding;
    double created = 0, expires = 0;
    QSet<QString> delivered;
    bool cancelled = false;
    double activationAt = 0;
    bool activationChannel = false;
};
QString ledgerShotHash(QJsonObject receipt) {
    receipt.remove(k("receipt_sha256"));
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(receipt).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
}
bool ledgerJhinShot(const QJsonObject &event,int champion,const QString &revision,const QMap<QString,CastInstance> &instances) {
    const auto receipt=event.value(k("_ledger_jhin_shot")).toObject(); int shot=0;
    if(champion!=202||event.value(k("type")).toString()!=k("cast")||event.value(k("slot")).toString()!=k("R")||receipt.isEmpty()||
       receipt.value(k("receipt_sha256")).toString()!=ledgerShotHash(receipt)||receipt.value(k("combat_revision")).toString()!=revision||
       receipt.value(k("event_id"))!=event.value(k("event_id"))||receipt.value(k("player_key"))!=event.value(k("player_key"))||
       receipt.value(k("time"))!=event.value(k("time"))||!integer(receipt.value(k("shot_number")),1,4,shot)||
       event.value(k("shot_number")).toInt()!=shot||event.value(k("facts")).toObject().value(k("jhin_r_shot_number")).toInt()!=shot||
       !event.value(k("facts")).toObject().value(k("verified_jhin_r_shot")).toBool()||!event.value(k("payload_emitted")).toBool())return false;
    const auto id=receipt.value(k("source_instance_id")).toString();if(!instances.contains(id))return false;const auto &instance=instances[id];
    return !instance.cancelled&&instance.actor==event.value(k("player_key")).toString()&&instance.binding==k("ledger_jhin_r")&&instance.slot==k("R")&&
        event.value(k("burst_instance_id")).toString()==receipt.value(k("burst_instance_id")).toString()&&
        !receipt.value(k("burst_instance_id")).toString().isEmpty()&&event.value(k("time")).toDouble()>=instance.created&&event.value(k("time")).toDouble()<instance.expires-eps;
}
struct Fighter {
    QString key, team, fingerprint;
    int champion = 0, level = 0, spreadCursor = 0;
    QList<int> items, augments;
    CombatContext context;
    CombatStats stats;
    ContinuousActionState action;
    ContinuousControlState control;
    ProgressionLedger progression;
    StatefulSkillLedgerV7 stateful;
    QJsonArray stateBuffs;
    QJsonObject damageReceipts, entityObservations, documentedProfile, documentedCharge;
    double stateContinuousAt = 0, naturalHealed = 0, entityDealt = 0, entityReceived = 0, unitDealt = 0, unitShieldDealt = 0;
    double maximum = 0, initial = 0, hp = 0, dealt = 0, received = 0, healed = 0, cost = 0;
    double shieldDealt = 0, absorbed = 0, externalLoss = 0, restored = 0, statHealthLoss = 0, threat = 0, deathAt = -1;
    double rawHealing = 0, amplifiedHealing = 0, rawShield = 0, generatedShield = 0, generatedSelfShield = 0, permanentHpBaseline = 0;
    double output = 1, taken = 1, support = 1;
    int deaths = 0;
    bool initiallyDead = false, referenceRanks = false, augmentsComplete = false, probeBaseAttackOnly = false;
    QJsonObject probeObservedAnchor;
    QVector<Shield> shields;
    QMap<QString,double> temporalReadyAt;
    QSet<QString> unknowns;
};
struct Weights { double damage = 1, survival = 1, death = 2, uncertainty = .1; };
struct SupportEffect { int target = -1; QString kind, slot; double amount = 0, duration = 0; };
void recordStatPassUnknowns(const CombatDatabase &db,Fighter &actor,const QStringList &values) {
    // A targetless attribute refresh does not execute Shadowflame damage.
    // Keep its target-health dependency in the targeted payload evaluator,
    // which still records every unresolved input before applying damage.
    // This exception is source-specific and expires if 4645 gains a
    // target-health-dependent attribute effect.
    bool damageOnlyHealthGate=false,attributeHealthGate=false;
    if(actor.items.contains(4645)) for(const auto &raw:db.items.value(4645).effects) {
        const auto effect=raw.toObject();
        if(!effect.contains(k("below_target_health"))&&!effect.value(k("target_current_hp")).toBool())continue;
        if(effect.value(k("op"))==k("damage_type_amp"))damageOnlyHealthGate=true;
        else attributeHealthGate=true;
    }
    for(const auto &value:values) {
        if(value==k("target_current_hp:4645")&&damageOnlyHealthGate&&!attributeHealthGate)continue;
        actor.unknowns.insert(value);
    }
}
int find(const QVector<Fighter> &p, const QString &key) { for (int i = 0; i < p.size(); ++i) if (p[i].key == key) return i; return -1; }
bool hasFlag(const CombatDatabase &db, const Fighter &a, const QString &flag) {
    auto in = [&](const CombatEntry &entry) { for (const auto &v : entry.effects) { const auto e = v.toObject(); if (e.value(k("op")).toString() == k("flag") && (e.value(k("stat")).toString() == flag || e.value(k("type")).toString() == flag)) return true; } return false; };
    for (int id : a.items) if (in(db.items.value(id))) return true;
    for (int id : a.augments) if (in(db.augments.value(id))) return true;
    return false;
}
std::optional<CombatStats> actionClockStats(const CombatDatabase &db,const Fighter &actor) {
    auto stats=actor.stats; bool only=false;double factor=1;
    auto apply=[&](const CombatEntry &entry) {
        for(const auto &raw:entry.effects) {const auto e=raw.toObject();if(e.value(k("op")).toString()!=k("flag") || (e.value(k("stat")).toString()!=k("ultimate_haste_only")&&e.value(k("type")).toString()!=k("ultimate_haste_only")))continue;
            only=true;std::optional<double> value;const auto rawValue=e.value(k("value"));
            if(rawValue.isDouble())value=rawValue.toDouble();
            else if(rawValue.isObject())value=compileCombatFormula(rawValue.toObject(),actor.context,1).evaluate(actor.context,actor.stats);
            else if(rawValue.isString()) {const auto name=rawValue.toString();const auto v=entry.values.value(name);if(v.isDouble())value=v.toDouble();else if(v.isArray()&&!v.toArray().isEmpty()&&v.toArray().first().isDouble())value=v.toArray().first().toDouble();else {auto expression=entry.calculations.value(name.startsWith(k("calc:"))?name.mid(5):name).toObject();value=compileCombatFormula(expression,actor.context,1).evaluate(actor.context,actor.stats);} }
            if(!value || !std::isfinite(*value) || *value < -1 || *value>1e4)return false;factor=1+*value;
        }return true;
    };
    for(int id:actor.items)if(!apply(db.items.value(id)))return {};
    for(int id:actor.augments)if(!apply(db.augments.value(id)))return {};
    if(only) {stats[Stat::RHaste]=(stats[Stat::Haste]+stats[Stat::RHaste])*factor;stats[Stat::Haste]=0;}
    return stats;
}
QString checkpointChecksum(QJsonObject checkpoint) {checkpoint.remove(k("integrity_sha256"));return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(checkpoint).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());}
QJsonObject actionRule(const CombatDatabase &db, const Fighter &a, const QString &slot, const QJsonObject &facts) {
    const auto rules = db.champions.value(a.champion).value(k("action_rules_v3")).toObject();
    const auto form = a.action.snapshot().value(k("form")).toString();
    const auto formRows = rules.value(k("forms")).toObject().value(form).toObject().value(k("slots")).toObject();
    auto rule = formRows.contains(slot) ? formRows.value(slot).toObject() : rules.value(k("slots")).toObject().value(slot).toObject();
    const auto phase = facts.value(k("phase")).toString(k("primary")); if (phase != k("primary")) rule = rule.value(k("recasts")).toObject().value(phase).toObject();
    return rule;
}
QJsonObject actionSkillState(const CombatDatabase &db,const Fighter &actor,const QString &slot) {
    const auto state=actor.action.snapshot();const auto rule=actionRule(db,actor,slot,{});const auto group=rule.value(k("cooldown_group")).toString(),form=state.value(k("form")).toString();
    const auto key=!group.isEmpty()?k("shared:")+group:(form.isEmpty()?slot:form+k(":")+slot);
    return state.value(k("skills")).toObject().value(key).toObject();
}

double receivedSustainMultiplier(const CombatDatabase &db,const Fighter &recipient,
                                 const QString &kind,const QString &deliverySource={}) {
    // Explicit 3065 mechanic observations already represent its incremental
    // amount; passing that delta through the recipient amplifier again would
    // count the same passive twice.
    if(deliverySource.startsWith(k("item:3065:"))) return 1;
    const auto modifiers=combatSelfSustainModifiers(db,recipient.context,recipient.items,recipient.augments,recipient.stats);
    return kind==k("shield")?modifiers.shieldingReceived:modifiers.healingReceived;
}

bool prepareMechanicClockObservation(const CombatDatabase &db,const Fighter &actor,
                                    QJsonObject &event,QString *error) {
    auto observation=event.value(k("observation")).isObject()?event.value(k("observation")).toObject():event;
    // This timeline applies received modifiers at every native heal/shield
    // delivery. The legacy delta replay is an alternative representation of
    // the same passive, and cannot be layered onto an already amplified pool.
    if(observation.value(k("source")).toString()==k("item")&&observation.value(k("id")).toInt()==3065) {
        if(error)*error=k("received_modifier_already_applied_in_continuous");return false;
    }
    if(observation.value(k("source")).toString()==k("augment")&&observation.value(k("id")).toInt()==1152) {
        auto facts=observation.value(k("facts")).toObject();
        // Inventory is an engine-owned prerequisite, not an extra fact the
        // caller must assert (or can manufacture when the item is absent).
        facts[k("item:3084")]=actor.items.contains(3084);observation[k("facts")]=facts;
        if(event.value(k("observation")).isObject())event[k("observation")]=observation;else event=observation;
    }
    if(observation.value(k("source")).toString()!=k("augment")||observation.value(k("id")).toInt()!=1058)return true;
    auto facts=observation.value(k("facts")).toObject();
    // The catalog's OnHit eligibility / affected slots are not certified by a
    // numeric CooldownRefund alone. Permit only an explicitly admitted scenario
    // contract, and always derive remaining time from the owning action clock.
    if(facts.value(k("verified_mystic_punch_on_hit"))!=QJsonValue(true)||
       facts.value(k("mystic_punch_slot_contract")).toString()!=k("compiled_slots")) {
        if(error)*error=k("mystic_punch_requires_explicit_on_hit_and_slot_contract");return false;
    }
    for(const auto &slot:{k("Q"),k("W"),k("E"),k("R")})
        facts[k("remaining_")+slot]=std::max(0.,actionSkillState(db,actor,slot).value(k("ready_at")).toDouble()-event.value(k("time")).toDouble());
    observation[k("facts")]=facts;
    if(event.value(k("observation")).isObject())event[k("observation")]=observation;else event=observation;
    return true;
}

bool consumeMechanicClockEffects(Fighter &actor,const QJsonArray &effects,double at,
                                QJsonArray &recipientEffects,QJsonObject &receipt,QString *error) {
    QJsonArray refunds;
    for(const auto &raw:effects) {
        const auto effect=raw.toObject();
        if(effect.value(k("op")).toString()==k("refund"))refunds.append(effect);
        else recipientEffects.append(effect);
    }
    if(refunds.isEmpty())return true;
    receipt=actor.action.applyCooldownRefunds(refunds,at);
    if(!receipt.value(k("accepted")).toBool()) {if(error)*error=receipt.value(k("reason")).toString();return false;}
    return true;
}

QJsonObject applyMysticPunchOnHit(const CombatDatabase &db,Fighter &actor,const QJsonObject &hit,double at) {
    if(!actor.augments.contains(1058))return {};
    auto staged=actor;QString error;
    QJsonObject observation{{k("source"),k("augment")},{k("id"),1058},{k("event"),k("attack_hit")},
        {k("target"),hit.value(k("target_key"))},{k("facts"),hit.value(k("facts"))}};
    QJsonObject event{{k("type"),k("mechanic_observation")},{k("event_id"),hit.value(k("event_id")).toString()+k(":mystic_punch")},
        {k("time"),at},{k("player_key"),actor.key},{k("observation"),observation}};
    if(!prepareMechanicClockObservation(db,staged,event,&error)) {
        actor.unknowns.insert(error);return {{k("accepted"),false},{k("reason"),error}};
    }
    if(!staged.progression.apply(db,event,&error,&actor.context))return {{k("accepted"),false},{k("reason"),error}};
    QJsonArray recipientEffects;QJsonObject receipt;
    if(!consumeMechanicClockEffects(staged,staged.progression.lastDecision().value(k("new_effects")).toArray(),at,recipientEffects,receipt,&error))
        return {{k("accepted"),false},{k("reason"),error}};
    for(const auto &raw:recipientEffects)if(raw.toObject().value(k("op")).toString()!=k("configure"))
        return {{k("accepted"),false},{k("reason"),k("mystic_punch_unhandled_recipient_effect")}};
    staged.unknowns.insert(k("mystic_punch_eligibility_and_slots_are_explicit_scenario_contract_min_cdr_unresolved"));
    actor=std::move(staged);receipt[k("trigger_event_id")]=hit.value(k("event_id"));
    return receipt;
}

QJsonObject primeArmorImpact(const CombatDatabase &db,Fighter &actor,const QJsonObject &cast,double at) {
    if(!actor.augments.contains(3049)||cast.value(k("facts")).toObject().value(k("verified_impact_timing_contract"))!=QJsonValue(true))return {};
    auto facts=cast.value(k("facts")).toObject();facts[k("verified_admitted_cast")]=true;
    facts[k("cast_instance_id")]=cast.value(k("event_id"));facts[k("cast_slot")]=cast.value(k("slot"));
    QJsonObject observation{{k("source"),k("augment")},{k("id"),3049},{k("event"),k("admitted_spell_cast")},{k("facts"),facts}};
    QJsonObject event{{k("type"),k("mechanic_observation")},{k("event_id"),cast.value(k("event_id")).toString()+k(":impact_prime")},
        {k("time"),at},{k("observation"),observation}};QString error;auto staged=actor;
    if(!staged.progression.apply(db,event,&error,&actor.context))return {{k("accepted"),false},{k("reason"),error}};
    actor=std::move(staged);return actor.progression.lastDecision();
}

QJsonObject consumeArmorImpact(const CombatDatabase &db,Fighter &actor,const QJsonObject &hit,
                              double at,const QJsonArray &roster,QVector<QJsonObject> &payloads) {
    const auto supplied=hit.value(k("facts")).toObject();
    if(!actor.augments.contains(3049)||supplied.value(k("verified_impact_target_set"))!=QJsonValue(true))return {};
    auto facts=supplied;facts[k("verified_accepted_hit")]=true;
    facts[k("attack_kind")]=hit.value(k("type"))==k("attack")?k("basic_attack"):k("skill_on_hit");
    auto staged=actor;QString error;const auto id=hit.value(k("event_id")).toString()+k(":impact_hit");
    QJsonObject observation{{k("source"),k("augment")},{k("id"),3049},{k("event"),k("spell_primed_attack_hit")},
        {k("facts"),facts},{k("event_id"),id}};
    QJsonObject event{{k("type"),k("mechanic_observation")},{k("event_id"),id},{k("time"),at},{k("observation"),observation}};
    if(!staged.progression.apply(db,event,&error,&actor.context))return {{k("accepted"),false},{k("reason"),error}};
    const auto plan=planMechanicDeliveryV4(staged.progression.lastDecision().value(k("new_effects")).toArray(),observation,actor.key,roster,at);
    if(!plan.value(k("accepted")).toBool())return {{k("accepted"),false},{k("reason"),plan.value(k("error"))}};
    QVector<QJsonObject> prepared;
    for(const auto &raw:plan.value(k("deliveries")).toArray()) {
        const auto delivery=raw.toObject();
        prepared.append(QJsonObject{{k("type"),k("effect_delivery")},{k("event_id"),id+k(":")+delivery.value(k("effect_id")).toString()},
            {k("time"),at},{k("player_key"),actor.key},{k("target_key"),delivery.value(k("recipient_key"))},{k("_delivery"),delivery}});
        if(hit.value(k("_state_hero_hit_receipt_v1")).isObject()) {
            auto &packet=prepared.last();packet[k("source_cast_instance_id")]=hit.value(k("source_cast_instance_id"));
            packet[k("hit_event_id")]=hit.value(k("hit_event_id"));packet[k("parent_event_id")]=hit.value(k("event_id"));
            packet[k("damage_packet_id")]=packet.value(k("event_id"));packet[k("origin_ability_id")]=k("augment:3049");
            packet[k("application_origin_family")]=k("augment_proc");packet[k("trigger_origin_family")]=hit.value(k("application_origin_family"));
        }
    }
    actor=std::move(staged);payloads+=prepared;return plan;
}
double shieldRemaining(const Fighter &p) { double n = 0; for (const auto &s : p.shields) n += s.amount; return n; }
void expire(Fighter &p, double at) { for (int i = p.shields.size() - 1; i >= 0; --i) if (p.shields[i].expires <= at + eps || p.shields[i].amount <= eps) p.shields.removeAt(i); }

bool refreshStats(const CombatDatabase &,Fighter &,double,QString *);
QJsonObject applyNative3116Hit(const CombatDatabase &db,Fighter &actor,const Fighter &target,const QJsonObject &hit,
 double at,bool boundSpell,const QJsonArray &roster,QVector<QJsonObject> &payloads) {
    auto facts=hit.value(k("facts")).toObject();facts[k("target_missing_hp")]=std::max(0.,target.maximum-target.hp);
    const auto hero=hit.value(k("_state_hero_hit_receipt_v1")).toObject();
    const auto parent=hit.value(k("event_id")).toString(),id=parent+k(":3116");
    const auto hitId=boundSpell?hero.value(k("hit_event_id")).toString():parent;
    QJsonObject receipt{{k("actor_key"),actor.key},{k("target_key"),target.key},{k("target_type"),k("champion")},
        {k("cycle_id"),hitId},{k("hit_event_id"),hitId},{k("parent_event_id"),parent},{k("time"),at},{k("verified_contact"),true},
        {k("cast_instance_id"),boundSpell?hero.value(k("cast_instance_id")):QJsonValue(parent)},
        {k("origin_ability_id"),boundSpell?hero.value(k("origin_ability_id")):QJsonValue(k("basic_attack"))},
        {k("application_origin_family"),boundSpell?k("bound_spell"):k("basic_attack")},{k("target_life_id"),QString::number(target.deathAt,'g',17)},
        {k("actor_life_id"),QString::number(actor.deathAt,'g',17)}};
    QJsonObject observation{{k("source"),k("augment")},{k("id"),3116},{k("event"),k("native_qualified_hit")},
        {k("actor_key"),actor.key},{k("target"),target.key},{k("receipt"),receipt},{k("facts"),facts},{k("time"),at},{k("event_id"),id}};
    QJsonObject event{{k("type"),k("mechanic_observation")},{k("event_id"),id},{k("time"),at},{k("player_key"),actor.key},{k("observation"),observation}};
    auto staged=actor;QString error;
    if(!staged.progression.apply(db,event,&error,&actor.context,true)||!refreshStats(db,staged,at,&error))return {{k("accepted"),false},{k("reason"),error}};
    const auto plan=planMechanicDeliveryV4(staged.progression.lastDecision().value(k("new_effects")).toArray(),observation,actor.key,roster,at);
    if(!plan.value(k("accepted")).toBool())return {{k("accepted"),false},{k("reason"),plan.value(k("error"))}};
    QVector<QJsonObject> prepared;
    for(const auto &raw:plan.value(k("deliveries")).toArray()){
        const auto delivery=raw.toObject();if(delivery.value(k("start_at")).toDouble()!=at)return {{k("accepted"),false},{k("reason"),k("3116_delayed_delivery_unreviewed")}};
        prepared.append(QJsonObject{{k("type"),k("effect_delivery")},{k("event_id"),id+k(":delivery")},{k("time"),at},
            {k("player_key"),actor.key},{k("target_key"),target.key},{k("_delivery"),delivery}});
    }
    actor=std::move(staged);payloads+=prepared;auto result=actor.progression.lastDecision();
    result[k("policy_provenance")]=facts.value(k("3116_policy_provenance"));result[k("live_certified")]=false;
    result[k("known_gaps")]=QJsonArray{k("live_stack_refresh_and_consume_order"),k("live_shield_heal_basis"),k("unbound_spell_DOT_proc_and_pet_eligibility")};return result;
}

QJsonObject stateProgram(const CombatDatabase &db, const Fighter &actor, const QString &id) {
    const auto documented = actor.documentedProfile.value(k("program")).toObject();
    if (!documented.isEmpty() && documented.value(k("binding_id")).toString() == id) return documented;
    for (const auto &raw : db.champions.value(actor.champion).value(k("stateful_bindings_v7")).toArray())
        if (raw.toObject().value(k("binding_id")).toString() == id &&
            (documented.isEmpty() || raw.toObject().value(k("slot")) != documented.value(k("slot")))) return raw.toObject();
    return {};
}
bool digestV9(const QString &value) {
    if(value.size()!=64)return false;
    for(const auto ch:value)if(!((ch>=QLatin1Char('0')&&ch<=QLatin1Char('9'))||(ch>=QLatin1Char('a')&&ch<=QLatin1Char('f'))))return false;
    return true;
}
QJsonObject documentedIdentity(const Fighter &actor) {
    if(actor.documentedProfile.isEmpty())return {};
    return {{k("profile_id"),actor.documentedProfile.value(k("profile_id"))},{k("profile_sha256"),actor.documentedProfile.value(k("profile_sha256"))},
        {k("execution_kind"),k("documented_model_v9")},{k("source_grade"),actor.documentedProfile.value(k("source_grade"))},{k("server_mechanics_certified"),false}};
}
QJsonObject payDocumentedFee(Fighter &actor,const QJsonObject &event,double at,const QMap<QString,CastInstance> &instances) {
    const auto contract=actor.documentedProfile.value(k("action_contract")).toObject();auto charge=actor.documentedCharge;
    const int phase=event.value(k("_state_fee_phase_v9")).toInt(-1);const auto phaseKey=QString::number(phase),id=event.value(k("cast_instance_id")).toString();
    auto receipts=charge.value(k("phases")).toObject();
    QJsonObject receipt=documentedIdentity(actor);receipt[k("type")]=k("documented_charge_fee_v9");receipt[k("event_id")]=event.value(k("event_id"));receipt[k("player_key")]=actor.key;
    receipt[k("instance_id")]=id;receipt[k("phase")]=phase;receipt[k("time")]=at;receipt[k("pre_health")]=actor.hp;receipt[k("maximum_health")]=actor.maximum;
    receipt[k("paid")]=0;receipt[k("post_health")]=actor.hp;receipt[k("committed")]=false;receipt[k("exempt")]=false;
    receipt[k("comparison")]=contract.value(k("exemption_comparison"));receipt[k("coincident_release_order")]=contract.value(k("coincident_release_order"));receipt[k("payment_health_basis")]=k("live_maximum_health");
    const auto offsets=contract.value(k("payment_offsets_seconds")).toArray();
    if(phase<0||phase>=offsets.size()||id!=charge.value(k("instance_id")).toString()||!instances.contains(id)||instances.value(id).cancelled||
       !charge.value(k("active")).toBool()||actor.hp<=eps||receipts.contains(phaseKey)||
       charge.value(k("profile_sha256"))!=actor.documentedProfile.value(k("profile_sha256"))||
       std::abs(at-charge.value(k("started_at")).toDouble()-offsets.at(phase).toDouble())>eps) {
        receipt[k("cancelled")]=true;receipt[k("reason")]=k("documented_charge_inactive_or_phase_consumed");return receipt;
    }
    const double threshold=contract.value(k("exemption_health_fraction")).toDouble()*actor.maximum;
    receipt[k("threshold_health")]=threshold;receipt[k("payment_fraction")]=contract.value(k("payment_maximum_health_fraction"));receipt[k("coefficient_basis")]=k("documented_decimal");
    const bool exempt=contract.value(k("exemption_comparison")).toString()==k("lt")?actor.hp<threshold:actor.hp<=threshold;
    const double payment=exempt?0:contract.value(k("payment_maximum_health_fraction")).toDouble()*actor.maximum;
    if(!std::isfinite(payment)||payment<0||actor.hp-payment<=eps) {receipt[k("cancelled")]=true;receipt[k("reason")]=k("documented_health_cost_unaffordable");return receipt;}
    actor.hp-=payment;actor.cost+=payment;receipt[k("paid")]=payment;receipt[k("post_health")]=actor.hp;receipt[k("exempt")]=exempt;receipt[k("committed")]=true;
    receipts[phaseKey]=receipt;charge[k("phases")]=receipts;actor.documentedCharge=charge;return receipt;
}
QJsonObject stateTransition(const QJsonObject &program, const QString &id) {
    for (const auto &raw : program.value(k("transitions")).toArray()) if (raw.toObject().value(k("id")).toString() == id) return raw.toObject();
    return {};
}
QJsonObject entityCombatStats(const Fighter &owner,const QString &binding,const QString &key,const QJsonObject &entity) {
    auto stats=entity.value(k("stats")).toObject();const auto observation=owner.entityObservations.value(binding+k("|")+key).toObject();
    if(observation.value(k("entity_created_at"))==entity.value(k("created_at"))) {const auto observed=observation.value(k("stats")).toObject();for(const auto &stat:{k("armor"),k("mr"),k("ms")})if(!stats.value(stat).isDouble())stats[stat]=observed.value(stat);}
    return stats;
}
CombatContext liveContext(const Fighter &,const Fighter *,const QJsonObject &);
QJsonObject attackSourceView(const Fighter &owner,const QJsonObject &event) {
    const auto context=liveContext(owner,nullptr,event);
    QJsonObject counters;for(auto it=context.counters.begin();it!=context.counters.end();++it)if(!it.key().startsWith(k("target_")))counters[it.key()]=it.value();
    return {{k("event_stats"),owner.context.eventStats.json()},{k("observed_offset"),owner.context.observedOffset.json()},{k("observed_input_offset"),owner.context.observedInputOffset.json()},
        {k("permanent_event_stats"),owner.context.permanentEventStats.json()},{k("form_stat_transforms"),owner.context.formStatTransforms},{k("source_counters"),counters},{k("stats"),owner.stats.json()}};
}
void applyAttackSourceView(CombatContext &context,const QJsonObject &view) {
    auto restoreStats=[&](const char *key,CombatStats &stats) {const auto values=view.value(k(key)).toObject();for(auto it=values.begin();it!=values.end();++it)if(const auto stat=combatStat(it.key()))stats[*stat]=it.value().toDouble();};
    restoreStats("event_stats",context.eventStats);restoreStats("observed_offset",context.observedOffset);restoreStats("observed_input_offset",context.observedInputOffset);restoreStats("permanent_event_stats",context.permanentEventStats);
    context.formStatTransforms=view.value(k("form_stat_transforms")).toArray();const auto counters=view.value(k("source_counters")).toObject();for(auto it=counters.begin();it!=counters.end();++it)context.counters[it.key()]=it.value().toDouble();
}
double stateDamageMultiplier(const QVector<Fighter> &players, int target, double at) {
    double multiplier = 1;
    for (const auto &owner : players) for (const auto &raw : owner.stateful.activeEffects(at)) {
        const auto effect = raw.toObject();
        if (effect.value(k("target_key")).toString() != players[target].key) continue;
        if (effect.value(k("op")).toString() == k("damage_reduction")) multiplier *= 1 - std::clamp(effect.value(k("amount")).toDouble(), 0., 1.);
        if (effect.value(k("op")).toString() == k("stat_buff") && effect.value(k("stat")).toString() == k("incoming_damage_multiplier")) multiplier *= std::clamp(effect.value(k("amount")).toDouble(1), 0., 10.);
    }
    return multiplier;
}
void syncStateBuffs(QVector<Fighter> &players, double at, const ItemZoneLedger *itemZones = nullptr) {
    for (auto &player : players) player.stateBuffs = QJsonArray{};
    for (const auto &owner : players) for (const auto &raw : owner.stateful.activeEffects(at)) {
        auto effect = raw.toObject(); if (effect.value(k("op")).toString() != k("stat_buff")) continue;
        const int target = find(players, effect.value(k("target_key")).toString()); if (target < 0) continue;
        if (!effect.contains(k("expires_at"))) effect[k("expires_at")] = 1e12;
        players[target].stateBuffs.append(effect);
    }
    if (itemZones) for (auto &player : players) {
        const double reduction = itemZones->flatMrReduction(player.key);
        if (reduction > 0) player.stateBuffs.append(QJsonObject{{k("op"),k("stat_buff")},
            {k("stat"),k("mr")},{k("amount"),-reduction},{k("mode"),k("add")},
            {k("source_key"),k("item3118:explicit_contact")},{k("target_key"),player.key},
            {k("expires_at"),1e12},{k("live_certified"),false}});
    }
}
QString stateTargetBlockReason(const QVector<Fighter> &players, int target, int source, double at, const QJsonObject &event) {
    for (const auto &owner : players) for (const auto &raw : owner.stateful.activeEffects(at)) {
        const auto effect = raw.toObject();
        const auto op=effect.value(k("op")).toString(),controlType=effect.value(k("control_type")).toString(effect.value(k("control")).toString());
        const bool concealed=op==k("control")&&QStringList{k("camouflage"),k("stealth"),k("invisibility")}.contains(controlType);
        if ((op != k("untargetable")&&!concealed) || effect.value(k("target_key")).toString() != players[target].key) continue;
        if (players[target].team == players[source].team) continue;
        const auto scope = effect.value(k("scope")).toString(),mode=effect.value(k("mode")).toString();const auto facts = event.value(k("facts")).toObject();
        if (scope == k("outside_zone") || mode==k("outside_zone_enemy_only") || effect.value(k("requires_source_outside_zone")).toBool()) {
            if (facts.value(k("verified_source_zone_relation"))!=QJsonValue(true) || !facts.value(k("source_outside_zone")).isBool()) return k("stateful_target_zone_relation_unverified");
            if (!facts.value(k("source_outside_zone")).toBool()) continue;
        }
        if(mode==k("stealth")||concealed) {
            if(facts.value(k("verified_area_contact"))==QJsonValue(true))continue;
            bool revealed=false;for(const auto &revealer:players)if(revealer.team==players[source].team)for(const auto &entry:revealer.stateful.activeEffects(at)) {const auto reveal=entry.toObject();if(reveal.value(k("op")).toString()==k("reveal")&&reveal.value(k("target_key")).toString()==players[target].key&&reveal.value(k("reveal_kind")).toString()==k("true_sight"))revealed=true;}
            if(revealed||(facts.value(k("verified_target_visibility"))==QJsonValue(true)&&facts.value(k("target_visible"))==QJsonValue(true)))continue;
            if(facts.value(k("verified_target_visibility"))!=QJsonValue(true))return k("stateful_target_stealth_access_unverified");
        } else if(event.value(k("type")).toString()==k("temporal_tick")&&event.value(k("_temporal_existing_effect")).toBool())continue;
        return k("stateful_target_untargetable");
    }
    return {};
}
double absorb(Fighter &p, double amount) {
    std::stable_sort(p.shields.begin(), p.shields.end(), [](const Shield &a, const Shield &b) { return a.expires < b.expires; });
    double remaining = amount;
    for (auto &s : p.shields) { const double use = std::min(s.amount, remaining); s.amount -= use; remaining -= use; }
    p.absorbed += amount - remaining; return amount - remaining;
}
CombatContext liveContext(const Fighter &a, const Fighter *b, const QJsonObject &event) {
    auto c = a.context; c.horizon = .001; c.explicitActionCounts = true; c.explicitAttackCount = 0;
    c.explicitCastCounts.clear(); c.explicitActiveSeconds.clear(); c.explicitProcCounts.clear(); c.explicitComponentKeys.clear();
    c.explicitComponentScales.clear(); c.restrictExplicitComponents = a.probeBaseAttackOnly;
    c.conditions.unite(a.action.formConditions());
    for (const auto &v : event.value(k("conditions")).toArray()) if (v.isString()) c.conditions.insert(v.toString());
    const auto state = a.action.snapshot();
    const auto counters = event.value(k("counters")).toObject();
    for (auto it = counters.begin(); it != counters.end(); ++it) if (number(it.value(), -1e9, 1e9)) c.counters[it.key()] = it.value().toDouble();
    const auto facts = event.value(k("facts")).toObject();
    for (auto it = facts.begin(); it != facts.end(); ++it) if (number(it.value(), -1e9, 1e9)) c.counters[it.key()] = it.value().toDouble();
    c.counters[k("self_current_hp")] = a.hp; c.counters[k("self_max_hp")]=a.maximum;c.counters[k("self_missing_hp")]=std::max(0.,a.maximum-a.hp);c.counters[k("self_health_fraction")] = a.hp / a.maximum;c.counters[k("self_missing_health_fraction")]=1-a.hp/a.maximum;
    if (state.value(k("resource_current")).isDouble()) { c.counters[k("self_current_mana")] = state.value(k("resource_current")).toDouble(); c.counters[k("current_resource")] = state.value(k("resource_current")).toDouble(); }
    if (number(facts.value(k("observed_control_duration")), 0, 120)) c.counters[k("observed_control_duration")] = facts.value(k("observed_control_duration")).toDouble();
    if (b) { c.targetHp = b->maximum; c.targetArmor = b->stats[Stat::Armor]; c.targetMr = b->stats[Stat::Mr]; c.targetMs = b->stats[Stat::Ms]; c.counters[k("target_current_hp")] = b->hp;c.counters[k("target_max_hp")]=b->maximum;c.counters[k("target_missing_hp")]=std::max(0.,b->maximum-b->hp);c.counters[k("target_health_fraction")] = b->hp / b->maximum;c.counters[k("target_missing_health_fraction")]=1-b->hp/b->maximum;
        for (const auto &raw : b->stateBuffs) {const auto buff=raw.toObject();if(buff.value(k("stat")).toString()==k("armor_shred_pct"))c.targetArmor*=1-std::clamp(buff.value(k("amount")).toDouble(),0.,1.);}
    }
    return c;
}
std::optional<double> formula(const QJsonObject &v, const Fighter &p, const CombatContext &c, int rank) {
    const auto n = compileCombatFormula(v, c, rank).evaluate(c, p.stats);
    if (!n || !std::isfinite(*n) || *n < 0 || *n > 1e8) return {}; return n;
}
int enemyTarget(QVector<Fighter> &p, int actor, const QString &policy, const QJsonObject &event) {
    const auto explicitKey = event.value(k("target_key")).toString();
    if (!explicitKey.isEmpty()) { const int i = find(p, explicitKey); return i >= 0 && p[i].team != p[actor].team && p[i].hp > eps ? i : -1; }
    QVector<int> eligible; for (int i = 0; i < p.size(); ++i) if (p[i].team != p[actor].team && p[i].hp > eps) eligible.append(i);
    if (eligible.isEmpty()) return -1;
    if (policy == k("spread")) return eligible[p[actor].spreadCursor % eligible.size()];
    int best = eligible.first(); double score = -1;
    for (int i : eligible) {
        double q = p[i].threat;
        if (policy == k("focus_frontline")) q = p[i].maximum * (1 + std::max(0., p[i].stats[Stat::Armor] + p[i].stats[Stat::Mr]) / 200.);
        if (q > score + eps) { best = i; score = q; }
    }
    return best;
}
int allyTarget(const QVector<Fighter> &p, int actor, bool other, const QJsonObject &event) {
    auto key = event.value(k("support_target_key")).toString();
    if (key.isEmpty()) { const int test = find(p, event.value(k("target_key")).toString()); if (test >= 0 && p[test].team == p[actor].team) key = p[test].key; }
    if (!key.isEmpty()) { const int i = find(p, key); return i >= 0 && p[i].team == p[actor].team && p[i].hp > eps && (!other || i != actor) ? i : -1; }
    int best = -1; double score = -1e100; const bool carry = event.value(k("target_policy")).toString() == k("carry");
    for (int i = 0; i < p.size(); ++i) if (p[i].team == p[actor].team && p[i].hp > eps && (!other || i != actor)) {
        const double value = carry ? p[i].threat : 1 - p[i].hp / p[i].maximum;
        if (value > score + eps) { best = i; score = value; }
    }
    return best;
}
QVector<SupportEffect> supportEffects(const CombatDatabase &db, const QVector<Fighter> &p, int actor, const QString &slot,
                                     const QJsonObject &event, double &cost, QStringList &unknowns, bool checkHealthCost = true) {
    QVector<SupportEffect> out; cost = 0; const auto &a = p[actor]; const int rank = a.context.ranks.value(slot);
    const auto c = liveContext(a, nullptr, event);
    for (const auto &v : db.champions.value(a.champion).value(k("support_components")).toArray()) {
        const auto component = v.toObject(); if (component.value(k("slot")).toString() != slot) continue;
        if(component.value(k("execution_route")).toString()==k("stateful_v7_only"))continue;
        const auto requiredPhase=component.value(k("required_action_phase")).toString();
        if(!requiredPhase.isEmpty()&&event.value(k("facts")).toObject().value(k("phase")).toString(k("primary"))!=requiredPhase) continue;
        const auto condition = component.value(k("condition")).toString();
        if (!condition.isEmpty() && !c.conditions.contains(condition)) { unknowns.append(k("support_condition_missing:") + slot + k(":") + condition); continue; }
        if (component.value(k("rate")).toString(k("cast")) != k("cast") || rank <= 0 || rank > component.value(k("max_rank")).toInt(6)) { unknowns.append(k("support_rate_or_rank_unavailable:" ) + slot); continue; }
        if (!component.value(k("damage_filter")).toString().isEmpty() && component.value(k("damage_filter")).toString() != k("all")) { unknowns.append(k("typed_shield_withheld_without_damage_type_ledger")); continue; }
        const auto amount = formula(component.value(k("expression")).toObject(), a, c, rank);
        const auto kind = component.value(k("kind")).toString(), target = component.value(k("target")).toString();
        double duration = 0;
        if (!amount || (kind != k("heal") && kind != k("shield"))) { unknowns.append(k("support_formula_unavailable:") + slot); continue; }
        if (kind == k("shield")) { const auto n = formula(component.value(k("duration_expression")).toObject(), a, c, rank); if (!n || *n <= 0) { unknowns.append(k("support_shield_duration_unavailable:") + slot); continue; } duration = *n; }
        double healthCost = 0, minimum = 0;
        if (checkHealthCost && component.contains(k("health_cost_fraction_expression"))) { const auto n = formula(component.value(k("health_cost_fraction_expression")).toObject(), a, c, rank); if (!n || *n > 1) { unknowns.append(k("support_health_cost_unknown")); continue; } healthCost = *n * a.maximum; }
        if (checkHealthCost && component.contains(k("health_minimum_fraction_expression"))) { const auto n = formula(component.value(k("health_minimum_fraction_expression")).toObject(), a, c, rank); if (!n || *n > 1) { unknowns.append(k("support_health_minimum_unknown")); continue; } minimum = *n * a.maximum; }
        if (a.hp - healthCost <= eps || a.hp - healthCost < minimum - eps) { cost = -1; return {}; }
        QVector<int> targets;
        if (target == k("self")) targets.append(actor);
        else if (target == k("all_allies_including_self")) { for (int i = 0; i < p.size(); ++i) if (p[i].team == a.team && p[i].hp > eps) targets.append(i); }
        else if (QStringList{k("ally_other"), k("self_or_ally"), k("self_and_ally_champion")}.contains(target)) {
            const int selected = allyTarget(p, actor, target == k("ally_other"), event);
            if (selected < 0) { unknowns.append(k("support_target_unavailable")); continue; }
            if (target == k("self_and_ally_champion")) targets.append(actor);
            if (!targets.contains(selected)) targets.append(selected);
        } else { unknowns.append(k("support_target_kind_unavailable")); continue; }
        cost = std::max(cost, healthCost);
        const double hit = event.value(k("hit_factor")).toDouble(1);
        for (int t : targets) out.append({t, kind, slot, *amount * a.support * hit * std::max(0., 1 + a.stats[Stat::HealShield]) * receivedSustainMultiplier(db,p[t],kind), duration});
    }
    return out;
}
QJsonObject shieldJson(const Shield &s) { return {{k("source"), s.source}, {k("slot"), s.slot}, {k("amount"), s.amount}, {k("expires_at"), s.expires}}; }
bool requiredInputs(const QJsonObject &binding, const Fighter &actor, const CombatContext &context, const QJsonObject &facts) {
    const auto form = binding.value(k("required_form")).toString();
    if (!form.isEmpty() && actor.action.snapshot().value(k("form")).toString() != form) return false;
    const auto condition = binding.value(k("condition")).toString(); if (!condition.isEmpty() && !context.conditions.contains(condition)) return false;
    for (const auto &v : binding.value(k("required_conditions")).toArray()) if (!v.isString() || !context.conditions.contains(v.toString())) return false;
    for (const auto &v : binding.value(k("required_facts")).toArray()) {
        const auto key = v.toString(); const auto value = facts.value(key);
        if (value.isUndefined() || value.isNull() || (value.isBool() && !value.toBool())) return false;
        if (key.startsWith(k("verified_")) && (!value.isBool() || !value.toBool())) return false;
    }
    const auto constraints = binding.value(k("fact_constraints")).toObject();
    for (auto it = constraints.begin(); it != constraints.end(); ++it) {
        const auto limit = it.value().toObject(); const auto value = facts.value(it.key());
        if (limit.contains(k("equals")) && value != limit.value(k("equals"))) return false;
        if (limit.contains(k("min")) && !number(value, limit.value(k("min")).toDouble(), 1e9)) return false;
        if (limit.contains(k("max")) && !number(value, -1e9, limit.value(k("max")).toDouble())) return false;
        if (limit.contains(k("minimum")) && !number(value, limit.value(k("minimum")).toDouble(), 1e9)) return false;
        if (limit.contains(k("maximum")) && !number(value, -1e9, limit.value(k("maximum")).toDouble())) return false;
        if (limit.value(k("integer")).toBool() && (!value.isDouble() || std::floor(value.toDouble()) != value.toDouble())) return false;
    }
    return true;
}


bool refreshStats(const CombatDatabase &db, Fighter &actor, double at, QString *error = nullptr) {
    actor.progression.advance(at, error);
    auto baseline = actor.progression.contextAt(at);
    baseline.explicitActionCounts = true;
    baseline.horizon = .001;
    actor.items = actor.progression.items(); actor.augments = actor.progression.augments();
    for (int i = actor.stateBuffs.size() - 1; i >= 0; --i) {
        const auto buff = actor.stateBuffs[i].toObject();
        if (buff.value(k("expires_at")).toDouble(-1) <= at + eps) { actor.stateBuffs.removeAt(i); continue; }
        const auto stat = combatStat(buff.value(k("stat")).toString());
        if (stat && buff.value(k("mode")).toString(k("add")) == k("add")) baseline.eventStats[*stat] += buff.value(k("amount")).toDouble();
    }
    const auto plain = evaluateCombat(db, baseline, actor.items, actor.augments);
    if (!plain.valid) { if (error) *error = k("progression_numeric_model_unavailable"); return false; }
    const auto form = evaluateFormStatsV4(db.champions.value(actor.champion), actor.action.snapshot().value(k("form")).toString(), baseline, plain.stats);
    if (!form.valid) { if (error) *error = k("form_numeric_model_unavailable"); return false; }
    const auto evaluated = evaluateCombat(db, form.context, actor.items, actor.augments);
    if (!evaluated.valid || evaluated.stats[Stat::Hp] <= 0) { if (error) *error = k("form_numeric_model_unavailable"); return false; }
    const double oldHp = actor.hp;
    if(std::abs(form.context.permanentEventStats[Stat::Hp]-actor.context.permanentEventStats[Stat::Hp])>eps&&
       std::abs(evaluated.stats[Stat::Hp]-actor.maximum)>eps)
        actor.unknowns.insert(k("permanent_hp_growth_preserves_absolute_current_health_in_sandbox_server_policy_unverified"));
    actor.context = form.context; actor.level = baseline.level; actor.stats = evaluated.stats; actor.maximum = actor.stats[Stat::Hp];
    for (const auto &raw : actor.stateBuffs) {
        const auto buff = raw.toObject(); const auto stat = combatStat(buff.value(k("stat")).toString());
        if (!stat || buff.value(k("mode")).toString(k("add")) == k("add")) continue;
        const auto mode = buff.value(k("mode")).toString();
        if (mode == k("multiply")) actor.stats[*stat] *= buff.value(k("amount")).toDouble(1);
        else if (mode == k("percent")) actor.stats[*stat] *= 1 + buff.value(k("amount")).toDouble();
        actor.unknowns.insert(k("state_stat_multiplier_order_is_explicit_scenario_model"));
    }
    for (int i = 0; i < static_cast<int>(Stat::Count); ++i) actor.context.observedOffset.values[i] += actor.stats.values[i] - evaluated.stats.values[i];
    actor.maximum = actor.stats[Stat::Hp];
    actor.hp = std::min(actor.hp, actor.maximum); actor.statHealthLoss += oldHp - actor.hp;
    actor.threat = (actor.stats[Stat::Ad] * std::max(0., actor.stats[Stat::As]) + .6 * actor.stats[Stat::Ap]) * actor.output;
    for (const auto &u : form.unknowns) actor.unknowns.insert(u);
    recordStatPassUnknowns(db,actor,evaluated.unknowns);
    const auto clocks=actionClockStats(db,actor);
    if (!clocks || !actor.action.updateContext(actor.context, *clocks, at, k("clamp_current"))) { if (error) *error = k("action_context_update_failed"); return false; }
    return true;
}
QJsonObject pendingJson(const Pending &p) {
    return {{k("time"),p.time},{k("event"),p.event},{k("kind"),p.kind},{k("cast_instance_id"),p.instance},
        {k("locks_actor"),p.locksActor},{k("cancel_on_interrupt"),p.cancelOnInterrupt},{k("cancel_on_source_death"),p.cancelOnSourceDeath},{k("cancel_on_target_death"),p.cancelOnTargetDeath}};
}
QJsonObject instanceJson(const CastInstance &v) {
    return {{k("cast_instance_id"),v.id},{k("player_key"),v.actor},{k("slot"),v.slot},{k("binding_id"),v.binding},
        {k("created_at"),v.created},{k("expires_at"),v.expires},{k("delivered_phases"),codes(v.delivered)},{k("cancelled"),v.cancelled},
        {k("activation_at"),v.activationAt},{k("activation_requires_completed_channel"),v.activationChannel}};
}

bool ordinaryAugmentOffer(const Catalog &catalog, const Fighter &actor, const QJsonObject &event, QString *error) {
    auto fail=[&](const QString &reason) {if(error)*error=reason;return false;};
    const auto offers=event.value(k("offered_augment_ids"));
    if(!offers.isArray()||offers.toArray().isEmpty()||offers.toArray().size()>3)return fail(k("augment_offers_must_be_one_to_three"));
    QSet<int> seen;QString tier;
    for(const auto &raw:offers.toArray()) {
        int id=0;if(!integer(raw,1,std::numeric_limits<int>::max(),id))return fail(k("augment_offer_unknown_duplicate_owned_or_level_invalid"));
        const auto entry=catalog.combat->augments.constFind(id);
        if(entry==catalog.combat->augments.cend()||seen.contains(id)||actor.augments.contains(id)||actor.level<entry->requiredLevel)return fail(k("augment_offer_unknown_duplicate_owned_or_level_invalid"));
        if(catalog.rewardOnlyAugment(id)||!catalog.ordinaryOfferEligible(id))return fail(k("augment_offer_not_ordinary_eligible"));
        const auto rarity=catalog.augments.value(id).value(k("rarity"));
        if(!rarity.isString()||!QStringList{k("kSilver"),k("kGold"),k("kPrismatic")}.contains(rarity.toString())||(!tier.isEmpty()&&tier!=rarity.toString()))return fail(k("augment_offer_tier_unknown_or_conflicting"));
        tier=rarity.toString();seen.insert(id);
    }
    return true;
}

QJsonObject simulate(const Catalog &catalog, QVector<Fighter> p, QVector<QJsonObject> events, QVector<Pending> pending,
                     QSet<QString> processed, QMap<QString, CastInstance> instances, double start, double end, double decisionStep, bool automatic,
                     const QString &policy, const QString &opponent, const QString &objective, const Weights &weights,
                     double nextDecision, int autoIndex, bool naturalRegeneration, CombatUnitsV8 units,
                     bool itemZoneModel, ItemZoneLedger itemZones) {
    const auto &db=*catalog.combat;
    QJsonArray trace, transitions; QSet<QString> unknowns;
    if(!units.snapshot().value(k("observations")).toArray().isEmpty()) {unknowns.insert(k("combat_units_are_verified_synthetic_observations_without_gold_xp_or_live_identity"));unknowns.insert(k("npc_damage_resolves_in_explicit_event_order_separate_from_simultaneous_player_damage"));}
    for (const auto &s : {k("no_movement_range_or_target_access_simulation"), k("explicit_actions_and_controls_are_scenario_inputs_not_observed_player_actions"),
         k("item_internal_cooldown_procs_require_explicit_proc_counts"), k("unbound_skill_phases_remain_withheld"), k("cast_time_missing_uses_instant_scenario_assumption"),
         k("attack_windup_projectile_travel_and_targetability_partial"), k("temporal_phase_origin_is_cast_completion_scenario_assumption"), k("server_caster_snapshot_policy_unverified_dynamic_current_stats_per_tick"), k("protect_carry_uses_enemy_output_threat_without_geometry")}) unknowns.insert(s);
    if (automatic) unknowns.insert(k("automatic_actions_and_hits_are_synthetic_policy_assumptions"));
    if (itemZoneModel) unknowns.insert(k("item_zone_3118_explicit_contact_rate_integral_not_server_tick_or_live_certification"));
    int cursor = 0, accepted = 0, rejected = 0, steps = 0;
    QString itemZoneFailure;
    double t = start;
    auto busyUntil = [&](int actor) { double at = -1.; for (const auto &v : pending) if (v.locksActor && v.event.value(k("player_key")).toString() == p[actor].key) at = std::max(at, v.time); return at; };
    auto reject = [&](QJsonObject e, const QString &reason) {
        e[k("accepted")] = false; e[k("reason")] = reason;
        if(e.contains(k("stateful_decision"))) {auto decision=e.value(k("stateful_decision")).toObject();if(decision.value(k("accepted")).toBool())e[k("stateful_prepared_decision")]=decision;decision[k("accepted")]=false;decision[k("reason")]=reason;decision[k("effects")]=QJsonArray{};e[k("stateful_decision")]=decision;e[k("stateful_delivery_committed")]=false;if(e.contains(k("stateful_before")))e[k("stateful_after")]=e.value(k("stateful_before"));}
        if (trace.size() < 20000) trace.append(e); ++rejected;
    };
    auto cancelPending = [&](int actor, bool death, const QString &reason) {
        if(p[actor].documentedCharge.value(k("active")).toBool()) {p[actor].documentedCharge[k("active")]=false;p[actor].documentedCharge[k("ended_at")]=t;p[actor].documentedCharge[k("end_reason")]=reason;}
        QSet<QString> cancelled;
        for (int i = pending.size() - 1; i >= 0; --i) {
            const auto &v = pending[i];
            const bool activating=!v.instance.isEmpty()&&instances.contains(v.instance)&&instances[v.instance].activationChannel&&t<instances[v.instance].activationAt-eps;
            if (v.event.value(k("player_key")).toString() != p[actor].key || (!activating&&(death ? !v.cancelOnSourceDeath : !v.cancelOnInterrupt))) continue;
            auto interrupted = v.event; interrupted[k("type")] = k("cast_interrupted"); interrupted[k("time")] = t;
            interrupted[k("reason")] = reason; interrupted[k("accepted")] = false; trace.append(interrupted);
            if (!v.instance.isEmpty()) cancelled.insert(v.instance); pending.removeAt(i);
        }
        for (const auto &id : cancelled) if (instances.contains(id)) instances[id].cancelled = true;
    };
    auto cancelTargetPending = [&](int target) {
        QSet<QString> cancelled;
        for(int i=pending.size()-1;i>=0;--i) if(pending[i].cancelOnTargetDeath&&pending[i].event.value(k("target_key")).toString()==p[target].key) {
            if(!pending[i].instance.isEmpty())cancelled.insert(pending[i].instance);auto rejected=pending[i].event;rejected[k("time")]=t;rejected[k("accepted")]=false;rejected[k("reason")]=k("temporal_target_died");trace.append(rejected);pending.removeAt(i);
        }
        for(const auto &id:cancelled) if(instances.contains(id))instances[id].cancelled=true;
    };
    // Integrate continuous effects with the stats and lifetime valid on the
    // left of the interval. Boundary selection prevents an expiry or growth
    // change from leaking into this interval.
    auto integrateState = [&](double until) {
        for (int index=0;index<p.size();++index) {
            auto &a=p[index];const double from=a.stateContinuousAt,dt=until-from;
            if(dt<=eps)continue;
            double manaFlat=0,manaCurrent=0,hpFlat=0,hpCurrent=0,healFlat=0,healCurrent=0;
            if(a.hp>eps) for(const auto &owner:p) for(const auto &raw:owner.stateful.activeEffects(from)) {
                const auto effect=raw.toObject();if(effect.value(k("target_key")).toString()!=a.key)continue;
                const auto op=effect.value(k("op")).toString();if(op!=k("resource_upkeep")&&op!=k("heal_over_time"))continue;
                const double rate=effect.value(k("rate")).toDouble(effect.value(k("amount")).toDouble());
                const auto basis=effect.value(k("basis")).toString(k("flat"));
                const bool health=effect.value(k("resource_kind")).toString(effect.value(k("resource")).toString())==k("health");
                if(op==k("heal_over_time")) {const double r=rate*owner.support*std::max(0.,1+owner.stats[Stat::HealShield])*receivedSustainMultiplier(db,a,k("heal"));if(basis==k("current_fraction"))healCurrent+=r;else healFlat+=r*(basis==k("max_fraction")?a.maximum:1);}
                else if(health) {if(basis==k("current_fraction"))hpCurrent+=rate;else hpFlat+=rate*(basis==k("max_fraction")?a.maximum:1);}
                else {if(basis==k("current_fraction"))manaCurrent+=rate;else manaFlat+=rate*(basis==k("max_fraction")?a.stats[Stat::Mana]:1);}
            }
            if(a.hp>eps&&(manaFlat>0||manaCurrent>0)) {
                const auto paid=a.action.integrateResourceUpkeep(manaFlat,manaCurrent,a.action.time(),until,true);
                if(!paid.value(k("accepted")).toBool())a.unknowns.insert(k("state_upkeep_integration_rejected"));
                else {auto row=paid;row[k("type")]=k("stateful_resource_integral");row[k("player_key")]=a.key;trace.append(row);if(paid.value(k("exhausted")).toBool())a.stateful.onResourceExhausted(paid.value(k("active_until")).toDouble(until));}
            }
            if(a.hp>eps) {
                const double natural=naturalRegeneration?std::max(0.,a.stats[Stat::HpRegen])*(1+std::max(0.,a.stats[Stat::HpRegenPercent])):0;
                healFlat+=natural;const double startHp=a.hp,A=healFlat-hpFlat,B=hpCurrent-healCurrent;
                auto value=[&](double d){return std::abs(B)<1e-12?startHp+A*d:A/B+(startHp-A/B)*std::exp(-B*d);};
                double active=dt,endHp=value(dt);const double slope=A-B*startHp;
                const double bound=slope>0?a.maximum:0;
                if((slope>0&&endHp>a.maximum)||(slope<0&&endHp<0)) {
                    if(std::abs(B)<1e-12)active=(bound-startHp)/A;
                    else {const double ratio=(bound-A/B)/(startHp-A/B);active=ratio>0?-std::log(ratio)/B:dt;}
                    active=std::clamp(active,0.,dt);endHp=bound;
                }
                double integral=std::abs(B)<1e-12?startHp*active+.5*A*active*active:(A*active-(endHp-startHp))/B;
                integral=std::max(0.,integral);double spent=hpFlat*active+hpCurrent*integral;
                double grossHeal=healFlat*active+healCurrent*integral;
                if(endHp>=a.maximum-eps&&active<dt) {const double rest=dt-active,costRate=hpFlat+hpCurrent*a.maximum;spent+=costRate*rest;grossHeal+=costRate*rest;}
                const double effectiveHeal=std::max(0.,endHp-startHp+spent);
                a.hp=std::clamp(endHp,0.,a.maximum);a.cost+=spent;a.healed+=effectiveHeal;
                const double naturalGross=natural*(endHp<=eps?active:dt);
                a.naturalHealed+=grossHeal>0?effectiveHeal*std::min(1.,naturalGross/grossHeal):0;
                if(spent>0||effectiveHeal>0)trace.append(QJsonObject{{k("type"),k("stateful_health_integral")},{k("player_key"),a.key},{k("from"),from},{k("to"),until},{k("active_until"),from+active},{k("health_cost"),spent},{k("effective_healing"),effectiveHeal},{k("current_health"),a.hp}});
                if(a.hp<=eps) {a.deaths++;a.deathAt=from+active;a.stateful.onDeath(until);a.action.interruptCast(until);a.shields.clear();a.control=ContinuousControlState{};a.control.advanceTo(until);}
            }
            a.action.advance(until,a.hp>eps);a.stateContinuousAt=until;
        }
        for(const auto &dead:p)if(dead.hp<=eps)for(auto &recipient:p)recipient.control.notifyActorDeath(dead.key,until,true);
    };
    auto integrateItemZones = [&](double until) {
        if (!itemZoneModel) return;
        QMap<QString,double> ap;for(const auto &owner:p)ap[owner.key]=owner.stats[Stat::Ap];
        QVector<ItemZoneLedger::DamageInterval> intervals;QString error;
        if(!itemZones.advance(until,ap,&intervals,&error)){itemZoneFailure=error;return;}
        // This acknowledged rate-integral model settles the preceding contact
        // span before upkeep and the actions at its right boundary. It does not
        // claim a server tick phase or infer any contact from the R hit itself.
        for(const auto &span:intervals) {
            const int actor=find(p,span.ownerKey),target=find(p,span.targetKey);
            if(actor<0||target<0||p[target].hp<=eps)continue;
            auto &source=p[actor];auto &victim=p[target];
            for(const auto &owner:p)for(const auto &raw:owner.stateful.activeEffects(span.from)) {
                const auto effect=raw.toObject();if(effect.value(k("target_key"))!=victim.key)continue;
                if(effect.value(k("op"))==k("heal_over_time")||
                   (effect.value(k("op"))==k("resource_upkeep")&&effect.value(k("resource_kind")).toString(effect.value(k("resource")).toString())==k("health"))) {
                    itemZoneFailure=k("item_zone_parallel_health_integral_unsupported");return;
                }
            }
            auto dynamicModifiers=[&](const CombatEntry &entry) {
                for(const auto &raw:entry.effects) {
                    const auto effect=raw.toObject();const auto bytes=QJsonDocument(effect).toJson(QJsonDocument::Compact);
                    if(effect.contains(k("below_target_health"))||effect.value(k("target_current_hp")).toBool()||
                       bytes.contains("target_current_hp")||bytes.contains("self_current_hp")||bytes.contains("target_health_fraction")||bytes.contains("self_health_fraction"))return true;
                }return false;
            };
            for(int item:source.items)if(dynamicModifiers(db.items.value(item))){itemZoneFailure=k("item_zone_health_gated_modifier_requires_threshold_integral");return;}
            for(int augment:source.augments)if(dynamicModifiers(db.augments.value(augment))){itemZoneFailure=k("item_zone_health_gated_modifier_requires_threshold_integral");return;}
            QJsonObject event{{k("type"),k("item_zone_integral")},{k("player_key"),source.key},{k("target_key"),victim.key},
                {k("time"),until},{k("origin_family"),k("item3118")},{k("source_slot"),k("EFFECT")}};
            auto context=liveContext(source,&victim,event);
            const auto modifiers=evaluateCombat(db,context,source.items,source.augments);QMap<QString,double> amps;
            for(auto it=modifiers.damageTypeMultipliers.begin();it!=modifiers.damageTypeMultipliers.end();++it)amps[it.key()]=it.value().toDouble();
            const auto mitigated=mitigateCombatDamage(k("magic"),span.rawDamage,modifiers.stats,context,amps);
            if(!modifiers.valid||!mitigated){itemZoneFailure=k("item_zone_numeric_model_unavailable");return;}
            const double requested=*mitigated*(1+modifiers.copiedTrueDamageRatio)*modifiers.globalDamageMultiplier*source.output*victim.taken*stateDamageMultiplier(p,target,span.from);
            if(!std::isfinite(requested)||requested<0){itemZoneFailure=k("item_zone_invalid_damage");return;}
            if(requested<=eps)continue;
            if(source.stats[Stat::Omnivamp]>eps){itemZoneFailure=k("item_zone_omnivamp_conversion_not_integrated");return;}
            const QStringList outgoingEvents{k("champion_damage"),k("damage_hit"),k("magic_damage"),k("champion_magic_or_true_damage"),k("item_or_augment_damage")};
            const QStringList incomingEvents{k("incoming_damage"),k("health_cross_below"),k("magic_health_cross_below"),k("low_health_magic_or_true")};
            auto hasReactive=[&](const Fighter &owner,const QStringList &events) {
                for(int id:owner.items)if(events.contains(db.items.value(id).triggerContract.value(k("event")).toString()))return true;
                for(int id:owner.augments)if(events.contains(db.augments.value(id).triggerContract.value(k("event")).toString()))return true;
                return false;
            };
            // Equipment damage is not an AA, spell, or ultimate. Only gates
            // whose declared family includes this item damage are relevant;
            // ordinary lifesteal and spell-only healing remain outside it.
            if(hasReactive(source,outgoingEvents)||hasReactive(victim,incomingEvents)) {
                itemZoneFailure=k("item_zone_damage_reaction_channel_not_integrated");return;
            }
            for(const auto &owner:p)if(hasReactive(owner,QStringList{k("linked_ally_damage")})) {
                itemZoneFailure=k("item_zone_linked_damage_channel_not_integrated");return;
            }
            const double before=victim.hp,shieldBefore=shieldRemaining(victim),blocked=absorb(victim,requested),remaining=requested-blocked,actual=std::min(before,remaining);
            victim.hp-=actual;victim.received+=actual;source.dealt+=actual;source.shieldDealt+=blocked;
            const auto id=k("item3118:")+source.key+k(":")+victim.key+k(":")+QString::number(span.from,'g',17)+k(":")+QString::number(span.until,'g',17);
            QJsonObject receipt{{k("type"),k("damage_receipt")},{k("receipt_id"),k("damage:")+id},{k("event_id"),id},
                {k("time"),until},{k("source_key"),source.key},{k("target_key"),victim.key},
                {k("actual_damage"),actual},{k("shield_damage"),blocked},{k("requested_damage"),requested},
                {k("origin_family"),k("item3118")},{k("source_slot"),k("EFFECT")},{k("target_death"),victim.hp<=eps&&actual>0},
                {k("live_certified"),false}};
            source.damageReceipts[receipt.value(k("receipt_id")).toString()]=receipt;victim.damageReceipts[receipt.value(k("receipt_id")).toString()]=receipt;
            event[k("from")]=span.from;event[k("until")]=span.until;event[k("raw_damage")]=span.rawDamage;
            event[k("requested_damage")]=requested;event[k("actual_damage")]=actual;event[k("shield_damage")]=blocked;
            event[k("zone_ids")]=QJsonArray::fromStringList(span.zoneIds);event[k("target_mr")]=context.targetMr;event[k("accepted")]=true;event[k("live_certified")]=false;
            trace.append(event);trace.append(receipt);
            if(victim.hp<=eps&&actual>0){victim.deaths++;victim.deathAt=span.from+(span.until-span.from)*std::clamp((before+shieldBefore)/requested,0.,1.);victim.stateful.onDeath(until);victim.action.interruptCast(until);victim.shields.clear();victim.control=ContinuousControlState{};victim.control.advanceTo(until);itemZones.clearTargetContacts(victim.key,until);cancelPending(target,true,k("item_zone_target_death"));cancelTargetPending(target);}
        }
    };
    auto scheduleBinding = [&](int actor, int target, const QJsonObject &sourceEvent, const QJsonObject &row, const QString &owner) {
        auto &a = p[actor]; const auto slot = sourceEvent.value(k("slot")).toString();
        const auto actionPhase=row.value(k("required_action_phase")).toString();
        if(!actionPhase.isEmpty()&&sourceEvent.value(k("facts")).toObject().value(k("phase")).toString(k("primary"))!=actionPhase) return false;
        if(a.champion==27&&slot==k("Q")&&owner.startsWith(k("hero:"))) {
            const auto upkeep=a.action.upkeepState(); const auto contact=sourceEvent.value(k("facts")).toObject();
            if(sourceEvent.value(k("type")).toString()!=k("poison_contact") || !upkeep.value(k("active")).toBool() ||
               !contact.value(k("verified_poison_contact")).toBool() || contact.value(k("source_toggle_instance_id"))!=upkeep.value(k("instance_id"))) {
                a.unknowns.insert(k("verified_active_poison_contact_required"));return false;
            }
        }
        auto facts = sourceEvent.value(k("facts")).toObject();
        const auto overrides=facts.value(k("temporal_bindings")).toObject().value(row.value(k("binding_id")).toString()).toObject();
        for(auto it=overrides.begin();it!=overrides.end();++it) facts[it.key()]=it.value();
        auto inputEvent=sourceEvent;inputEvent[k("facts")]=facts;
        auto c = liveContext(a, target >= 0 ? &p[target] : nullptr, inputEvent);
        c.conditions.insert(k("continuous_action_verified")); c.conditions.insert(k("continuous_temporal_verified"));
        if (!requiredInputs(row, a, c, facts) || !requiredInputs(row.value(k("trigger")).toObject(),a,c,facts)) { a.unknowns.insert(k("temporal_binding_required_inputs_missing:") + row.value(k("binding_id")).toString()); return false; }
        const auto bounds=row.value(k("timing_fact_bounds")).toObject();for(auto it=bounds.begin();it!=bounds.end();++it) {const auto limit=it.value().toObject();if(!number(facts.value(it.key()),limit.value(k("minimum")).toDouble(0),limit.value(k("maximum")).toDouble(120))) {a.unknowns.insert(k("temporal_timing_fact_out_of_bounds"));return false;} }
        QString readyKey=owner+k(":")+row.value(k("binding_id")).toString();
        if(row.value(k("icd_scope")).toString()==k("slot_target")) readyKey+=k(":")+slot+k(":")+(target>=0?p[target].key:QString{});
        if(a.temporalReadyAt.value(readyKey,-1)>t+eps) {a.unknowns.insert(k("temporal_internal_cooldown_not_ready"));return false;}
        const auto id = sourceEvent.value(k("event_id")).toString() + k(":instance:") + owner + k(":") + row.value(k("binding_id")).toString();
        if (instances.contains(id)) { a.unknowns.insert(k("duplicate_temporal_cast_instance")); return false; }
        const auto rankSlot=row.value(k("rank_slot")).toString(slot); const int rank=rankSlot==k("P")||rankSlot==k("EFFECT")?1:a.context.ranks.value(rankSlot);
        auto duration = formula(row.value(k("duration_expression")).toObject(), a, c, rank);
        const double lifetime = duration.value_or(0);
        double activationDelay=0;
        if(row.contains(k("activation_delay_expression"))) {
            const auto activation=formula(row.value(k("activation_delay_expression")).toObject(),a,c,rank);
            if(!activation||*activation<0||*activation>120) {a.unknowns.insert(k("temporal_activation_delay_unavailable"));return false;}
            activationDelay=*activation;
        }
        const bool activationChannel=row.value(k("activation_requires_completed_channel")).toBool();
        if(activationChannel&&activationDelay<=eps) {a.unknowns.insert(k("positive_activation_channel_required"));return false;}
        double triggerDelay=0;
        if(row.contains(k("arm_time_expression"))) {
            const auto arm=formula(row.value(k("arm_time_expression")).toObject(),a,c,rank);
            if(!arm || !facts.value(k("verified_trap_trigger_time")).toBool() || !number(facts.value(k("trap_trigger_delay")),*arm,120)) {a.unknowns.insert(k("verified_armed_trap_trigger_time_required"));return false;}
            triggerDelay=facts.value(k("trap_trigger_delay")).toDouble();
            if(row.contains(k("source_empowered_lifetime_expression"))) {
                const auto life=formula(row.value(k("source_empowered_lifetime_expression")).toObject(),a,c,rank);
                if(!life||triggerDelay>*life+eps) {a.unknowns.insert(k("trap_trigger_after_source_lifetime"));return false;}
            }
        }
        QVector<double> offsets, scales;
        const auto timing = row.value(k("timing_mode")).toString(k("source_closed"));
        if (timing == k("verified_tick_schedule_required")) {
            if (!facts.value(k("verified_tick_schedule")).toBool() || lifetime <= 0) { a.unknowns.insert(k("verified_temporal_tick_schedule_required")); return false; }
            const auto at = facts.value(k("tick_offsets")).toArray(), weights = facts.value(k("tick_fractions")).toArray();
            if (at.isEmpty() || at.size() != weights.size() || at.size() > 1000) { a.unknowns.insert(k("invalid_temporal_tick_schedule")); return false; }
            double previous = -1, total = 0;
            for (int i=0;i<at.size();++i) { if (!number(at[i],0,lifetime) || at[i].toDouble() <= previous || !number(weights[i],eps,1)) { a.unknowns.insert(k("invalid_temporal_tick_schedule")); return false; } previous=at[i].toDouble(); total+=weights[i].toDouble(); offsets.append(previous); scales.append(weights[i].toDouble()); }
            if (total > 1 + eps) { a.unknowns.insert(k("temporal_tick_fraction_exceeds_total")); return false; }
            const auto constraints=row.value(k("tick_schedule_constraints_v6")).toObject();
            if((constraints.contains(k("first_offset_exact"))&&(!number(constraints.value(k("first_offset_exact")),0,lifetime)||std::abs(offsets.first()-constraints.value(k("first_offset_exact")).toDouble())>eps))||
               (constraints.contains(k("first_fraction_exact"))&&(!number(constraints.value(k("first_fraction_exact")),0,1)||std::abs(scales.first()-constraints.value(k("first_fraction_exact")).toDouble())>eps))||
               (constraints.contains(k("fraction_sum_exact"))&&(!number(constraints.value(k("fraction_sum_exact")),0,1)||std::abs(total-constraints.value(k("fraction_sum_exact")).toDouble())>eps))||
               (constraints.contains(k("tick_count_exact"))&&(!number(constraints.value(k("tick_count_exact")),1,1000)||offsets.size()!=constraints.value(k("tick_count_exact")).toDouble()))) {a.unknowns.insert(k("temporal_tick_schedule_source_constraint_failed"));return false;}
            if(row.contains(k("allocation_count_expression"))) {
                const auto count=formula(row.value(k("allocation_count_expression")).toObject(),a,c,rank);
                if(!count||*count<1||*count>1000||std::floor(*count)!=*count) {a.unknowns.insert(k("temporal_allocation_count_unavailable"));return false;}
                for(const double scale:scales)if(std::abs(scale**count-std::round(scale**count))>eps) {a.unknowns.insert(k("temporal_fractional_projectile_allocation_rejected"));return false;}
            }
        } else if (row.contains(k("interval_expression"))) {
            const auto interval = formula(row.value(k("interval_expression")).toObject(),a,c,rank);
            const auto count = formula(row.value(k("count_expression")).toObject(row.value(k("tick_count_expression")).toObject()),a,c,rank);
            auto first = formula(row.value(k("first_tick_offset_expression")).toObject(),a,c,rank);
            if (!first && facts.value(k("verified_first_tick_offset")).toBool() && number(facts.value(k("first_tick_offset")),0,120)) first=facts.value(k("first_tick_offset")).toDouble();
            if (!interval || *interval <= 0 || !count || std::floor(*count) != *count || *count<1 || *count>1000 || !first || *first > *interval + eps) { a.unknowns.insert(k("temporal_periodic_timing_unavailable")); return false; }
            for(int i=0;i<int(*count);++i) { const double at=*first+i**interval; if (lifetime>0 && at>lifetime+eps) { a.unknowns.insert(k("temporal_tick_exceeds_duration")); return false; } offsets.append(at); scales.append(1); }
        }
        auto phases = row.value(k("phases")).toArray();const int repeatedPhaseCount=phases.size();for(const auto &v:row.value(k("initial_phases")).toArray()) phases.append(v);
        if(row.value(k("phase_constraints")).toObject().value(k("return_after_outbound")).toBool() && !(facts.value(k("return_hit_delay")).toDouble(-1)>facts.value(k("outbound_hit_delay")).toDouble(-1))) {a.unknowns.insert(k("temporal_phase_order_invalid"));return false;}
        double previousDelay=-1;
        for(const auto &key:row.value(k("phase_constraints")).toObject().value(k("ordered_delay_keys")).toArray()) {
            if(!key.isString()||!number(facts.value(key.toString()),0,120)||facts.value(key.toString()).toDouble()<=previousDelay) {a.unknowns.insert(k("temporal_phase_order_invalid"));return false;}
            previousDelay=facts.value(key.toString()).toDouble();
        }
        if (phases.isEmpty() || phases.size()>100) { a.unknowns.insert(k("temporal_phase_definitions_missing")); return false; }
        QVector<Pending> staged; double last=0; const auto kind=row.value(k("kind")).toString(k("multiphase"));
        for(int phaseIndex=0;phaseIndex<phases.size();++phaseIndex) {
            const auto phase=phases[phaseIndex].toObject(); const auto phaseId=phase.value(k("phase_id")).toString();
            if(phaseId.isEmpty() || phase.value(k("component")).toObject().isEmpty()) { a.unknowns.insert(k("invalid_temporal_phase")); return false; }
            if(!requiredInputs(phase,a,c,facts)) {a.unknowns.insert(k("temporal_phase_required_inputs_missing:")+phaseId);continue;}
            bool phaseTimingValid=true;const auto phaseBounds=phase.value(k("timing_fact_bounds")).toObject();
            for(auto it=phaseBounds.begin();it!=phaseBounds.end();++it) {const auto bound=it.value().toObject();if(!number(facts.value(it.key()),bound.value(k("minimum")).toDouble(0),bound.value(k("maximum")).toDouble(120)))phaseTimingValid=false;}
            if(!phaseTimingValid) {a.unknowns.insert(k("temporal_phase_timing_fact_out_of_bounds"));continue;}
            QVector<double> phaseOffsets=phaseIndex<repeatedPhaseCount?offsets:QVector<double>{}, phaseScales=phaseIndex<repeatedPhaseCount?scales:QVector<double>{};
            if(phaseOffsets.isEmpty()) { const auto offset=formula(phase.value(k("offset_expression")).toObject(),a,c,rank); if(!offset || *offset>120) { a.unknowns.insert(k("temporal_phase_delay_unavailable")); return false; } phaseOffsets.append(*offset); phaseScales.append(1); }
            for(int i=0;i<phaseOffsets.size();++i) {
                auto event=inputEvent; auto component=phase.value(k("component")).toObject(); component[k("slot")]=slot;
                if(owner.startsWith(k("item:"))||owner.startsWith(k("augment:"))) {
                    component[k("rank_slot")]=k("EFFECT");component[k("max_rank")]=1;component[k("rate")]=k("proc_phase");
                }
                component[k("condition")]=k("continuous_temporal_verified");component[k("phase_id")]=phaseId;
                const auto phaseKey=phaseId+k(":")+QString::number(i); event[k("event_id")]=id+k(":phase:")+phaseKey; event[k("cast_instance_id")]=id;
                event[k("type")]=k("temporal_tick"); event[k("time")]=t+activationDelay+triggerDelay+phaseOffsets[i]; event[k("temporal_kind")]=kind; event[k("temporal_phase_id")]=phaseId; event[k("temporal_tick_index")]=i;
                event[k("_temporal_component")]=component; event[k("_temporal_scale")]=phaseScales[i]; event[k("_temporal_phase_key")]=phaseKey;
                event[k("_temporal_owner")]=owner; event[k("_temporal_binding_id")]=row.value(k("binding_id"));event[k("_temporal_existing_effect")]=kind==k("dot"); event.remove(k("prepared_support"));event.remove(k("_ledger_jhin_shot"));
                auto tickFacts=event.value(k("facts")).toObject();tickFacts.remove(k("proc_counts"));tickFacts.remove(k("verified_proc_counts"));event[k("facts")]=tickFacts;
                if(target>=0) event[k("target_key")]=p[target].key;
                Pending q; q.time=t+activationDelay+triggerDelay+phaseOffsets[i]; q.event=event; q.kind=kind; q.instance=id;
                q.locksActor=row.value(k("locks_actor")).toBool(kind==k("channel")); q.cancelOnInterrupt=row.value(k("cancel_on_interrupt")).toBool(kind==k("channel"));
                q.cancelOnSourceDeath=row.value(k("cancel_on_source_death")).toBool(kind!=k("dot"));q.cancelOnTargetDeath=row.value(k("cancel_on_target_death")).toBool(true); staged.append(q); last=std::max(last,phaseOffsets[i]);
            }
        }
        const double expires=t+activationDelay+triggerDelay+std::max(lifetime,last);
        if(activationChannel) {
            if(kind==k("channel")||!a.action.beginChannel(slot,t,t+activationDelay,id,true)) {a.unknowns.insert(k("temporal_activation_channel_unavailable"));return false;}
            Pending lock;lock.time=t+activationDelay;lock.kind=k("channel_end");lock.instance=id;lock.locksActor=true;
            lock.cancelOnInterrupt=false;lock.cancelOnSourceDeath=false;lock.cancelOnTargetDeath=false;
            lock.event={{k("event_id"),id+k(":activation_end")},{k("player_key"),a.key},{k("cast_instance_id"),id},{k("type"),k("channel_end")}};staged.append(lock);
        }
        if(kind==k("channel") && expires>t+eps) { if(!a.action.beginChannel(slot,t,expires,id,true)) { a.unknowns.insert(k("temporal_channel_lock_unavailable")); return false; } Pending lock;lock.time=expires;lock.kind=k("channel_end");lock.instance=id;lock.event={{k("event_id"),id+k(":end")},{k("player_key"),a.key},{k("cast_instance_id"),id},{k("type"),k("channel_end")}};staged.append(lock); }
        CastInstance instance; instance.id=id;instance.actor=a.key;instance.slot=slot;instance.binding=row.value(k("binding_id")).toString();instance.created=t;instance.expires=expires;
        instance.activationAt=t+activationDelay;instance.activationChannel=activationChannel;
        const auto refresh=row.value(k("stacking")).toString(row.value(k("refresh_policy")).toString());
        if(refresh==k("refresh")||refresh==k("refresh_without_stacking")) {
            QSet<QString> replaced;for(int i=pending.size()-1;i>=0;--i) {const auto e=pending[i].event;if(e.value(k("player_key")).toString()==a.key&&e.value(k("_temporal_owner")).toString()==owner&&e.value(k("_temporal_binding_id"))==row.value(k("binding_id"))&&e.value(k("target_key"))==sourceEvent.value(k("target_key"))) {replaced.insert(pending[i].instance);pending.removeAt(i);} }for(const auto &old:replaced) if(instances.contains(old)) instances[old].cancelled=true;
        }
        auto icd=formula(row.value(k("icd_expression")).toObject(),a,c,rank);if(!icd&&number(row.value(k("icd_seconds")),0,3600))icd=row.value(k("icd_seconds")).toDouble();
        if(icd) a.temporalReadyAt[readyKey]=t+*icd;
        instances[id]=instance; pending+=staged;
        QJsonArray phaseKeys;for(const auto &phase:staged)if(phase.event.value(k("type"))==k("temporal_tick"))phaseKeys.append(phase.event.value(k("_temporal_phase_key")));
        trace.append(QJsonObject{{k("type"),k("temporal_scheduled")},{k("time"),t},{k("player_key"),a.key},{k("cast_instance_id"),id},
            {k("source_event_id"),sourceEvent.value(k("event_id"))},{k("source_owner"),owner},{k("source_binding_id"),row.value(k("binding_id"))},
            {k("source_binding_sha256"),row.value(k("binding_sha256"))},{k("slot"),slot},{k("scheduled_phase_keys"),phaseKeys},
            {k("kind"),kind},{k("phase_count"),staged.size()},{k("expires_at"),expires},{k("accepted"),true}});
        return true;
    };
    while (t < end - eps && steps++ < 20000) {
        integrateItemZones(t);if(!itemZoneFailure.isEmpty())return invalid(itemZoneFailure);integrateState(t);for(auto &a:p)a.stateful.advance(t);syncStateBuffs(p,t,itemZoneModel?&itemZones:nullptr);
        for(auto &a:p)if(a.documentedCharge.value(k("active")).toBool()&&t>=a.documentedCharge.value(k("active_until")).toDouble()) {a.documentedCharge[k("active")]=false;a.documentedCharge[k("ended_at")]=a.documentedCharge.value(k("active_until"));a.documentedCharge[k("end_reason")]=k("maximum_charge_reached_without_observed_release_hit");}
        for (auto &a : p) { a.action.advance(t, a.hp > eps); a.control.advanceTo(t); expire(a, t); QString error; if (!refreshStats(db, a, t, &error)) a.unknowns.insert(error); }
        QVector<QJsonObject> now;
        while (cursor < events.size() && events[cursor].value(k("time")).toDouble() <= t + eps) now.append(events[cursor++]);
        // Administrative/control events take effect before casts at the same timestamp.
        std::stable_sort(now.begin(), now.end(), [](const QJsonObject &a, const QJsonObject &b) {
            const auto priority = [](const QJsonObject &x) { const auto type = x.value(k("type")).toString(); return type == k("death") || type == k("respawn") ? 0 : type == k("control") ? 1 : 2; };
            return priority(a) < priority(b);
        });
        QVector<QJsonObject> actions, administrativePayloads, deferredItemContacts;
        auto applyItemContact=[&](QJsonObject event,bool allowDeferred) {
            const int owner=find(p,event.value(k("player_key")).toString()),target=find(p,event.value(k("target_key")).toString());
            const auto action=event.value(k("action")).toString();
            if(!itemZoneModel){reject(event,k("item_zone_model_not_acknowledged"));return;}
            if(owner<0||target<0||p[owner].team==p[target].team||p[target].hp<=eps||
                (action!=k("enter")&&action!=k("exit"))||event.value(k("zone_id")).toString().isEmpty()) {
                reject(event,k("item_zone_contact_identity_or_action_invalid"));return;
            }
            ItemZoneLedger::Contact contact{event.value(k("event_id")).toString(),p[owner].key,event.value(k("zone_id")).toString(),p[target].key,t,action==k("enter")};
            QString error;
            if(!itemZones.setContact(contact,&error)) {
                // A contact at the exact first R timestamp is committed only
                // after that native R receipt exists, never before its damage.
                if(allowDeferred&&error==k("item_zone_contact_not_owned")){deferredItemContacts.append(event);return;}
                if(error==k("item_zone_cross_owner_curse_overlap_unverified"))itemZoneFailure=error;
                reject(event,error);return;
            }
            syncStateBuffs(p,t,&itemZones);for(auto &changed:p){QString refreshError;if(!refreshStats(db,changed,t,&refreshError))changed.unknowns.insert(refreshError);}
            event[k("accepted")]=true;event[k("conditional_model")]=k("explicit_contact_rate_integral");event[k("live_certified")]=false;
            event[k("opportunity_only")]=!p[owner].items.contains(3118);trace.append(event);++accepted;
        };
        for (auto event : now) {
            const auto id = event.value(k("event_id")).toString();
            if (processed.contains(id)) { reject(event, k("duplicate_event_id")); continue; } processed.insert(id);
            int actor = find(p, event.value(k("player_key")).toString());
            const auto type = event.value(k("type")).toString();
            if(type==k("item_zone_contact")){applyItemContact(event,true);continue;}
            if (type == k("control")) {
                auto control = event.value(k("control")).toObject(); if (control.isEmpty()) control = event;
                control[k("time")] = t; control[k("event_id")] = id;
                if (control.value(k("target")).toString().isEmpty()) control[k("target")] = event.value(k("target_key"));
                const int target = find(p, control.value(k("target")).toString());
                QString error;
                if (target < 0 || find(p, control.value(k("source")).toString()) < 0 || p[target].hp <= eps || !p[target].control.apply(control, p[target].stats, &error)) { reject(event, error.isEmpty() ? k("control_actor_or_target_unavailable") : error); continue; }
                event[k("accepted")] = true; event[k("target_key")] = p[target].key; trace.append(event); ++accepted;
                if (p[target].control.interruptsCast(t)) { p[target].action.interruptCast(t); p[target].stateful.onInterrupt(t); cancelPending(target,false,k("interrupting_control")); }
                continue;
            }
            if (actor < 0) { reject(event, k("actor_unavailable")); continue; }
            auto &a = p[actor];
            if (type == k("death")) {
                if (!event.value(k("verified_death")).toBool() || a.hp <= eps) { reject(event, k("verified_living_death_required")); continue; }
                a.externalLoss += a.hp; a.hp = 0; a.deaths++; a.deathAt = t; a.action.interruptCast(t); a.action.advance(t, false); a.shields.clear(); a.control = ContinuousControlState{}; a.control.advanceTo(t); a.stateful.onDeath(t);for(auto &recipient:p)recipient.control.notifyActorDeath(a.key,t,true);
                cancelPending(actor,true,k("source_death"));cancelTargetPending(actor);
                if(itemZoneModel)itemZones.clearTargetContacts(a.key,t);
                event[k("accepted")] = true; trace.append(event); ++accepted; continue;
            }
            if (type == k("respawn")) {
                if (!event.value(k("verified_respawn")).toBool() || a.hp > eps || !number(event.value(k("current_health")), eps, a.maximum)) { reject(event, k("explicit_verified_respawn_health_required")); continue; }
                a.hp = event.value(k("current_health")).toDouble(); a.restored += a.hp; a.action.advance(t, true);
                if(itemZoneModel)itemZones.clearTargetContacts(a.key,t);
                if (event.contains(k("resource_restore"))) a.action.restoreResource(event.value(k("resource_restore")).toDouble(), t, event.value(k("verified_resource_restore")).toBool());
                event[k("accepted")] = true; event[k("cooldowns_reset")] = false; trace.append(event); ++accepted; continue;
            }
            if (type == k("idle")) { event[k("accepted")] = true; trace.append(event); ++accepted; continue; }
            if(type==k("item_proc")) {
                const auto facts=event.value(k("facts")).toObject();const int item=event.value(k("item_id")).toInt();
                const auto profile=combatDrainProfile(db,a.context,a.stats,item);
                if(item!=2502||!a.items.contains(item)||a.hp<=eps||!profile.valid) {reject(event,k("owned_source_bound_drain_profile_required"));continue;}
                const double interval=profile.cooldown/(1+std::max(0.,a.stats[Stat::ItemHaste])/100.);
                const auto clock=k("chain:item:")+QString::number(item);
                if(facts.value(k("verified_periodic_trigger"))!=QJsonValue(true)||facts.value(k("in_champion_combat"))!=QJsonValue(true)||
                   !number(facts.value(k("combat_started_at")),0,t)||
                   facts.value(k("verified_target_ranges"))!=QJsonValue(true)||a.temporalReadyAt.value(clock,-1)>t+eps) {
                    reject(event,k("drain_requires_verified_combat_period_range_and_ready_clock"));continue;
                }
                const auto targets=event.value(k("target_keys")).toArray();const auto distances=facts.value(k("target_distances")).toObject();
                QSet<QString> seen;QVector<QJsonObject> planned;bool valid=!targets.isEmpty()&&targets.size()<=5;
                for(const auto &raw:targets) {
                    const auto key=raw.toString();const int target=find(p,key);
                    if(!raw.isString()||seen.contains(key)||target<0||p[target].team==a.team||p[target].hp<=eps||
                       !number(distances.value(key),0,profile.range)) {valid=false;break;}
                    seen.insert(key);
                    QJsonObject delivery{{k("op"),k("damage")},{k("recipient_key"),key},{k("amount"),profile.rawDamage},
                        {k("damage_type"),profile.damageType},{k("start_at"),t},{k("expires_at"),-1},{k("source"),k("item:2502:periodic")}};
                    QJsonObject payload{{k("type"),k("effect_delivery")},{k("event_id"),event.value(k("event_id")).toString()+k(":drain:")+key},
                        {k("time"),t},{k("player_key"),a.key},{k("target_key"),key},{k("_delivery"),delivery},
                        {k("_state_drain_multiplier"),profile.healingFromDamage},{k("_state_drain_item"),item},
                        {k("_state_drain_basis_verified"),facts.value(k("verified_heal_damage_basis"))},
                        {k("_state_drain_basis"),facts.value(k("heal_damage_basis"))}};
                    planned.append(payload);
                }
                if(!valid) {reject(event,k("drain_requires_explicit_alive_enemy_recipient_ranges"));continue;}
                a.temporalReadyAt[clock]=t+interval;administrativePayloads+=planned;
                event[k("accepted")]=true;event[k("phase")]=k("source_bound_drain_activation");event[k("next_proc_ready_at")]=t+interval;
                event[k("first_proc_phase_explicit")]=true;event[k("period_phase_basis")]=k("verified_periodic_trigger_at_event_time_not_combat_start_plus_cooldown");
                event[k("raw_damage_per_target")]=profile.rawDamage;event[k("healing_multiplier")]=profile.healingFromDamage;
                event[k("all_range_contacts_inferred")]=false;trace.append(event);++accepted;continue;
            }
            if (QStringList{k("level_up"), k("skill_allocate"), k("augment_select"), k("mechanic_observation"), k("observed_stack"), k("purchase"), k("sell")}.contains(type)) {
                Fighter updated = a; QString error;
                if(type==k("mechanic_observation")) {
                    const auto obs=event.value(k("observation")).isObject()?event.value(k("observation")).toObject():event;
                    if(obs.value(k("source"))==k("augment")&&obs.value(k("id")).toInt()==3116){
                        const auto receipt=obs.value(k("receipt")).toObject();const int recipient=find(p,obs.value(k("target")).toString());
                        if(receipt.value(k("actor_key"))!=a.key||obs.value(k("actor_key"))!=a.key||
                            receipt.value(k("actor_life_id"))!=QString::number(a.deathAt,'g',17)||recipient<0||p[recipient].team==a.team){reject(event,k("3116_observation_owner_epoch_or_target_mismatch"));continue;}
                        if(obs.value(k("event"))==k("consume_damage_resolved")&&receipt.value(k("damage_time")).toDouble(-1)<a.deathAt){reject(event,k("3116_external_damage_predates_owner_life"));continue;}
                        if(obs.value(k("event"))!=k("consume_damage_resolved")){
                            const auto block=stateTargetBlockReason(p,recipient,actor,t,event);
                            if(p[recipient].hp<=eps||!block.isEmpty()){reject(event,block.isEmpty()?k("3116_target_not_alive"):block);continue;}
                        }
                    }
                }
                if(type==k("mechanic_observation")&&!prepareMechanicClockObservation(db,updated,event,&error)) {reject(event,error);continue;}
                if(type==k("augment_select")&&!ordinaryAugmentOffer(catalog,a,event,&error)) {reject(event,error);continue;}
                if(type==k("purchase")&&event.contains(k("consume_item_ids"))) {
                    int item=0;if(!integer(event.value(k("item_id")),1,std::numeric_limits<int>::max(),item)) {reject(event,k("purchase_requires_observed_transaction"));continue;}
                    const auto quote=planInventoryPurchase(catalog,a.champion,a.level,a.progression.items(),item);
                    if(!quote.value(k("legal")).toBool()) {reject(event,k("purchase_catalog_gate:")+quote.value(k("reason")).toString());continue;}
                }
                if (!updated.progression.apply(db, event, &error, &a.context)) { reject(event, error); continue; }
                if(!refreshStats(db,updated,t,&error)) {reject(event,error);continue;}
                QJsonArray recipientEffects;QJsonObject clockReceipt;
                if(!consumeMechanicClockEffects(updated,updated.progression.lastDecision().value(k("new_effects")).toArray(),t,recipientEffects,clockReceipt,&error)) {reject(event,error);continue;}
                QJsonArray roster;for(int recipientIndex=0;recipientIndex<p.size();++recipientIndex) {const auto &recipient=recipientIndex==actor?updated:p[recipientIndex];roster.append(QJsonObject{{k("player_key"),recipient.key},{k("team"),recipient.team},{k("alive"),recipient.hp>eps},{k("current_health"),recipient.hp},{k("maximum_health"),recipient.maximum}});}
                auto observation=event.value(k("observation")).isObject()?event.value(k("observation")).toObject():event;observation[k("event_id")]=event.value(k("event_id"));
                const auto deliveryPlan=planMechanicDeliveryV4(recipientEffects,observation,a.key,roster,t);
                if(!deliveryPlan.value(k("accepted")).toBool()) {reject(event,deliveryPlan.value(k("error")).toString(k("unsupported_progression_recipient_effect")));continue;}
                QVector<Pending> stagedDeliveries;QVector<QJsonObject> immediateDeliveries;bool deliveryCostsAffordable=true;
                for(const auto &raw:deliveryPlan.value(k("deliveries")).toArray()) {
                    const auto delivery=raw.toObject();
                    if(delivery.value(k("op")).toString()==k("health_cost")) {const double cost=delivery.value(k("amount")).toDouble();if(delivery.value(k("start_at")).toDouble(t)>t+eps||updated.hp-cost<=eps) {deliveryCostsAffordable=false;break;}updated.hp-=cost;updated.cost+=cost;continue;}
                    QJsonObject payload{{k("event_id"),event.value(k("event_id")).toString()+k(":delivery:")+delivery.value(k("effect_id")).toString()}, {k("type"),k("effect_delivery")},{k("player_key"),a.key},{k("time"),delivery.value(k("start_at"))},{k("_delivery"),delivery}};
                    if(delivery.value(k("start_at")).toDouble(t)<=t+eps) immediateDeliveries.append(payload);
                    else {Pending q;q.time=delivery.value(k("start_at")).toDouble();q.event=payload;q.kind=k("mechanic_delivery");q.locksActor=false;q.cancelOnInterrupt=delivery.value(k("cancel_on_interrupt")).toBool(false);q.cancelOnSourceDeath=delivery.value(k("cancel_on_source_death")).toBool(true);q.cancelOnTargetDeath=true;q.event[k("target_key")]=delivery.value(k("recipient_key"));stagedDeliveries.append(q);}
                }
                if(!deliveryCostsAffordable) {reject(event,k("mechanic_delivery_health_cost_unaffordable"));continue;}
                pending+=stagedDeliveries;administrativePayloads+=immediateDeliveries;
                a = updated; event[k("accepted")] = true; event[k("progression_decision")] = a.progression.lastDecision();event[k("delivery_plan")]=deliveryPlan;
                if(!clockReceipt.isEmpty()) {event[k("action_clock_refunds")]=clockReceipt;a.unknowns.insert(k("mystic_punch_eligibility_and_slots_are_explicit_scenario_contract_min_cdr_unresolved"));}
                event[k("maximum_health")] = a.maximum; event[k("current_health")] = a.hp; event[k("resource_or_cooldowns_reset")] = false;
                trace.append(event); ++accepted; continue;
            }
            actions.append(event);
        }
        // Administrative interruption/death can remove a source's buffs from
        // other actors at this same instant, before their admitted actions.
        if(std::any_of(p.begin(),p.end(),[](const Fighter &player){return !player.stateBuffs.isEmpty();})) {syncStateBuffs(p,t,itemZoneModel?&itemZones:nullptr);for(auto &changed:p) {QString error;if(!refreshStats(db,changed,t,&error))changed.unknowns.insert(error);}}
        if (automatic && t + eps >= nextDecision) {
            for (int i = 0; i < p.size(); ++i) {
                const auto actorPolicy = p[i].team == objective ? policy : opponent;
                if (p[i].hp <= eps || actorPolicy == k("hold") || p[i].output <= 0) continue;
                for (const auto &slot : {k("Q"), k("W"), k("E"), k("R")}) actions.append(QJsonObject{{k("type"), k("cast")}, {k("time"), t}, {k("event_id"), k("auto:") + QString::number(autoIndex++)}, {k("player_key"), p[i].key}, {k("slot"), slot}, {k("hit_confirmed"), true}, {k("synthetic"), true}, {k("target_policy"), k("lowest_health_fraction")}});
                actions.append(QJsonObject{{k("type"), k("attack")}, {k("time"), t}, {k("event_id"), k("auto:") + QString::number(autoIndex++)}, {k("player_key"), p[i].key}, {k("hit_confirmed"), true}, {k("synthetic"), true}});
            }
            do { nextDecision += decisionStep; } while (nextDecision <= t + eps);
        }
        if(!itemZoneFailure.isEmpty())return invalid(itemZoneFailure);
        QVector<QJsonObject> payloads=administrativePayloads;
        auto drainPending = [&]() {
            for (int i = pending.size() - 1; i >= 0; --i) if (pending[i].time <= t + eps) {
                const auto entry=pending[i]; auto event=entry.event; pending.removeAt(i); const int actor=find(p,event.value(k("player_key")).toString());
                if (actor<0 || (entry.cancelOnSourceDeath && p[actor].hp<=eps)) { reject(event,k("cast_completion_actor_dead")); continue; }
                if (!entry.instance.isEmpty() && (!instances.contains(entry.instance) || instances[entry.instance].cancelled)) { reject(event,k("temporal_instance_cancelled")); continue; }
                event[k("time")]=t;
                if(entry.kind==k("stateful_completion")||entry.kind==k("stateful_transition")) {actions.append(event);continue;}
                if(entry.kind==k("channel_end")) { event[k("accepted")]=true;trace.append(event);continue; }
                if(entry.kind==k("cast_completion")) event[k("type")]=k("cast_complete");
                payloads.append(event);
            }
        };
        drainPending();
        QSet<QString> documentedFeesHandled;
        auto deliverDocumentedFees=[&](bool beforeRelease) {
            for(const auto &payload:payloads)if(payload.value(k("type")).toString()==k("documented_charge_fee_v9")) {
                const auto id=payload.value(k("event_id")).toString();if(documentedFeesHandled.contains(id))continue;
                const int actor=find(p,payload.value(k("player_key")).toString());if(actor<0)continue;
                const bool before=p[actor].documentedProfile.value(k("action_contract")).toObject().value(k("coincident_release_order")).toString()==k("payment_before_release");
                if(before!=beforeRelease)continue;trace.append(payDocumentedFee(p[actor],payload,t,instances));documentedFeesHandled.insert(id);
            }
        };
        deliverDocumentedFees(true);
        for (auto event : actions) {
            const int actor = find(p, event.value(k("player_key")).toString());
            if (actor < 0) { reject(event, k("actor_unavailable")); continue; }
            auto &a = p[actor]; auto type = event.value(k("type")).toString();
            if(!a.probeBaseAttackOnly&&(a.champion==89||a.champion==39)) {
                const auto binding=QString::number(a.champion)+k(":Q:hero_events_v1");
                if(!stateProgram(db,a,binding).isEmpty()) {
                    if(type==k("cast")&&event.value(k("slot")).toString()==k("Q")) {
                        type=k("skill_state");event[k("type")]=type;event[k("binding_id")]=binding;
                        const bool contact=a.champion==39&&event.value(k("facts")).toObject().contains(k("observed_hero_hit"));
                        event[k("transition_id")]=a.champion==89?k("arm"):(contact?k("dash_contact"):k("cast_no_contact"));
                        if(!contact)event[k("target_key")]=a.key;
                    } else if(a.champion==89&&type==k("attack")&&!event.contains(k("unit_target"))&&!event.contains(k("entity_target"))&&
                              a.stateful.snapshot().value(k("instances")).toObject().value(binding).toObject().value(k("state")).toString()==k("armed")) {
                        type=k("skill_state");event[k("type")]=type;event[k("binding_id")]=binding;
                        event[k("transition_id")]=k("empowered_hit");
                    }
                }
            }
            if(!a.probeBaseAttackOnly&&a.champion==111&&type==k("cast")&&event.value(k("slot")).toString()==k("W")&&
               !stateProgram(db,a,k("111:W:stateful_v7")).isEmpty()) {
                type=k("skill_state");event[k("type")]=type;event[k("binding_id")]=k("111:W:stateful_v7");
                event[k("transition_id")]=k("titans_wrath_shield");event[k("target_key")]=a.key;
                event[k("_state_routed_from_cast")]=true;
            }
            if(!a.probeBaseAttackOnly&&a.champion==111&&type==k("attack")&&!event.contains(k("unit_target"))&&!event.contains(k("entity_target"))&&
               event.value(k("hit_confirmed"))==QJsonValue(true)&&event.value(k("hit_factor")).toDouble(1)>=1-eps) {
                const auto instance=a.stateful.snapshot().value(k("instances")).toObject().value(k("111:W:stateful_v7")).toObject();
                double owned=0;for(const auto &shield:a.shields)if(shield.source==a.key+k(":mechanic:stateful:111:W:stateful_v7"))owned+=shield.amount;
                if(instance.value(k("state")).toString()==k("shielded")&&owned>eps) {
                    type=k("skill_state");event[k("type")]=type;event[k("binding_id")]=k("111:W:stateful_v7");
                    event[k("transition_id")]=k("enchanted_attack");event[k("_state_routed_from_attack")]=true;
                } else if(owned>eps&&instance.value(k("state")).toString()!=k("idle")) {
                    // Stage this consequence on the candidate action. A native
                    // rejection (attack timer/control/target) must preserve the
                    // previously admitted isolated DOT and its pending receipt.
                    event[k("_state_invalidate_w_isolated_dot")]=true;
                }
            }
            if(type==k("attack")&&event.contains(k("unit_target"))) {
                const auto unitId=event.value(k("unit_target")).toString();
                if(unitId.isEmpty()||event.contains(k("entity_target"))||(!event.value(k("target_key")).toString().isEmpty()&&event.value(k("target_key")).toString()!=unitId)||!units.contains(unitId)) {reject(event,k("unit_attack_target_unavailable"));continue;}
                const auto unit=units.unit(unitId);
                if(a.hp<=eps||!unit.value(k("alive")).toBool()||busyUntil(actor)>t+eps||!a.control.canAttackTarget(t,unitId)) {reject(event,k("unit_attack_actor_or_target_unavailable"));continue;}
                bool needsTargetSpeed=false;auto inspectSpeed=[&](const CombatEntry &entry) {for(const auto &raw:entry.effects)if(raw.toObject().value(k("op")).toString()==k("speed_damage"))needsTargetSpeed=true;};for(int id:a.items)inspectSpeed(db.items.value(id));for(int id:a.augments)inspectSpeed(db.augments.value(id));
                if(needsTargetSpeed&&!number(unit.value(k("movement_speed")),0,1e5)) {reject(event,k("unit_movement_speed_observation_required"));continue;}
                auto attacker=a;auto stagedUnits=units;const auto admission=attacker.action.attack(t,attacker.stats[Stat::As],true);
                if(!admission.value(k("accepted")).toBool()) {reject(event,admission.value(k("reason")).toString());continue;}
                auto context=liveContext(attacker,nullptr,event);context.explicitAttackCount=1;context.targetHp=unit.value(k("maximum_health")).toDouble();context.targetArmor=unit.value(k("armor")).toDouble();context.targetMr=unit.value(k("mr")).toDouble();context.targetMs=unit.value(k("movement_speed")).toDouble();context.counters[k("target_current_hp")]=unit.value(k("current_health")).toDouble();context.counters[k("target_health_fraction")]=unit.value(k("current_health")).toDouble()/context.targetHp;
                const auto evaluated=evaluateCombat(db,context,attacker.items,attacker.augments);const double requested=(evaluated.attackDamage+evaluated.skillDamage+evaluated.procDamage)*attacker.output*event.value(k("hit_factor")).toDouble(1);
                if(!evaluated.valid||!std::isfinite(requested)||requested<0) {reject(event,k("unit_attack_numeric_model_unavailable"));continue;}
                if(event.value(k("hit_confirmed"))!=QJsonValue(true)) {p[actor]=attacker;event[k("accepted")]=true;event[k("unit_damage")]=0;event[k("phase")]=k("unit_attack_withheld_hit_not_confirmed");event[k("action_admission")]=admission;trace.append(event);++accepted;continue;}
                QString error;auto receipt=stagedUnits.receiveDamage(unitId,attacker.key,event.value(k("event_id")).toString(),t,requested,&error);
                if(!receipt.value(k("accepted")).toBool()) {reject(event,error.isEmpty()?k("unit_damage_rejected"):error);continue;}
                const auto receiptId=receipt.value(k("receipt_id")).toString();attacker.damageReceipts[receiptId]=receipt;attacker.unitDealt+=receipt.value(k("actual_damage")).toDouble();attacker.unitShieldDealt+=receipt.value(k("shield_damage")).toDouble();attacker.unknowns.insert(k("npc_unit_is_verified_synthetic_observation_not_live_or_source_repair"));
                for(const auto &unknown:evaluated.unknowns)attacker.unknowns.insert(unknown);units=stagedUnits;p[actor]=attacker;
                event[k("accepted")]=true;event[k("phase")]=k("unit_attack_payload");event[k("unit_damage")]=receipt.value(k("actual_damage"));event[k("unit_shield_damage")]=receipt.value(k("shield_damage"));event[k("action_admission")]=admission;event[k("numeric_stats")]=evaluated.stats.json();event[k("unit_stats")]=unit;trace.append(event);trace.append(receipt);++accepted;
                if(receipt.value(k("actual_damage")).toDouble()>eps&&units.unit(unitId).value(k("alive")).toBool()) {
                    const int recipient=actor==0?1:0;const auto saved=p[recipient];const auto current=units.unit(unitId);p[recipient].key=unitId;p[recipient].hp=current.value(k("current_health")).toDouble();p[recipient].maximum=current.value(k("maximum_health")).toDouble();p[recipient].stats[Stat::Armor]=current.value(k("armor")).toDouble();p[recipient].stats[Stat::Mr]=current.value(k("mr")).toDouble();p[recipient].stats[Stat::Ms]=current.value(k("movement_speed")).toDouble();p[recipient].stateBuffs={};
                    auto triggerEvent=event;triggerEvent[k("target_key")]=unitId;triggerEvent[k("slot")]=k("P");auto hitFacts=event.value(k("facts")).toObject();hitFacts[k("first_hit_confirmed")]=true;hitFacts[k("confirmed_hit")]=true;triggerEvent[k("facts")]=hitFacts;
                    auto ownerRows=[&](const QMap<int,CombatEntry> &pool,const QList<int> &owned,const QString &kind) {for(int id:owned)for(const auto &raw:pool.value(id).temporalBindingsV4) {const auto row=raw.toObject(),trigger=row.value(k("trigger")).toObject();if(trigger.value(k("type")).toString()==k("spell_hit"))continue;if(!trigger.value(k("slots")).toArray().isEmpty()&&!trigger.value(k("slots")).toArray().contains(k("P")))continue;scheduleBinding(actor,recipient,triggerEvent,row,kind+k(":")+QString::number(id));}};
                    ownerRows(db.items,p[actor].items,k("item"));ownerRows(db.augments,p[actor].augments,k("augment"));p[recipient]=saved;drainPending();
                }
                continue;
            }
            if(type==k("attack")&&event.value(k("entity_target")).isObject()) {
                const auto targetSpec=event.value(k("entity_target")).toObject();const int recipient=find(p,targetSpec.value(k("owner_key")).toString());
                const auto binding=targetSpec.value(k("binding_id")).toString(),entityKey=targetSpec.value(k("entity_key")).toString();
                if(recipient<0||p[recipient].team==a.team||a.hp<=eps||busyUntil(actor)>t+eps||!a.control.canAttack(t)||!a.control.forcedAttackTarget(t).isEmpty()) {reject(event,k("entity_attack_actor_or_target_unavailable"));continue;}
                auto staged=p;auto &attacker=staged[actor];auto &targetOwner=staged[recipient];auto entity=targetOwner.stateful.snapshot().value(k("instances")).toObject().value(binding).toObject().value(k("entities")).toObject().value(entityKey).toObject();
                if(entity.isEmpty()||entity.value(k("owner_key")).toString()!=targetOwner.key||entity.value(k("binding_id")).toString()!=binding) {reject(event,k("stateful_owned_entity_required"));continue;}
                auto facts=event.value(k("facts")).toObject();QString error;
                if(!entity.value(k("hp_known")).toBool()) {
                    if(facts.value(k("verified_entity_health_observation"))!=QJsonValue(true)||!targetOwner.stateful.seedObservedEntityStats(binding,entityKey,event.value(k("event_id")).toString()+k(":observation"),t,facts.value(k("entity_observed_stats")).toObject(),&error)) {reject(event,error.isEmpty()?k("stateful_entity_health_observation_required"):error);continue;}
                    entity=targetOwner.stateful.snapshot().value(k("instances")).toObject().value(binding).toObject().value(k("entities")).toObject().value(entityKey).toObject();targetOwner.unknowns.insert(k("entity_health_is_verified_scenario_observation_not_source_numeric_repair"));
                }
                auto entityStats=entityCombatStats(targetOwner,binding,entityKey,entity);bool resistancesKnown=true;
                for(const auto &stat:{k("armor"),k("mr"),k("ms")})if(!entityStats.value(stat).isDouble())resistancesKnown=false;
                if(!resistancesKnown) {
                    const auto observed=facts.value(k("entity_observed_stats")).toObject();
                    if(facts.value(k("verified_entity_resistances"))!=QJsonValue(true)||!number(observed.value(k("armor")),-1e4,1e6)||!number(observed.value(k("mr")),-1e4,1e6)||!number(observed.value(k("ms")),0,1e5)) {reject(event,k("stateful_entity_resistances_observation_required"));continue;}
                    for(const auto &stat:{k("armor"),k("mr"),k("ms")})if(!entityStats.value(stat).isDouble())entityStats[stat]=observed.value(stat);
                    targetOwner.entityObservations[binding+k("|")+entityKey]=QJsonObject{{k("entity_created_at"),entity.value(k("created_at"))},{k("observation_id"),event.value(k("event_id"))},{k("time"),t},{k("stats"),entityStats}};
                    attacker.unknowns.insert(k("entity_resistances_are_verified_scenario_observations_not_source_numeric_repair"));
                }
                const auto admission=attacker.action.attack(t,attacker.stats[Stat::As],true);if(!admission.value(k("accepted")).toBool()) {reject(event,admission.value(k("reason")).toString());continue;}
                auto context=liveContext(attacker,nullptr,event);context.explicitAttackCount=1;context.targetHp=entity.value(k("maximum_hp")).toDouble();context.targetArmor=entityStats.value(k("armor")).toDouble();context.targetMr=entityStats.value(k("mr")).toDouble();context.targetMs=entityStats.value(k("ms")).toDouble();context.counters[k("target_current_hp")]=entity.value(k("hp")).toDouble();context.counters[k("target_health_fraction")]=entity.value(k("hp")).toDouble()/context.targetHp;
                const auto evaluated=evaluateCombat(db,context,attacker.items,attacker.augments);const double requested=(evaluated.attackDamage+evaluated.skillDamage+evaluated.procDamage)*attacker.output*event.value(k("hit_factor")).toDouble(1);
                if(!evaluated.valid||!std::isfinite(requested)||requested<0) {reject(event,k("stateful_entity_attack_numeric_model_unavailable"));continue;}
                if(event.value(k("hit_confirmed"))!=QJsonValue(true)) {p[actor]=attacker;event[k("accepted")]=true;event[k("damage")]=0;event[k("phase")]=k("entity_attack_withheld_hit_not_confirmed");trace.append(event);++accepted;continue;}
                const auto receiptId=k("entity-damage:")+event.value(k("event_id")).toString()+k(":")+targetOwner.key+k(":")+binding+k(":")+entityKey;
                auto receipt=targetOwner.stateful.receiveEntityDamage(binding,entityKey,receiptId,attacker.key,t,requested,&error);
                if(!receipt.value(k("accepted")).toBool()) {reject(event,error);continue;}
                receipt[k("type")]=k("entity_damage_receipt");receipt[k("receipt_id")]=receiptId;receipt[k("event_id")]=event.value(k("event_id"));receipt[k("time")]=t;receipt[k("source_key")]=attacker.key;receipt[k("target_key")]=entityKey;receipt[k("target_owner_key")]=targetOwner.key;receipt[k("shield_damage")]=0;receipt[k("requested_damage")]=requested;
                attacker.damageReceipts[receiptId]=receipt;targetOwner.damageReceipts[receiptId]=receipt;attacker.entityDealt+=receipt.value(k("actual_damage")).toDouble();targetOwner.entityReceived+=receipt.value(k("actual_damage")).toDouble();
                for(const auto &u:evaluated.unknowns)attacker.unknowns.insert(u);p=staged;
                event[k("accepted")]=true;event[k("phase")]=k("entity_attack_payload");event[k("entity_damage")]=receipt.value(k("actual_damage"));event[k("action_admission")]=admission;event[k("numeric_stats")]=evaluated.stats.json();event[k("entity_stats")]=entityStats;trace.append(event);trace.append(receipt);++accepted;
                if(receipt.value(k("actual_damage")).toDouble()>eps&&receipt.value(k("hp_after")).toDouble()>eps) {
                    // Reuse the same source-bound temporal scheduler with a
                    // temporary recipient view. The queued payload retains
                    // entity_target, so it never damages the owner champion.
                    const auto saved=p[recipient];p[recipient].key=entityKey;p[recipient].hp=receipt.value(k("hp_after")).toDouble();p[recipient].maximum=entity.value(k("maximum_hp")).toDouble();p[recipient].stats[Stat::Armor]=entityStats.value(k("armor")).toDouble();p[recipient].stats[Stat::Mr]=entityStats.value(k("mr")).toDouble();p[recipient].stats[Stat::Ms]=entityStats.value(k("ms")).toDouble();
                    auto triggerEvent=event;triggerEvent[k("target_key")]=entityKey;triggerEvent[k("slot")]=k("P");auto hitFacts=event.value(k("facts")).toObject();hitFacts[k("first_hit_confirmed")]=true;hitFacts[k("confirmed_hit")]=true;triggerEvent[k("facts")]=hitFacts;
                    auto ownerRows=[&](const QMap<int,CombatEntry> &pool,const QList<int> &owned,const QString &kind) {for(int id:owned)for(const auto &raw:pool.value(id).temporalBindingsV4) {const auto row=raw.toObject(),trigger=row.value(k("trigger")).toObject();if(trigger.value(k("type")).toString()==k("spell_hit"))continue;if(!trigger.value(k("slots")).toArray().isEmpty()&&!trigger.value(k("slots")).toArray().contains(k("P")))continue;scheduleBinding(actor,recipient,triggerEvent,row,kind+k(":")+QString::number(id));}};
                    ownerRows(db.items,p[actor].items,k("item"));ownerRows(db.augments,p[actor].augments,k("augment"));p[recipient]=saved;drainPending();
                }
                continue;
            }
            if(type==k("skill_state")) {
                const auto program=stateProgram(db,a,event.value(k("binding_id")).toString());
                const auto transition=stateTransition(program,event.value(k("transition_id")).toString());
                if(program.isEmpty()||transition.isEmpty()) {reject(event,k("stateful_unknown_binding_or_transition"));continue;}
                if(a.champion==111&&event.value(k("binding_id")).toString()==k("111:W:stateful_v7")&&
                   event.value(k("transition_id")).toString()==k("titans_wrath_shield")&&
                   a.stateful.snapshot().value(k("instances")).toObject().value(k("111:W:stateful_v7")).toObject().value(k("state")).toString()==k("dot_attached")) {
                    a.unknowns.insert(k("nautilus_w_pending_dot_rearm_requires_independent_effect_instance_policy"));
                    reject(event,k("nautilus_w_pending_dot_rearm_policy_unverified"));continue;
                }
                const auto before=a.stateful.snapshot();QVector<Fighter> staged=p;auto &owner=staged[actor];
                auto facts=event.value(k("facts")).toObject();const bool completion=event.value(k("_state_cast_completion")).toBool(),cast=transition.value(k("requires_admitted_cast")).toBool(),attack=transition.value(k("requires_admitted_attack")).toBool();
                auto slot=transition.value(k("admission_slot")).toString(program.value(k("admission_slot")).toString(program.value(k("slot")).toString()));
                const auto admissionSlots=transition.value(k("admission_slots")).toArray();
                if(!admissionSlots.isEmpty()) {
                    const auto requestedSlot=event.value(k("admission_slot")).toString();
                    if(!cast||!admissionSlots.contains(requestedSlot)||!QStringList{k("Q"),k("W"),k("E"),k("R")}.contains(requestedSlot)) {reject(event,k("stateful_admission_slot_mismatch"));continue;}
                    slot=requestedSlot;
                } else if(event.contains(k("admission_slot"))&&event.value(k("admission_slot")).toString()!=slot) {reject(event,k("stateful_admission_slot_mismatch"));continue;}
                if(transition.value(k("target_policy")).toString()==k("self")&&event.value(k("target_key")).toString().isEmpty())event[k("target_key")]=owner.key;
                if(attack&&event.value(k("target_key")).toString().isEmpty()) {const auto forced=owner.control.forcedAttackTarget(t);if(!forced.isEmpty())event[k("target_key")]=forced;else {const int enemy=enemyTarget(staged,actor,owner.team==objective?policy:opponent,event);if(enemy>=0)event[k("target_key")]=staged[enemy].key;}}
                const int target=find(staged,event.value(k("target_key")).toString());
                QJsonObject admission=completion?event.value(k("_state_admission")).toObject():QJsonObject{{k("accepted"),true}};double nativeHealthCost=0;
                if((cast||attack)&&!completion) {
                    if(owner.hp<=eps) {reject(event,k("actor_dead"));continue;}
                    if(busyUntil(actor)>t+eps) {reject(event,k("cast_in_progress"));continue;}
                    if((cast&&!owner.control.canCast(t))||(attack&&(target<0||staged[target].hp<=eps||staged[target].team==owner.team||!owner.control.canAttackTarget(t,staged[target].key)))) {reject(event,cast?k("control_blocks_cast"):k("control_blocks_attack_or_forced_target"));continue;}
                    if(cast&&slot==k("R")&&hasFlag(db,owner,k("seal_r"))) {reject(event,k("augment_seals_ultimate"));continue;}
                    if(cast) {
                        const auto rule=actionRule(db,owner,slot,facts);const auto clocks=actionClockStats(db,owner);
                        if(!clocks) {reject(event,k("ultimate_haste_source_value_unavailable"));continue;}
                        const bool documentedHealth=owner.documentedProfile.value(k("program")).toObject().value(k("binding_id"))==event.value(k("binding_id"))&&owner.documentedProfile.value(k("action_contract")).toObject().value(k("kind")).toString()==k("documented_health_schedule_v9");
                        if(!documentedHealth&&(rule.value(k("requires_verified_health_cost")).toBool()||rule.value(k("requires_observed_health_cost")).toBool())) {
                            if(!facts.value(k("verified_health_cost")).toBool()||!number(facts.value(k("health_cost")),0,owner.maximum)||owner.hp-facts.value(k("health_cost")).toDouble()<=eps) {reject(event,k("verified_health_cost_required"));continue;}
                            nativeHealthCost=facts.value(k("health_cost")).toDouble();
                        }
                        if(documentedHealth)admission=owner.action.castSourceHealthScheduleV9(slot,t,*clocks,facts,owner.documentedProfile.value(k("profile_id")).toString(),owner.documentedProfile.value(k("profile_sha256")).toString(),event.value(k("binding_id")).toString(),program.value(k("program_sha256")).toString());
                        else if(program.value(k("source_upkeep_only_v7")).isObject()&&rule.value(k("continuously_managed_resource_v7")).toObject().value(k("binding_id"))==event.value(k("binding_id")))admission=owner.action.castSourceUpkeep(slot,t,*clocks,facts,event.value(k("binding_id")).toString(),program.value(k("program_sha256")).toString());
                        else admission=owner.action.cast(slot,t,*clocks,facts);
                    } else admission=owner.action.attack(t,owner.stats[Stat::As],true);
                    if(!admission.value(k("accepted")).toBool()) {reject(event,admission.value(k("reason")).toString(k("stateful_action_admission_failed")));continue;}
                    owner.hp-=nativeHealthCost;owner.cost+=nativeHealthCost;
                }
                const auto admittedOwner=owner;const auto actionState=owner.action.snapshot();double ownedShield=0;
                if(program.value(k("hero_event_contract_v1")).toBool()&&cast&&!completion&&
                   (facts.value(k("verified_hero_cast_timing"))!=QJsonValue(true)||
                    !number(facts.value(k("hero_cast_startup_seconds")),0,3600)||
                    std::abs(facts.value(k("hero_cast_startup_seconds")).toDouble()-(admission.value(k("cast_complete_at")).toDouble(t)-t))>eps)) {
                    reject(event,k("hero_event_observed_cast_timing_required"));continue;
                }
                for(const auto &shield:owner.shields)if(shield.source==owner.key+k(":mechanic:stateful:")+event.value(k("binding_id")).toString())ownedShield+=shield.amount;
                QJsonObject runtime{{k("owner_alive"),owner.hp>eps},{k("owned_death_receipt"),false},{k("self_current_hp"),owner.hp},{k("self_max_hp"),owner.maximum},
                    {k("self_current_mana"),actionState.value(k("resource_current"))},{k("target_key"),event.value(k("target_key"))},{k("cast_admitted"),cast},{k("attack_admitted"),attack},{k("owned_shield_remaining"),ownedShield},
                    {k("base_attack_raw"),owner.stats[Stat::Ad]*(1+owner.stats[Stat::Crit]*std::max(0.,owner.stats[Stat::CritDamage]-1))},{k("current_form"),actionState.value(k("form"))}};
                if(cast&&admission.value(k("accepted")).toBool()) {runtime[k("cast_resource_cost")]=admission.value(k("resource_cost")).toDouble();runtime[k("cast_admission_event_id")]=event.value(k("event_id"));runtime[k("cast_admission_slot")]=slot;runtime[k("cast_admission_time")]=admission.value(k("time")).toDouble(t);}
                else {const auto activation=before.value(k("instances")).toObject().value(event.value(k("binding_id")).toString()).toObject().value(k("activation_cast_receipt")).toObject();if(activation.value(k("owner_key")).toString()==owner.key&&activation.value(k("binding_id"))==event.value(k("binding_id"))&&number(activation.value(k("resource_cost")),0,1e12)&&number(activation.value(k("time")),0,t)) {runtime[k("cast_resource_cost")]=activation.value(k("resource_cost"));runtime[k("cast_resource_cost_receipt")]=activation;}}
                if(attack&&!cast&&admission.value(k("accepted")).toBool()) {runtime[k("owner_attack_ready_at")]=actionState.value(k("attack_ready_at"));runtime[k("owner_attack_receipt_id")]=event.value(k("event_id"));runtime[k("owner_attack_owner_key")]=owner.key;runtime[k("owner_attack_receipt_time")]=t;}
                if(transition.value(k("entity_attack_clock")).toString()==k("verified_observation")) {
                    const auto observation=transition.value(k("entity_attack_interval_observation")).toObject();const auto verifiedKey=observation.value(k("verification_fact")).toString(),intervalKey=observation.value(k("interval_fact")).toString();
                    const double minimum=observation.value(k("minimum")).toDouble(observation.value(k("min")).toDouble(.001)),maximum=observation.value(k("maximum")).toDouble(observation.value(k("max")).toDouble(300));
                    if(!verifiedKey.isEmpty()&&!intervalKey.isEmpty()&&facts.value(verifiedKey)==QJsonValue(true)&&number(facts.value(intervalKey),minimum,maximum)) {
                        runtime[k("entity_attack_interval_verified")]=true;runtime[k("entity_attack_interval")]=facts.value(intervalKey);runtime[k("entity_attack_interval_observation_id")]=event.value(k("event_id"));runtime[k("entity_attack_interval_owner_key")]=owner.key;runtime[k("entity_attack_interval_binding_id")]=event.value(k("binding_id"));runtime[k("entity_attack_interval_entity_key")]=event.value(k("entity_key")).toString(event.value(k("entity_id")).toString());runtime[k("entity_attack_interval_observation_time")]=t;
                    }
                }
                if(transition.value(k("requires_timer_observation")).toString()==k("awakening")&&facts.value(k("verified_awakening_cooldown_started"))==QJsonValue(true)) {runtime[k("owned_timer_observed")]=true;runtime[k("owned_timer_observation_id")]=event.value(k("event_id"));runtime[k("owned_timer_owner_key")]=owner.key;runtime[k("owned_timer_binding_id")]=event.value(k("binding_id"));runtime[k("owned_timer_key")]=k("awakening");runtime[k("owned_timer_observation_time")]=t;}
                if(target>=0) {runtime[k("target_current_hp")]=staged[target].hp;runtime[k("target_max_hp")]=staged[target].maximum;runtime[k("target_team")]=staged[target].team;}
                const auto heroHitContract=transition.value(k("hero_hit_contract_v1")).toObject();
                if(!heroHitContract.isEmpty()) {
                    if(event.value(k("hit_confirmed"))!=QJsonValue(true)||event.value(k("hit_factor")).toDouble(1)<1-eps||
                       event.contains(k("unit_target"))||event.contains(k("entity_target"))||target<0||staged[target].hp<=eps||
                       staged[target].team==owner.team||(!cast&&!attack)||
                       (heroHitContract.value(k("same_event_cast_contact")).toBool()&&admission.value(k("cast_complete_at")).toDouble(t)>t+eps)) {
                        reject(event,k("hero_event_actual_native_champion_contact_required"));continue;
                    }
                    const auto targetBlock=stateTargetBlockReason(staged,target,actor,t,event);
                    if(!targetBlock.isEmpty()) {reject(event,targetBlock);continue;}
                    auto hit=facts.value(k("observed_hero_hit")).toObject();
                    // Runtime provenance comes only from this admitted action;
                    // public facts cannot supply the internal envelope.
                    hit[k("binding_id")]=event.value(k("binding_id"));runtime[k("hero_hit_receipt_v1")]=hit;
                }
                if(target>=0&&staged[target].hp<=eps)for(auto it=owner.damageReceipts.begin();it!=owner.damageReceipts.end();++it) {const auto death=it.value().toObject();const auto instance=before.value(k("instances")).toObject().value(event.value(k("binding_id")).toString()).toObject();if(death.value(k("source_key")).toString()==owner.key&&death.value(k("target_key")).toString()==staged[target].key&&death.value(k("target_death")).toBool()&&death.value(k("actual_damage")).toDouble()>0&&death.value(k("time")).toDouble()>=instance.value(k("started_at")).toDouble()) {runtime[k("owned_death_receipt")]=true;runtime[k("owned_death_time")]=death.value(k("time"));runtime[k("owned_death_target_key")]=death.value(k("target_key"));runtime[k("owned_death_receipt_id")]=death.value(k("receipt_id"));runtime[k("kill_receipt_id")]=death.value(k("kill_receipt_id"));runtime[k("kill_receipt_time")]=death.value(k("time"));runtime[k("kill_target_key")]=death.value(k("target_key"));runtime[k("kill_target_type")]=k("champion");runtime[k("kill_source_key")]=death.value(k("kill_source_key"));runtime[k("kill_participant_keys")]=death.value(k("kill_participant_keys"));}}
                const auto requestedKill=event.value(k("kill_receipt_id")).toString();
                if(!requestedKill.isEmpty()||event.value(k("runtime_kill_receipt_from_previous_damage"))==QJsonValue(true)) {
                    QJsonObject kill;const auto instance=before.value(k("instances")).toObject().value(event.value(k("binding_id")).toString()).toObject();const bool takedown=transition.value(k("kill_receipt_subject")).toString()==k("takedown");
                    for(auto it=owner.damageReceipts.begin();it!=owner.damageReceipts.end();++it) {
                        auto candidate=it.value().toObject();const auto killId=candidate.value(k("kill_receipt_id")).toString(),victim=candidate.value(k("target_key")).toString();
                        if(killId.isEmpty()||(!requestedKill.isEmpty()&&killId!=requestedKill)||candidate.value(k("source_key")).toString()!=owner.key||!candidate.value(k("target_death")).toBool()||candidate.value(k("actual_damage")).toDouble()<=0||!number(candidate.value(k("time")),instance.value(k("started_at")).toDouble(),t))continue;
                        if(candidate.value(k("kill_source_key")).toString()!=owner.key&&(!takedown||!candidate.value(k("kill_participant_keys")).toArray().contains(owner.key)))continue;
                        if(event.contains(k("kill_target_key"))&&event.value(k("kill_target_key")).toString()!=victim)continue;
                        const int victimPlayer=find(staged,victim);if(victimPlayer>=0) {if(staged[victimPlayer].team==owner.team)continue;candidate[k("kill_target_type")]=k("champion");}
                        else if(!units.contains(victim)||units.receipt(candidate.value(k("receipt_id")).toString())!=candidate)continue;
                        if(candidate.value(k("kill_target_type")).toString().isEmpty())continue;
                        if(kill.isEmpty()||candidate.value(k("time")).toDouble()>kill.value(k("time")).toDouble()||(candidate.value(k("time"))==kill.value(k("time"))&&killId>kill.value(k("kill_receipt_id")).toString()))kill=candidate;
                    }
                    if(kill.isEmpty()) {reject(event,k("stateful_kill_receipt_unavailable"));continue;}
                    runtime[k("kill_receipt_id")]=kill.value(k("kill_receipt_id"));runtime[k("kill_receipt_time")]=kill.value(k("time"));runtime[k("kill_target_key")]=kill.value(k("target_key"));runtime[k("kill_target_type")]=kill.value(k("kill_target_type"));runtime[k("kill_source_key")]=kill.value(k("kill_source_key"));runtime[k("kill_participant_keys")]=kill.value(k("kill_participant_keys"));
                }
                QJsonObject receipt;const auto requestedReceipt=event.value(k("damage_receipt_id")).toString();
                if(!requestedReceipt.isEmpty())receipt=owner.damageReceipts.value(requestedReceipt).toObject();
                else if(event.value(k("runtime_receipt_from_previous_damage")).toBool()) {
                    const bool incoming=event.value(k("runtime_receipt_direction")).toString()==k("incoming");const auto entityKey=event.value(k("entity_key")).toString(event.value(k("entity_id")).toString());
                    for(auto it=owner.damageReceipts.begin();it!=owner.damageReceipts.end();++it) {const auto candidate=it.value().toObject();
                        const bool entityReceipt=!entityKey.isEmpty()&&candidate.value(k("damage_target_key")).toString()==entityKey&&candidate.value(k("damage_target_owner_key")).toString()==owner.key&&candidate.value(k("damage_target_binding_id"))==event.value(k("binding_id"));
                        const bool matches=entityReceipt||(incoming?candidate.value(k("target_key")).toString()==owner.key&&(target<0||target==actor||candidate.value(k("source_key")).toString()==staged[target].key):candidate.value(k("source_key")).toString()==owner.key&&(target<0||candidate.value(k("target_key")).toString()==staged[target].key));
                        if(event.contains(k("receipt_source_key"))&&candidate.value(k("source_key"))!=event.value(k("receipt_source_key")))continue;
                        if(matches&&candidate.value(k("time")).toDouble()<=t&&(receipt.isEmpty()||candidate.value(k("time")).toDouble()>receipt.value(k("time")).toDouble()||
                            (candidate.value(k("time"))==receipt.value(k("time"))&&candidate.value(k("receipt_id")).toString()>receipt.value(k("receipt_id")).toString())))receipt=candidate;
                    }
                }
                if(!receipt.isEmpty()) {runtime[k("actual_damage")]=receipt.value(k("actual_damage"));runtime[k("damage_receipt_id")]=receipt.value(k("receipt_id"));runtime[k("damage_receipt_time")]=receipt.value(k("time"));runtime[k("damage_source_key")]=receipt.value(k("source_key"));runtime[k("damage_target_key")]=receipt.value(k("target_key"));for(const auto &field:{k("damage_target_key"),k("damage_target_owner_key"),k("damage_target_binding_id"),k("entity_death_receipt"),k("entity_death_receipt_id"),k("entity_death_entity_key"),k("entity_death_owner_key"),k("entity_death_binding_id"),k("entity_death_time")})if(receipt.contains(field))runtime[field]=receipt.value(field);}
                QString error;const auto context=liveContext(owner,target>=0?&staged[target]:nullptr,event);
                const auto decision=owner.stateful.apply(event,context,owner.stats,runtime,&error);
                event[k("stateful_decision")]=decision;event[k("stateful_runtime")]=runtime;event[k("stateful_numeric_stats")]=owner.stats.json();
                QJsonObject ranks;for(auto it=owner.context.ranks.begin();it!=owner.context.ranks.end();++it)ranks[it.key()]=it.value();event[k("stateful_ranks")]=ranks;event[k("stateful_before")]=before;
                if(!decision.value(k("accepted")).toBool()) {reject(event,decision.value(k("reason")).toString(error));continue;}
                if(runtime.value(k("entity_attack_interval_verified"))==QJsonValue(true))owner.unknowns.insert(k("entity_attack_clock_is_verified_scenario_observation_not_source_cadence_repair"));
                if(runtime.value(k("owned_timer_observed"))==QJsonValue(true))owner.unknowns.insert(k("awakening_pool_starts_from_verified_preconsumed_scenario_observation"));
                QVector<QJsonObject> stagedPayloads;QVector<Pending> stagedPending;QMap<QString,CastInstance> stagedInstances;QJsonArray deliveries;
                bool valid=true,replaceAttack=transition.value(k("attack_payload_policy")).toString()==k("replace_owner_base_attack");int effectIndex=0;
                for(const auto &raw:decision.value(k("effects")).toArray()) {
                    auto effect=raw.toObject();auto op=effect.value(k("op")).toString();if(op==k("execute")) {op=k("damage");effect[k("op")]=op;effect[k("delivery_mode")]=k("execute_health");bool ownedThresholdState=false;for(const auto &rawTransition:program.value(k("transitions")).toArray()) {const auto predecessor=rawTransition.toObject();if(predecessor.value(k("to"))==decision.value(k("state_before"))&&predecessor.value(k("context_constraints")).toObject().value(k("target_health_fraction")).toObject().contains(k("maximum")))ownedThresholdState=true;}effect[k("source_execute_state_authorized")]=ownedThresholdState;}
                    const double amount=effect.value(k("amount")).toDouble();const int recipient=find(staged,effect.value(k("target_key")).toString());
                    if(effect.contains(k("onhit_scale"))&&!number(effect.value(k("onhit_scale")),0,16)) {error=k("stateful_invalid_onhit_scale");valid=false;break;}
                    const auto childId=event.value(k("event_id")).toString()+k(":state:")+QString::number(effectIndex++);
                    if(runtime.contains(k("hero_hit_receipt_v1"))) {
                        const auto hit=runtime.value(k("hero_hit_receipt_v1")).toObject();
                        effect[k("source_cast_instance_id")]=hit.value(k("cast_instance_id"));effect[k("parent_event_id")]=event.value(k("event_id"));
                        for(const auto &field:{k("hit_event_id"),k("origin_ability_id"),k("application_origin_family")})effect[field]=hit.value(field);
                        if(op==k("damage"))effect[k("damage_packet_id")]=childId;
                    }
                    if(effect.value(k("replaces_attack_damage")).toBool()||effect.value(k("delivery_mode")).toString()==k("replace_base_attack"))replaceAttack=true;
                    if(QStringList{k("record_damage"),k("set_value"),k("counter"),k("reset_counter"),k("spawn_entity"),k("destroy_entity"),k("entity_damage"),k("mark"),k("attach"),k("detach"),k("movement"),k("position")}.contains(op)) {
                        if(op==k("movement")||op==k("position"))owner.unknowns.insert(k("state_movement_geometry_not_simulated"));deliveries.append(effect);continue;
                    }
                    if(recipient<0||staged[recipient].hp<=eps) {error=k("stateful_effect_recipient_dead_or_unavailable");valid=false;break;}
                    auto &recipientState=staged[recipient];
                    if(op==k("health_cost")||(op==k("resource")&&effect.value(k("resource_kind")).toString()==k("health")&&amount<0)) {
                        const double cost=op==k("health_cost")?amount:-amount;if(recipient!=actor||owner.hp-cost<=eps) {error=k("stateful_health_cost_unaffordable");valid=false;break;}owner.hp-=cost;owner.cost+=cost;effect[k("applied_amount")]=cost;
                    } else if(op==k("resource")&&effect.value(k("mode")).toString()!=k("upkeep_stop")) {
                        if(recipient!=actor) {error=k("stateful_resource_recipient_must_own_ledger");valid=false;break;}
                        if(effect.value(k("resource_kind")).toString()==k("health")) {const double healed=std::min(amount,owner.maximum-owner.hp);owner.hp+=healed;owner.healed+=healed;effect[k("applied_amount")]=healed;}
                        else if(amount<0) {const auto payment=owner.action.payResource(-amount,t,true);if(!payment.value(k("accepted")).toBool()) {error=k("stateful_resource_payment_rejected");valid=false;break;}effect[k("payment")]=payment;}
                        else {const double prior=owner.action.snapshot().value(k("resource_current")).toDouble();if(!owner.action.restoreResource(std::min(owner.action.snapshot().value(k("resource_maximum")).toDouble(),prior+amount),t,true)) {error=k("stateful_resource_restore_rejected");valid=false;break;}effect[k("applied_amount")]=owner.action.snapshot().value(k("resource_current")).toDouble()-prior;}
                    } else if(op==k("cooldown")) {
                        if(effect.value(k("ledger_applied")).toBool()) {deliveries.append(effect);continue;}
                        const auto cooldownSlot=effect.value(k("slot")).toString(effect.value(k("source_slot")).toString());const auto mode=effect.value(k("mode")).toString(k("reduce"));
                        if(!QStringList{k("reset"),k("refund_fraction"),k("reduce"),k("reduce_seconds")}.contains(mode)) {error=k("stateful_cooldown_mode_unavailable");valid=false;break;}
                        double seconds=amount;if(mode==k("reset"))seconds=1e7;
                        else if(mode==k("refund_fraction")) {const auto clocks=actionSkillState(db,owner,cooldownSlot);if(clocks.isEmpty()||clocks.value(k("last_cast_at")).toDouble(-1)<0) {error=k("stateful_cooldown_commitment_unavailable");valid=false;break;}const double committed=std::max(0.,clocks.value(k("ready_at")).toDouble()-clocks.value(k("last_cast_at")).toDouble(t));seconds=committed*amount;effect[k("committed_cooldown_seconds")]=committed;}
                        if(recipient!=actor||!QStringList{k("Q"),k("W"),k("E"),k("R")}.contains(cooldownSlot)||!owner.action.reduceCooldown(cooldownSlot,std::max(0.,seconds),t)) {error=k("stateful_cooldown_pool_or_mode_unavailable");valid=false;break;}effect[k("applied_amount")]=seconds;
                    } else if(op==k("control")) {
                        const auto controlType=effect.value(k("control_type")).toString(effect.value(k("control")).toString());
                        if(QStringList{k("camouflage"),k("stealth"),k("invisibility")}.contains(controlType)) {owner.unknowns.insert(k("visibility_state_requires_verified_detection_geometry"));deliveries.append(effect);continue;}
                        QJsonObject control{{k("event_id"),childId},{k("source"),owner.key},{k("target"),recipientState.key},{k("type"),controlType},{k("time"),t},{k("duration"),effect.value(k("duration"))}};
                        if(controlType==k("slow")) {auto slow=effect.value(k("slow_fraction"));if(effect.value(k("slow_fraction_expression")).isObject()) {const auto value=formula(effect.value(k("slow_fraction_expression")).toObject(),owner,context,owner.context.ranks.value(program.value(k("slot")).toString(),1));if(value)slow=*value;}if(!slow.isDouble())slow=amount;control[k("slow_fraction")]=slow;}
                        if(!recipientState.control.apply(control,recipientState.stats,&error)) {valid=false;break;}effect[k("applied_control")]=control;
                        if(recipientState.control.interruptsCast(t)) {recipientState.action.interruptCast(t);recipientState.stateful.onInterrupt(t);}
                    } else if(op==k("stat_buff")) {
                        const auto stat=effect.value(k("stat")).toString();if(!combatStat(stat)&&stat!=k("incoming_damage_multiplier")&&stat!=k("armor_shred_pct")) {error=k("stateful_stat_executor_unavailable:")+stat;valid=false;break;}
                    } else if(QStringList{k("resource_upkeep"),k("heal_over_time"),k("damage_reduction"),k("reveal"),k("untargetable")}.contains(op)||(op==k("resource")&&effect.value(k("mode")).toString()==k("upkeep_stop"))) {
                        // The active-effect ledger supplies the exact lifetime;
                        // integration and target eligibility consume it below.
                    } else if(op==k("shield")&&effect.value(k("remove")).toBool()) {
                        const auto source=owner.key+k(":mechanic:stateful:")+effect.value(k("source_binding_id")).toString();double removed=0;
                        for(int i=recipientState.shields.size()-1;i>=0;--i)if(recipientState.shields[i].source==source) {removed+=recipientState.shields[i].amount;recipientState.shields.removeAt(i);}effect[k("applied_amount")]=removed;
                    } else if(op==k("damage")||op==k("replay_damage")||op==k("heal")||op==k("shield")) {
                        if(effect.value(k("delivery_mode")).toString()==k("execute_health")) {auto guards=transition.value(k("guards")).toArray();for(const auto &guard:effect.value(k("guards")).toArray())guards.append(guard);if(guards.isEmpty()&&!effect.value(k("source_execute_state_authorized")).toBool()) {error=k("stateful_execute_requires_owned_threshold_guard");valid=false;break;}effect[k("execute_guards")]=guards;}
                        QJsonObject payload{{k("event_id"),childId},{k("player_key"),owner.key},{k("target_key"),recipientState.key},{k("time"),t},{k("slot"),program.value(k("slot"))},{k("hit_confirmed"),true},
                            {k("facts"),facts},{k("_state_binding_id"),event.value(k("binding_id"))},{k("_state_transition_id"),event.value(k("transition_id"))},{k("_state_parent_event_id"),event.value(k("event_id"))},{k("_state_raw_amount"),amount},{k("_state_onhit_scale"),effect.value(k("onhit_scale")).toDouble(0)}};
                        if(runtime.contains(k("hero_hit_receipt_v1"))) {
                            const auto hit=runtime.value(k("hero_hit_receipt_v1")).toObject();
                            payload[k("_state_hero_hit_receipt_v1")]=hit;payload[k("source_cast_instance_id")]=hit.value(k("cast_instance_id"));
                            for(const auto &field:{k("hit_event_id"),k("origin_ability_id"),k("application_origin_family")})payload[field]=hit.value(field);
                            payload[k("parent_event_id")]=event.value(k("event_id"));payload[k("damage_packet_id")]=childId;
                        }
                        if(op==k("damage")&&effect.value(k("delivery_mode")).toString()!=k("execute_health")) {
                            const auto stateSlot=program.value(k("slot")).toString();const auto phase=k("state:")+QString::number(effectIndex);
                            const bool liveDocumented=effect.value(k("evaluation_phase")).toString()==k("payload_live_context")&&owner.champion==25&&owner.documentedProfile.value(k("program")).toObject()==program;
                            QJsonObject component{{k("slot"),stateSlot},{k("rank_slot"),stateSlot},{k("max_rank"),6},{k("source_pointer"),program.value(k("source_pointer")).toString(k("stateful/v7"))+k("/")+event.value(k("binding_id")).toString()},
                                {k("calculation"),childId},{k("damage_type"),effect.value(k("damage_type")).toString(k("physical"))},{k("rate"),k("cast")},{k("condition"),k("continuous_temporal_verified")},{k("expression"),QJsonObject{{k("op"),k("const")},{k("value"),amount}}}};
                            if(liveDocumented) {component[k("expression")]=effect.value(k("expression"));payload[k("_state_documented_live_v9")]=documentedIdentity(owner);effect[k("amount_evaluation_phase")]=k("prepared_before_payload_live_context");}
                            payload[k("type")]=k("temporal_tick");payload[k("cast_instance_id")]=childId;payload[k("_temporal_phase_key")]=phase;payload[k("_temporal_component")]=component;payload[k("_temporal_owner")]=k("hero:")+QString::number(owner.champion)+k(":stateful_v7");
                            CastInstance instance;instance.id=childId;instance.actor=owner.key;instance.slot=stateSlot;instance.binding=event.value(k("binding_id")).toString();instance.created=t;instance.expires=std::max(t+1.,effect.value(k("expires_at")).toDouble(t+1));stagedInstances[childId]=instance;
                        } else {effect[k("recipient_key")]=recipientState.key;effect[k("source")]=k("stateful:")+effect.value(k("source_binding_id")).toString();effect[k("key")]=effect.value(k("source_slot"));if(op==k("replay_damage")) {effect[k("op")]=k("damage");effect[k("amount_already_mitigated")]=true;}payload[k("type")]=k("effect_delivery");payload[k("_delivery")]=effect;}
                        const double deliveryTime=cast?std::max(t,admission.value(k("cast_complete_at")).toDouble(t)):t;payload[k("time")]=deliveryTime;
                        if(deliveryTime>t+eps) {Pending q;q.time=deliveryTime;q.event=payload;q.kind=k("stateful_delivery");q.instance=payload.value(k("cast_instance_id")).toString();q.locksActor=false;q.cancelOnInterrupt=program.value(k("cancel_on_interrupt")).toBool();q.cancelOnSourceDeath=!program.value(k("persist_on_owner_death")).toBool();q.cancelOnTargetDeath=true;stagedPending.append(q);}else stagedPayloads.append(payload);
                    } else {error=k("stateful_effect_executor_unavailable:")+op;valid=false;break;}
                    deliveries.append(effect);
                }
                if(!valid) {reject(event,error);continue;}
                const double completeAt=admission.value(k("cast_complete_at")).toDouble(t);
                if(cast&&!completion&&completeAt>t+eps&&program.value(k("effect_application_phase")).toString()!=k("cast_start")) {
                    // Admission has paid the native costs; the source state and
                    // every effect become real only when startup completes.
                    // A trusted queued receipt prevents a second cast charge.
                    auto committedOwner=admittedOwner;auto primeEvent=event;primeEvent[k("slot")]=slot;
                    const auto prime=primeArmorImpact(db,committedOwner,primeEvent,t);if(!prime.isEmpty())event[k("armor_impact_prime")]=prime;
                    p[actor]=committedOwner;auto queued=event;queued[k("time")]=completeAt;queued[k("_state_cast_completion")]=true;queued[k("_state_admission")]=admission;
                    Pending q;q.time=completeAt;q.event=queued;q.kind=k("stateful_completion");q.locksActor=true;q.cancelOnInterrupt=true;q.cancelOnSourceDeath=true;pending.append(q);
                    event[k("stateful_after")]=before;event[k("stateful_prepared_decision")]=decision;auto awaiting=decision;awaiting[k("accepted")]=false;awaiting[k("reason")]=k("awaiting_native_cast_completion");awaiting[k("effects")]=QJsonArray{};event[k("stateful_decision")]=awaiting;event[k("stateful_delivery_committed")]=false;event[k("phase")]=k("stateful_cast_admission");event[k("action_admission")]=admission;event[k("accepted")]=true;trace.append(event);++accepted;continue;
                }
                syncStateBuffs(staged,t,itemZoneModel?&itemZones:nullptr);for(auto &changed:staged)if(!refreshStats(db,changed,t,&error)) {valid=false;break;}
                if(!valid) {reject(event,error);continue;}
                if(attack) {auto hit=event;hit[k("type")]=k("attack");hit[k("slot")]=slot;hit[k("hit_confirmed")]=true;hit[k("event_id")]=event.value(k("event_id")).toString()+k(":base_attack");hit[k("_state_replace_attack_damage")]=replaceAttack;hit[k("_state_parent_event_id")]=event.value(k("event_id"));hit[k("_state_attack_source")]=attackSourceView(admittedOwner,event);stagedPayloads.prepend(hit);}
                p=staged;pending+=stagedPending;payloads+=stagedPayloads;for(auto it=stagedInstances.begin();it!=stagedInstances.end();++it)instances[it.key()]=it.value();
                if(cast&&!completion) {auto primeEvent=event;primeEvent[k("slot")]=slot;
                    const auto prime=primeArmorImpact(db,p[actor],primeEvent,t);if(!prime.isEmpty())event[k("armor_impact_prime")]=prime;}
                if(event.value(k("_state_routed_from_attack"))==QJsonValue(true)&&event.value(k("transition_id")).toString()==k("enchanted_attack")) {
                    // A confirmed first isolated hit admits the source's total
                    // two-second DOT. No intermediate tick cadence, repeated
                    // attack refresh or stacking policy is inferred.
                    auto completion=event;completion[k("event_id")]=event.value(k("event_id")).toString()+k(":isolated_w_dot_total");
                    completion[k("transition_id")]=k("two_second_dot_total");completion[k("time")]=t+2;
                    completion.remove(k("_state_routed_from_attack"));
                    completion[k("_state_isolated_w_dot")]=true;
                    Pending q;q.time=t+2;q.event=completion;q.kind=k("stateful_transition");q.locksActor=false;
                    q.cancelOnInterrupt=false;q.cancelOnSourceDeath=true;q.cancelOnTargetDeath=true;pending.append(q);
                    p[actor].unknowns.insert(k("nautilus_w_isolated_total_at_two_seconds_not_server_tick_distribution"));
                }
                if(!p[actor].documentedProfile.isEmpty()&&p[actor].documentedProfile.value(k("program")).toObject().value(k("binding_id"))==event.value(k("binding_id"))) {
                    auto &documentedOwner=p[actor];const auto contract=documentedOwner.documentedProfile.value(k("action_contract")).toObject();event[k("documented_model_v9")]=documentedIdentity(documentedOwner);
                    if(cast&&contract.value(k("kind")).toString()==k("documented_health_schedule_v9")) {
                        const auto chargeId=event.value(k("event_id")).toString()+k(":documented_charge_v9");const double startAt=admission.value(k("cast_complete_at")).toDouble(t),until=startAt+contract.value(k("maximum_charge_seconds")).toDouble();
                        documentedOwner.documentedCharge={{k("instance_id"),chargeId},{k("binding_id"),event.value(k("binding_id"))},{k("profile_id"),documentedOwner.documentedProfile.value(k("profile_id"))},{k("profile_sha256"),documentedOwner.documentedProfile.value(k("profile_sha256"))},{k("started_at"),startAt},{k("active_until"),until},{k("active"),true},{k("phases"),QJsonObject{}}};
                        CastInstance charge;charge.id=chargeId;charge.actor=documentedOwner.key;charge.slot=slot;charge.binding=event.value(k("binding_id")).toString();charge.created=startAt;charge.expires=until;instances[chargeId]=charge;
                        const auto offsets=contract.value(k("payment_offsets_seconds")).toArray();for(int phase=0;phase<offsets.size();++phase) {
                            const double feeAt=startAt+offsets[phase].toDouble();QJsonObject fee{{k("type"),k("documented_charge_fee_v9")},{k("event_id"),chargeId+k(":fee:")+QString::number(phase)},{k("player_key"),documentedOwner.key},{k("cast_instance_id"),chargeId},{k("time"),feeAt},{k("_state_fee_phase_v9"),phase}};
                            if(feeAt<=t+eps)trace.append(payDocumentedFee(documentedOwner,fee,t,instances));else {Pending q;q.time=feeAt;q.event=fee;q.kind=k("documented_charge_fee_v9");q.instance=chargeId;q.locksActor=false;q.cancelOnInterrupt=true;q.cancelOnSourceDeath=true;pending.append(q);}
                        }
                    } else if(!documentedOwner.documentedCharge.isEmpty()&&decision.value(k("state_after")).toString()!=k("charging")) {documentedOwner.documentedCharge[k("active")]=false;documentedOwner.documentedCharge[k("ended_at")]=t;documentedOwner.documentedCharge[k("end_reason")]=event.value(k("transition_id"));}
                }
                for(int recipient=0;recipient<p.size();++recipient)if(p[recipient].control.interruptsCast(t))cancelPending(recipient,false,k("interrupting_stateful_control"));
                event[k("stateful_after")]=p[actor].stateful.snapshot();event[k("stateful_deliveries")]=deliveries;event[k("stateful_delivery_committed")]=true;event[k("stateful_action_state_after")]=p[actor].action.snapshot();event[k("action_admission")]=admission;event[k("health_cost")]=nativeHealthCost;event[k("accepted")]=true;if(completion)event[k("type")]=k("stateful_cast_complete");trace.append(event);if(!completion&&event.value(k("_state_isolated_w_dot"))!=QJsonValue(true))++accepted;continue;
            }
            if(type==k("manual_phase_hit")) {
                const auto id=event.value(k("cast_instance_id")).toString();const auto phase=event.value(k("temporal_phase_id")).toString()+k(":")+QString::number(event.value(k("temporal_tick_index")).toInt(-1));
                if(!instances.contains(id)||instances[id].actor!=a.key||instances[id].cancelled||t>instances[id].expires+eps) reject(event,k("temporal_instance_expired_or_cancelled"));
                else if(instances[id].delivered.contains(phase)) reject(event,k("duplicate_temporal_phase"));
                else reject(event,k("temporal_phase_requires_queued_due_event"));
                continue;
            }
            if (a.hp <= eps) { reject(event, k("actor_dead")); continue; }
            if(type==k("cast")&&a.champion==27&&event.value(k("slot")).toString()==k("Q")) {reject(event,k("toggle_requires_explicit_state_transition"));continue;}
            if(type==k("poison_contact")) {
                const int target=find(p,event.value(k("target_key")).toString());
                if(a.champion!=27||event.value(k("slot")).toString()!=k("Q")||target<0||p[target].team==a.team||p[target].hp<=eps) {reject(event,k("verified_poison_contact_target_required"));continue;}
                bool scheduled=false;
                for(const auto &raw:db.champions.value(a.champion).value(k("temporal_bindings_v4")).toArray()) {
                    const auto row=raw.toObject();if(row.value(k("slot")).toString()==k("Q")) scheduled|=scheduleBinding(actor,target,event,row,k("hero:27"));
                }
                if(!scheduled) {reject(event,k("verified_active_poison_contact_required"));continue;}
                event[k("accepted")]=true;event[k("resource_cost")]=0;trace.append(event);++accepted;continue;
            }
            if (type == k("cast") && event.value(k("slot")).toString() == k("R") && hasFlag(db, a, k("seal_r"))) { reject(event, k("augment_seals_ultimate")); continue; }
            if(type==k("attack")) {
                const auto forced=a.control.forcedAttackTarget(t);
                if(!forced.isEmpty()) {if(event.value(k("target_key")).toString().isEmpty())event[k("target_key")]=forced;const int target=find(p,event.value(k("target_key")).toString());if(target<0||p[target].hp<=eps||p[target].team==a.team||!a.control.canAttackTarget(t,p[target].key)) {reject(event,!a.control.canAttack(t)?k("control_blocks_attack"):k("control_blocks_attack_or_forced_target"));continue;}}
                else if(!event.value(k("target_key")).toString().isEmpty()&&!a.control.canAttackTarget(t,event.value(k("target_key")).toString())) {reject(event,k("control_blocks_attack"));continue;}
            }
            if (busyUntil(actor) > t + eps) { reject(event, k("cast_in_progress")); continue; }
            if ((type == k("attack") && !a.control.canAttack(t)) || ((type == k("cast") || type == k("transform") || type == k("toggle")) && !a.control.canCast(t))) { reject(event, type == k("attack") ? k("control_blocks_attack") : k("control_blocks_cast")); continue; }
            QJsonObject result; auto facts = event.value(k("facts")).toObject();
            if (event.value(k("hit_confirmed")).toBool()) { facts[k("first_hit_confirmed")] = true; facts[k("confirmed_hit")] = true; }
            if (!event.value(k("target_key")).toString().isEmpty()) facts[k("target_key")] = event.value(k("target_key"));
            if(type==k("cast")&&!a.documentedProfile.isEmpty()&&a.documentedProfile.value(k("program")).toObject().value(k("slot"))==event.value(k("slot"))) {reject(event,k("documented_profile_requires_state_executor"));continue;}
            QJsonArray preparedSupport; double preparedCost = 0;
            if (type == k("attack")) result = a.action.attack(t, a.stats[Stat::As], true);
            else if (type == k("cast")) {
                QStringList supportUnknowns; const auto effects = supportEffects(db, p, actor, event.value(k("slot")).toString(), event, preparedCost, supportUnknowns);
                if (preparedCost < 0) { reject(event, k("support_health_cost_unaffordable")); continue; }
                const auto rule = actionRule(db, a, event.value(k("slot")).toString(), facts);
                if (rule.value(k("requires_verified_health_cost")).toBool() || rule.value(k("requires_observed_health_cost")).toBool()) {
                    if (!facts.value(k("verified_health_cost")).toBool() || !number(facts.value(k("health_cost")), 0, a.maximum)) { a.unknowns.insert(k("verified_health_cost_required")); reject(event, k("verified_health_cost_required")); continue; }
                    preparedCost += facts.value(k("health_cost")).toDouble();
                    if (a.hp - preparedCost <= eps) { reject(event, k("health_cost_unaffordable")); continue; }
                }
                for (const auto &u : supportUnknowns) a.unknowns.insert(u);
                for (const auto &effect : effects) preparedSupport.append(QJsonObject{{k("target_key"), p[effect.target].key}, {k("kind"), effect.kind}, {k("slot"), effect.slot}, {k("amount"), effect.amount}, {k("duration"), effect.duration}});
                const auto clocks=actionClockStats(db,a);if(!clocks) {a.unknowns.insert(k("ultimate_haste_source_value_unavailable"));reject(event,k("ultimate_haste_source_value_unavailable"));continue;}
                result = a.action.cast(event.value(k("slot")).toString(), t, *clocks, facts);
            } else if (type == k("toggle")) {
                if(!event.value(k("enabled")).isBool()) {reject(event,k("explicit_toggle_state_required"));continue;}
                if(event.value(k("enabled")).toBool()) facts[k("toggle_instance_id")]=event.value(k("event_id"));
                result=a.action.toggle(event.value(k("slot")).toString(),event.value(k("enabled")).toBool(),t,a.stats,facts);
                a.unknowns.insert(k("continuous_toggle_payment_phase_is_scenario_assumption"));
                a.unknowns.insert(k("contacts_from_lingering_cloud_after_toggle_off_not_inferred"));
            } else if (type == k("transform")) result = a.action.transform(event.value(k("form")).toString(), t, facts);
            else if (type == k("cooldown_reduction")) {
                const bool ok = event.value(k("verified_cooldown_reduction")).toBool() && number(event.value(k("seconds")), 0, 3600) && a.action.reduceCooldown(event.value(k("slot")).toString(), event.value(k("seconds")).toDouble(), t);
                result = {{k("accepted"), ok}, {k("reason"), ok ? k("observed_cooldown_reduction") : k("verified_cooldown_reduction_required")}};
            } else { reject(event, k("unsupported_action_type")); continue; }
            for (auto it = result.begin(); it != result.end(); ++it) if (it.key() != k("type")) event[it.key()] = it.value();
            for(const auto &name:{k("verified_jhin_r_shot"),k("jhin_r_shot_number"),k("jhin_r_fourth_shot")}) {facts.remove(name);if(!result.contains(name))event.remove(name);}
            for(const auto &name:{k("verified_jhin_r_shot"),k("jhin_r_shot_number"),k("jhin_r_fourth_shot"),k("shot_number"),k("fourth_shot")}) if(result.contains(name)) facts[name]=result.value(name);
            event[k("facts")]=facts;
            if (!result.value(k("accepted")).toBool()) { reject(event, result.value(k("reason")).toString(k("action_rejected"))); continue; }
            if(a.champion==202&&type==k("cast")&&event.value(k("slot")).toString()==k("R")) {
                const auto burstId=result.value(k("burst_instance_id")).toString();const auto instanceId=k("ledger:")+a.key+k(":")+burstId;
                if(result.value(k("action_payload_kind")).toString()==k("channel_start")&&!burstId.isEmpty()) {
                    CastInstance instance;instance.id=instanceId;instance.actor=a.key;instance.slot=k("R");instance.binding=k("ledger_jhin_r");instance.created=t;instance.expires=result.value(k("burst_expires_at")).toDouble();instances[instanceId]=instance;
                } else if(result.value(k("verified_jhin_r_shot")).toBool()&&result.value(k("phase")).toString()==k("burst")&&instances.contains(instanceId)) {
                    QJsonObject receipt{{k("combat_revision"),db.revision},{k("event_id"),event.value(k("event_id"))},{k("player_key"),a.key},{k("time"),t},{k("shot_number"),result.value(k("shot_number"))},{k("source_instance_id"),instanceId},{k("burst_instance_id"),burstId}};
                    receipt[k("receipt_sha256")]=ledgerShotHash(receipt);event[k("_ledger_jhin_shot")]=receipt;
                }
            }
            if ((type == k("cast") || type == k("attack")) && event.value(k("target_key")).toString().isEmpty()) {
                const auto actorPolicy = a.team == objective ? policy : opponent;
                const int target = enemyTarget(p, actor, actorPolicy, event);
                if (target >= 0) { event[k("target_key")] = p[target].key; if (actorPolicy == k("spread")) a.spreadCursor++; }
            }
            if (type == k("cast")) {
                a.hp -= preparedCost; a.cost += preparedCost; event[k("health_cost")] = preparedCost; event[k("prepared_support")] = preparedSupport;
                const auto prime=primeArmorImpact(db,a,event,t);if(!prime.isEmpty())event[k("armor_impact_prime")]=prime;
                if(event.value(k("support_target_key")).toString().isEmpty()&&!preparedSupport.isEmpty()) {QString selected;for(const auto &raw:preparedSupport) {const auto key=raw.toObject().value(k("target_key")).toString();if(selected.isEmpty()||key!=a.key)selected=key;if(key!=a.key)break;}event[k("support_target_key")]=selected;}
            }
            if(type==k("transform") || result.value(k("form")).isString()) { QString error; if(!refreshStats(db,a,t,&error)) a.unknowns.insert(error); }
            if(type==k("cast") && facts.value(k("phase")).toString(k("primary"))==k("primary")) {
                auto c=liveContext(a,nullptr,event);
                for(const auto &raw:db.champions.value(a.champion).value(k("temporal_bindings_v4")).toArray()) {
                    const auto row=raw.toObject();const bool activation=row.value(k("activation_requires_completed_channel")).toBool();
                    if(row.value(k("slot")).toString()!=event.value(k("slot")).toString()||(!activation&&!row.value(k("locks_actor")).toBool(row.value(k("kind")).toString()==k("channel"))))continue;
                    const auto requiredForm=row.value(k("required_form")).toString();if(!requiredForm.isEmpty()&&requiredForm!=a.action.snapshot().value(k("form")).toString())continue;
                    const auto duration=formula(row.value(activation?k("activation_delay_expression"):k("duration_expression")).toObject(),a,c,a.context.ranks.value(row.value(k("rank_slot")).toString(event.value(k("slot")).toString())));
                    if(!duration||*duration<=eps)continue;
                    Pending lock;lock.time=result.value(k("cast_complete_at")).toDouble(t)+*duration;lock.kind=k("channel_end");lock.locksActor=true;lock.cancelOnInterrupt=row.value(k("cancel_on_interrupt")).toBool(true);lock.cancelOnSourceDeath=row.value(k("cancel_on_source_death")).toBool(true);lock.cancelOnTargetDeath=false;
                    lock.event={{k("event_id"),event.value(k("event_id")).toString()+k(":channel_reservation:")+row.value(k("binding_id")).toString()},{k("player_key"),a.key},{k("type"),k("channel_end")},{k("time"),lock.time}};pending.append(lock);
                }
            }
            ++accepted; event[k("accepted")] = true; trace.append(event);
            if (type != k("cast") && type != k("attack")) continue;
            if(result.contains(k("payload_emitted")) && !result.value(k("payload_emitted")).toBool()) continue;
            const double complete = type == k("cast") ? result.value(k("cast_complete_at")).toDouble(t) : t;
            if (complete > t + eps) pending.append({complete, event}); else payloads.append(event);
        }
        deliverDocumentedFees(false);
        std::array<std::array<double, 10>, 10> damage{}, executionDamage{}; QVector<QJsonObject> landedControls;
        struct DamagePacket {int source,target;double amount;QJsonObject event;bool execution=false;};QVector<DamagePacket> damagePackets;
        // Resolve this clock's confirmed support before reading health-based
        // damage. Plan amounts against one pre-support state; cap simultaneous
        // healing proportionally so event names cannot change combat health.
        struct SupportPlan {int payload,actor,target;QString kind,slot;double amount,duration;bool delivery;};
        QVector<SupportPlan> supportPlans;std::array<double,10> healingTotals{};QSet<int> supportDeliveries;
        for(int index=0;index<payloads.size();++index) {
            const auto e=payloads[index];const int actor=find(p,e.value(k("player_key")).toString());if(actor<0)continue;
            const auto type=e.value(k("type")).toString();
            if(type==k("effect_delivery")) {
                const auto delivery=e.value(k("_delivery")).toObject();const auto op=delivery.value(k("op")).toString();
                const int recipient=find(p,delivery.value(k("recipient_key")).toString());
                if(recipient<0||p[recipient].hp<=eps||!QStringList{k("heal"),k("shield")}.contains(op))continue;
                const double duration=op==k("shield")?delivery.value(k("expires_at")).toDouble(-1)-t:0;
                if(op==k("shield")&&duration<=eps)continue;
                const double amount=delivery.value(k("amount")).toDouble()*p[actor].support*std::max(0.,1+p[actor].stats[Stat::HealShield])*
                    receivedSustainMultiplier(db,p[recipient],op,delivery.value(k("source")).toString());
                if(!std::isfinite(amount)||amount<0)continue;
                supportPlans.append({index,actor,recipient,op,delivery.value(k("key")).toString(),amount,duration,true});
                if(op==k("heal"))healingTotals[recipient]+=amount;
            } else if(type!=k("attack")&&type!=k("temporal_tick")&&p[actor].hp>eps&&e.value(k("hit_confirmed")).toBool()) {
                double paidCost=0;QStringList unknown;const auto effects=supportEffects(db,p,actor,e.value(k("slot")).toString(),e,paidCost,unknown,false);
                for(const auto &u:unknown)p[actor].unknowns.insert(u);
                for(const auto &effect:effects)if(p[effect.target].hp>eps) {
                    supportPlans.append({index,actor,effect.target,effect.kind,effect.slot,effect.amount,effect.duration,false});
                    if(effect.kind==k("heal"))healingTotals[effect.target]+=effect.amount;
                }
            }
        }
        std::array<double,10> healingScale{};for(int i=0;i<p.size();++i)healingScale[i]=healingTotals[i]>0?std::min(1.,std::max(0.,p[i].maximum-p[i].hp)/healingTotals[i]):1;
        for(const auto &effect:supportPlans) {
            auto &recipient=p[effect.target];double amount=effect.amount;auto e=payloads[effect.payload];
            const auto deliverySource=effect.delivery?e.value(k("_delivery")).toObject().value(k("source")).toString():QString{};
            const double receivedMultiplier=receivedSustainMultiplier(db,recipient,effect.kind,deliverySource);
            if(effect.kind==k("heal")) {recipient.rawHealing+=amount/receivedMultiplier;recipient.amplifiedHealing+=amount;amount*=healingScale[effect.target];recipient.hp+=amount;recipient.healed+=amount;}
            else {
                recipient.rawShield+=amount/receivedMultiplier;recipient.generatedShield+=amount;if(effect.actor==effect.target)recipient.generatedSelfShield+=amount;
                const auto delivery=e.value(k("_delivery")).toObject();const auto source=effect.delivery?p[effect.actor].key+k(":mechanic:")+delivery.value(k("source")).toString():p[effect.actor].key;
                for(int j=recipient.shields.size()-1;j>=0;--j)if(recipient.shields[j].source==source&&recipient.shields[j].slot==effect.slot)recipient.shields.removeAt(j);
                recipient.shields.append({source,effect.slot,amount,t+effect.duration});
            }
            if(effect.delivery) {supportDeliveries.insert(effect.payload);e[k("accepted")]=true;e[k("phase")]=k("mechanic_delivery");e[k("effective_amount")]=amount;e[k("target_key")]=recipient.key;}
            else {auto effects=e.value(k("support_effects")).toArray();effects.append(QJsonObject{{k("target_key"),recipient.key},{k("kind"),effect.kind},{k("effective_amount"),amount}});e[k("support_effects")]=effects;}
            payloads[effect.payload]=e;
        }
        for (int payloadIndex=0;payloadIndex<payloads.size();++payloadIndex) {
            auto event=payloads[payloadIndex];
            const int actor = find(p, event.value(k("player_key")).toString()); if(actor<0) { reject(event,k("actor_unavailable"));continue; } auto &a = p[actor]; const auto type = event.value(k("type")).toString();
            if(documentedFeesHandled.contains(event.value(k("event_id")).toString()))continue;
            if(supportDeliveries.contains(payloadIndex)) {trace.append(event);continue;}
            if(type==k("temporal_tick")&&event.value(k("unit_target")).isString()) {
                const auto unitId=event.value(k("unit_target")).toString(),instanceId=event.value(k("cast_instance_id")).toString(),phase=event.value(k("_temporal_phase_key")).toString();const auto unit=units.unit(unitId);
                if(!units.contains(unitId)||!unit.value(k("alive")).toBool()||!instances.contains(instanceId)||instances[instanceId].cancelled||instances[instanceId].actor!=a.key||t>instances[instanceId].expires+eps||instances[instanceId].delivered.contains(phase)) {reject(event,k("unit_temporal_instance_or_target_unavailable"));continue;}
                bool needsTargetSpeed=false;auto inspectSpeed=[&](const CombatEntry &entry) {for(const auto &raw:entry.effects)if(raw.toObject().value(k("op")).toString()==k("speed_damage"))needsTargetSpeed=true;};for(int id:a.items)inspectSpeed(db.items.value(id));for(int id:a.augments)inspectSpeed(db.augments.value(id));if(needsTargetSpeed&&!number(unit.value(k("movement_speed")),0,1e5)) {reject(event,k("unit_movement_speed_observation_required"));continue;}
                auto context=liveContext(a,nullptr,event);context.conditions.insert(k("continuous_temporal_verified"));context.conditions.insert(k("continuous_action_verified"));context.targetHp=unit.value(k("maximum_health")).toDouble();context.targetArmor=unit.value(k("armor")).toDouble();context.targetMr=unit.value(k("mr")).toDouble();context.targetMs=unit.value(k("movement_speed")).toDouble();context.counters[k("target_current_hp")]=unit.value(k("current_health")).toDouble();context.counters[k("target_health_fraction")]=unit.value(k("current_health")).toDouble()/context.targetHp;
                const auto slot=event.value(k("slot")).toString();const auto component=event.value(k("_temporal_component")).toObject();const auto identity=combatComponentIdentity(component);context.restrictExplicitComponents=true;context.explicitComponentKeys.insert(identity);context.explicitComponentScales[identity]=event.value(k("_temporal_scale")).toDouble(1);context.explicitCastCounts[slot]=1;if(component.value(k("rate")).toString()==k("per_second"))context.explicitActiveSeconds[slot]=event.value(k("_temporal_active_seconds")).toDouble();
                auto payloadDb=db;auto hero=payloadDb.champions.value(a.champion);hero[k("components")]=QJsonArray{component};payloadDb.champions[a.champion]=hero;const auto evaluated=evaluateCombat(payloadDb,context,a.items,a.augments);const double amount=(evaluated.attackDamage+evaluated.skillDamage+evaluated.procDamage)*a.output*event.value(k("hit_factor")).toDouble(1);
                if(!evaluated.valid||!std::isfinite(amount)||amount<0) {reject(event,k("unit_temporal_numeric_model_unavailable"));continue;}
                QString error;auto receipt=units.receiveDamage(unitId,a.key,event.value(k("event_id")).toString(),t,amount,&error);if(!receipt.value(k("accepted")).toBool()) {reject(event,error);continue;}
                instances[instanceId].delivered.insert(phase);a.damageReceipts[receipt.value(k("receipt_id")).toString()]=receipt;a.unitDealt+=receipt.value(k("actual_damage")).toDouble();a.unitShieldDealt+=receipt.value(k("shield_damage")).toDouble();for(const auto &unknown:evaluated.unknowns)a.unknowns.insert(unknown);
                event[k("accepted")]=true;event[k("phase")]=k("unit_temporal_payload");event[k("unit_damage")]=receipt.value(k("actual_damage"));event[k("unit_shield_damage")]=receipt.value(k("shield_damage"));event[k("numeric_stats")]=evaluated.stats.json();event[k("unit_stats")]=unit;trace.append(event);trace.append(receipt);continue;
            }
            if(type==k("temporal_tick")&&event.value(k("entity_target")).isObject()) {
                const auto spec=event.value(k("entity_target")).toObject();const int target=find(p,spec.value(k("owner_key")).toString());const auto binding=spec.value(k("binding_id")).toString(),entityKey=spec.value(k("entity_key")).toString();
                const auto instanceId=event.value(k("cast_instance_id")).toString(),phase=event.value(k("_temporal_phase_key")).toString();
                if(target<0||!instances.contains(instanceId)||instances[instanceId].cancelled||instances[instanceId].actor!=a.key||t>instances[instanceId].expires+eps||instances[instanceId].delivered.contains(phase)) {reject(event,k("entity_temporal_instance_expired_or_duplicate"));continue;}
                const auto entity=p[target].stateful.snapshot().value(k("instances")).toObject().value(binding).toObject().value(k("entities")).toObject().value(entityKey).toObject();const auto stats=entityCombatStats(p[target],binding,entityKey,entity);
                if(entity.isEmpty()||!entity.value(k("hp_known")).toBool()||entity.value(k("hp")).toDouble()<=eps||!stats.value(k("armor")).isDouble()||!stats.value(k("mr")).isDouble()||!stats.value(k("ms")).isDouble()) {reject(event,k("entity_temporal_target_or_stats_unavailable"));continue;}
                auto context=liveContext(a,nullptr,event);context.conditions.insert(k("continuous_temporal_verified"));context.conditions.insert(k("continuous_action_verified"));context.targetHp=entity.value(k("maximum_hp")).toDouble();context.targetArmor=stats.value(k("armor")).toDouble();context.targetMr=stats.value(k("mr")).toDouble();context.targetMs=stats.value(k("ms")).toDouble();context.counters[k("target_current_hp")]=entity.value(k("hp")).toDouble();context.counters[k("target_health_fraction")]=entity.value(k("hp")).toDouble()/context.targetHp;
                const auto slot=event.value(k("slot")).toString();const auto component=event.value(k("_temporal_component")).toObject();const auto identity=combatComponentIdentity(component);context.restrictExplicitComponents=true;context.explicitComponentKeys.insert(identity);context.explicitComponentScales[identity]=event.value(k("_temporal_scale")).toDouble(1);context.explicitCastCounts[slot]=1;if(component.value(k("rate")).toString()==k("per_second"))context.explicitActiveSeconds[slot]=event.value(k("_temporal_active_seconds")).toDouble();
                auto payloadDb=db;auto hero=payloadDb.champions.value(a.champion);hero[k("components")]=QJsonArray{component};payloadDb.champions[a.champion]=hero;const auto evaluated=evaluateCombat(payloadDb,context,a.items,a.augments);
                const double amount=(evaluated.attackDamage+evaluated.skillDamage+evaluated.procDamage)*a.output*event.value(k("hit_factor")).toDouble(1);
                if(!evaluated.valid||!std::isfinite(amount)||amount<0) {reject(event,k("entity_temporal_numeric_model_unavailable"));continue;}
                const auto receiptId=k("entity-damage:")+event.value(k("event_id")).toString()+k(":")+p[target].key+k(":")+binding+k(":")+entityKey;QString error;
                auto receipt=p[target].stateful.receiveEntityDamage(binding,entityKey,receiptId,a.key,t,amount,&error);if(!receipt.value(k("accepted")).toBool()) {reject(event,error);continue;}
                instances[instanceId].delivered.insert(phase);receipt[k("type")]=k("entity_damage_receipt");receipt[k("receipt_id")]=receiptId;receipt[k("event_id")]=event.value(k("event_id"));receipt[k("time")]=t;receipt[k("source_key")]=a.key;receipt[k("target_key")]=entityKey;receipt[k("target_owner_key")]=p[target].key;receipt[k("shield_damage")]=0;receipt[k("requested_damage")]=amount;
                a.damageReceipts[receiptId]=receipt;p[target].damageReceipts[receiptId]=receipt;a.entityDealt+=receipt.value(k("actual_damage")).toDouble();p[target].entityReceived+=receipt.value(k("actual_damage")).toDouble();for(const auto &unknown:evaluated.unknowns)a.unknowns.insert(unknown);
                event[k("accepted")]=true;event[k("phase")]=k("entity_temporal_payload");event[k("entity_damage")]=receipt.value(k("actual_damage"));event[k("numeric_stats")]=evaluated.stats.json();event[k("entity_stats")]=stats;trace.append(event);trace.append(receipt);continue;
            }
            if(event.contains(k("_ledger_jhin_shot"))) {
                const auto receipt=event.value(k("_ledger_jhin_shot")).toObject();const auto instanceId=receipt.value(k("source_instance_id")).toString();const auto shot=k("shot:")+QString::number(receipt.value(k("shot_number")).toInt());
                if(!ledgerJhinShot(event,a.champion,db.revision,instances)||instances[instanceId].delivered.contains(shot)) {reject(event,k("invalid_or_replayed_ledger_channel_shot"));continue;}
                instances[instanceId].delivered.insert(shot);
            }
            if(type==k("effect_delivery")) {
                const auto delivery=event.value(k("_delivery")).toObject();const auto op=delivery.value(k("op")).toString();const int recipient=find(p,delivery.value(k("recipient_key")).toString());
                if(recipient<0||p[recipient].hp<=eps) {reject(event,k("mechanic_delivery_recipient_dead_or_unavailable"));continue;}
                auto &target=p[recipient];double amount=delivery.value(k("amount")).toDouble();
                if(op==k("heal")) {amount*=a.support*std::max(0.,1+a.stats[Stat::HealShield]);target.rawHealing+=amount;amount*=receivedSustainMultiplier(db,target,op,delivery.value(k("source")).toString());target.amplifiedHealing+=amount;amount=std::min(amount,target.maximum-target.hp);target.hp+=amount;target.healed+=amount;}
                else if(op==k("shield")) {const double expiry=delivery.value(k("expires_at")).toDouble(-1);if(expiry<=t+eps) {reject(event,k("mechanic_delivery_shield_expired"));continue;}amount*=a.support*std::max(0.,1+a.stats[Stat::HealShield]);target.rawShield+=amount;amount*=receivedSustainMultiplier(db,target,op,delivery.value(k("source")).toString());target.generatedShield+=amount;if(actor==recipient)target.generatedSelfShield+=amount;const auto source=a.key+k(":mechanic:")+delivery.value(k("source")).toString();const auto slot=delivery.value(k("key")).toString();for(int i=target.shields.size()-1;i>=0;--i)if(target.shields[i].source==source&&target.shields[i].slot==slot)target.shields.removeAt(i);target.shields.append({source,slot,amount,expiry});}
                else if(op==k("health_cost")) {if(recipient!=actor||a.hp-amount<=eps) {reject(event,k("mechanic_delivery_health_cost_unaffordable"));continue;}a.hp-=amount;a.cost+=amount;}
                else if(op==k("damage")) {
                    const auto targetBlock=stateTargetBlockReason(p,recipient,actor,t,event);if(!targetBlock.isEmpty()) {p[recipient].unknowns.insert(targetBlock);reject(event,targetBlock);continue;}
                    if(delivery.value(k("delivery_mode")).toString()==k("execute_health")) {
                        // Only source-program guarded effects can create this
                        // internal delivery. Normal true damage uses shields.
                        bool guardsPass=true;const auto context=liveContext(a,&target,event);const int rank=a.context.ranks.value(event.value(k("slot")).toString(),1);
                        for(const auto &raw:delivery.value(k("execute_guards")).toArray()) {const auto guard=raw.toObject();const auto left=compileCombatFormula(guard.value(k("left_expression")).toObject(),context,rank).evaluate(context,a.stats),right=compileCombatFormula(guard.value(k("right_expression")).toObject(),context,rank).evaluate(context,a.stats);const auto relation=guard.value(k("op")).toString();if(!left||!right||!((relation==k("le")&&*left<=*right+eps)||(relation==k("lt")&&*left<*right)||(relation==k("ge")&&*left+eps>=*right)||(relation==k("gt")&&*left>*right)||(relation==k("eq")&&std::abs(*left-*right)<=eps))) {guardsPass=false;break;}}
                        if(!guardsPass) {reject(event,k("stateful_execute_health_guard_failed_at_delivery"));continue;}
                        amount=target.hp;executionDamage[actor][recipient]+=amount;damagePackets.append({actor,recipient,amount,event,true});
                        event[k("accepted")]=true;event[k("phase")]=k("stateful_execute");event[k("effective_amount")]=amount;event[k("target_key")]=target.key;trace.append(event);continue;
                    }
                    if(delivery.value(k("amount_already_mitigated")).toBool()) {
                        amount*=target.taken*stateDamageMultiplier(p,recipient,t);damage[actor][recipient]+=amount;damagePackets.append({actor,recipient,amount,event,false});
                        event[k("accepted")]=true;event[k("phase")]=k("stateful_replayed_damage");event[k("effective_amount")]=amount;event[k("target_key")]=target.key;trace.append(event);continue;
                    }
                    const auto c=liveContext(a,&target,event);const auto modifiers=evaluateCombat(db,c,a.items,a.augments);
                    QMap<QString,double> typeAmps;for(auto it=modifiers.damageTypeMultipliers.begin();it!=modifiers.damageTypeMultipliers.end();++it)typeAmps[it.key()]=it.value().toDouble();
                    const auto mitigated=mitigateCombatDamage(delivery.value(k("damage_type")).toString(),amount,modifiers.stats,c,typeAmps);
                    if(!modifiers.valid||!mitigated) {reject(event,k("mechanic_delivery_numeric_model_unavailable"));continue;}
                    amount=*mitigated*(1+modifiers.copiedTrueDamageRatio)*modifiers.globalDamageMultiplier*a.output*target.taken*stateDamageMultiplier(p,recipient,t);
                    if(!std::isfinite(amount)||amount<0) {reject(event,k("mechanic_delivery_invalid_damage"));continue;}
                    damage[actor][recipient]+=amount;damagePackets.append({actor,recipient,amount,event,false});for(const auto &u:modifiers.unknowns)a.unknowns.insert(u);
                    if(modifiers.copiedTrueDamageRatio>0&&typeAmps.contains(k("true")))a.unknowns.insert(k("copied_true_damage_amplifier_order"));
                } else {reject(event,k("unsupported_mechanic_delivery"));continue;}
                event[k("accepted")]=true;event[k("phase")]=k("mechanic_delivery");event[k("effective_amount")]=amount;event[k("target_key")]=target.key;trace.append(event);continue;
            }
            const bool temporal = type==k("temporal_tick");
            if(temporal) {
                const auto id=event.value(k("cast_instance_id")).toString(), phase=event.value(k("_temporal_phase_key")).toString();
                if(!instances.contains(id)||instances[id].cancelled||t>instances[id].expires+eps) { reject(event,k("temporal_instance_expired_or_cancelled"));continue; }
                if(instances[id].delivered.contains(phase)) { reject(event,k("duplicate_temporal_phase"));continue; }
                instances[id].delivered.insert(phase);
            }
            const auto slot = event.value(k("slot")).toString();
            if (!event.value(k("hit_confirmed")).toBool()) { event[k("phase")] = k("payload_withheld_hit_not_confirmed"); event[k("damage")] = 0; trace.append(event); continue; }
            const auto actorPolicy = a.team == objective ? policy : opponent;
            const int target = enemyTarget(p, actor, actorPolicy, event);
            double amount = 0;
            if(temporal && target<0) { reject(event,k("temporal_target_dead_or_unavailable"));continue; }
            if (target >= 0) {
                const auto targetBlock=stateTargetBlockReason(p,target,actor,t,event);if(!targetBlock.isEmpty()) {p[target].unknowns.insert(targetBlock);reject(event,targetBlock);continue;}
                if(type==k("attack")&&event.value(k("_state_invalidate_w_isolated_dot"))==QJsonValue(true)) {
                    a.unknowns.insert(k("nautilus_w_additional_attack_dot_refresh_and_stacking_withheld"));
                    for(int n=pending.size()-1;n>=0;--n)if(pending[n].event.value(k("player_key")).toString()==a.key&&
                        pending[n].event.value(k("_state_isolated_w_dot"))==QJsonValue(true))pending.removeAt(n);
                }
                if(!temporal) {
                    for(const auto &raw:db.champions.value(a.champion).value(k("temporal_bindings_v4")).toArray()) {
                        const auto row=raw.toObject(); const auto trigger=row.value(k("trigger_action")).toString(k("cast"));
                        if((type==k("attack") && trigger==k("attack")) || (type!=k("attack") && trigger!=k("attack") && row.value(k("slot")).toString()==slot)) {
                            auto triggerEvent=event; if(type==k("attack")) triggerEvent[k("slot")]=row.value(k("slot"));
                            auto repeated=row; repeated.remove(k("initial_phases"));scheduleBinding(actor,target,triggerEvent,repeated,k("hero:" )+QString::number(a.champion));
                            if(!row.value(k("initial_phases")).toArray().isEmpty()) {
                                auto initial=row;initial[k("binding_id")]=row.value(k("binding_id")).toString()+k(":initial");initial[k("phases")]=row.value(k("initial_phases"));
                                initial.remove(k("initial_phases"));initial.remove(k("interval_expression"));initial.remove(k("count_expression"));initial.remove(k("tick_count_expression"));initial.remove(k("duration_expression"));
                                initial.remove(k("timing_fact_bounds"));initial.remove(k("phase_constraints"));initial.remove(k("tick_schedule_constraints_v6"));initial.remove(k("allocation_count_expression"));
                                initial[k("timing_mode")]=k("source_closed");initial[k("required_facts")]=QJsonArray{};initial[k("locks_actor")]=false;
                                scheduleBinding(actor,target,triggerEvent,initial,k("hero:" )+QString::number(a.champion));
                            }
                        }
                    }
                }
                auto c = liveContext(a, &p[target], event);
                if(type==k("attack")&&event.value(k("_state_attack_source")).isObject())applyAttackSourceView(c,event.value(k("_state_attack_source")).toObject());
                c.conditions.insert(k("continuous_action_verified"));
                if (type == k("attack")) c.explicitAttackCount = 1; else c.explicitCastCounts[slot] = 1;
                const auto facts = event.value(k("facts")).toObject();
                if (facts.value(k("verified_active_seconds")).toBool() && number(facts.value(k("active_seconds")), 0, 120)) c.explicitActiveSeconds[slot] = facts.value(k("active_seconds")).toDouble();
                for (const auto &v : facts.value(k("component_keys")).toArray()) if (v.isString()) c.explicitComponentKeys.insert(v.toString());
                const auto proc = facts.value(k("proc_counts")).toObject();
                if (facts.value(k("verified_proc_counts")).toBool()) for (auto it = proc.begin(); it != proc.end(); ++it) if (number(it.value(), 0, 100)) c.explicitProcCounts[it.key()] = it.value().toDouble();
                auto payloadDb = db; auto hero = db.champions.value(a.champion); auto components = hero.value(k("components")).toArray();
                if(!temporal) for(const auto &raw:hero.value(k("withheld_generic_damage_v6")).toArray()) {
                    const auto identity=raw.toObject();
                    for(int i=components.size()-1;i>=0;--i) {
                        const auto old=components[i].toObject();
                        if(old.value(k("slot"))==identity.value(k("slot"))&&old.value(k("source_pointer"))==identity.value(k("source_pointer"))&&old.value(k("calculation"))==identity.value(k("calculation"))) {
                            components.removeAt(i);a.unknowns.insert(k("unbound_stateful_generic_damage_withheld:")+identity.value(k("calculation")).toString());
                        }
                    }
                }
                bool temporalSlot=false;
                for(const auto &raw:hero.value(k("temporal_bindings_v4")).toArray()) {
                    const auto row=raw.toObject(); if(row.value(k("slot")).toString()!=slot && row.value(k("trigger_action")).toString()!=type) continue;temporalSlot=true;
                    const auto replaces=row.value(k("replaces_generic_identity")).toArray();
                    for(int i=components.size()-1;i>=0;--i) { const auto old=components[i].toObject();bool remove=false;
                        if(replaces.isEmpty()) remove=old.value(k("slot")).toString()==slot;
                        for(const auto &identity:replaces) { const auto o=identity.toObject();remove|=old.value(k("slot"))==o.value(k("slot"))&&old.value(k("source_pointer"))==o.value(k("source_pointer"))&&old.value(k("calculation"))==o.value(k("calculation")); }
                        if(remove) components.removeAt(i);
                    }
                }
                if(temporal) {
                    c.conditions.insert(k("continuous_temporal_verified")); c.restrictExplicitComponents=true;
                    const auto component=event.value(k("_temporal_component")).toObject(); const auto identity=combatComponentIdentity(component);
                    components=QJsonArray{component}; c.explicitComponentKeys.insert(identity);c.explicitComponentScales[identity]=event.value(k("_temporal_scale")).toDouble(1);
                    if(event.contains(k("_state_parent_event_id")))c.explicitAttackCount=event.value(k("_state_onhit_scale")).toDouble(0)>0?1:0;
                    if(component.value(k("rate")).toString()==k("per_second")) c.explicitActiveSeconds[slot]=event.value(k("_temporal_active_seconds")).toDouble(0);
                }
                const auto timeline = temporal || temporalSlot ? QJsonArray{} : hero.value(k("timeline_damage_components_v3")).toArray();
                // A migrated phase never falls back to its old generic binding
                // when its new form, charge or hit facts are unavailable.
                for (const auto &raw : timeline) {
                    const auto replacement = raw.toObject().value(k("replaces_generic_identity")).toObject();
                    if (replacement.isEmpty()) continue;
                    for (int i = components.size() - 1; i >= 0; --i) {
                        const auto old = components[i].toObject();
                        if (old.value(k("slot")) == replacement.value(k("slot")) && old.value(k("source_pointer")) == replacement.value(k("source_pointer")) && old.value(k("calculation")) == replacement.value(k("calculation"))) components.removeAt(i);
                    }
                }
                const bool second = event.value(k("phase")).toString() == k("second");
                if (second) { c.explicitCastCounts[slot] = 0; a.unknowns.insert(k("recast_damage_phase_not_bound_to_continuous_payload")); }
                QVector<QJsonObject> phaseEligible; QMap<QString,int> phaseGroups;
                for (const auto &raw : timeline) {
                    const auto component = raw.toObject(); if (component.value(k("slot")).toString() != slot || type == k("attack")) continue;
                    if (!requiredInputs(component, a, c, facts)) continue;
                    if (component.value(k("delivery")).toString(k("instant")) != k("instant")) { a.unknowns.insert(k("timeline_delayed_damage_delivery_withheld")); continue; }
                    const auto gate = component.value(k("required_resource_gate")).toString();
                    if (gate == k("spell_charge") && !event.value(k("charges_remaining")).isDouble()) { a.unknowns.insert(k("timeline_spell_charge_gate_unavailable")); continue; }
                    if (gate == k("channel_shot") && !ledgerJhinShot(event,a.champion,db.revision,instances)) { a.unknowns.insert(k("timeline_resource_gate_not_implemented:") + gate); continue; }
                    if (gate == k("armed_trap_instance")) { a.unknowns.insert(k("timeline_resource_gate_not_implemented:") + gate); continue; }
                    phaseEligible.append(component); const auto group = component.value(k("exclusive_group")).toString(); if (!group.isEmpty()) phaseGroups[group]++;
                }
                for (const auto &component : phaseEligible) {
                    const auto group = component.value(k("exclusive_group")).toString(); if (!group.isEmpty() && phaseGroups[group] > 1) { a.unknowns.insert(k("conflicting_timeline_damage_exclusive_group")); continue; }
                    components.append(component);
                }
                hero[k("components")] = components; payloadDb.champions[a.champion] = hero;
                const auto evaluated = evaluateCombat(payloadDb, c, a.items, a.augments);
                if(temporal&&event.value(k("_state_documented_live_v9")).isObject()) {
                    const auto expression=event.value(k("_temporal_component")).toObject().value(k("expression")).toObject();const int rank=a.context.ranks.value(slot,1);
                    const auto raw=compileCombatFormula(expression,c,rank).evaluate(c,evaluated.stats);
                    if(!raw||!std::isfinite(*raw)||*raw<0) {reject(event,k("documented_live_damage_formula_unavailable"));continue;}
                    const double fraction=std::clamp(p[target].hp/p[target].maximum,0.,1.),multiplier=1+(1-fraction);
                    auto receipt=event.value(k("_state_documented_live_v9")).toObject();receipt[k("time")]=t;receipt[k("player_key")]=a.key;receipt[k("target_key")]=p[target].key;
                    receipt[k("target_current_hp")]=p[target].hp;receipt[k("target_max_hp")]=p[target].maximum;receipt[k("owner_ap")]=evaluated.stats[Stat::Ap];receipt[k("rank")]=rank;
                    receipt[k("raw_amount")]=*raw;receipt[k("multiplier")]=multiplier;receipt[k("source_base_tick_quantity")]=*raw/multiplier;receipt[k("evaluation_phase")]=k("payload_live_context");
                    event[k("documented_model_receipt_v9")]=receipt;event[k("_state_raw_amount")]=*raw;
                }
                if(event.contains(k("_state_parent_event_id")))event[k("stateful_delivery_stats")]=evaluated.stats.json();
                for (const auto &u : evaluated.unknowns) a.unknowns.insert(u);
                if (evaluated.valid) {
                    const bool statePhase=temporal&&event.contains(k("_state_parent_event_id")),omitBase=event.value(k("_state_replace_attack_damage")).toBool()||statePhase;
                    const double baseCopy=omitBase?evaluated.attackDamage*evaluated.copiedTrueDamageRatio:0;
                    double procs=evaluated.procDamage-baseCopy;
                    if(statePhase&&event.value(k("_state_onhit_scale")).toDouble()>0)procs+=std::max(0.,evaluated.onhitDamage-baseCopy)*(event.value(k("_state_onhit_scale")).toDouble()-1);
                    amount = ((omitBase?0:evaluated.attackDamage) + evaluated.skillDamage + procs) * a.output * p[target].taken * stateDamageMultiplier(p,target,t) * event.value(k("hit_factor")).toDouble(1);
                    if (std::isfinite(amount) && amount >= 0) {
                        if(itemZoneModel&&slot==k("R")&&type!=k("attack")&&evaluated.skillDamage>eps&&
                           (!temporal||event.value(k("_temporal_owner")).toString().startsWith(k("hero:")))) {
                            // The packet may also contain item procs. Preserve
                            // only its native R contribution, never relabel the
                            // whole mixed packet as champion ultimate damage.
                            event[k("_item_zone_ultimate_amount")]=evaluated.skillDamage*a.output*p[target].taken*stateDamageMultiplier(p,target,t)*event.value(k("hit_factor")).toDouble(1);
                            event[k("_item_zone_cast_instance")]=event.value(k("cast_instance_id")).toString(event.value(k("event_id")).toString());
                        }
                        damage[actor][target] += amount;damagePackets.append({actor,target,amount,event,false});
                    } else { amount = 0; a.unknowns.insert(k("invalid_action_damage_withheld")); }
                }
                if(type==k("attack")&&event.value(k("hit_factor")).toDouble(1)>=1-eps&&a.items.contains(3084)&&facts.value(k("verified_heartsteel_charged_attack"))==QJsonValue(true)) {
                    auto updated=a;QString error;
                    const auto procId=event.value(k("event_id")).toString()+k(":heartsteel");
                    QJsonObject observation{{k("source"),k("item")},{k("id"),3084},{k("event"),k("charged_attack")},
                        {k("time"),t},{k("target"),p[target].key},{k("facts"),facts}};
                    QJsonObject growthEvent{{k("type"),k("mechanic_observation")},{k("event_id"),procId},{k("time"),t},
                        {k("player_key"),a.key},{k("observation"),observation}};
                    bool valid=updated.progression.apply(db,growthEvent,&error,&a.context)&&refreshStats(db,updated,t,&error);
                    QJsonObject plan;QVector<QJsonObject> deliveries;
                    if(valid) {
                        QJsonArray roster;for(int recipient=0;recipient<p.size();++recipient) {const auto &state=recipient==actor?updated:p[recipient];
                            roster.append(QJsonObject{{k("player_key"),state.key},{k("team"),state.team},{k("alive"),state.hp>eps},
                                {k("current_health"),state.hp},{k("maximum_health"),state.maximum}});}
                        observation[k("event_id")]=procId;
                        plan=planMechanicDeliveryV4(updated.progression.lastDecision().value(k("new_effects")).toArray(),observation,a.key,roster,t);
                        valid=plan.value(k("accepted")).toBool();if(!valid)error=plan.value(k("error")).toString();
                        if(valid)for(const auto &raw:plan.value(k("deliveries")).toArray()) {
                            const auto delivery=raw.toObject();
                            // The reviewed charged-hit program is immediate;
                            // a future source change must introduce a queued
                            // lifetime policy rather than silently emit now.
                            if(std::abs(delivery.value(k("start_at")).toDouble(-1)-t)>eps) {valid=false;error=k("heartsteel_delayed_delivery_policy_unverified");break;}
                            deliveries.append(QJsonObject{{k("type"),k("effect_delivery")},{k("event_id"),procId+k(":delivery:")+delivery.value(k("effect_id")).toString()},
                                {k("player_key"),a.key},{k("time"),t},{k("target_key"),delivery.value(k("recipient_key"))},{k("_delivery"),delivery}});
                        }
                    }
                    if(valid) {a=std::move(updated);payloads+=deliveries;event[k("heartsteel_growth")]=a.progression.lastDecision();event[k("heartsteel_maximum_health_after")]=a.maximum;}
                    else {event[k("heartsteel_growth")]=QJsonObject{{k("accepted"),false},{k("reason"),error}};a.unknowns.insert(k("heartsteel_charged_hit_withheld:")+error);}
                }
                const bool nativeHeroOnhit=temporal&&evaluated.valid&&event.value(k("_state_onhit_scale")).toDouble()==1&&
                    event.value(k("_state_hero_hit_receipt_v1")).toObject().value(k("origin_ability_id")).toString()==k("39:Q");
                if(a.augments.contains(3116)&&event.value(k("hit_factor")).toDouble(1)>=1-eps&&(type==k("attack")||nativeHeroOnhit)){
                    QJsonArray roster;for(const auto &recipient:p)roster.append(QJsonObject{{k("player_key"),recipient.key},{k("team"),recipient.team},
                        {k("alive"),recipient.hp>eps},{k("current_health"),recipient.hp},{k("maximum_health"),recipient.maximum}});
                    const auto result=applyNative3116Hit(db,a,p[target],event,t,nativeHeroOnhit,roster,payloads);event[k("augment_3116_hit")]=result;
                    if(!result.value(k("accepted")).toBool())a.unknowns.insert(k("3116_qualified_hit_withheld:")+result.value(k("reason")).toString());
                }
                if((!temporal||nativeHeroOnhit)&&event.value(k("hit_factor")).toDouble(1)>=1-eps&&
                   (type==k("attack")||nativeHeroOnhit||facts.value(k("verified_skill_onhit_eligibility"))==QJsonValue(true))) {
                    QJsonArray roster;for(const auto &recipient:p)roster.append(QJsonObject{{k("player_key"),recipient.key},{k("team"),recipient.team},
                        {k("alive"),recipient.hp>eps},{k("current_health"),recipient.hp},{k("maximum_health"),recipient.maximum}});
                    auto hit=event;
                    if(nativeHeroOnhit) {
                        auto qualified=facts;qualified[k("verified_skill_onhit_eligibility")]=true;
                        qualified[k("cast_instance_id")]=event.value(k("source_cast_instance_id"));hit[k("facts")]=qualified;
                        if(a.augments.contains(3049)&&facts.value(k("verified_same_cast_prime_before_hit"))!=QJsonValue(true)) {
                            event[k("armor_impact_hit")]=QJsonObject{{k("accepted"),false},{k("reason"),k("armor_impact_same_cast_order_unverified")}};
                            a.unknowns.insert(k("irelia_q_armor_impact_order_requires_verified_scenario_receipt"));
                        }
                    }
                    if(!nativeHeroOnhit||facts.value(k("verified_same_cast_prime_before_hit"))==QJsonValue(true)) {
                        const auto impact=consumeArmorImpact(db,a,hit,t,roster,payloads);if(!impact.isEmpty())event[k("armor_impact_hit")]=impact;
                    }
                }
                if((type==k("attack")||(!temporal&&facts.value(k("verified_mystic_punch_skill_on_hit"))==QJsonValue(true)&&
                    facts.value(k("verified_skill_onhit_eligibility"))==QJsonValue(true)))&&
                   event.value(k("hit_factor")).toDouble(1)>=1-eps&&a.augments.contains(1058)) {
                    const auto refund=applyMysticPunchOnHit(db,a,event,t);
                    if(!refund.isEmpty())event[k("mystic_punch_action_clock")]=refund;
                }
                const bool heroHit=!temporal||event.value(k("_temporal_owner")).toString().startsWith(k("hero:"));
                if(amount>eps&&heroHit) {
                    auto ownerRows=[&](const QMap<int,CombatEntry> &pool,const QList<int> &owned,const QString &kind) {
                        for(int id:owned) for(const auto &raw:pool.value(id).temporalBindingsV4) {
                            const auto row=raw.toObject();const auto trigger=row.value(k("trigger")).toObject();const auto triggerType=trigger.value(k("type")).toString();
                            if((triggerType==k("attack_hit")&&type!=k("attack"))||(triggerType==k("spell_hit")&&type==k("attack")))continue;
                            if(!trigger.value(k("slots")).toArray().isEmpty()&&!trigger.value(k("slots")).toArray().contains(slot))continue;
                            auto triggerEvent=event;if(type==k("attack"))triggerEvent[k("slot")]=k("P");
                            scheduleBinding(actor,target,triggerEvent,row,kind+k(":")+QString::number(id));
                        }
                    };
                    ownerRows(db.items,a.items,k("item"));ownerRows(db.augments,a.augments,k("augment"));
                }
                drainPending();
                if (type != k("attack") && !temporal && !temporalSlot) {
                    QVector<QJsonObject> eligible; QMap<QString, int> groups;
                    for (const auto &raw : hero.value(k("control_bindings_v3")).toArray()) {
                        const auto binding = raw.toObject(); bool matches = binding.value(k("slot")).toString() == slot;
                        for (const auto &v : binding.value(k("triggering_slots")).toArray()) matches |= v.toString() == slot;
                        if (!matches || !requiredInputs(binding, a, c, facts)) continue;
                        if (!binding.value(k("consumes_fact")).toString().isEmpty()) { a.unknowns.insert(k("control_persistent_consumable_fact_not_implemented")); continue; }
                        if (binding.value(k("application_phase")).toString(k("hit")) == k("expiration")) { a.unknowns.insert(k("control_expiration_phase_not_implemented")); continue; }
                        eligible.append(binding); const auto group = binding.value(k("exclusive_group")).toString(); if (!group.isEmpty()) groups[group]++;
                    }
                    for (const auto &binding : eligible) {
                        const auto group = binding.value(k("exclusive_group")).toString(); if (!group.isEmpty() && groups[group] > 1) { a.unknowns.insert(k("conflicting_control_exclusive_group")); continue; }
                        const auto duration = formula(binding.value(k("duration_expression")).toObject(), a, c, a.context.ranks.value(binding.value(k("slot")).toString() == k("P") ? slot : binding.value(k("slot")).toString()));
                        if (!duration || *duration <= 0 || *duration > 120) { a.unknowns.insert(k("control_duration_formula_unavailable")); continue; }
                        QJsonObject control{{k("event_id"), event.value(k("event_id")).toString() + k(":cc:") + binding.value(k("binding_id")).toString()}, {k("source"), a.key}, {k("target"), p[target].key}, {k("type"), binding.value(k("type"))}, {k("time"), t}, {k("duration"), *duration}};
                        if (binding.value(k("type")).toString() == k("slow")) {
                            auto slow = formula(binding.value(k("slow_fraction_expression")).toObject(), a, c, a.context.ranks.value(slot));
                            if (!slow && number(binding.value(k("slow_fraction")), 0, 1)) slow = binding.value(k("slow_fraction")).toDouble();
                            if (!slow || *slow > 1) { a.unknowns.insert(k("control_slow_fraction_unavailable")); continue; } control[k("slow_fraction")] = *slow;
                        }
                        landedControls.append(control);
                    }
                }
            }
            event[k("phase")] = k("payload"); event[k("time")] = t; event[k("damage")] = amount; if (target >= 0) event[k("resolved_target_key")] = p[target].key;
            if(event.contains(k("_state_parent_event_id"))) {if(!event.contains(k("stateful_delivery_stats")))event[k("stateful_delivery_stats")]=a.stats.json();if(target>=0)event[k("stateful_target_stats")]=p[target].stats.json();}
            trace.append(event);
        }
        double effective = 0, absorbed = 0; QJsonArray deaths;
        std::array<double,10> drainHealing{},healing3116{};QJsonArray drainReceipts;
        for (int b = 0; b < p.size(); ++b) {
            double total = 0,execute=0; for (int a = 0; a < p.size(); ++a) {total += damage[a][b];execute+=executionDamage[a][b];}
            if (p[b].hp <= eps) continue;
            const double shield = absorb(p[b], total), regularHp = std::min(p[b].hp, std::max(0., total - shield)),executedHp=std::min(std::max(0.,p[b].hp-regularHp),execute),hp=regularHp+executedHp;
            for (int a = 0; a < p.size(); ++a) {if (total > 0) {p[a].dealt += regularHp * damage[a][b] / total;p[a].shieldDealt += shield * damage[a][b] / total;}if(execute>0)p[a].dealt+=executedHp*executionDamage[a][b]/execute;}
            QString killSource,killEvent;double killAmount=-1;QJsonArray participants;
            if(hp>=p[b].hp-eps&&hp>0)for(const auto &packet:damagePackets)if(packet.target==b&&packet.amount>0) {const auto source=p[packet.source].key;if(!participants.contains(source))participants.append(source);if(packet.amount>killAmount||(packet.amount==killAmount&&packet.event.value(k("event_id")).toString()>killEvent)) {killAmount=packet.amount;killSource=source;killEvent=packet.event.value(k("event_id")).toString();}}
            for(const auto &packet:damagePackets)if(packet.target==b) {
                const double actual=packet.execution?(execute>0?executedHp*packet.amount/execute:0):(total>0?regularHp*packet.amount/total:0),blocked=packet.execution?0:(total>0?shield*packet.amount/total:0);
                const auto receiptId=k("damage:")+packet.event.value(k("event_id")).toString()+k(":")+p[b].key;
                QJsonObject receipt{{k("type"),k("damage_receipt")},{k("receipt_id"),receiptId},{k("event_id"),packet.event.value(k("event_id"))},{k("time"),t},{k("source_key"),p[packet.source].key},{k("target_key"),p[b].key},{k("actual_damage"),actual},{k("shield_damage"),blocked},{k("requested_damage"),packet.amount},{k("target_death"),hp>=p[b].hp-eps&&hp>0},{k("execution"),packet.execution},{k("stateful_parent_event_id"),packet.event.value(k("_state_parent_event_id"))}};
                if(itemZoneModel&&packet.amount>eps&&packet.event.value(k("_item_zone_ultimate_amount")).toDouble()>eps) {
                    const double contribution=actual*std::clamp(packet.event.value(k("_item_zone_ultimate_amount")).toDouble()/packet.amount,0.,1.);
                    receipt[k("item_zone_origin_family")]=k("champion_ultimate");receipt[k("item_zone_source_slot")]=k("R");
                    receipt[k("item_zone_cast_instance_id")]=packet.event.value(k("_item_zone_cast_instance"));
                    receipt[k("item_zone_ultimate_actual_damage")]=contribution;
                    receipt[k("item_zone_owner_had_item")]=p[packet.source].items.contains(3118);
                    receipt[k("item_zone_contribution_policy")]=k("native_R_component_fraction_of_mixed_packet_actual_hp_damage");
                    if(contribution>eps) {
                        ItemZoneLedger::UltimateDamageReceipt native{receiptId,p[packet.source].key,p[b].key,
                            packet.event.value(k("_item_zone_cast_instance")).toString(),k("R"),t,contribution,
                            ItemZoneLedger::Origin::ChampionUltimate,p[packet.source].items.contains(3118)};
                        const auto decision=itemZones.trigger(native);
                        trace.append(QJsonObject{{k("type"),k("item_zone_trigger")},{k("time"),t},{k("player_key"),native.ownerKey},
                            {k("target_key"),native.targetKey},{k("damage_receipt_id"),receiptId},{k("zone_id"),decision.zoneId},
                            {k("accepted"),decision.accepted},{k("created"),decision.created},{k("refreshed"),decision.refreshed},
                            {k("reason"),decision.reason},{k("expires_at"),decision.expiresAt},{k("opportunity_only"),!native.ownerCurrentlyHasItem},{k("live_certified"),false}});
                    }
                }
                if(!killSource.isEmpty()) {receipt[k("kill_source_key")]=killSource;receipt[k("kill_receipt_id")]=k("death:")+p[b].key+k(":")+QString::number(t,'g',17);receipt[k("kill_target_key")]=p[b].key;receipt[k("kill_target_type")]=k("champion");receipt[k("kill_participant_keys")]=participants;if(participants.size()>1)p[packet.source].unknowns.insert(k("simultaneous_kill_credit_uses_largest_post_mitigation_packet_then_event_id"));}
                p[packet.source].damageReceipts[receiptId]=receipt;p[b].damageReceipts[receiptId]=receipt;trace.append(receipt);
                auto causal3116=packet.event.value(k("_delivery")).toObject().value(k("causal_3116")).toObject();
                if(!causal3116.isEmpty()){
                    auto provenance=receipt;provenance[k("type")]=k("augment_3116_damage_receipt");provenance[k("causal_3116")]=causal3116;provenance[k("live_certified")]=false;trace.append(provenance);
                }
                if(causal3116.value(k("kind"))==k("consume")){
                    auto &source=p[packet.source];causal3116[k("damage_receipt_id")]=receiptId;
                    causal3116[k("damage_source")]=k("augment:3116:consume");causal3116[k("damage_event_id")]=packet.event.value(k("event_id"));
                    causal3116[k("damage_time")]=t;causal3116[k("post_mitigation_damage")]=packet.amount;
                    causal3116[k("actual_hp_damage")]=actual;causal3116[k("shield_damage")]=blocked;causal3116[k("target_hp_before")]=p[b].hp;
                    causal3116[k("heal_damage_basis")]=k("actual_hp_damage");
                    const auto healId=causal3116.value(k("damage_packet_id")).toString()+k(":heal");
                    QJsonObject obs{{k("source"),k("augment")},{k("id"),3116},{k("event"),k("consume_damage_resolved")},
                        {k("actor_key"),source.key},{k("target"),p[b].key},{k("receipt"),causal3116},
                        {k("facts"),QJsonObject{{k("verified_consume_damage_identity"),true},{k("actual_damage_dealt"),actual}}}};
                    QJsonObject event{{k("type"),k("mechanic_observation")},{k("event_id"),healId},{k("time"),t},{k("player_key"),source.key},{k("observation"),obs}};
                    QString error;const bool applied=source.progression.apply(db,event,&error,&source.context,true);
                    if(applied)for(const auto &raw:source.progression.lastDecision().value(k("new_effects")).toArray()){
                        const auto effect=raw.toObject();if(effect.value(k("op"))==k("heal")&&effect.value(k("target"))==k("self"))
                            healing3116[packet.source]+=effect.value(k("value")).toDouble()*source.support*std::max(0.,1+source.stats[Stat::HealShield]);
                    }
                    if(!applied)source.unknowns.insert(k("3116_consume_heal_withheld:")+error);
                    trace.append(QJsonObject{{k("type"),k("augment_3116_heal_receipt")},{k("time"),t},{k("accepted"),applied},
                        {k("reason"),error},{k("damage_receipt_id"),receiptId},{k("actual_hp_damage"),actual},{k("shield_damage"),blocked},
                        {k("policy_provenance"),causal3116.value(k("policy_provenance"))},{k("live_certified"),false}});
                }
                if(packet.event.value(k("_state_drain_item")).toInt()==2502) {
                    const auto &event=packet.event;const auto policy=event.value(k("_state_drain_basis")).toString();
                    const bool explicitBasis=event.value(k("_state_drain_basis_verified"))==QJsonValue(true);
                    double basis=-1;
                    if(explicitBasis&&policy==k("actual_hp_damage"))basis=actual;
                    else if(explicitBasis&&policy==k("health_and_shields"))basis=actual+blocked;
                    else if(explicitBasis&&policy==k("post_mitigation_requested"))basis=packet.amount;
                    else if(blocked<=eps&&std::abs(packet.amount-actual)<=eps)basis=actual;
                    if(basis<0) {
                        p[packet.source].unknowns.insert(k("drain_healing_shield_or_overkill_basis_requires_explicit_contract"));
                        drainReceipts.append(QJsonObject{{k("type"),k("drain_healing_receipt")},{k("source_key"),p[packet.source].key},
                            {k("damage_receipt_id"),receiptId},{k("accepted"),false},{k("reason"),k("drain_healing_damage_basis_unverified")}});
                    } else {
                        const auto &source=p[packet.source];
                        const double raw=basis*event.value(k("_state_drain_multiplier")).toDouble()*source.support*std::max(0.,1+source.stats[Stat::HealShield]);
                        drainHealing[packet.source]+=raw;
                        drainReceipts.append(QJsonObject{{k("type"),k("drain_healing_receipt")},{k("source_key"),source.key},{k("damage_receipt_id"),receiptId},
                            {k("accepted"),true},{k("damage_basis"),explicitBasis?policy:k("unambiguous_health_damage")},{k("damage_for_healing"),basis},{k("raw_healing"),raw}});
                    }
                }
            }
            p[b].hp = std::max(0., p[b].hp - hp); p[b].received += hp; effective += hp; absorbed += shield;
            if (p[b].hp <= eps && hp > 0) { p[b].deaths++; p[b].deathAt = t; p[b].action.interruptCast(t); p[b].action.advance(t, false); p[b].shields.clear(); p[b].control = ContinuousControlState{}; p[b].control.advanceTo(t);p[b].stateful.onDeath(t); deaths.append(p[b].key); }
        }
        for(const auto &event:deferredItemContacts)applyItemContact(event,false);
        if(!itemZoneFailure.isEmpty())return invalid(itemZoneFailure);
        if(itemZoneModel)for(const auto &dead:p)if(dead.hp<=eps)itemZones.clearTargetContacts(dead.key,t);
        for(const auto &raw:drainReceipts)trace.append(raw);
        for(int actor=0;actor<p.size();++actor)if(healing3116[actor]>eps){
            auto &source=p[actor];const auto modifiers=combatSelfSustainModifiers(db,source.context,source.items,source.augments,source.stats);
            const auto settlement=settleCombatSelfHeal(healing3116[actor],std::max(0.,source.maximum-source.hp),modifiers);
            const bool ok=source.hp>eps&&settlement.valid;
            if(ok){source.hp+=settlement.effective;source.healed+=settlement.effective;source.rawHealing+=settlement.raw;source.amplifiedHealing+=settlement.amplified;}
            trace.append(QJsonObject{{k("type"),k("augment_3116_heal_settlement")},{k("time"),t},{k("player_key"),source.key},{k("accepted"),ok},
                {k("raw_healing"),settlement.raw},{k("effective_healing"),ok?settlement.effective:0},{k("after_simultaneous_damage"),true},{k("live_certified"),false}});
        }
        // Each drain target contributes a damage receipt; heal once per source
        // after simultaneous damage, so target enumeration cannot change the
        // missing-health cap. This explicit model never resurrects the owner.
        for(int actor=0;actor<p.size();++actor)if(drainHealing[actor]>eps) {
            auto &source=p[actor];const auto modifiers=combatSelfSustainModifiers(db,source.context,source.items,source.augments,source.stats);
            const auto settlement=settleCombatSelfHeal(drainHealing[actor],std::max(0.,source.maximum-source.hp),modifiers);
            const bool accepted=source.hp>eps&&settlement.valid;
            if(accepted) {source.hp+=settlement.effective;source.healed+=settlement.effective;source.rawHealing+=settlement.raw;source.amplifiedHealing+=settlement.amplified;}
            trace.append(QJsonObject{{k("type"),k("drain_healing_settlement")},{k("time"),t},{k("player_key"),source.key},{k("accepted"),accepted},
                {k("raw_healing"),settlement.raw},{k("amplified_healing"),settlement.amplified},{k("effective_healing"),accepted?settlement.effective:0},
                {k("overheal"),settlement.overheal},{k("current_health"),source.hp},{k("simultaneous_damage_before_drain_healing"),true}});
        }
        for (int actor=0;actor<p.size();++actor) if(p[actor].hp<=eps) {cancelPending(actor,true,k("source_death"));cancelTargetPending(actor);for(auto &recipient:p)recipient.control.notifyActorDeath(p[actor].key,t,true);}
        for (const auto &control : landedControls) {
            const int target = find(p, control.value(k("target")).toString()); if (target < 0 || p[target].hp <= eps) continue;
            QString error; const bool ok = p[target].control.apply(control, p[target].stats, &error);
            auto result = control; result[k("phase")] = k("landed_control"); result[k("accepted")] = ok; if (!ok) { result[k("reason")] = error; p[target].unknowns.insert(error); } trace.append(result);
            if (ok && p[target].control.interruptsCast(t)) { p[target].action.interruptCast(t);p[target].stateful.onInterrupt(t); cancelPending(target,false,k("interrupting_landed_control")); }
        }
        if(std::any_of(p.begin(),p.end(),[](const Fighter &player){return !player.stateBuffs.isEmpty();})) {syncStateBuffs(p,t,itemZoneModel?&itemZones:nullptr);for(auto &changed:p) {QString error;if(!refreshStats(db,changed,t,&error))changed.unknowns.insert(error);}}
        if (effective > 0 || absorbed > 0 || !deaths.isEmpty()) transitions.append(QJsonObject{{k("time"), t}, {k("effective_damage"), effective}, {k("shield_damage"), absorbed}, {k("deaths"), deaths}, {k("eligible_for_action_value_training"), false}, {k("real_world_reward"), QJsonValue::Null}});
        double next = end;
        if (cursor < events.size()) next = std::min(next, events[cursor].value(k("time")).toDouble());
        if (automatic) next = std::min(next, nextDecision);
        if(itemZoneModel)next=itemZones.nextBoundary(t,next);
        for (const auto &v : pending) if (v.time > t + eps) next = std::min(next, v.time);
        for (const auto &a : p) { next = a.action.nextBoundary(t, next); next = a.control.nextBoundary(t, next);next=a.progression.nextBoundary(t,next);next=a.stateful.nextBoundary(t,next); for (const auto &s : a.shields) if (s.expires > t + eps) next = std::min(next, s.expires); }
        for(const auto &a:p)if(a.hp>eps) {
            double flat=0,current=0;for(const auto &owner:p)for(const auto &raw:owner.stateful.activeEffects(t)) {const auto e=raw.toObject();if(e.value(k("op")).toString()!=k("resource_upkeep")||e.value(k("target_key")).toString()!=a.key||e.value(k("resource_kind")).toString(e.value(k("resource")).toString())==k("health"))continue;const double rate=e.value(k("rate")).toDouble();const auto basis=e.value(k("basis")).toString(k("flat"));if(basis==k("current_fraction"))current+=rate;else flat+=rate*(basis==k("max_fraction")?a.stats[Stat::Mana]:1);}
            if(flat>0||current>0) {auto preview=a.action;const auto result=preview.integrateResourceUpkeep(flat,current,preview.time(),next,true);if(result.value(k("accepted")).toBool()&&result.value(k("exhausted_at")).isDouble()&&result.value(k("exhausted_at")).toDouble()>t+eps)next=std::min(next,result.value(k("exhausted_at")).toDouble());}
            double heal=naturalRegeneration?std::max(0.,a.stats[Stat::HpRegen])*(1+std::max(0.,a.stats[Stat::HpRegenPercent])):0,hpFlat=0,hpCurrent=0,healCurrent=0;
            for(const auto &owner:p)for(const auto &raw:owner.stateful.activeEffects(t)) {const auto e=raw.toObject();if(e.value(k("target_key")).toString()!=a.key)continue;const auto op=e.value(k("op")).toString(),basis=e.value(k("basis")).toString(k("flat"));const double rate=e.value(k("rate")).toDouble();if(op==k("heal_over_time")) {const double value=rate*owner.support*std::max(0.,1+owner.stats[Stat::HealShield])*receivedSustainMultiplier(db,a,k("heal"));if(basis==k("current_fraction"))healCurrent+=value;else heal+=value*(basis==k("max_fraction")?a.maximum:1);}else if(op==k("resource_upkeep")&&e.value(k("resource_kind")).toString(e.value(k("resource")).toString())==k("health")) {if(basis==k("current_fraction"))hpCurrent+=rate;else hpFlat+=rate*(basis==k("max_fraction")?a.maximum:1);}}
            const double A=heal-hpFlat,B=hpCurrent-healCurrent,dt=next-t,slope=A-B*a.hp;
            if(slope<0) {const double endHp=std::abs(B)<1e-12?a.hp+A*dt:A/B+(a.hp-A/B)*std::exp(-B*dt);if(endHp<=0) {double death=std::abs(B)<1e-12?-a.hp/A:-std::log((-A/B)/(a.hp-A/B))/B;if(std::isfinite(death)&&death>eps)next=std::min(next,t+death);}}
        }
        if (next <= t + eps) next = std::min(end, t + decisionStep);
        t = next;
    }
    if (steps >= 20000) unknowns.insert(k("timeline_event_limit_reached"));
    integrateItemZones(end);if(!itemZoneFailure.isEmpty())return invalid(itemZoneFailure);integrateState(end);for(auto &a:p)a.stateful.advance(end);syncStateBuffs(p,end,itemZoneModel?&itemZones:nullptr);units.advanceTo(end);
    for(auto &a:p)if(a.documentedCharge.value(k("active")).toBool()&&end>=a.documentedCharge.value(k("active_until")).toDouble()) {a.documentedCharge[k("active")]=false;a.documentedCharge[k("ended_at")]=a.documentedCharge.value(k("active_until"));a.documentedCharge[k("end_reason")]=k("maximum_charge_reached_without_observed_release_hit");}
    for (auto &a : p) { a.action.advance(end, a.hp > eps); a.control.advanceTo(end); expire(a, end); QString error; if (!refreshStats(db, a, end, &error)) a.unknowns.insert(error); }
    QJsonObject totals, objectives; QJsonArray final, checkpointPlayers;
    for (const auto &side : {k("ORDER"), k("CHAOS")}) {
        double initial = 0, maximum = 0, remaining = 0, dealt = 0, received = 0, shieldDealt = 0, shieldAbsorbed = 0, healed = 0, cost = 0, shield = 0, external = 0, restored = 0, statLoss=0; int alive = 0, deaths = 0, uncertain = 0;
        double enemyInitial = 0, enemyRemaining = 0, enemyMaximum = 0, enemyCost = 0, enemyExternal = 0, enemyRestored = 0, enemyStatLoss=0;
        for (const auto &a : p) if (a.team == side) { initial += a.initial; maximum += a.maximum; remaining += a.hp; dealt += a.dealt; received += a.received; shieldDealt += a.shieldDealt; shieldAbsorbed += a.absorbed; healed += a.healed; cost += a.cost; shield += shieldRemaining(a); external += a.externalLoss; restored += a.restored; statLoss+=a.statHealthLoss; alive += a.hp > eps; deaths += a.deaths; uncertain += !a.unknowns.isEmpty() || !a.augmentsComplete; }
        else { enemyInitial += a.initial; enemyRemaining += a.hp; enemyMaximum += a.maximum; enemyCost += a.cost; enemyExternal += a.externalLoss; enemyRestored += a.restored;enemyStatLoss+=a.statHealthLoss; }
        const double net = std::max(0., enemyInitial + enemyRestored - enemyRemaining - enemyCost - enemyExternal - enemyStatLoss);
        const double d = std::clamp(net / std::max(1., enemyMaximum), 0., 1.), s = std::clamp(remaining / std::max(1., maximum), 0., 1.), dead = deaths / 5., u = uncertain / 5.;
        totals[side] = QJsonObject{{k("initial_hp"), initial}, {k("maximum_hp"), maximum}, {k("remaining_hp"), remaining}, {k("effective_damage_dealt"), dealt}, {k("effective_damage_received"), received}, {k("shield_damage_dealt"), shieldDealt}, {k("shield_absorbed"), shieldAbsorbed}, {k("effective_healing_received"), healed}, {k("health_cost_received"), cost}, {k("remaining_shield"), shield}, {k("external_health_loss"), external}, {k("respawn_health_restored"), restored}, {k("health_stat_adjustment"), -statLoss}, {k("health_stat_clamp_loss"),statLoss}, {k("alive"), alive}, {k("new_deaths"), deaths}};
        objectives[side] = QJsonObject{{k("value"), weights.damage * d + weights.survival * s - weights.death * dead - weights.uncertainty * u}, {k("effective_enemy_damage_normalized"), d}, {k("remaining_own_health_normalized"), s}, {k("new_own_deaths_normalized"), dead}, {k("model_uncertain_player_fraction"), u}, {k("enemy_net_health_loss"), net}, {k("uncertainty_is_probability"), false}};
    }
    for (const auto &a : p) {
        QJsonArray shields; for (const auto &s : a.shields) shields.append(shieldJson(s));
        const auto action = a.action.snapshot(), control = a.control.checkpoint();
        for (const auto &u : a.unknowns) unknowns.insert(u);
        final.append(QJsonObject{{k("player_key"), a.key}, {k("champion_id"), a.champion}, {k("team"), a.team}, {k("level"), a.level}, {k("maximum_health"), a.maximum}, {k("initial_health"), a.initial}, {k("remaining_health"), a.hp}, {k("alive"), a.hp > eps}, {k("initially_dead"), a.initiallyDead}, {k("death_time"), a.deathAt < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(a.deathAt)}, {k("effective_damage_dealt"), a.dealt}, {k("effective_damage_received"), a.received}, {k("shield_damage_dealt"), a.shieldDealt}, {k("shield_absorbed"), a.absorbed}, {k("effective_healing_received"), a.healed}, {k("health_cost_received"), a.cost}, {k("external_health_loss"), a.externalLoss}, {k("respawn_health_restored"), a.restored}, {k("health_stat_adjustment"),-a.statHealthLoss}, {k("health_stat_clamp_loss"),a.statHealthLoss}, {k("stats"),a.stats.json()}, {k("progression_state"),a.progression.snapshot()}, {k("stateful_state_v7"),a.stateful.snapshot()}, {k("natural_health_regen_received"),a.naturalHealed}, {k("entity_damage_dealt"),a.entityDealt}, {k("entity_damage_received"),a.entityReceived}, {k("unit_damage_dealt"),a.unitDealt}, {k("unit_shield_damage_dealt"),a.unitShieldDealt}, {k("damage_receipts_v7"),a.damageReceipts}, {k("remaining_shield"), shieldRemaining(a)}, {k("final_action_state"), action}, {k("control_state"), a.control.snapshot(end)}, {k("unknowns"), codes(a.unknowns)}});
        auto sustain=final.last().toObject();
        sustain[k("raw_healing_received")]=a.rawHealing;sustain[k("amplified_healing_received")]=a.amplifiedHealing;
        sustain[k("raw_shield_received")]=a.rawShield;sustain[k("generated_shield_received")]=a.generatedShield;
        sustain[k("generated_self_shield")]=a.generatedSelfShield;
        sustain[k("permanent_health_growth")]=a.context.permanentEventStats[Stat::Hp]-a.permanentHpBaseline;
        sustain[k("sustain_totals_scope")]=k("this_window_explicit_support_deliveries_and_drain_receipts_excluding_natural_regeneration_and_continuous_hot");
        final[final.size()-1]=sustain;
        QJsonObject temporalReady;for(auto it=a.temporalReadyAt.begin();it!=a.temporalReadyAt.end();++it)temporalReady[it.key()]=it.value();
        checkpointPlayers.append(QJsonObject{{k("player_key"), a.key}, {k("fingerprint"), a.fingerprint}, {k("remaining_health"), a.hp}, {k("action_state"), action}, {k("control_state"), control}, {k("shields"), shields}, {k("spread_cursor"), a.spreadCursor}, {k("progression_state"), a.progression.snapshot()}, {k("temporal_ready_at"),temporalReady}, {k("maximum_health"),a.maximum}, {k("stateful_state_v7"),a.stateful.snapshot()}, {k("damage_receipts_v7"),a.damageReceipts}, {k("entity_observations_v7"),a.entityObservations}, {k("death_time"),a.deathAt}, {k("state_continuous_at_v7"),a.stateContinuousAt}});
        if(!a.documentedProfile.isEmpty()) {
            auto finalPlayer=final.last().toObject(),savedPlayer=checkpointPlayers.last().toObject();finalPlayer[k("documented_profile_v9")]=documentedIdentity(a);savedPlayer[k("documented_profile_v9")]=documentedIdentity(a);
            if(!a.documentedCharge.isEmpty()) {finalPlayer[k("documented_charge_v9")]=a.documentedCharge;savedPlayer[k("documented_charge_v9")]=a.documentedCharge;}
            final[final.size()-1]=finalPlayer;checkpointPlayers[checkpointPlayers.size()-1]=savedPlayer;
        }
    }
    QJsonArray queued; for (const auto &v : pending) queued.append(pendingJson(v));
    QJsonArray castInstances; for (const auto &v : instances) castInstances.append(instanceJson(v));
    auto ids = processed.values(); ids.sort();
    QJsonObject checkpoint{{k("schema_version"), 4}, {k("as_of_time"), end}, {k("combat_revision"), db.revision}, {k("players"), checkpointPlayers}, {k("combat_units_v8"),units.snapshot()}, {k("pending_casts"), queued}, {k("pending_events"), queued}, {k("cast_instances"), castInstances}, {k("processed_event_ids"), QJsonArray::fromStringList(ids)}, {k("next_decision_time"), nextDecision}, {k("auto_event_index"), autoIndex}, {k("decision_step_seconds"), decisionStep}, {k("automatic_actions"), automatic}, {k("simulate_natural_regeneration"),naturalRegeneration}};
    if(itemZoneModel)checkpoint[k("item_zone_model_v1")]=itemZones.snapshot();
    checkpoint[k("integrity_sha256")]=checkpointChecksum(checkpoint);
    const int orderAlive = totals.value(k("ORDER")).toObject().value(k("alive")).toInt(), chaosAlive = totals.value(k("CHAOS")).toObject().value(k("alive")).toInt();
    return {{k("policy"), policy}, {k("opponent_policy"), opponent}, {k("objective_team"), objective}, {k("objective_value"), objectives.value(objective).toObject().value(k("value"))}, {k("players_final"), final}, {k("combat_units_v8"),units.snapshot()}, {k("team_totals"), totals}, {k("objective_components"), objectives}, {k("trace"), trace}, {k("training_transitions"), transitions}, {k("checkpoint"), checkpoint}, {k("model_unknowns"), codes(unknowns)}, {k("start_time"), start}, {k("end_time"), end}, {k("elapsed_seconds"), end - start}, {k("events_accepted"), accepted}, {k("events_rejected"), rejected}, {k("end_state"), orderAlive == 0 && chaosAlive == 0 ? k("mutual_elimination") : orderAlive == 0 ? k("ORDER_eliminated") : chaosAlive == 0 ? k("CHAOS_eliminated") : k("horizon_reached")}, {k("eligible_for_action_value_training"), false}, {k("real_world_reward"), QJsonValue::Null}, {k("automatic_actions_synthetic"), automatic}};
}
}

QJsonObject runContinuousSandbox(const Catalog &catalog, const QJsonObject &request) {
    if (!catalog.combat) return invalid(k("combat_database_unavailable")); const auto &db = *catalog.combat;
    const double start = request.value(k("start_time")).toDouble(0), requestedHorizon = request.value(k("horizon")).toDouble(8), step = request.value(k("decision_step_seconds")).toDouble(.1);
    if ((request.contains(k("start_time")) && !number(request.value(k("start_time")), 0, 1e7)) || !std::isfinite(start) || (request.contains(k("horizon")) && !number(request.value(k("horizon")), .001, 300)) || (request.contains(k("decision_step_seconds")) && !number(request.value(k("decision_step_seconds")), .01, 1))) return invalid(k("invalid_time_bounds"));
    double end = start + requestedHorizon, horizon = requestedHorizon;
    if(request.contains(k("end_time"))) {
        const auto value=request.value(k("end_time"));
        if(!value.isDouble()||!std::isfinite(value.toDouble())||value.toDouble()<=start||value.toDouble()<start+.001||value.toDouble()>start+300)return invalid(k("invalid_time_bounds"));
        end=value.toDouble();horizon=end-start;
        // Accept only the rounding introduced by endpoint/duration arithmetic;
        // the supplied absolute endpoint itself remains the authoritative clock.
        const double arithmeticTolerance=4*std::numeric_limits<double>::epsilon()*std::max({1.,std::abs(start),std::abs(end),std::abs(requestedHorizon)});
        if(request.contains(k("horizon"))&&std::abs(end-(start+requestedHorizon))>arithmeticTolerance)return invalid(k("end_time_horizon_mismatch"));
    }
    const bool probe = request.value(k("participant_scope")) == QJsonValue(k("basic_attack_probe_v1"));
    if (request.contains(k("participant_scope")) && !probe) return invalid(k("unknown_participant_scope"));
    if (probe) { const auto reason = validateWorldAttackProbeContinuousV1(request); if (!reason.isEmpty()) return invalid(reason); }
    const int participantCount = probe ? 2 : 10;
    if (!request.value(k("players")).isArray() || request.value(k("players")).toArray().size() != participantCount) return invalid(probe ? k("probe_requires_two_fixed_players") : k("exactly_ten_players_required"));
    if (request.contains(k("auto_actions")) && !request.value(k("auto_actions")).isBool()) return invalid(k("auto_actions_must_be_explicit_boolean"));
    const bool automatic = request.value(k("auto_actions")).toBool(false);
    if(request.contains(k("simulate_natural_regeneration"))&&!request.value(k("simulate_natural_regeneration")).isBool())return invalid(k("natural_regeneration_requires_boolean"));
    const bool naturalRegeneration=request.value(k("simulate_natural_regeneration")).toBool(false);
    if(request.contains(k("acknowledgement_item_zone_model_v1"))&&!request.value(k("acknowledgement_item_zone_model_v1")).isBool())return invalid(k("item_zone_acknowledgement_must_be_boolean"));
    bool itemZoneModel=false;
    if(request.contains(k("item_zone_models"))) {
        const auto models=request.value(k("item_zone_models"));
        if(!models.isArray()||models.toArray().size()>1||(!models.toArray().isEmpty()&&models.toArray().first()!=QJsonValue(3118)))return invalid(k("unsupported_item_zone_model"));
        itemZoneModel=!models.toArray().isEmpty();
        if(itemZoneModel&&request.value(k("acknowledgement_item_zone_model_v1"))!=QJsonValue(true))return invalid(k("item_zone_model_acknowledgement_required"));
    }
    if(itemZoneModel&&naturalRegeneration)return invalid(k("item_zone_parallel_natural_regeneration_unsupported"));
    if(request.contains(k("acknowledge_documented_models_v9"))&&!request.value(k("acknowledge_documented_models_v9")).isBool())return invalid(k("documented_model_acknowledgement_must_be_boolean"));
    const auto objective = request.value(k("objective_team")).toString(k("ORDER")); if (objective != k("ORDER") && objective != k("CHAOS")) return invalid(k("invalid_objective_team"));
    QStringList selected;
    if (request.contains(k("policies"))) { if (!request.value(k("policies")).isArray()) return invalid(k("invalid_policies")); for (const auto &v : request.value(k("policies")).toArray()) { if (!v.isString() || !policies.contains(v.toString()) || selected.contains(v.toString())) return invalid(k("invalid_policy")); selected.append(v.toString()); } }
    else selected.append(request.value(k("policy")).toString(k("focus_damage")));
    if (selected.isEmpty() || selected.size() > 5 || !policies.contains(selected.first())) return invalid(k("invalid_policy"));
    auto opponent = request.value(k("opponent_policy")).toString(request.value(k("enemy_policy")).toString(request.contains(k("policies")) ? k("focus_damage") : selected.first()));
    const auto teamPolicies = request.value(k("team_policies")).toObject();
    if (teamPolicies.contains(objective) && !request.contains(k("policies"))) selected = {teamPolicies.value(objective).toString()};
    const auto enemyTeam = objective == k("ORDER") ? k("CHAOS") : k("ORDER"); if (teamPolicies.contains(enemyTeam)) opponent = teamPolicies.value(enemyTeam).toString();
    if (!policies.contains(opponent) || !policies.contains(selected.first())) return invalid(k("invalid_policy"));
    Weights weights; const auto weightValues = request.value(k("objective")).toObject();
    for (const auto &key : {k("damage_weight"), k("survival_weight"), k("death_penalty"), k("uncertainty_penalty")}) if (weightValues.contains(key) && !number(weightValues.value(key), 0, 100)) return invalid(k("invalid_objective_weight"));
    weights.damage = weightValues.value(k("damage_weight")).toDouble(1); weights.survival = weightValues.value(k("survival_weight")).toDouble(1); weights.death = weightValues.value(k("death_penalty")).toDouble(2); weights.uncertainty = weightValues.value(k("uncertainty_penalty")).toDouble(.1);
    const auto checkpoint = request.value(k("checkpoint")).toObject(); QMap<QString, QJsonObject> previous;
    if(!checkpoint.isEmpty()&&checkpoint.contains(k("item_zone_model_v1"))!=itemZoneModel)return invalid(k("checkpoint_item_zone_model_selection_mismatch"));
    if (!checkpoint.isEmpty()) {
        if(checkpoint.value(k("schema_version")).toInt()>=4 && checkpoint.value(k("integrity_sha256")).toString()!=checkpointChecksum(checkpoint)) return invalid(k("checkpoint_integrity_mismatch"));
        if (!number(checkpoint.value(k("as_of_time")), start - eps, start + eps) || checkpoint.value(k("combat_revision")).toString() != db.revision || checkpoint.value(k("players")).toArray().size() != participantCount) return invalid(k("checkpoint_time_revision_or_roster_mismatch"));
        for (const auto &v : checkpoint.value(k("players")).toArray()) { const auto q = v.toObject(); const auto key = q.value(k("player_key")).toString(); if (key.isEmpty() || previous.contains(key)) return invalid(k("checkpoint_duplicate_or_missing_player_key")); previous[key] = q; }
    }
    QVector<Fighter> p; QMap<QString, int> teams; QSet<QString> keys;
    for (const auto &v : request.value(k("players")).toArray()) {
        if (!v.isObject()) return invalid(k("invalid_player")); const auto row = v.toObject(); Fighter a;
        if (!probe && row.contains(k("probe_observed_stats"))) return invalid(k("probe_observed_stats_require_restricted_probe_scope"));
        a.probeBaseAttackOnly = probe;
        a.team = row.value(k("team")).toString();
        if ((a.team != k("ORDER") && a.team != k("CHAOS")) || !integer(row.value(k("champion_id")), 1, std::numeric_limits<int>::max(), a.champion) || !db.champions.contains(a.champion) || !integer(row.value(k("level")), 1, 18, a.level)) return invalid(k("invalid_player_identity"));
        ++teams[a.team]; a.key = row.value(k("player_key")).toString(a.team + k(":") + QString::number(a.champion) + k(":1"));
        if (a.key.isEmpty() || keys.contains(a.key)) return invalid(k("explicit_unique_player_keys_required_for_duplicate_champions")); keys.insert(a.key);
        if(row.contains(k("documented_profile_v9"))) {
            if(!row.value(k("documented_profile_v9")).isString()||row.value(k("documented_profile_v9")).toString().isEmpty())return invalid(k("invalid_documented_profile_selection"));
            if(request.value(k("acknowledge_documented_models_v9"))!=QJsonValue(true))return invalid(k("documented_model_acknowledgement_required"));
            const auto selectedProfile=row.value(k("documented_profile_v9")).toString();int matches=0;
            for(const auto &raw:db.champions.value(a.champion).value(k("documented_profiles_v9")).toArray())if(raw.toObject().value(k("profile_id")).toString()==selectedProfile) {a.documentedProfile=raw.toObject();++matches;}
            const auto program=a.documentedProfile.value(k("program")).toObject();const auto binding=program.value(k("binding_id")).toString();
            if(matches!=1||a.documentedProfile.value(k("execution_kind")).toString()!=k("documented_model_v9")||a.documentedProfile.value(k("server_mechanics_certified"))!=QJsonValue(false)||
               a.documentedProfile.value(k("acknowledgement_required"))!=QJsonValue(true)||!digestV9(a.documentedProfile.value(k("profile_sha256")).toString())||
               !digestV9(program.value(k("program_sha256")).toString())||program.value(k("champion_id")).toInt()!=a.champion||
               !((a.champion==25&&binding==k("25:W:documented_v9")&&program.value(k("slot")).toString()==k("W"))||(a.champion==8&&binding==k("8:E:documented_v9")&&program.value(k("slot")).toString()==k("E"))))return invalid(k("unknown_or_invalid_documented_profile_v9"));
            a.unknowns.insert(k("explicit_documented_model_v9_not_server_certified"));
            for(const auto &unknown:program.value(k("unknowns")).toArray())if(unknown.isString())a.unknowns.insert(unknown.toString());
            if(a.champion==8)a.unknowns.insert(k("documented_health_schedule_live_maximum_health_basis_is_explicit_model_policy"));
        }
        if(previous.contains(a.key)&&previous[a.key].value(k("documented_profile_v9")).toObject()!=documentedIdentity(a))return invalid(k("checkpoint_documented_profile_mismatch"));
        auto readIds = [&](const QJsonValue &raw, const QMap<int, CombatEntry> &entries, QList<int> &ids, bool unique) { if (!raw.isArray() || raw.toArray().size() > 6) return false; for (const auto &id : raw.toArray()) { int n = 0; if (!integer(id, 1, std::numeric_limits<int>::max(), n) || !entries.contains(n) || (unique && ids.contains(n))) return false; ids.append(n); } std::sort(ids.begin(), ids.end()); return true; };
        const auto augmentIds = row.contains(k("augments")) ? row.value(k("augments")) : row.value(k("augment_ids"));
        if (!readIds(row.value(k("items")), db.items, a.items, false) || !readIds(augmentIds.isUndefined() ? QJsonValue(QJsonArray{}) : augmentIds, db.augments, a.augments, true)) return invalid(k("unknown_or_invalid_loadout"));
        for (int id : a.augments) if (db.augments[id].requiredLevel > a.level) return invalid(k("augment_required_level_not_reached"));
        a.augmentsComplete = row.value(k("augments_complete")).toBool(false); if (!a.augmentsComplete) a.unknowns.insert(k("unobserved_augments_may_be_missing"));
        a.context = db.context(a.champion, a.level); a.context.explicitActionCounts = true; a.context.horizon = .001; a.context.ranks = referenceCombatRanks(db, a.champion, a.level);
        a.context.restrictExplicitComponents = probe;
        const auto ranks = row.contains(k("ranks")) ? row.value(k("ranks")) : row.value(k("abilities"));
        if (!ranks.isUndefined() && !ranks.isObject()) return invalid(k("invalid_skill_ranks"));
        const auto rankValues = ranks.toObject(); a.referenceRanks = rankValues.size() < 4;
        for (auto it = rankValues.begin(); it != rankValues.end(); ++it) { const auto n = it.value().isObject() ? it.value().toObject().value(k("abilityLevel")) : it.value(); int rank = 0; if (!QStringList{k("Q"), k("W"), k("E"), k("R")}.contains(it.key()) || !integer(n, 0, 6, rank)) return invalid(k("invalid_skill_rank")); a.context.ranks[it.key()] = rank; }
        QStringList rankUnknowns; if (!sandboxSkillRanksLegal(db, a.champion, a.level, a.context.ranks, &rankUnknowns)) return invalid(k("skill_rank_points_or_unlock_level_invalid")); for (const auto &u : rankUnknowns) a.unknowns.insert(u);
        if (a.referenceRanks) a.unknowns.insert(k("reference_skill_ranks_are_scenario_assumptions"));
        for (const auto &condition : row.value(k("conditions")).toArray()) if (condition.isString()) a.context.conditions.insert(condition.toString());
        const auto counters = row.value(k("counters")).toObject(); for (auto it = counters.begin(); it != counters.end(); ++it) if (number(it.value(), -1e9, 1e9)) a.context.counters[it.key()] = it.value().toDouble();
        const auto execution = row.value(k("execution")).toObject();
        for (auto it = execution.begin(); it != execution.end(); ++it) if (!QStringList{k("output_factor"), k("damage_taken_factor"), k("support_factor")}.contains(it.key()) || !number(it.value(), 0, it.key() == k("damage_taken_factor") ? 2 : 1)) return invalid(k("invalid_execution_factor"));
        a.output = execution.value(k("output_factor")).toDouble(1); a.taken = execution.value(k("damage_taken_factor")).toDouble(1); a.support = execution.value(k("support_factor")).toDouble(1);
        if (probe) {
            a.probeObservedAnchor = anchorObservedCombatStats(db, a.context, a.items, a.augments, row.value(k("probe_observed_stats")).toObject());
            if (a.probeObservedAnchor.value(k("baseline_matched")) != QJsonValue(true)) return invalid(k("probe_pre_contact_panel_anchor_failed"));
            a.unknowns.insert(k("given_contact_base_attack_only_mode_multiplier_one_unverified"));
        }
        const auto evaluated = evaluateCombat(db, a.context, a.items, a.augments); if (!evaluated.valid || !std::isfinite(evaluated.stats[Stat::Hp]) || evaluated.stats[Stat::Hp] <= 0) return invalid(k("loadout_numeric_model_unavailable"));
        a.stats = evaluated.stats; a.maximum = a.stats[Stat::Hp]; a.threat = (a.stats[Stat::Ad] * std::max(0., a.stats[Stat::As]) + .6 * a.stats[Stat::Ap]) * a.output;
        recordStatPassUnknowns(db,a,evaluated.unknowns);
        const bool dead = row.value(k("is_dead")).toBool(false);
        if (row.contains(k("current_health")) && !number(row.value(k("current_health")), 0, 1e9)) return invalid(k("invalid_current_health"));
        if (dead && row.value(k("current_health")).toDouble() > 0) return invalid(k("dead_positive_health_conflict"));
        a.hp = dead ? 0 : row.value(k("current_health")).toDouble(a.maximum);
        auto rule = db.champions.value(a.champion).value(k("action_rules_v3")).toObject(); if (rule.isEmpty()) return invalid(k("reviewed_action_rules_unavailable"));
        if(a.documentedProfile.value(k("action_contract")).toObject().value(k("kind")).toString()==k("documented_health_schedule_v9"))rule[k("documented_profile_v9")]=a.documentedProfile;
        QJsonArray items, augments; for (int id : a.items) items.append(id); for (int id : a.augments) augments.append(id);
        QJsonObject rankJson; for (auto it = a.context.ranks.begin(); it != a.context.ranks.end(); ++it) rankJson[it.key()] = it.value();
        a.fingerprint = QString::fromUtf8(QJsonDocument(QJsonObject{{k("champion"), a.champion}, {k("team"), a.team}, {k("level"), a.level}, {k("items"), items}, {k("augments"), augments}, {k("ranks"), rankJson}, {k("execution"), execution}, {k("conditions"), row.value(k("conditions"))}, {k("counters"), row.value(k("counters"))}}).toJson(QJsonDocument::Compact));
        if(!a.documentedProfile.isEmpty())a.fingerprint+=QString::fromUtf8(QJsonDocument(documentedIdentity(a)).toJson(QJsonDocument::Compact));
        auto initial = row.value(k("action_state")).toObject(); const auto stateOverrides = request.value(k("action_states")).toObject(); if (stateOverrides.value(a.key).isObject()) initial = stateOverrides.value(a.key).toObject();
        initial[k("alive")] = a.hp > eps;
        if (previous.contains(a.key)) {
            const auto q = previous[a.key]; if (q.value(k("fingerprint")).toString() != a.fingerprint || !number(q.value(k("remaining_health")), 0, 1e9)) return invalid(k("checkpoint_player_model_mismatch"));
            a.hp = q.value(k("remaining_health")).toDouble(); initial = {{k("checkpoint"), q.value(k("action_state"))}, {k("initial_form"), q.value(k("action_state")).toObject().value(k("form"))}};
            QString error; if (!a.control.restore(q.value(k("control_state")).toObject(), &error)) return invalid(k("checkpoint_control:") + error);
            a.spreadCursor = q.value(k("spread_cursor")).toInt();
            for (const auto &s : q.value(k("shields")).toArray()) { const auto shield = s.toObject(); if (!number(shield.value(k("amount")), 0, 1e9) || !number(shield.value(k("expires_at")), start, 1e7)) return invalid(k("checkpoint_invalid_shield")); a.shields.append({shield.value(k("source")).toString(), shield.value(k("slot")).toString(), shield.value(k("amount")).toDouble(), shield.value(k("expires_at")).toDouble()}); }
        } else if (!previous.isEmpty()) return invalid(k("checkpoint_player_missing"));
        QString error;
        if (previous.contains(a.key) && previous[a.key].value(k("progression_state")).isObject()) {
            if (!a.progression.restore(db, previous[a.key].value(k("progression_state")).toObject(), &error)) return invalid(k("progression_checkpoint:") + error);
            a.context = a.progression.contextAt(start); a.level = a.context.level; a.items = a.progression.items(); a.augments = a.progression.augments();
            const auto current = evaluateCombat(db, a.context, a.items, a.augments); if (!current.valid) return invalid(k("progression_checkpoint_numeric_model")); a.stats = current.stats;
        } else if (!a.progression.load(db, a.context, a.items, a.augments, start, &error)) return invalid(k("progression_state:") + error);
        if (!a.action.load(rule, a.context, a.stats, initial, start, &error)) return invalid(k("action_state:") + a.key + k(":") + error);
        for(const auto &unknown:a.action.snapshot().value(k("resource_capacity_unknowns")).toArray())if(unknown.isString())a.unknowns.insert(unknown.toString());
        auto statePrograms=db.champions.value(a.champion).value(k("stateful_bindings_v7")).toArray();
        if(!a.documentedProfile.isEmpty()) {const auto program=a.documentedProfile.value(k("program")).toObject();for(int i=statePrograms.size()-1;i>=0;--i)if(statePrograms[i].toObject().value(k("slot"))==program.value(k("slot")))statePrograms.removeAt(i);statePrograms.append(program);}
        if(!a.stateful.load(statePrograms,a.key,a.team,a.champion,db.revision,start,&error))return invalid(k("stateful_programs:")+error);
        if(previous.contains(a.key)&&previous[a.key].contains(k("documented_charge_v9"))) {
            a.documentedCharge=previous[a.key].value(k("documented_charge_v9")).toObject();const auto charge=a.documentedCharge;
            if(a.documentedProfile.isEmpty()||charge.value(k("profile_id"))!=a.documentedProfile.value(k("profile_id"))||charge.value(k("profile_sha256"))!=a.documentedProfile.value(k("profile_sha256"))||
               charge.value(k("binding_id"))!=a.documentedProfile.value(k("program")).toObject().value(k("binding_id"))||!number(charge.value(k("started_at")),0,start)||
               !number(charge.value(k("active_until")),charge.value(k("started_at")).toDouble(),1e7+300)||!charge.value(k("active")).isBool()||charge.value(k("instance_id")).toString().isEmpty())return invalid(k("checkpoint_documented_charge_invalid"));
            const auto fees=charge.value(k("phases")).toObject();for(auto fee=fees.begin();fee!=fees.end();++fee) {const auto r=fee.value().toObject();int phase=-1;if(!integer(r.value(k("phase")),0,3,phase)||fee.key()!=QString::number(phase)||r.value(k("instance_id"))!=charge.value(k("instance_id"))||r.value(k("profile_sha256"))!=charge.value(k("profile_sha256"))||
                !number(r.value(k("time")),0,start)||!number(r.value(k("pre_health")),0,1e9)||!number(r.value(k("post_health")),0,1e9)||!number(r.value(k("paid")),0,1e9)||
                std::abs(r.value(k("pre_health")).toDouble()-r.value(k("post_health")).toDouble()-r.value(k("paid")).toDouble())>eps||r.value(k("committed"))!=QJsonValue(true))return invalid(k("checkpoint_documented_fee_invalid"));}
        }
        if(previous.contains(a.key)&&previous[a.key].value(k("stateful_state_v7")).isObject()) {
            if(!a.stateful.restore(previous[a.key].value(k("stateful_state_v7")).toObject(),&error))return invalid(k("stateful_checkpoint:")+error);
            a.damageReceipts=previous[a.key].value(k("damage_receipts_v7")).toObject();a.entityObservations=previous[a.key].value(k("entity_observations_v7")).toObject();
            for(auto it=a.entityObservations.begin();it!=a.entityObservations.end();++it) {const auto observation=it.value().toObject(),stats=observation.value(k("stats")).toObject();if(!number(observation.value(k("time")),0,start)||!number(observation.value(k("entity_created_at")),0,start)||!number(stats.value(k("armor")),-1e4,1e6)||!number(stats.value(k("mr")),-1e4,1e6)||!number(stats.value(k("ms")),0,1e5))return invalid(k("stateful_checkpoint_invalid_entity_observation"));}
            for(auto it=a.damageReceipts.begin();it!=a.damageReceipts.end();++it) {const auto receipt=it.value().toObject();if(receipt.value(k("receipt_id")).toString()!=it.key()||!number(receipt.value(k("time")),0,start)||!number(receipt.value(k("actual_damage")),0,1e12)||!number(receipt.value(k("shield_damage")),0,1e12)||(receipt.value(k("source_key")).toString()!=a.key&&receipt.value(k("target_key")).toString()!=a.key&&receipt.value(k("damage_target_owner_key")).toString()!=a.key))return invalid(k("stateful_checkpoint_invalid_damage_receipt"));}
            a.deathAt=previous[a.key].value(k("death_time")).toDouble(-1);
        }
        const auto stateInstances=a.stateful.snapshot().value(k("instances")).toObject();for(auto instance=stateInstances.begin();instance!=stateInstances.end();++instance) {const auto row=instance.value().toObject();if(!row.value(k("owned_timer_observations")).toObject().isEmpty())a.unknowns.insert(k("awakening_pool_starts_from_verified_preconsumed_scenario_observation"));const auto entities=row.value(k("entities")).toObject();for(auto entity=entities.begin();entity!=entities.end();++entity)if(entity.value().toObject().value(k("attack_interval_basis")).toString()==k("observed"))a.unknowns.insert(k("entity_attack_clock_is_verified_scenario_observation_not_source_cadence_repair"));}
        a.stateContinuousAt=start;
        for(const auto &raw:a.stateful.activeEffects(start)) {auto buff=raw.toObject();if(buff.value(k("op")).toString()==k("stat_buff")&&buff.value(k("target_key")).toString()==a.key) {if(!buff.contains(k("expires_at")))buff[k("expires_at")]=1e12;a.stateBuffs.append(buff);}}
        const double savedHp=a.hp;
        if (!refreshStats(db, a, start, &error)) return invalid(k("initial_form_stats:") + error);
        if((previous.contains(a.key)||row.contains(k("current_health"))) && savedHp>a.maximum+eps) return invalid(previous.contains(a.key)?k("checkpoint_health_exceeds_current_form_maximum"):k("current_health_exceeds_initial_form_maximum"));
        if(!previous.contains(a.key)&&!row.contains(k("current_health"))&&!dead)a.hp=a.maximum;
        if(previous.contains(a.key) && previous[a.key].contains(k("maximum_health")) && !number(previous[a.key].value(k("maximum_health")),a.maximum-eps,a.maximum+eps)) return invalid(k("checkpoint_current_form_maximum_mismatch"));
        if(previous.contains(a.key)) {const auto ready=previous[a.key].value(k("temporal_ready_at")).toObject();for(auto it=ready.begin();it!=ready.end();++it) {if(!number(it.value(),0,1e7))return invalid(k("checkpoint_invalid_temporal_ready_time"));a.temporalReadyAt[it.key()]=it.value().toDouble();}}
        a.initial = a.hp; a.initiallyDead = a.hp <= eps;
        a.permanentHpBaseline=a.context.permanentEventStats[Stat::Hp];
        for (const auto &u : rule.value(k("unknowns")).toArray()) if (u.isString()) a.unknowns.insert(u.toString());
        p.append(a);
    }
    if (teams.value(k("ORDER")) != participantCount / 2 || teams.value(k("CHAOS")) != participantCount / 2) return invalid(probe ? k("probe_requires_one_player_per_team") : k("exactly_five_players_per_team_required"));
    if(request.contains(k("combat_units_v8"))&&!request.value(k("combat_units_v8")).isArray())return invalid(k("combat_units_v8_must_be_observation_array"));
    CombatUnitsV8 units;QString unitError;
    if(checkpoint.contains(k("combat_units_v8"))) {
        if(!checkpoint.value(k("combat_units_v8")).isObject()||!units.restore(checkpoint.value(k("combat_units_v8")).toObject(),keys,&unitError)||units.time()>start||!units.advanceTo(start,&unitError))return invalid(k("combat_units_checkpoint:")+unitError);
        if(request.contains(k("combat_units_v8"))) {CombatUnitsV8 observed;if(!observed.loadObserved(request.value(k("combat_units_v8")).toArray(),keys,start,&unitError)||observed.snapshot().value(k("observations"))!=units.snapshot().value(k("observations")))return invalid(k("combat_units_resume_observation_mismatch"));}
    } else if(!units.loadObserved(request.value(k("combat_units_v8")).toArray(),keys,start,&unitError))return invalid(k("combat_units_observation:")+unitError);
    for(const auto &owner:p)for(auto it=owner.damageReceipts.begin();it!=owner.damageReceipts.end();++it) {const auto receipt=it.value().toObject();if(units.contains(receipt.value(k("target_key")).toString())&&receipt.value(k("damage_target_owner_key")).toString().isEmpty()&&units.receipt(it.key())!=receipt)return invalid(k("combat_units_checkpoint_receipt_mismatch"));}
    std::sort(p.begin(), p.end(), [](const Fighter &a, const Fighter &b) { return a.key < b.key; });
    ItemZoneLedger itemZones;
    if(itemZoneModel) {
        QVector<ItemZoneLedger::Participant> roster;for(const auto &owner:p)roster.append({owner.key,owner.team});QString zoneError;
        if(!itemZones.load(db,roster,start,&zoneError))return invalid(zoneError);
        if(!checkpoint.isEmpty()) {
            const auto saved=checkpoint.value(k("item_zone_model_v1")).toObject();
            if(!itemZones.restore(saved,&zoneError)||std::abs(itemZones.time()-start)>eps)return invalid(k("item_zone_checkpoint:")+zoneError);
            // A replayed journal proves clocks/contacts; stored native damage
            // receipts additionally bind each trigger to this R pipeline.
            for(const auto &raw:saved.value(k("journal")).toArray()) {
                const auto entry=raw.toObject();if(entry.value(k("op"))!=k("trigger"))continue;
                const auto trigger=entry.value(k("receipt")).toObject();const int actor=find(p,trigger.value(k("owner_key")).toString());
                if(actor<0)return invalid(k("item_zone_checkpoint_owner_receipt_missing"));
                const auto receipt=p[actor].damageReceipts.value(trigger.value(k("receipt_id")).toString()).toObject();
                if(receipt.value(k("source_key"))!=trigger.value(k("owner_key"))||receipt.value(k("target_key"))!=trigger.value(k("target_key"))||
                   receipt.value(k("time"))!=trigger.value(k("time"))||receipt.value(k("item_zone_origin_family"))!=k("champion_ultimate")||
                   receipt.value(k("item_zone_source_slot"))!=k("R")||receipt.value(k("item_zone_cast_instance_id"))!=trigger.value(k("cast_instance_id"))||
                   receipt.value(k("item_zone_ultimate_actual_damage"))!=trigger.value(k("actual_damage"))||
                   receipt.value(k("item_zone_owner_had_item"))!=trigger.value(k("owner_currently_has_item")))return invalid(k("item_zone_checkpoint_native_R_receipt_mismatch"));
            }
        }
    }
    syncStateBuffs(p,start,itemZoneModel?&itemZones:nullptr);for(auto &a:p) {QString error;if(!refreshStats(db,a,start,&error))return invalid(k("stateful_initial_stats:")+error);}
    QVector<QJsonObject> events; QSet<QString> eventIds, processed;
    for (const auto &v : checkpoint.value(k("processed_event_ids")).toArray()) if (v.isString()) processed.insert(v.toString());
    if (request.contains(k("action_events")) && (!request.value(k("action_events")).isArray() || request.value(k("action_events")).toArray().size() > 5000)) return invalid(k("invalid_action_events"));
    for (const auto &v : request.value(k("action_events")).toArray()) {
        if (!v.isObject()) return invalid(k("action_event_must_be_object")); auto event = v.toObject(); const auto id = event.value(k("event_id")).toString();
        if (id.isEmpty() || id.size() > 160 || id.startsWith(k("auto:")) || eventIds.contains(id) || !number(event.value(k("time")), start, end) || event.value(k("time")).toDouble() >= end) return invalid(k("invalid_action_event_id_or_time"));
        for(auto it=event.begin();it!=event.end();++it) if(it.key().startsWith(k("_temporal"))||it.key().startsWith(k("_ledger_"))||it.key().startsWith(k("_state"))||it.key()==k("_delivery")||it.key().startsWith(k("_item_zone"))) return invalid(k("reserved_internal_temporal_field"));
        if (event.contains(k("hit_factor")) && !number(event.value(k("hit_factor")), 0, 1)) return invalid(k("invalid_event_hit_factor"));
        if (event.contains(k("hit_confirmed")) && !event.value(k("hit_confirmed")).isBool()) return invalid(k("invalid_hit_confirmation"));
        eventIds.insert(id); events.append(event);
    }
    std::stable_sort(events.begin(), events.end(), [](const QJsonObject &a, const QJsonObject &b) { const auto at = a.value(k("time")).toDouble(), bt = b.value(k("time")).toDouble(); return std::abs(at - bt) > eps ? at < bt : a.value(k("event_id")).toString() < b.value(k("event_id")).toString(); });
    QMap<QString,CastInstance> instances;
    for (const auto &raw : checkpoint.value(k("cast_instances")).toArray()) {
        const auto q=raw.toObject();CastInstance v;v.id=q.value(k("cast_instance_id")).toString();v.actor=q.value(k("player_key")).toString();
        v.slot=q.value(k("slot")).toString();v.binding=q.value(k("binding_id")).toString();v.created=q.value(k("created_at")).toDouble();
        v.expires=q.value(k("expires_at")).toDouble();v.cancelled=q.value(k("cancelled")).toBool();
        v.activationAt=q.value(k("activation_at")).toDouble(0);v.activationChannel=q.value(k("activation_requires_completed_channel")).toBool(false);
        for(const auto &phase:q.value(k("delivered_phases")).toArray())v.delivered.insert(phase.toString());
        if(v.id.isEmpty()||instances.contains(v.id)||find(p,v.actor)<0||!number(q.value(k("created_at")),0,1e7)||!number(q.value(k("expires_at")),v.created,1e7)||
           (q.contains(k("activation_at"))&&!number(q.value(k("activation_at")),0,1e7))||
           (q.contains(k("activation_requires_completed_channel"))&&!q.value(k("activation_requires_completed_channel")).isBool())||
           (v.activationChannel&&(v.activationAt<=v.created||v.activationAt>v.expires)))return invalid(k("checkpoint_invalid_cast_instance"));
        instances[v.id]=v;
    }
    QVector<Pending> pending;
    for (const auto &v : checkpoint.value(k("pending_casts")).toArray()) { const auto q = v.toObject(); const auto event = q.value(k("event")).toObject(); if (!number(q.value(k("time")), start, 1e7) || find(p, event.value(k("player_key")).toString()) < 0) return invalid(k("checkpoint_invalid_pending_cast")); Pending entry;entry.time=q.value(k("time")).toDouble();entry.event=event;entry.kind=q.value(k("kind")).toString(k("cast_completion"));entry.instance=q.value(k("cast_instance_id")).toString();entry.locksActor=q.value(k("locks_actor")).toBool(true);entry.cancelOnInterrupt=q.value(k("cancel_on_interrupt")).toBool(true);entry.cancelOnSourceDeath=q.value(k("cancel_on_source_death")).toBool(true);entry.cancelOnTargetDeath=q.value(k("cancel_on_target_death")).toBool(false);if(!entry.instance.isEmpty()&&!instances.contains(entry.instance))return invalid(k("checkpoint_pending_instance_missing"));pending.append(entry); }
    for(const auto &entry:pending) if(entry.event.contains(k("_ledger_jhin_shot"))) {
        const auto receipt=entry.event.value(k("_ledger_jhin_shot")).toObject();const auto instanceId=receipt.value(k("source_instance_id")).toString();const auto shot=k("shot:")+QString::number(receipt.value(k("shot_number")).toInt());const int actor=find(p,entry.event.value(k("player_key")).toString());
        if(!ledgerJhinShot(entry.event,p[actor].champion,db.revision,instances)||entry.time!=receipt.value(k("time")).toDouble()||instances[instanceId].delivered.contains(shot))return invalid(k("checkpoint_invalid_ledger_channel_shot"));
    }
    for(const auto &entry:pending)if(entry.kind==k("documented_charge_fee_v9")) {
        const int actor=find(p,entry.event.value(k("player_key")).toString());const auto &owner=p[actor];const auto charge=owner.documentedCharge;
        const auto contract=owner.documentedProfile.value(k("action_contract")).toObject();const int phase=entry.event.value(k("_state_fee_phase_v9")).toInt(-1);const auto offsets=contract.value(k("payment_offsets_seconds")).toArray();
        if(contract.value(k("kind")).toString()!=k("documented_health_schedule_v9")||phase<0||phase>=offsets.size()||entry.instance!=charge.value(k("instance_id")).toString()||
           entry.event.value(k("cast_instance_id")).toString()!=entry.instance||entry.locksActor||!entry.cancelOnInterrupt||!entry.cancelOnSourceDeath||entry.cancelOnTargetDeath||
           charge.value(k("phases")).toObject().contains(QString::number(phase))||std::abs(entry.time-charge.value(k("started_at")).toDouble()-offsets.at(phase).toDouble())>eps)
            return invalid(k("checkpoint_documented_fee_pending_invalid"));
    }
    QJsonArray results; QJsonObject best; double bestValue = -std::numeric_limits<double>::infinity();
    double nextDecision = start; int autoIndex = 0;
    if (!checkpoint.isEmpty()) {
        if (!number(checkpoint.value(k("next_decision_time")), 0, 1e7) || !integer(checkpoint.value(k("auto_event_index")), 0, 100000000, autoIndex) || !number(checkpoint.value(k("decision_step_seconds")), step - eps, step + eps) || checkpoint.value(k("automatic_actions")).toBool() != automatic || checkpoint.value(k("simulate_natural_regeneration")).toBool(false)!=naturalRegeneration) return invalid(k("checkpoint_automatic_schedule_mismatch"));
        nextDecision = checkpoint.value(k("next_decision_time")).toDouble();
        if (automatic && nextDecision < start - eps) return invalid(k("checkpoint_automatic_schedule_before_start"));
    }
    for (const auto &policy : selected) { auto result = simulate(catalog, p, events, pending, processed, instances, start, end, step, automatic, policy, opponent, objective, weights, nextDecision, autoIndex, naturalRegeneration,units,itemZoneModel,itemZones); if(result.value(k("available"))==QJsonValue(false))return result; const double value = result.value(k("objective_value")).toDouble(); if (value > bestValue + eps) { best = result; bestValue = value; } results.append(result); }
    QJsonObject out = best;
    out[k("schema_version")] = 4; out[k("available")] = true; out[k("status")] = k("conditional_continuous_action_sandbox"); out[k("patch")] = catalog.patch; out[k("combat_revision")] = db.revision;
    out[k("best_policy")] = best.value(k("policy")); out[k("policy_results")] = results; out[k("actual_win_probability")] = QJsonValue::Null; out[k("globally_optimal_gameplay")] = false;
    out[k("timing_contract")] = k("absolute_half_open_window; explicit_progression_and_control_then_action_admission_then_due_temporal_phases_then_support_then_simultaneous_damage; cooldown_ammo_resource_progression_pending_instances_persist");
    out[k("numeric_formula_source")] = k("evaluateCombat_explicitActionCounts_only_no_horizon_invented_casts_or_attacks"); out[k("horizon_seconds")] = horizon; out[k("decision_step_seconds")] = step; out[k("deterministic")] = true;
    if(itemZoneModel)out[k("item_zone_model_v1")]=QJsonObject{{k("item_ids"),QJsonArray{3118}},{k("model"),k("conditional_explicit_contact_rate_integral")},
        {k("settlement_order"),k("preceding_contact_integral_then_upkeep_then_boundary_actions; first_R_before_new_zone_contact")},
        {k("existing_zone_inventory_loss_policy"),k("creation_owned_zone_persists_to_expiry_as_explicit_model")},{k("parallel_health_integrals"),k("unsupported_rejected")},{k("health_gated_damage_modifiers"),k("unsupported_rejected")},{k("server_tick_phase_assumed"),false},{k("contacts_are_scenario_inputs"),true},{k("live_certified"),false}};
    out[k("objective_formula")] = k("damage_weight*clamp(net_enemy_HP_loss_excluding_self_cost_external_loss_with_respawn_restore/enemy_max_HP,0,1)+survival_weight*clamp(own_remaining_HP/own_max_HP,0,1)-death_penalty*own_deaths/5-uncertainty_penalty*uncertain_players/5");
    out[k("policy_search_scope")] = k("enumerated_synthetic_target_policies_with_fixed_opponent_not_global_game_optimum");
    if (probe) {
        QJsonObject anchors; for (const auto &actor : p) anchors[actor.key] = actor.probeObservedAnchor;
        out[k("participant_scope")] = k("basic_attack_probe_v1"); out[k("probe_pre_contact_anchors")] = anchors;
        out[k("objective_value")] = QJsonValue::Null; out[k("objective_components")] = QJsonObject{};
        out[k("objective_formula")] = k("not_applicable_single_given_contact_impact");
        out[k("policy_search_scope")] = k("none_single_explicit_base_attack_impact");
    }
    return out;
}
