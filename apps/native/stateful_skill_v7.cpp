#include "stateful_skill_v7.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QString k(const char *s) { return QString::fromLatin1(s); }
constexpr double eps = 1e-8;
bool finite(const QJsonValue &v, double lo=-1e12, double hi=1e12) {
    return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble()>=lo && v.toDouble()<=hi;
}
bool token(const QString &s) { return !s.isEmpty() && s.size()<=512; }
bool fail(QString *error, const QString &reason) { if(error)*error=reason;return false; }
QJsonObject rejected(QString *error,const QString &reason) {
    fail(error,reason); return {{k("accepted"),false},{k("reason"),reason},{k("effects"),QJsonArray{}}};
}
QString hash(const QJsonArray &rows) {
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(rows).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
}
QJsonObject fresh(const QJsonObject &program,double at) {
    return {{k("state"),program.value(k("initial_state")).toString(k("idle"))},
        {k("entered_at"),at},{k("started_at"),at},{k("last_event_at"),at},{k("expires_at"),-1},
        {k("target_key"),QString()},{k("counters"),QJsonObject{}},{k("counter_expiries"),QJsonObject{}},
        {k("entities"),QJsonObject{}},{k("entity_damage_receipts"),QJsonObject{}},{k("records"),QJsonObject{}},{k("record_receipts"),QJsonArray{}},
        {k("active_effects"),QJsonArray{}},{k("clocks"),QJsonObject{}},{k("resource_pools"),QJsonObject{}}};
}
QJsonValue valueOrExpression(const QJsonObject &object,const QString &key) {
    return object.contains(key+k("_expression"))?object.value(key+k("_expression")):object.value(key);
}
std::optional<double> evaluate(const QJsonValue &expression,const CombatContext &context,const CombatStats &stats,int rank) {
    if(expression.isDouble()) return finite(expression)?std::optional<double>(expression.toDouble()):std::nullopt;
    if(!expression.isObject())return {};
    return compileCombatFormula(expression.toObject(),context,rank).evaluate(context,stats);
}
QString counterKey(const QJsonObject &counter,const QString &target) {
    const auto name=counter.value(k("shared_key")).toString(counter.value(k("key")).toString());
    return name+(counter.value(k("per_target")).toBool()?k("|")+target:QString());
}
QString recordKey(const QJsonObject &effect,const QString &target) {
    return effect.value(k("key")).toString(k("damage"))+(effect.value(k("per_target")).toBool()?k("|")+target:QString());
}
QSet<QString> protectedKeys() {
    return {k("elapsed"),k("state_elapsed"),k("transition_elapsed"),k("state_counter"),k("counter_before"),k("counter_after"),
        k("counter_triggered"),k("recorded_damage"),k("entity_count"),k("self_current_hp"),k("self_max_hp"),
        k("self_missing_hp"),k("self_health_fraction"),k("self_current_mana"),k("current_resource"),k("resource_current"),
        k("target_current_hp"),k("target_max_hp"),k("target_hp"),k("target_missing_hp"),k("target_health_fraction"),
        k("target_health_ratio"),k("actual_damage"),k("owned_shield_remaining"),k("shield_remaining"),k("source_rank"),k("current_rank"),
        k("target_is_bound"),k("base_attack_raw"),k("cast_resource_cost"),k("entity_current_hp"),k("entity_attacks"),k("champion_level"),
        k("owner_attack_ready_at"),k("owner_attack_receipt_time"),k("owner_attack_receipt_id"),k("owner_attack_owner_key"),k("attack_admitted"),k("hero_hit_receipt_v1")};
}
bool reservedFact(const QString &name) {
    return protectedKeys().contains(name)||name.startsWith(k("counter:"))||name.startsWith(k("state_counter:"))||
        name.startsWith(k("ledger_counter:"))||name.startsWith(k("recorded_damage:"))||name.startsWith(k("owned_timer_remaining:"));
}
void addCounterAliases(CombatContext &context,const QJsonObject &values,const QString &target) {
    for(auto it=values.begin();it!=values.end();++it) {
        if(!finite(it.value()))continue;
        QString name=it.key();const auto separator=name.indexOf(k("|"));
        if(separator>=0) { if(name.mid(separator+1)!=target)continue;name=name.left(separator); }
        for(const auto &prefix:{k("counter:"),k("state_counter:"),k("ledger_counter:")})context.counters[prefix+name]=it.value().toDouble();
    }
}
bool matchState(const QJsonValue &from,const QString &state) {
    if(from.isString())return from.toString()==state||from.toString()==k("*");
    for(const auto &entry:from.toArray())if(entry.toString()==state||entry.toString()==k("*"))return true;
    return false;
}
bool compare(double left,const QString &op,double right) {
    if(op==k("le"))return left<=right+eps;
    if(op==k("lt"))return left<right;
    if(op==k("ge"))return left+eps>=right;
    if(op==k("gt"))return left>right;
    if(op==k("eq"))return std::abs(left-right)<=eps;
    if(op==k("ne"))return std::abs(left-right)>eps;
    return false;
}
bool numericMap(const QJsonObject &map,double low=-1e12,double high=1e12) {
    for(auto it=map.begin();it!=map.end();++it)if(!finite(it.value(),low,high))return false;
    return true;
}
bool poolActive(const QJsonObject &pool,const QString &state) {return pool.value(k("active_states")).toArray().contains(state);}
double poolDepletionAt(const QJsonObject &pool,double value,double from) {
    if(value<=eps)return from;
    const double late=pool.value(k("late_at")).toDouble(-1),rate=pool.value(k("decay_rate")).toDouble();
    if(late>from) {
        if(rate>0&&value<=rate*(late-from)+eps)return from+value/rate;
        value-=rate*(late-from);from=late;
    }
    const double effectiveRate=late>=0&&from>=late?pool.value(k("late_decay_rate")).toDouble(rate):rate;
    return effectiveRate>0?from+value/effectiveRate:std::numeric_limits<double>::infinity();
}
const QSet<QString> effectOps={k("damage"),k("heal"),k("shield"),k("health_cost"),k("stat_buff"),k("control"),k("resource"),
    k("cooldown"),k("spawn_entity"),k("destroy_entity"),k("entity_damage"),k("reveal"),k("untargetable"),
    k("record_damage"),k("replay_damage"),k("resource_upkeep"),k("heal_over_time"),k("counter"),k("reset_counter"),
    k("damage_reduction"),k("movement"),k("position"),k("attach"),k("detach"),k("mark"),k("execute")};
}

bool StatefulSkillLedgerV7::load(const QJsonArray &programs,const QString &ownerKey,const QString &team,
    int championId,const QString &revision,double start,QString *error) {
    if(!token(ownerKey)||!token(team)||championId<=0||!token(revision)||!finite(start,0,1e9))return fail(error,k("stateful_invalid_identity"));
    StatefulSkillLedgerV7 next;next.owner_=ownerKey;next.team_=team;next.champion_=championId;next.revision_=revision;
    next.time_=start;next.programHash_=hash(programs);
    for(const auto &entry:programs) {
        if(!entry.isObject())return fail(error,k("stateful_invalid_program"));
        const auto program=entry.toObject();const auto id=program.value(k("binding_id")).toString();
        if(!token(id)||next.programs_.contains(id)||program.value(k("champion_id")).toInt()!=championId||
            !token(program.value(k("slot")).toString())||!program.value(k("transitions")).isArray()||program.value(k("transitions")).toArray().isEmpty())return fail(error,k("stateful_invalid_program_identity"));
        QSet<QString> transitions;
        for(const auto &raw:program.value(k("transitions")).toArray()) {
            if(!raw.isObject())return fail(error,k("stateful_invalid_transition"));
            const auto transition=raw.toObject();const auto tid=transition.value(k("id")).toString();
            if(!token(tid)||transitions.contains(tid)||(!transition.value(k("from")).isArray()&&!transition.value(k("from")).isString())||
                !token(transition.value(k("to")).toString()))return fail(error,k("stateful_invalid_transition"));
            transitions.insert(tid);
            for(const auto &effect:transition.value(k("effects")).toArray())if(!effect.isObject()||!effectOps.contains(effect.toObject().value(k("op")).toString()))return fail(error,k("stateful_unsupported_effect"));
        }
        next.programs_[id]=program;next.instances_[id]=fresh(program,start);
    }
    next.loaded_=true;*this=next;return true;
}

bool StatefulSkillLedgerV7::advanceInPlace(double at,QString *error) {
    if(!loaded_||!finite(at,time_,1e9))return fail(error,k("stateful_nonmonotonic_time"));
    for(auto it=instances_.begin();it!=instances_.end();++it) {
        auto instance=it.value();const auto program=programs_.value(it.key());
        auto pools=instance.value(k("resource_pools")).toObject(),poolCounters=instance.value(k("counters")).toObject();
        for(auto poolIt=pools.begin();poolIt!=pools.end();++poolIt) {
            auto pool=poolIt.value().toObject();if(!poolActive(pool,instance.value(k("state")).toString()))continue;
            const double from=pool.value(k("last_integrated_at")).toDouble(),value=poolCounters.value(poolIt.key()).toDouble();
            const double depletedAt=poolDepletionAt(pool,value,from),late=pool.value(k("late_at")).toDouble(-1);
            double drain=0;
            if(late<0||at<=late)drain=(at-from)*pool.value(k("decay_rate")).toDouble();
            else drain=std::max(0.,late-from)*pool.value(k("decay_rate")).toDouble()+(at-std::max(from,late))*pool.value(k("late_decay_rate")).toDouble();
            poolCounters[poolIt.key()]=std::max(0.,value-std::max(0.,drain));pool[k("last_integrated_at")]=at;poolIt.value()=pool;
            if(depletedAt<=at+eps) {
                instance[k("state")]=pool.value(k("on_empty_state")).toString(program.value(k("initial_state")).toString(k("idle")));
                instance[k("entered_at")]=depletedAt;instance[k("expires_at")]=-1;instance[k("target_key")]=QString();
                instance[k("active_effects")]=QJsonArray{};instance[k("stop_reason")]=k("resource_pool_exhausted");instance[k("stopped_at")]=depletedAt;
            }
        }
        instance[k("resource_pools")]=pools;instance[k("counters")]=poolCounters;
        const double expires=instance.value(k("expires_at")).toDouble(-1);
        if(instance.value(k("expiry_tombstone")).toObject().value(k("expires_at")).toDouble(-1)<at)instance.remove(k("expiry_tombstone"));
        if(expires>=0&&at>=expires) {
            if(at==expires) {
                QJsonObject tombstone;for(const auto &name:{k("state"),k("entered_at"),k("started_at"),k("last_event_at"),k("expires_at"),k("target_key")})tombstone[name]=instance.value(name);
                instance[k("expiry_tombstone")]=tombstone;
            }
            const auto expiryState=instance.value(k("expiry_state")).toString(program.value(k("expiry_state")).toString(program.value(k("initial_state")).toString(k("idle"))));
            instance[k("state")]=expiryState;instance[k("entered_at")]=expires;instance[k("expires_at")]=-1;instance.remove(k("expiry_state"));
            if(expiryState==program.value(k("initial_state")).toString(k("idle"))||program.value(k("clear_target_on_expiry")).toBool())instance[k("target_key")]=QString();
            if(program.value(k("clear_counters_on_expiry")).toBool()) {instance[k("counters")]=QJsonObject{};instance[k("counter_expiries")]=QJsonObject{};}
        }
        auto counters=instance.value(k("counters")).toObject(),counterExpiries=instance.value(k("counter_expiries")).toObject();
        for(auto expiry=counterExpiries.begin();expiry!=counterExpiries.end();) {
            if(expiry.value().toDouble()<=at) {counters.remove(expiry.key());expiry=counterExpiries.erase(expiry);}else ++expiry;
        }
        instance[k("counters")]=counters;instance[k("counter_expiries")]=counterExpiries;
        auto entities=instance.value(k("entities")).toObject();
        for(auto entity=entities.begin();entity!=entities.end();) {
            const auto object=entity.value().toObject();
            if(object.value(k("expires_at")).toDouble(-1)>=0&&object.value(k("expires_at")).toDouble()<=at)entity=entities.erase(entity);else ++entity;
        }
        instance[k("entities")]=entities;
        QJsonArray active;
        for(const auto &raw:instance.value(k("active_effects")).toArray()) {
            const auto effect=raw.toObject();const double end=effect.value(k("expires_at")).toDouble(-1);
            if(end<0||end>at)active.append(effect);
        }
        instance[k("active_effects")]=active;it.value()=instance;
    }
    for(auto it=sharedCounterExpiries_.begin();it!=sharedCounterExpiries_.end();) {
        if(it.value().toDouble()<=at){sharedCounters_.remove(it.key());it=sharedCounterExpiries_.erase(it);}else ++it;
    }
    time_=at;return true;
}

bool StatefulSkillLedgerV7::advance(double at,QString *error) {
    auto next=*this;if(!next.advanceInPlace(at,error))return false;*this=next;return true;
}

QJsonObject StatefulSkillLedgerV7::apply(const QJsonObject &event,const CombatContext &context,
    const CombatStats &stats,const QJsonObject &runtime,QString *error) {
    auto next=*this;const auto decision=next.applyInPlace(event,context,stats,runtime,error);
    if(decision.value(k("accepted")).toBool())*this=next;
    return decision;
}

QJsonObject StatefulSkillLedgerV7::receiveEntityDamage(const QString &bindingId,const QString &entityKey,
    const QString &receiptId,const QString &sourceKey,double at,double damage,QString *error) {
    auto next=*this;
    if(!token(bindingId)||!token(entityKey)||!token(receiptId)||!token(sourceKey)||!finite(damage,0,1e12)||
        !next.instances_.contains(bindingId)||!next.advanceInPlace(at,error))return rejected(error,k("stateful_invalid_entity_damage"));
    auto instance=next.instances_.value(bindingId),entities=instance.value(k("entities")).toObject(),stored=instance.value(k("entity_damage_receipts")).toObject();
    if(stored.contains(receiptId))return rejected(error,k("stateful_duplicate_entity_damage_receipt"));
    if(!entities.contains(entityKey))return rejected(error,k("stateful_owned_entity_required"));
    auto entity=entities.value(entityKey).toObject();
    if(entity.value(k("owner_key")).toString()!=owner_||entity.value(k("binding_id")).toString()!=bindingId)return rejected(error,k("stateful_entity_owner_mismatch"));
    if(!entity.value(k("hp_known")).toBool())return rejected(error,k("stateful_entity_health_unknown"));
    const double hp=entity.value(k("hp")).toDouble();if(hp<=0)return rejected(error,k("stateful_entity_expired_or_dead"));
    const double actual=std::min(hp,damage);entity[k("hp")]=hp-actual;entities[entityKey]=entity;
    QJsonObject receipt{{k("accepted"),true},{k("actual_damage"),actual},{k("damage_receipt_id"),receiptId},{k("damage_receipt_time"),at},
        {k("damage_source_key"),sourceKey},{k("damage_target_key"),entityKey},{k("damage_target_owner_key"),owner_},{k("damage_target_binding_id"),bindingId},
        {k("hp_before"),hp},{k("hp_after"),hp-actual},{k("entity_death_receipt"),hp-actual<=eps}};
    if(hp-actual<=eps) {
        receipt[k("entity_death_receipt_id")]=receiptId+k(":death");receipt[k("entity_death_entity_key")]=entityKey;
        receipt[k("entity_death_owner_key")]=owner_;receipt[k("entity_death_binding_id")]=bindingId;receipt[k("entity_death_time")]=at;
    }
    stored[receiptId]=receipt;instance[k("entities")]=entities;instance[k("entity_damage_receipts")]=stored;
    next.instances_[bindingId]=instance;*this=next;return receipt;
}

bool StatefulSkillLedgerV7::seedObservedEntityStats(const QString &bindingId,const QString &entityKey,
    const QString &observationId,double at,const QJsonObject &observed,QString *error) {
    auto next=*this;
    if(!token(observationId)||!next.instances_.contains(bindingId)||!next.advanceInPlace(at,error)||
        !finite(observed.value(k("current_hp")),eps)||!finite(observed.value(k("maximum_hp")),eps)||
        observed.value(k("current_hp")).toDouble()>observed.value(k("maximum_hp")).toDouble())return fail(error,k("stateful_invalid_entity_observation"));
    auto instance=next.instances_.value(bindingId),entities=instance.value(k("entities")).toObject();
    if(!entities.contains(entityKey))return fail(error,k("stateful_owned_entity_required"));
    auto entity=entities.value(entityKey).toObject();
    if(entity.value(k("hp_known")).toBool()||entity.contains(k("observation_id")))return fail(error,k("stateful_entity_health_already_known"));
    auto stats=entity.value(k("stats")).toObject();
    for(const auto &name:{k("armor"),k("mr"),k("ms")})if(observed.contains(name)) {
        if(!finite(observed.value(name),name==k("ms")?0:-10000,1e6)||stats.contains(name))return fail(error,k("stateful_invalid_entity_observation"));
        stats[name]=observed.value(name);
    }
    entity[k("hp")]=observed.value(k("current_hp"));entity[k("maximum_hp")]=observed.value(k("maximum_hp"));
    entity[k("hp_known")]=true;entity[k("hp_source_known")]=false;entity[k("hp_basis")]=k("observed");
    entity[k("observation_id")]=observationId;entity[k("observation_time")]=at;entity[k("stats")]=stats;
    entities[entityKey]=entity;instance[k("entities")]=entities;next.instances_[bindingId]=instance;*this=next;return true;
}

QJsonObject StatefulSkillLedgerV7::applyInPlace(const QJsonObject &event,const CombatContext &baseContext,
    const CombatStats &stats,const QJsonObject &runtime,QString *error) {
    const auto eventId=event.value(k("event_id")).toString(),bindingId=event.value(k("binding_id")).toString();
    const auto transitionId=event.value(k("transition_id")).toString();
    if(!loaded_||!token(eventId)||!token(bindingId)||!token(transitionId)||!finite(event.value(k("time")),time_,1e9))return rejected(error,k("stateful_invalid_event"));
    if(eventIds_.contains(eventId))return rejected(error,k("stateful_duplicate_event_id"));
    if(!programs_.contains(bindingId))return rejected(error,k("stateful_unknown_binding"));
    if((event.contains(k("player_key"))&&event.value(k("player_key")).toString()!=owner_)||baseContext.championId!=champion_)return rejected(error,k("stateful_owner_mismatch"));
    const auto program=programs_.value(bindingId);QJsonObject transition;
    for(const auto &raw:program.value(k("transitions")).toArray())if(raw.toObject().value(k("id")).toString()==transitionId){transition=raw.toObject();break;}
    if(transition.isEmpty())return rejected(error,k("stateful_unknown_transition"));
    const double at=event.value(k("time")).toDouble();
    if(!advanceInPlace(at,error))return rejected(error,k("stateful_nonmonotonic_time"));
    auto instance=instances_.value(bindingId);
    QString killReceiptKey,ownedDeathReceiptKey,damageReceiptClaim,heroHitReceiptKey,heroHitEventKey;
    const auto tombstone=instance.value(k("expiry_tombstone")).toObject();
    if(transition.value(k("allow_at_expiry")).toBool()&&tombstone.value(k("expires_at")).toDouble(-1)==at) {
        for(const auto &name:{k("state"),k("entered_at"),k("started_at"),k("last_event_at"),k("target_key")})instance[name]=tombstone.value(name);
        instance.remove(k("expiry_tombstone"));
    }
    const auto requestedTarget=event.value(k("target_key")).toString();
    if(program.value(k("reset_on_target_change")).toBool()&&!instance.value(k("target_key")).toString().isEmpty()&&
        !requestedTarget.isEmpty()&&requestedTarget!=instance.value(k("target_key")).toString()) {
        const auto oldClocks=instance.value(k("clocks"));instance=fresh(program,at);instance[k("clocks")]=oldClocks;
    }
    const auto before=instance.value(k("state")).toString();
    if(!matchState(transition.value(k("from")),before))return rejected(error,k("stateful_state_mismatch"));
    if(runtime.contains(k("owner_alive"))&&!runtime.value(k("owner_alive")).toBool()&&!program.value(k("can_act_while_dead")).toBool())return rejected(error,k("stateful_owner_dead"));
    if(transition.value(k("requires_admitted_cast")).toBool()&&runtime.value(k("cast_admitted"))!=QJsonValue(true))return rejected(error,k("stateful_cast_admission_required"));
    if(transition.contains(k("requires_timer_observation"))) {
        const auto timer=transition.value(k("requires_timer_observation")).toString();bool declared=false;
        for(const auto &raw:program.value(k("owned_timer_pools")).toArray())if(raw.toObject().value(k("timer_key")).toString()==timer&&raw.toObject().value(k("initialization")).toString()==k("observed_preconsumed_full_source_cooldown"))declared=true;
        if(!declared||runtime.value(k("owned_timer_observed"))!=QJsonValue(true)||!token(runtime.value(k("owned_timer_observation_id")).toString())||
            runtime.value(k("owned_timer_owner_key")).toString()!=owner_||runtime.value(k("owned_timer_binding_id")).toString()!=bindingId||
            runtime.value(k("owned_timer_key")).toString()!=timer||!finite(runtime.value(k("owned_timer_observation_time")),at,at))return rejected(error,k("stateful_owned_timer_observation_required"));
        auto observations=instance.value(k("owned_timer_observations")).toObject();
        if(observations.contains(timer))return rejected(error,k("stateful_owned_timer_already_observed"));
        observations[timer]=QJsonObject{{k("observation_id"),runtime.value(k("owned_timer_observation_id"))},{k("time"),at},{k("programs_sha256"),programHash_}};
        instance[k("owned_timer_observations")]=observations;
    }
    if(transition.value(k("requires_admitted_attack")).toBool()&&runtime.value(k("attack_admitted"))!=QJsonValue(true))return rejected(error,k("stateful_attack_admission_required"));
    if(transition.value(k("requires_owned_death_receipt")).toBool()&&runtime.value(k("owned_death_receipt"))!=QJsonValue(true))return rejected(error,k("stateful_owned_death_receipt_required"));
    if(transition.value(k("requires_owned_death_receipt")).toBool()&&
        (!finite(runtime.value(k("owned_death_time")),instance.value(k("started_at")).toDouble(),at)||
         runtime.value(k("owned_death_target_key")).toString()!=requestedTarget||!token(runtime.value(k("owned_death_receipt_id")).toString())||
         !finite(runtime.value(k("target_current_hp")),0,eps)))return rejected(error,k("stateful_owned_death_receipt_required"));
    if(transition.value(k("requires_owned_death_receipt")).toBool()) {
        ownedDeathReceiptKey=k("owned_death|")+runtime.value(k("owned_death_receipt_id")).toString();
        if(instance.value(k("record_receipts")).toArray().contains(ownedDeathReceiptKey))return rejected(error,k("stateful_duplicate_owned_death_receipt"));
    }
    if(transition.value(k("requires_kill_receipt")).toBool()) {
        const auto receiptId=runtime.value(k("kill_receipt_id")).toString();
        const bool ownerKill=runtime.value(k("kill_source_key")).toString()==owner_;
        const bool takedown=transition.value(k("kill_receipt_subject")).toString()==k("takedown")&&runtime.value(k("kill_participant_keys")).toArray().contains(owner_);
        if(!token(receiptId)||!token(runtime.value(k("kill_target_key")).toString())||!finite(runtime.value(k("kill_receipt_time")),instance.value(k("started_at")).toDouble(),at)||
            (!ownerKill&&!takedown)||(transition.contains(k("required_kill_type"))&&runtime.value(k("kill_target_type"))!=transition.value(k("required_kill_type"))))return rejected(error,k("stateful_kill_receipt_required"));
        killReceiptKey=k("kill|")+receiptId;
        if(instance.value(k("record_receipts")).toArray().contains(killReceiptKey))return rejected(error,k("stateful_duplicate_kill_receipt"));
    }
    if(transition.value(k("requires_damage_receipt")).toBool()&&(!token(runtime.value(k("damage_receipt_id")).toString())||!finite(runtime.value(k("actual_damage")),0,1e12)||
        !finite(runtime.value(k("damage_receipt_time")),instance.value(k("started_at")).toDouble(),at)||
        (runtime.value(k("damage_source_key")).toString()!=owner_&&runtime.value(k("damage_target_key")).toString()!=owner_&&
         (runtime.value(k("damage_target_owner_key")).toString()!=owner_||runtime.value(k("damage_target_binding_id")).toString()!=bindingId))))return rejected(error,k("stateful_actual_damage_receipt_required"));
    if(transition.value(k("requires_damage_receipt")).toBool()) {
        damageReceiptClaim=k("damage_event|")+transitionId+k("|")+runtime.value(k("damage_receipt_id")).toString();
        if(instance.value(k("record_receipts")).toArray().contains(damageReceiptClaim))return rejected(error,k("stateful_duplicate_damage_event_receipt"));
    }
    if(transition.contains(k("required_form"))) {
        const auto form=runtime.value(k("current_form")).toString(runtime.value(k("form")).toString());
        if(form.isEmpty()||form!=transition.value(k("required_form")).toString())return rejected(error,k("stateful_form_mismatch"));
    }
    if((transition.value(k("requires_owned_shield")).toBool()||transition.contains(k("requires_live_shield")))&&
        (!finite(runtime.value(k("owned_shield_remaining")),0,1e12)||runtime.value(k("owned_shield_remaining")).toDouble()<=0))return rejected(error,k("stateful_owned_shield_required"));
    auto facts=event.value(k("facts")).toObject();
    QJsonArray requiredFacts=program.value(k("required_facts")).toArray();for(const auto &raw:transition.value(k("required_facts")).toArray())requiredFacts.append(raw);
    for(const auto &raw:requiredFacts) {
        const auto key=raw.toString();if(!token(key)||facts.value(key)!=QJsonValue(true))return rejected(error,k("stateful_required_fact_missing:")+key);
    }
    const auto constraints=transition.value(k("fact_constraints")).toObject();
    for(auto it=constraints.begin();it!=constraints.end();++it) {
        const auto rule=it.value().toObject();const auto value=facts.value(it.key());
        if((rule.contains(k("equals"))&&value!=rule.value(k("equals")))||
            (rule.contains(k("allowed"))&&!rule.value(k("allowed")).toArray().contains(value)))return rejected(error,k("stateful_fact_constraint:")+it.key());
        if(rule.contains(k("min"))||rule.contains(k("max"))||rule.value(k("integer")).toBool()) {
            if(!finite(value)||value.toDouble()<rule.value(k("min")).toDouble(-1e12)||value.toDouble()>rule.value(k("max")).toDouble(1e12)||
                (rule.value(k("integer")).toBool()&&std::floor(value.toDouble())!=value.toDouble()))return rejected(error,k("stateful_fact_constraint:")+it.key());
        }
    }
    QString target=event.value(k("target_key")).toString();
    if(runtime.contains(k("target_key"))&&runtime.value(k("target_key")).toString()!=target)return rejected(error,k("stateful_runtime_target_mismatch"));
    const auto targetPolicy=transition.value(k("target_policy")).toString(k("any"));
    const auto bound=instance.value(k("target_key")).toString();
    if(targetPolicy==k("same")&&(!token(target)||target!=bound))return rejected(error,k("stateful_target_mismatch"));
    if(targetPolicy==k("self")) {if(!target.isEmpty()&&target!=owner_)return rejected(error,k("stateful_target_mismatch"));target=owner_;}
    const auto relation=transition.value(k("target_relation")).toString((targetPolicy==k("enemy")||targetPolicy==k("ally"))?targetPolicy:QString());
    if(relation==k("enemy")||relation==k("ally")) {
        const auto targetTeam=runtime.value(k("target_team")).toString();
        if(!token(target)||!token(targetTeam)||(relation==k("enemy")?(targetTeam==team_):(targetTeam!=team_)))return rejected(error,k("stateful_target_relation_mismatch"));
    }
    if(targetPolicy==k("bind")||transition.value(k("bind_target")).toBool()) {if(!token(target))return rejected(error,k("stateful_target_required"));instance[k("target_key")]=target;}
    CombatContext context=baseContext;
    for(auto it=context.counters.begin();it!=context.counters.end();)if(reservedFact(it.key()))it=context.counters.erase(it);else ++it;
    for(auto it=facts.begin();it!=facts.end();++it)if(!reservedFact(it.key())&&(finite(it.value())||it.value().isBool()))context.counters[it.key()]=it.value().isBool()?(it.value().toBool()?1:0):it.value().toDouble();
    for(auto it=runtime.begin();it!=runtime.end();++it)if(finite(it.value())&&!it.key().startsWith(k("counter:"))&&!it.key().startsWith(k("state_counter:"))&&!it.key().startsWith(k("ledger_counter:"))&&!it.key().startsWith(k("recorded_damage:"))&&!it.key().startsWith(k("owned_timer_remaining:")))context.counters[it.key()]=it.value().toDouble();
    auto activation=instance.value(k("activation_cast_receipt")).toObject();
    if(transition.value(k("requires_admitted_cast")).toBool()&&runtime.contains(k("cast_resource_cost"))) {
        if(!finite(runtime.value(k("cast_resource_cost")),0,1e12)||!token(runtime.value(k("cast_admission_event_id")).toString())||
            !token(runtime.value(k("cast_admission_slot")).toString())||!finite(runtime.value(k("cast_admission_time")),0,at))return rejected(error,k("stateful_cast_payment_receipt_required"));
        activation=QJsonObject{{k("event_id"),runtime.value(k("cast_admission_event_id"))},{k("slot"),runtime.value(k("cast_admission_slot"))},
            {k("time"),runtime.value(k("cast_admission_time"))},{k("resource_cost"),runtime.value(k("cast_resource_cost"))},{k("binding_id"),bindingId},{k("owner_key"),owner_}};
        instance[k("activation_cast_receipt")]=activation;
    }
    const auto heroHitContract=transition.value(k("hero_hit_contract_v1")).toObject();
    if(!heroHitContract.isEmpty()) {
        const auto hit=runtime.value(k("hero_hit_receipt_v1")).toObject();
        if(!program.value(k("hero_event_contract_v1")).toBool()||activation.isEmpty()||
           activation.value(k("binding_id")).toString()!=bindingId||activation.value(k("owner_key")).toString()!=owner_||
           activation.value(k("slot")).toString()!=program.value(k("slot")).toString()||
           !token(activation.value(k("event_id")).toString())||!finite(activation.value(k("time")),0,at)||
           !token(hit.value(k("receipt_id")).toString())||!token(hit.value(k("hit_event_id")).toString())||
           hit.value(k("verified_contact"))!=QJsonValue(true)||hit.value(k("actor_key")).toString()!=owner_||
           hit.value(k("target_key")).toString()!=target||hit.value(k("target_type")).toString()!=k("champion")||
           hit.value(k("parent_event_id")).toString()!=eventId||hit.value(k("binding_id")).toString()!=bindingId||
           hit.value(k("cast_instance_id"))!=activation.value(k("event_id"))||
           hit.value(k("origin_ability_id"))!=heroHitContract.value(k("origin_ability_id"))||
           hit.value(k("application_origin_family"))!=heroHitContract.value(k("application_origin_family"))||
           !finite(hit.value(k("time")),at,at))return rejected(error,k("stateful_owned_hero_hit_receipt_required"));
        if(heroHitContract.value(k("same_event_cast_contact")).toBool()&&activation.value(k("event_id")).toString()!=eventId)
            return rejected(error,k("stateful_hero_hit_cast_identity_mismatch"));
        heroHitReceiptKey=k("hero_hit|")+hit.value(k("receipt_id")).toString();
        heroHitEventKey=k("hero_hit_event|")+hit.value(k("hit_event_id")).toString();
        if(!token(heroHitReceiptKey)||!token(heroHitEventKey))return rejected(error,k("stateful_hero_hit_identity_too_long"));
        if(instance.value(k("record_receipts")).toArray().contains(heroHitReceiptKey)||
           instance.value(k("record_receipts")).toArray().contains(heroHitEventKey))return rejected(error,k("stateful_duplicate_hero_hit_receipt"));
    }
    if(!activation.isEmpty())context.counters[k("cast_resource_cost")]=activation.value(k("resource_cost")).toDouble();
    const double elapsed=at-instance.value(k("entered_at")).toDouble();
    context.counters[k("elapsed")]=elapsed;context.counters[k("state_elapsed")]=elapsed;context.counters[k("transition_elapsed")]=at-instance.value(k("last_event_at")).toDouble();
    context.counters[k("target_is_bound")]=(!target.isEmpty()&&target==bound)?1:0;
    context.counters[k("champion_level")]=context.level;
    if(runtime.contains(k("self_current_mana"))) {context.counters[k("current_resource")]=runtime.value(k("self_current_mana")).toDouble();context.counters[k("resource_current")]=runtime.value(k("self_current_mana")).toDouble();}
    if(runtime.contains(k("target_max_hp"))) {context.targetHp=runtime.value(k("target_max_hp")).toDouble();context.counters[k("target_hp")]=context.targetHp;}
    if(runtime.contains(k("self_current_hp"))) {
        const double hp=runtime.value(k("self_current_hp")).toDouble(),maximum=stats[Stat::Hp];
        context.counters[k("self_current_hp")]=hp;context.counters[k("self_max_hp")]=maximum;context.counters[k("self_missing_hp")]=std::max(0.,maximum-hp);
        context.counters[k("self_health_fraction")]=maximum>0?hp/maximum:0;
    }
    if(runtime.contains(k("target_current_hp"))) {
        const double hp=runtime.value(k("target_current_hp")).toDouble(),maximum=context.targetHp;
        context.counters[k("target_missing_hp")]=std::max(0.,maximum-hp);context.counters[k("target_health_fraction")]=maximum>0?hp/maximum:0;context.counters[k("target_health_ratio")]=maximum>0?hp/maximum:0;
    }
    const auto rankSlot=program.value(k("rank_slot")).toString(program.value(k("slot")).toString());
    const int rank=rankSlot==k("P")?1:context.ranks.value(rankSlot,0);
    if(rank<1&&!program.value(k("allow_unlearned")).toBool())return rejected(error,k("stateful_unlearned_skill"));
    context.counters[k("source_rank")]=rank;context.counters[k("current_rank")]=rank;
    if(runtime.contains(k("owned_shield_remaining")))context.counters[k("shield_remaining")]=runtime.value(k("owned_shield_remaining")).toDouble();
    auto counters=instance.value(k("counters")).toObject(),expiries=instance.value(k("counter_expiries")).toObject();
    if(transition.value(k("reset_counters")).toBool()||transition.value(k("clear_counters")).toBool()){counters=QJsonObject{};expiries=QJsonObject{};}
    for(const auto &keyValue:transition.value(k("reset_counter_keys")).toArray()) {
        const auto key=keyValue.toString();for(auto it=counters.begin();it!=counters.end();) {
            if(it.key()==key||it.key().startsWith(key+k("|"))){expiries.remove(it.key());it=counters.erase(it);}else ++it;
        }
    }
    // Only source-declared counters/records have a known empty initial value.
    // Arbitrary missing context keys remain missing, rather than becoming zero.
    for(auto declared=programs_.begin();declared!=programs_.end();++declared)for(const auto &stepValue:declared.value().value(k("transitions")).toArray()) {
        const auto step=stepValue.toObject();auto declarations=step.value(k("counters")).toArray();if(step.value(k("counter")).isObject())declarations.append(step.value(k("counter")));
        for(const auto &counterValue:declarations) {
            const auto definition=counterValue.toObject();if(declared.key()!=bindingId&&!definition.contains(k("shared_key")))continue;
            const auto name=definition.value(k("shared_key")).toString(definition.value(k("key")).toString());if(name.isEmpty())continue;
            for(const auto &prefix:{k("counter:"),k("state_counter:"),k("ledger_counter:")})if(!context.counters.contains(prefix+name))context.counters[prefix+name]=0;
        }
        if(declared.key()==bindingId)for(const auto &effectValue:step.value(k("effects")).toArray()) {
            const auto effect=effectValue.toObject();if(effect.value(k("op")).toString()==k("record_damage")||effect.value(k("op")).toString()==k("replay_damage")) {
                const auto name=effect.value(k("key")).toString(k("damage"));if(!context.counters.contains(k("recorded_damage:")+name))context.counters[k("recorded_damage:")+name]=0;
            }
        }
    }
    addCounterAliases(context,counters,target);addCounterAliases(context,sharedCounters_,target);
    auto records=transition.value(k("clear_records")).toBool()?QJsonObject{}:instance.value(k("records")).toObject();
    for(auto it=records.begin();it!=records.end();++it) {
        auto name=it.key();const auto separator=name.indexOf(k("|"));
        if(separator>=0) {if(name.mid(separator+1)!=target)continue;name=name.left(separator);}
        context.counters[k("recorded_damage:")+name]=it.value().toDouble();
    }
    context.counters[k("recorded_damage")]=records.value(k("damage|")+target).toDouble(records.value(k("damage")).toDouble());
    int evaluationRank=std::max(1,rank);
    auto numeric=[&](const QJsonValue &value)->std::optional<double>{return evaluate(value,context,stats,evaluationRank);};
    const auto window=transition.value(k("window")).toObject();
    QString reference=window.value(k("reference")).toString(k("entered_at"));
    if(reference!=k("entered_at")&&reference!=k("started_at")&&reference!=k("last_event_at"))return rejected(error,k("stateful_invalid_window_reference"));
    const double windowElapsed=at-instance.value(reference).toDouble();
    if(window.contains(k("min"))||window.contains(k("min_expression"))) {const auto minimum=numeric(valueOrExpression(window,k("min")));if(!minimum||windowElapsed+eps<*minimum)return rejected(error,k("stateful_window_not_open"));}
    if(window.contains(k("max"))||window.contains(k("max_expression"))) {const auto maximum=numeric(valueOrExpression(window,k("max")));if(!maximum||windowElapsed>*maximum)return rejected(error,k("stateful_window_expired"));}
    auto clocks=instance.value(k("clocks")).toObject();
    for(auto clock=clocks.begin();clock!=clocks.end();++clock)if(clock.key().startsWith(k("owned:")))context.counters[k("owned_timer_remaining:")+clock.key().mid(6)]=std::max(0.,clock.value().toDouble()-at);
    const auto clockName=transition.value(k("shared_cooldown_key")).toString(transition.value(k("cooldown_key")).toString(transitionId));
    const auto clockKey=clockName+(transition.contains(k("per_target_cooldown_expression"))?k("|")+target:QString());
    const bool sharedClock=transition.contains(k("shared_cooldown_key"));
    if((sharedClock?sharedClocks_:clocks).value(clockKey).toDouble() > at+eps)return rejected(error,k("stateful_transition_cooldown"));
    for(const auto &raw:transition.value(k("guards")).toArray()) {
        const auto guard=raw.toObject();const auto left=numeric(guard.value(k("left_expression"))),right=numeric(guard.value(k("right_expression")));
        if(!left||!right||!compare(*left,guard.value(k("op")).toString(),*right))return rejected(error,k("stateful_guard_failed"));
    }
    const auto contextConstraints=transition.value(k("context_constraints")).toObject();
    for(auto it=contextConstraints.begin();it!=contextConstraints.end();++it) {
        if(!context.counters.contains(it.key()))return rejected(error,k("stateful_live_context_missing:")+it.key());
        const auto constraint=it.value().toObject();const double value=context.counters.value(it.key());
        for(const auto &name:{k("minimum"),k("maximum")})if(constraint.contains(name)||constraint.contains(name+k("_expression"))) {
            const auto boundValue=numeric(valueOrExpression(constraint,name));
            if(!boundValue||(name==k("minimum")?value<*boundValue:value>*boundValue))return rejected(error,k("stateful_live_context_constraint:")+it.key());
        }
    }
    QJsonArray counterRows=transition.value(k("counters")).toArray();
    if(transition.value(k("counter")).isObject())counterRows.prepend(transition.value(k("counter")));
    for(const auto &raw:counterRows) {
        const auto counter=raw.toObject();const auto key=counterKey(counter,target);
        if(!token(key)||(counter.value(k("per_target")).toBool()&&!token(target)))return rejected(error,k("stateful_invalid_counter"));
        const bool shared=counter.contains(k("shared_key"));auto values=shared?sharedCounters_:counters;auto times=shared?sharedCounterExpiries_:expiries;
        const double prior=values.value(key).toDouble();context.counters[k("state_counter")]=prior;context.counters[k("counter_before")]=prior;
        if(counter.contains(k("min_before"))||counter.contains(k("min_before_expression"))) {const auto minimum=numeric(valueOrExpression(counter,k("min_before")));if(!minimum||prior+eps<*minimum)return rejected(error,k("stateful_counter_below_minimum"));}
        auto delta=numeric(counter.contains(k("delta"))||counter.contains(k("delta_expression"))?valueOrExpression(counter,k("delta")):QJsonValue(1));
        if(!delta)return rejected(error,k("stateful_counter_formula_invalid"));double after=prior+*delta;
        if(counter.contains(k("set"))||counter.contains(k("set_expression"))) {const auto value=numeric(valueOrExpression(counter,k("set")));if(!value)return rejected(error,k("stateful_counter_formula_invalid"));after=*value;}
        if(!std::isfinite(after)||after<0||after>1e12)return rejected(error,k("stateful_counter_out_of_range"));
        if(counter.contains(k("max_after"))||counter.contains(k("max_after_expression"))) {const auto maximum=numeric(valueOrExpression(counter,k("max_after")));if(!maximum)return rejected(error,k("stateful_counter_formula_invalid"));if(after>*maximum+eps){if(counter.value(k("clamp")).toBool())after=*maximum;else return rejected(error,k("stateful_counter_above_maximum"));}}
        bool triggered=false;double stored=after;
        if(counter.contains(k("consume_at"))||counter.contains(k("consume_at_expression"))) {const auto threshold=numeric(valueOrExpression(counter,k("consume_at")));if(!threshold||*threshold<=0)return rejected(error,k("stateful_counter_formula_invalid"));triggered=after+eps>=*threshold;if(triggered)stored=0;}
        values[key]=stored;
        if(counter.contains(k("duration_expression"))) {const auto duration=numeric(counter.value(k("duration_expression")));if(!duration||*duration<=0||*duration>1e9)return rejected(error,k("stateful_counter_duration_invalid"));times[key]=at+*duration;}
        if(shared){sharedCounters_=values;sharedCounterExpiries_=times;}else {counters=values;expiries=times;}
        context.counters[k("counter_after")]=after;context.counters[k("counter_triggered")]=triggered?1:0;
        const auto name=counter.value(k("shared_key")).toString(counter.value(k("key")).toString());
        for(const auto &prefix:{k("counter:"),k("state_counter:"),k("ledger_counter:")})context.counters[prefix+name]=stored;
    }
    auto receipts=instance.value(k("record_receipts")).toArray();
    if(!heroHitReceiptKey.isEmpty())receipts.append(heroHitReceiptKey);
    if(!heroHitEventKey.isEmpty())receipts.append(heroHitEventKey);
    if(!killReceiptKey.isEmpty())receipts.append(killReceiptKey);
    if(!ownedDeathReceiptKey.isEmpty())receipts.append(ownedDeathReceiptKey);
    if(!damageReceiptClaim.isEmpty())receipts.append(damageReceiptClaim);
    auto entities=instance.value(k("entities")).toObject();QString entityId=event.value(k("entity_id")).toString(event.value(k("entity_key")).toString());
    if(entityId.isEmpty()&&transition.value(k("uses_entity")).isString())entityId=transition.value(k("uses_entity")).toString();
    if(entityId.isEmpty())entityId=transition.value(k("entity_key")).toString();
    const auto bridge=transition.value(k("entity_bridge")).toObject();
    QJsonObject bridgeSource,bridgeSourceEntities,borrowedEntity;QString bridgeBinding,bridgeEntityKey;bool bridgeConsume=false;
    if(!bridge.isEmpty()) {
        bridgeBinding=bridge.value(k("source_binding_id")).toString();bridgeEntityKey=bridge.value(k("source_entity_key")).toString();
        const auto operation=bridge.value(k("operation")).toString();bridgeConsume=operation==k("consume");
        const bool invalidConsume=bridgeConsume&&(!transition.value(k("consume_entity")).toBool()||
            transition.value(k("entity_attack")).toBool()||transition.contains(k("entity_damage_expression")));
        if(!token(bridgeBinding)||!token(bridgeEntityKey)||bridgeBinding==bindingId||!programs_.contains(bridgeBinding)||
            (operation!=k("consume")&&operation!=k("transfer"))||invalidConsume)return rejected(error,k("stateful_entity_bridge_invalid"));
        if(!entityId.isEmpty()&&entityId!=bridgeEntityKey)return rejected(error,k("stateful_entity_bridge_identity_mismatch"));
        bridgeSource=instances_.value(bridgeBinding);bridgeSourceEntities=bridgeSource.value(k("entities")).toObject();
        if(!bridgeSourceEntities.contains(bridgeEntityKey))return rejected(error,k("stateful_entity_bridge_source_missing"));
        borrowedEntity=bridgeSourceEntities.value(bridgeEntityKey).toObject();
        if(borrowedEntity.value(k("owner_key")).toString()!=owner_||borrowedEntity.value(k("binding_id")).toString()!=bridgeBinding||
            borrowedEntity.value(k("source_program_sha256")).toString()!=hash(QJsonArray{programs_.value(bridgeBinding)}))return rejected(error,k("stateful_entity_bridge_source_mismatch"));
        if((borrowedEntity.value(k("hp_known")).toBool()&&borrowedEntity.value(k("hp")).toDouble()<=0)||
            (borrowedEntity.value(k("expires_at")).toDouble(-1)>=0&&at>=borrowedEntity.value(k("expires_at")).toDouble()))return rejected(error,k("stateful_entity_bridge_source_dead_or_expired"));
        entityId=bridgeEntityKey;
        if(!bridgeConsume) {
            if(transition.value(k("consume_entity")).toBool())return rejected(error,k("stateful_entity_bridge_invalid"));
            entityId=bridge.value(k("target_entity_key")).toString(bridgeEntityKey);
            if(!token(entityId)||entities.contains(entityId))return rejected(error,k("stateful_entity_bridge_destination_exists"));
            borrowedEntity[k("entity_id")]=entityId;borrowedEntity[k("binding_id")]=bindingId;borrowedEntity[k("source_program_sha256")]=hash(QJsonArray{program});
            borrowedEntity[k("transferred_from_binding_id")]=bridgeBinding;borrowedEntity[k("transferred_from_entity_key")]=bridgeEntityKey;
            entities[entityId]=borrowedEntity;
        }
    }
    // A pet cannot spend an arbitrary real fighter receipt. Receipt amounts,
    // ownership, recipient and time are supplied only by the damage engine.
    auto applyEntityDamage=[&](QJsonObject &entity,const QString &id,double damage)->bool {
        const auto receiptId=runtime.value(k("damage_receipt_id")).toString();
        const auto receiptKey=k("entity|")+id+k("|")+receiptId;
        const auto native=instance.value(k("entity_damage_receipts")).toObject().value(receiptId).toObject();
        if(!entity.value(k("hp_known")).toBool(true)||!token(receiptId)||
            !finite(runtime.value(k("actual_damage")),0,1e12)||
            !finite(runtime.value(k("damage_receipt_time")),entity.value(k("created_at")).toDouble(),at)||
            runtime.value(k("damage_target_key")).toString()!=id||
            runtime.value(k("damage_target_owner_key")).toString()!=owner_||
            runtime.value(k("damage_target_binding_id")).toString()!=bindingId||
            std::abs(runtime.value(k("actual_damage")).toDouble()-damage)>eps||receipts.contains(receiptKey)||native.isEmpty()||
            native.value(k("damage_target_key")).toString()!=id||native.value(k("actual_damage"))!=runtime.value(k("actual_damage"))||
            native.value(k("damage_receipt_time"))!=runtime.value(k("damage_receipt_time")))return false;
        receipts.append(receiptKey);return true;
    };
    context.counters[k("entity_count")]=entities.size();
    if(transition.value(k("uses_entity")).toBool()||transition.value(k("uses_entity")).isString()||transition.value(k("entity_attack")).toBool()||transition.value(k("consume_entity")).toBool()||transition.contains(k("entity_damage_expression"))) {
        if(!token(entityId)||(!entities.contains(entityId)&&!bridgeConsume))return rejected(error,k("stateful_owned_entity_required"));
        auto entity=bridgeConsume?borrowedEntity:entities.value(entityId).toObject();
        if(entity.value(k("owner_key")).toString()!=owner_||entity.value(k("binding_id")).toString()!=(bridgeConsume?bridgeBinding:bindingId))return rejected(error,k("stateful_entity_owner_mismatch"));
        if((entity.value(k("hp_known")).toBool(true)&&entity.value(k("hp")).toDouble()<=0&&
            ((!transition.value(k("allow_dead_entity")).toBool()&&!transition.contains(k("entity_damage_expression")))||transition.value(k("entity_attack")).toBool()))||
            (entity.value(k("expires_at")).toDouble(-1)>=0&&at>=entity.value(k("expires_at")).toDouble()))return rejected(error,k("stateful_entity_expired_or_dead"));
        if(transition.value(k("requires_dead_entity")).toBool()&&(!entity.value(k("hp_known")).toBool(true)||entity.value(k("hp")).toDouble()>0))return rejected(error,k("stateful_entity_death_required"));
        if(transition.value(k("requires_entity_death_receipt")).toBool()) {
            const auto receiptId=runtime.value(k("entity_death_receipt_id")).toString();const auto receiptKey=k("entity_death|")+entityId+k("|")+receiptId;
            if(runtime.value(k("entity_death_receipt"))!=QJsonValue(true)||!token(receiptId)||
                runtime.value(k("entity_death_entity_key")).toString()!=entityId||runtime.value(k("entity_death_owner_key")).toString()!=owner_||
                runtime.value(k("entity_death_binding_id")).toString()!=bindingId||
                !finite(runtime.value(k("entity_death_time")),entity.value(k("created_at")).toDouble(),at)||receipts.contains(receiptKey))return rejected(error,k("stateful_entity_death_receipt_required"));
            bool foundNative=false;const auto nativeReceipts=instance.value(k("entity_damage_receipts")).toObject();
            for(auto native=nativeReceipts.begin();native!=nativeReceipts.end();++native)if(native.value().toObject().value(k("entity_death_receipt_id")).toString()==receiptId&&native.value().toObject().value(k("entity_death_entity_key")).toString()==entityId)foundNative=true;
            if(!foundNative)return rejected(error,k("stateful_entity_death_receipt_required"));
            entity[k("hp")]=0;receipts.append(receiptKey);
        }
        if(transition.value(k("entity_attack")).toBool()) {
            if(entity.value(k("attack_ready_at")).toDouble()>at+eps)return rejected(error,k("stateful_entity_attack_cooldown"));
            const auto clockBasis=transition.value(k("entity_attack_clock")).toString();
            if(clockBasis==k("owner_admitted_attack")) {
                const auto receiptId=runtime.value(k("owner_attack_receipt_id")).toString(),receiptKey=k("owner_attack|")+entityId+k("|")+receiptId;
                if(!transition.value(k("requires_admitted_attack")).toBool()||runtime.value(k("attack_admitted"))!=QJsonValue(true)||
                    transition.value(k("attack_payload_policy")).toString()!=k("replace_owner_base_attack"))return rejected(error,k("stateful_entity_owner_attack_contract_required"));
                if(!token(receiptId)||runtime.value(k("owner_attack_owner_key")).toString()!=owner_||
                    !finite(runtime.value(k("owner_attack_receipt_time")),at,at)||!finite(runtime.value(k("owner_attack_ready_at")),at,1e9))return rejected(error,k("stateful_entity_owner_attack_receipt_required"));
                if(receipts.contains(receiptKey))return rejected(error,k("stateful_duplicate_owner_attack_receipt"));
                entity[k("attack_ready_at")]=runtime.value(k("owner_attack_ready_at"));entity[k("attack_clock_basis")]=clockBasis;
                entity[k("last_owner_attack_receipt_id")]=receiptId;entity[k("last_owner_attack_receipt_time")]=at;receipts.append(receiptKey);
            } else {
                if(clockBasis==k("verified_observation")) {
                    const auto observation=transition.value(k("entity_attack_interval_observation")).toObject();
                    if(observation.value(k("one_observation_per_entity"))!=QJsonValue(true)||!finite(observation.value(k("minimum")),eps,3600)||
                        !finite(observation.value(k("maximum")),observation.value(k("minimum")).toDouble(),3600))return rejected(error,k("stateful_entity_clock_observation_not_declared"));
                    const bool provided=runtime.value(k("entity_attack_interval_verified"))==QJsonValue(true);
                    if(entity.value(k("attack_interval")).toDouble(-1)<0) {
                        if(!provided||!finite(runtime.value(k("entity_attack_interval")),observation.value(k("minimum")).toDouble(),observation.value(k("maximum")).toDouble())||
                            !token(runtime.value(k("entity_attack_interval_observation_id")).toString())||runtime.value(k("entity_attack_interval_owner_key")).toString()!=owner_||
                            runtime.value(k("entity_attack_interval_binding_id")).toString()!=bindingId||runtime.value(k("entity_attack_interval_entity_key")).toString()!=entityId||
                            !finite(runtime.value(k("entity_attack_interval_observation_time")),at,at)||entity.value(k("source_program_sha256")).toString()!=hash(QJsonArray{program}))return rejected(error,k("stateful_entity_clock_observation_required"));
                        entity[k("attack_interval")]=runtime.value(k("entity_attack_interval"));entity[k("attack_interval_basis")]=k("observed");
                        entity[k("clock_observation_id")]=runtime.value(k("entity_attack_interval_observation_id"));entity[k("clock_observation_time")]=at;
                        entity[k("clock_observation_programs_sha256")]=programHash_;
                    } else if(provided&&(entity.value(k("attack_interval_basis")).toString()!=k("observed")||
                        runtime.value(k("entity_attack_interval"))!=entity.value(k("attack_interval"))))return rejected(error,k("stateful_entity_clock_already_known"));
                    if(entity.value(k("attack_interval_basis")).toString()==k("observed")&&entity.value(k("clock_observation_programs_sha256")).toString()!=programHash_)return rejected(error,k("stateful_entity_clock_source_mismatch"));
                } else if(!clockBasis.isEmpty()&&clockBasis!=k("independent"))return rejected(error,k("stateful_entity_attack_clock_unavailable"));
                const double interval=entity.value(k("attack_interval")).toDouble(-1);if(interval<0)return rejected(error,k("stateful_entity_attack_clock_unknown"));
                entity[k("attack_ready_at")]=at+interval;entity[k("attack_clock_basis")]=entity.value(k("attack_interval_basis")).toString()==k("observed")?k("verified_observation"):k("independent");
            }
            entity[k("attacks")]=entity.value(k("attacks")).toInt()+1;
        }
        if(transition.contains(k("entity_damage_expression"))) {
            if(!entity.value(k("hp_known")).toBool(true))return rejected(error,k("stateful_entity_health_unknown"));
            const auto damage=numeric(transition.value(k("entity_damage_expression")));if(!damage||*damage<0||!applyEntityDamage(entity,entityId,*damage))return rejected(error,k("stateful_entity_damage_receipt_required"));
        }
        context.counters[k("entity_current_hp")]=entity.value(k("hp")).toDouble();context.counters[k("entity_attacks")]=entity.value(k("attacks")).toInt();
        if(!bridgeConsume)entities[entityId]=entity;
    }
    QJsonArray effects,active=transition.value(k("clear_active_effects")).toBool()?QJsonArray{}:instance.value(k("active_effects")).toArray();
    double resourceDelta=0,healthCost=0;bool instanceExpiryAdjusted=false;
    for(const auto &raw:transition.value(k("effects")).toArray()) {
        auto effect=raw.toObject();const auto op=effect.value(k("op")).toString();if(!effectOps.contains(op))return rejected(error,k("stateful_unsupported_effect"));
        effect.remove(k("ledger_applied"));
        const auto effectRankSlot=effect.value(k("rank_slot")).toString(rankSlot);
        evaluationRank=effectRankSlot==k("P")?1:context.ranks.value(effectRankSlot,0);
        if(evaluationRank<1)return rejected(error,k("stateful_effect_source_rank_unlearned"));
        QJsonArray effectFacts=effect.value(k("required_facts")).toArray();for(const auto &fact:effect.value(k("requires_facts")).toArray())effectFacts.append(fact);
        for(const auto &fact:effectFacts)if(facts.value(fact.toString())!=QJsonValue(true))return rejected(error,k("stateful_effect_required_fact_missing:")+fact.toString());
        bool guardAllows=true;
        for(const auto &guardValue:effect.value(k("guards")).toArray()) {
            const auto guard=guardValue.toObject();const auto left=numeric(guard.value(k("left_expression"))),right=numeric(guard.value(k("right_expression")));
            if(!left||!right)return rejected(error,k("stateful_effect_guard_formula_invalid"));
            if(!compare(*left,guard.value(k("op")).toString(),*right))guardAllows=false;
        }
        if(!guardAllows)continue;
        if(effect.value(k("when_counter_triggered")).toBool()&&context.counters.value(k("counter_triggered"))<1)continue;
        if(effect.contains(k("when_counter_equals"))&&std::abs(context.counters.value(k("counter_after"))-effect.value(k("when_counter_equals")).toDouble())>eps)continue;
        if(effect.contains(k("when_counter_at_least"))&&context.counters.value(k("counter_after"))+eps<effect.value(k("when_counter_at_least")).toDouble())continue;
        const auto when=effect.value(k("when")).toObject();
        if(when.contains(k("fact"))&&facts.value(when.value(k("fact")).toString())!=when.value(k("equals")))continue;
        QString recipient=effect.value(k("target")).toString(k("event_target"));
        if(recipient==k("self")||recipient==k("entity_owner"))recipient=owner_;
        else if(recipient==k("event_target")||recipient==k("target"))recipient=target;
        else if(recipient==k("bound_target"))recipient=instance.value(k("target_key")).toString();
        if(!token(recipient)&&op!=k("spawn_entity")&&op!=k("destroy_entity")&&op!=k("record_damage")&&op!=k("set_value")&&op!=k("counter")&&op!=k("reset_counter"))return rejected(error,k("stateful_effect_recipient_required"));
        double amount=0,duration=-1;
        if(effect.contains(k("duration_expression"))) {const auto value=numeric(effect.value(k("duration_expression")));if(!value||*value<0||*value>1e9)return rejected(error,k("stateful_effect_duration_invalid"));duration=*value;}
        if(op==k("record_damage")) {
            const auto receiptId=runtime.value(k("damage_receipt_id")).toString();const auto actual=runtime.value(k("actual_damage"));
            const bool received=effect.value(k("receipt_subject")).toString(effect.value(k("record_direction")).toString(k("dealt")))==k("received")||effect.value(k("record_direction")).toString()==k("incoming");
            if(!token(receiptId)||!finite(actual,0,1e12)||!finite(runtime.value(k("damage_receipt_time")),instance.value(k("started_at")).toDouble(),at)||
                (received?runtime.value(k("damage_target_key")).toString()!=owner_:runtime.value(k("damage_source_key")).toString()!=owner_))return rejected(error,k("stateful_actual_damage_receipt_required"));
            if(effect.value(k("per_target")).toBool()&&runtime.value(received?k("damage_source_key"):k("damage_target_key")).toString()!=target)return rejected(error,k("stateful_damage_receipt_target_mismatch"));
            const auto key=recordKey(effect,target);const auto receiptKey=key+k("|")+receiptId;
            if(receipts.contains(receiptKey))return rejected(error,k("stateful_duplicate_damage_receipt"));
            const auto multiplier=effect.contains(k("expression"))?numeric(effect.value(k("expression"))):std::optional<double>(1);
            if(!multiplier||*multiplier<0)return rejected(error,k("stateful_effect_formula_invalid"));amount=records.value(key).toDouble()+actual.toDouble()* *multiplier;
            if(effect.contains(k("cap_expression"))) {const auto cap=numeric(effect.value(k("cap_expression")));if(!cap||*cap<0)return rejected(error,k("stateful_effect_formula_invalid"));amount=std::min(amount,*cap);}
            records[key]=amount;receipts.append(receiptKey);context.counters[k("recorded_damage")]=amount;context.counters[k("recorded_damage:")+effect.value(k("key")).toString(k("damage"))]=amount;
        } else if(op==k("replay_damage")) {
            const auto key=recordKey(effect,target);const double recorded=records.value(key).toDouble();context.counters[k("recorded_damage")]=recorded;
            std::optional<double> quantity;
            if(effect.contains(k("coefficient_expression"))){const auto coefficient=numeric(effect.value(k("coefficient_expression")));if(coefficient)quantity=recorded* *coefficient;}
            else quantity=effect.contains(k("expression"))?numeric(effect.value(k("expression"))):std::optional<double>(recorded);
            if(!quantity||*quantity<0)return rejected(error,k("stateful_effect_formula_invalid"));amount=*quantity;
            if(effect.value(k("consume")).toBool(true))records.remove(key);
        } else if(op==k("spawn_entity")) {
            QString id=effect.value(k("entity_key")).toString(entityId);if(id.isEmpty())id=eventId+k(":entity");
            if(!token(id)||entities.contains(id))return rejected(error,k("stateful_entity_already_exists"));
            if(effect.contains(k("max_entities"))&&entities.size()>=effect.value(k("max_entities")).toInt())return rejected(error,k("stateful_entity_capacity"));
            if(duration<0&&!effect.value(k("until_destroyed")).toBool())return rejected(error,k("stateful_entity_lifetime_required"));
            const auto hp=effect.contains(k("hp_expression"))?numeric(effect.value(k("hp_expression"))):std::optional<double>(1);
            const auto interval=effect.contains(k("attack_interval_expression"))?numeric(effect.value(k("attack_interval_expression"))):std::optional<double>(-1);
            if(!hp||*hp<=0||!interval||*interval>1e6||*interval< -1)return rejected(error,k("stateful_entity_stats_invalid"));
            QJsonObject entity{{k("entity_id"),id},{k("owner_key"),owner_},{k("binding_id"),bindingId},{k("created_at"),at},{k("expires_at"),duration<0?-1:at+duration},
                {k("hp"),*hp},{k("maximum_hp"),*hp},{k("attack_interval"),*interval},{k("attack_ready_at"),at},{k("attacks"),0}};
            entity[k("hp_known")]=effect.contains(k("hp_expression"));entity[k("hp_source_known")]=effect.contains(k("hp_expression"));
            entity[k("hp_basis")]=effect.contains(k("hp_expression"))?k("source"):k("unknown");
            entity[k("source_program_sha256")]=hash(QJsonArray{program});entity[k("attack_interval_basis")]=effect.contains(k("attack_interval_expression"))?k("source"):k("unknown");
            if(effect.contains(k("entity_damage_expression")))entity[k("damage_expression")]=effect.value(k("entity_damage_expression"));
            QJsonObject entityStats;
            const auto statsExpressions=effect.value(k("stats")).toObject();
            for(auto it=statsExpressions.begin();it!=statsExpressions.end();++it) {const auto value=numeric(it.value());if(!value)return rejected(error,k("stateful_entity_stats_invalid"));entityStats[it.key()]=*value;}
            entity[k("stats")]=entityStats;entities[id]=entity;effect[k("entity_id")]=id;effect[k("entity")]=entity;
        } else if(op==k("destroy_entity")||op==k("entity_damage")) {
            const auto id=effect.value(k("entity_key")).toString(entityId);if(!entities.contains(id))return rejected(error,k("stateful_owned_entity_required"));
            if(op==k("destroy_entity"))entities.remove(id);
            else {const auto damage=numeric(effect.value(k("expression")));if(!damage||*damage<0)return rejected(error,k("stateful_entity_damage_invalid"));auto entity=entities.value(id).toObject();if(!applyEntityDamage(entity,id,*damage))return rejected(error,k("stateful_entity_damage_receipt_required"));entities[id]=entity;amount=*damage;}
            effect[k("entity_id")]=id;
        } else if(op==k("counter")||op==k("reset_counter")) {
            const auto key=counterKey(effect,target);if(!token(key))return rejected(error,k("stateful_invalid_counter"));
            const auto delta=op==k("reset_counter")?std::optional<double>(0):numeric(effect.value(k("expression")));
            if(!delta)return rejected(error,k("stateful_counter_formula_invalid"));
            auto values=effect.contains(k("shared_key"))?sharedCounters_:counters;values[key]=op==k("reset_counter")?0:values.value(key).toDouble()+*delta;
            if(!numericMap(values,0,1e12))return rejected(error,k("stateful_counter_out_of_range"));
            if(effect.contains(k("shared_key")))sharedCounters_=values;else counters=values;
            addCounterAliases(context,values,target);
            if(effect.contains(k("duration_expression"))) {
                if(duration<0)return rejected(error,k("stateful_counter_duration_invalid"));
                if(effect.contains(k("shared_key")))sharedCounterExpiries_[key]=at+duration;else expiries[key]=at+duration;
            }
        } else if(op!=k("set_value")) {
            auto expression=effect.value(k("expression"));
            if(expression.isUndefined()&&effect.contains(k("amount_expression")))expression=effect.value(k("amount_expression"));
            if(expression.isUndefined()&&effect.contains(k("rate_expression")))expression=effect.value(k("rate_expression"));
            const bool metadataOnly=op==k("movement")||op==k("position")||op==k("attach")||op==k("detach")||op==k("mark")||
                op==k("control")||op==k("reveal")||op==k("untargetable")||(op==k("cooldown")&&effect.value(k("mode")).toString()==k("reset"));
            const auto value=expression.isUndefined()&&metadataOnly?std::optional<double>(0):numeric(expression);
            if(!value||!std::isfinite(*value)||std::abs(*value)>1e12)return rejected(error,k("stateful_effect_formula_invalid"));amount=*value;
            if(op!=k("resource")&&op!=k("stat_buff")&&op!=k("cooldown")&&amount<0)return rejected(error,k("stateful_negative_effect_amount"));
        }
        if(op==k("resource")) {
            const auto mode=effect.value(k("mode")).toString();
            if(mode==k("upkeep_start")){effect[k("op")]=k("resource_upkeep");effect[k("rate")]=amount;}
            else if(mode!=k("upkeep_stop")) {
                if(effect.value(k("resource_kind")).toString()==k("health")) {if(amount<0)healthCost-=amount;}
                else resourceDelta+=amount;
            }
        }
        if(op==k("health_cost"))healthCost+=amount;
        if(op==k("cooldown")&&effect.contains(k("scope"))) {
            const auto scope=effect.value(k("scope")).toString(),mode=effect.value(k("mode")).toString(k("reduce"));
            if(amount<0)return rejected(error,k("stateful_invalid_cooldown"));
            double ready=-1;
            QString timerKey;
            if(scope==k("instance_expiry"))ready=instance.value(k("expires_at")).toDouble(-1);
            else if(scope==k("owned_timer")) {
                timerKey=effect.value(k("timer_key")).toString();if(!token(timerKey))return rejected(error,k("stateful_owned_timer_key_required"));
                if(mode==k("seed")&&!program.value(k("owned_timer_pools")).toArray().isEmpty()&&transition.value(k("requires_timer_observation")).toString()!=timerKey)return rejected(error,k("stateful_owned_timer_observation_required"));
                timerKey=k("owned:")+timerKey;ready=clocks.value(timerKey).toDouble(-1);
            } else return rejected(error,k("stateful_cooldown_scope_unavailable"));
            const double priorReady=ready;
            if(mode==k("seed")&&scope==k("owned_timer")) {
                if(clocks.contains(timerKey))return rejected(error,k("stateful_owned_timer_already_initialized"));ready=at+amount;
            } else {
                if(ready<0)return rejected(error,k("stateful_owned_cooldown_uninitialized"));
                if(mode==k("reset"))ready=at;
                else if(mode==k("reduce")||mode==k("reduce_seconds"))ready=std::max(at,ready-amount);
                else if(mode==k("refund_fraction")&&amount<=1)ready=at+std::max(0.,ready-at)*(1-amount);
                else return rejected(error,k("stateful_cooldown_mode_unavailable"));
            }
            if(scope==k("instance_expiry")){instance[k("expires_at")]=ready;instanceExpiryAdjusted=true;}
            else {clocks[timerKey]=ready;context.counters[k("owned_timer_remaining:")+timerKey.mid(6)]=std::max(0.,ready-at);}
            effect[k("ledger_applied")]=true;effect[k("ready_at_before")]=priorReady;effect[k("ready_at_after")]=ready;
        }
        if(op==k("damage_reduction")&&(amount<0||amount>1))return rejected(error,k("stateful_damage_reduction_out_of_range"));
        effect[k("amount")]=amount;effect[k("source_key")]=owner_;effect[k("source_binding_id")]=bindingId;effect[k("source_slot")]=program.value(k("slot"));
        effect[k("source_transition_id")]=transitionId;effect[k("target_key")]=recipient;effect[k("at")]=at;effect[k("start_time")]=at;
        if(duration>=0){effect[k("duration")]=duration;effect[k("expires_at")]=at+duration;}
        if(op==k("resource_upkeep")||effect.value(k("op")).toString()==k("resource_upkeep")||op==k("heal_over_time"))effect[k("rate")]=amount;
        const bool timed=op==k("stat_buff")||op==k("control")||op==k("reveal")||op==k("untargetable")||op==k("damage_reduction")||
            op==k("resource_upkeep")||op==k("heal_over_time")||effect.value(k("op")).toString()==k("resource_upkeep");
        if(effect.value(k("remove")).toBool()) {
            QJsonArray retained;for(const auto &row:active) {
                const auto prior=row.toObject();const bool same=prior.value(k("op"))==effect.value(k("op"))&&
                    (effect.value(k("status_key")).toString().isEmpty()||prior.value(k("status_key"))==effect.value(k("status_key")))&&prior.value(k("target_key"))==effect.value(k("target_key"));
                if(!same)retained.append(row);
            }active=retained;
        } else if(effect.value(k("mode")).toString()==k("upkeep_stop")) {
            QJsonArray retained;for(const auto &row:active)if(row.toObject().value(k("op")).toString()!=k("resource_upkeep"))retained.append(row);active=retained;
        } else if(timed) {
            if(duration<0&&!effect.value(k("until_cancelled")).toBool()&&!effect.value(k("permanent")).toBool())return rejected(error,k("stateful_effect_lifetime_required"));
            if(effect.value(k("replace_existing")).toBool(true)) {QJsonArray retained;for(const auto &row:active){const auto old=row.toObject();if(old.value(k("op"))!=effect.value(k("op"))||old.value(k("stat"))!=effect.value(k("stat"))||old.value(k("target_key"))!=effect.value(k("target_key")))retained.append(row);}active=retained;}
            active.append(effect);
        }
        effects.append(effect);
        if(effect.value(k("consume_recorded_damage")).toBool())records.remove(recordKey(effect,target));
    }
    evaluationRank=std::max(1,rank);
    if(resourceDelta<0&&(!finite(runtime.value(k("self_current_mana")),0,1e12)||runtime.value(k("self_current_mana")).toDouble()+resourceDelta< -eps))return rejected(error,k("stateful_insufficient_resource"));
    if(healthCost>0&&(!finite(runtime.value(k("self_current_hp")),0,1e12)||runtime.value(k("self_current_hp")).toDouble()-healthCost<=eps))return rejected(error,k("stateful_health_cost_unaffordable"));
    if(transition.value(k("consume_entity")).toBool())entities.remove(entityId);
    if(transition.value(k("consume_shield")).toBool()||transition.value(k("consume_owned_shield")).toBool())effects.append(QJsonObject{{k("op"),k("shield")},{k("remove"),true},{k("consume"),true},
        {k("amount"),runtime.value(k("owned_shield_remaining"))},{k("target_key"),owner_},{k("source_key"),owner_},
        {k("source_slot"),program.value(k("slot"))},{k("source_binding_id"),bindingId},{k("at"),at}});
    for(const auto &name:{k("cooldown_expression"),k("per_target_cooldown_expression")})if(transition.contains(name)) {
        const auto duration=numeric(transition.value(name));if(!duration||*duration<0||*duration>1e9)return rejected(error,k("stateful_invalid_cooldown"));
        if(sharedClock)sharedClocks_[clockKey]=at+*duration;else clocks[clockKey]=at+*duration;
    }
    auto after=transition.value(k("to")).toString();
    auto pools=instance.value(k("resource_pools")).toObject();
    for(const auto &raw:program.value(k("resource_pools")).toArray()) {
        const auto definition=raw.toObject();const auto key=definition.value(k("key")).toString();
        if(!token(key)||!definition.value(k("active_states")).isArray())return rejected(error,k("stateful_resource_pool_invalid"));
        if(!poolActive(definition,after)){pools.remove(key);continue;}
        if(pools.contains(key)&&poolActive(definition,before)) {
            if(counters.value(key).toDouble()>pools.value(key).toObject().value(k("maximum")).toDouble()+eps)return rejected(error,k("stateful_resource_pool_above_maximum"));
            continue;
        }
        const auto maximum=numeric(definition.value(k("maximum_expression"))),initial=numeric(definition.value(k("initial_expression"))),rate=numeric(definition.value(k("decay_per_second_expression")));
        const auto delay=definition.contains(k("after_seconds_expression"))?numeric(definition.value(k("after_seconds_expression"))):std::optional<double>(-1);
        const auto lateRate=definition.contains(k("late_decay_per_second_expression"))?numeric(definition.value(k("late_decay_per_second_expression"))):rate;
        if(!maximum||!initial||!rate||!delay||!lateRate||*maximum<=0||*initial<0||*initial>*maximum||*rate<0||*lateRate<0||*delay< -1)return rejected(error,k("stateful_resource_pool_formula_invalid"));
        if(!counters.contains(key))counters[key]=*initial;
        if(counters.value(key).toDouble()>*maximum+eps)return rejected(error,k("stateful_resource_pool_above_maximum"));
        pools[key]=QJsonObject{{k("maximum"),*maximum},{k("decay_rate"),*rate},{k("late_decay_rate"),*lateRate},{k("late_at"),*delay<0?-1:at+*delay},
            {k("last_integrated_at"),at},{k("active_states"),definition.value(k("active_states"))},{k("on_empty_state"),definition.value(k("on_empty_state")).toString(k("idle"))}};
    }
    if(transition.contains(k("reset_clock"))?transition.value(k("reset_clock")).toBool():after!=before)instance[k("entered_at")]=at;
    if((before==program.value(k("initial_state")).toString(k("idle"))&&after!=before)||
        (transition.value(k("requires_admitted_cast")).toBool()&&(transition.value(k("reset_counters")).toBool()||transition.value(k("clear_counters")).toBool()))||
        transition.value(k("reset_instance_clock")).toBool())instance[k("started_at")]=at;
    if(transition.value(k("clear_expiry")).toBool()){instance[k("expires_at")]=-1;instance.remove(k("expiry_state"));}
    if(transition.contains(k("duration_expression"))) {
        const auto duration=numeric(transition.value(k("duration_expression")));if(!duration||*duration<0||*duration>1e9)return rejected(error,k("stateful_invalid_duration"));
        instance[k("expires_at")]=at+*duration;
        if(transition.contains(k("expiry_state")))instance[k("expiry_state")]=transition.value(k("expiry_state"));
        else instance.remove(k("expiry_state"));
    }
    if(instanceExpiryAdjusted&&instance.value(k("expires_at")).toDouble(-1)>=0&&instance.value(k("expires_at")).toDouble()<=at) {
        after=instance.value(k("expiry_state")).toString(program.value(k("expiry_state")).toString(program.value(k("initial_state")).toString(k("idle"))));
        instance[k("expires_at")]=-1;instance[k("entered_at")]=at;instance.remove(k("expiry_state"));
        if(after==program.value(k("initial_state")).toString(k("idle")))instance[k("target_key")]=QString();
    }
    if(transition.value(k("clear_target")).toBool())instance[k("target_key")]=QString();
    if(transition.value(k("clear_effects")).toBool())active=QJsonArray{};
    instance[k("state")]=after;instance[k("last_event_at")]=at;instance[k("counters")]=counters;instance[k("counter_expiries")]=expiries;
    instance[k("entities")]=entities;instance[k("records")]=records;instance[k("record_receipts")]=receipts;instance[k("active_effects")]=active;instance[k("clocks")]=clocks;instance[k("resource_pools")]=pools;
    if(!bridge.isEmpty()) {
        bridgeSourceEntities.remove(bridgeEntityKey);bridgeSource[k("entities")]=bridgeSourceEntities;
        auto consumed=bridgeSource.value(k("record_receipts")).toArray();consumed.append(k("entity_bridge|")+bindingId+k("|")+eventId+k("|")+bridgeEntityKey);
        bridgeSource[k("record_receipts")]=consumed;instances_[bridgeBinding]=bridgeSource;
        auto bridgeReceipts=instance.value(k("entity_bridge_receipts")).toArray();bridgeReceipts.append(QJsonObject{{k("source_binding_id"),bridgeBinding},{k("source_entity_key"),bridgeEntityKey},
            {k("source_program_sha256"),hash(QJsonArray{programs_.value(bridgeBinding)})},{k("operation"),bridge.value(k("operation"))},{k("event_id"),eventId},{k("time"),at}});instance[k("entity_bridge_receipts")]=bridgeReceipts;
    }
    instances_[bindingId]=instance;eventIds_.insert(eventId);
    return {{k("accepted"),true},{k("reason"),QJsonValue::Null},{k("event_id"),eventId},{k("binding_id"),bindingId},
        {k("transition_id"),transitionId},{k("time"),at},{k("state_before"),before},{k("state_after"),after},
        {k("effects"),effects},{k("state_elapsed"),elapsed},{k("target_key"),instance.value(k("target_key"))}};
}

bool StatefulSkillLedgerV7::onDeath(double at,QString *error) {
    auto next=*this;if(!next.advanceInPlace(at,error))return false;
    for(auto it=next.instances_.begin();it!=next.instances_.end();++it)if(!next.programs_.value(it.key()).value(k("persist_on_owner_death")).toBool()) {
        const auto old=it.value();it.value()=fresh(next.programs_.value(it.key()),at);
        if(!next.programs_.value(it.key()).value(k("reset_cooldowns_on_death")).toBool())it.value()[k("clocks")]=old.value(k("clocks"));
    }
    QSet<QString> persistentShared;
    for(auto p=next.programs_.begin();p!=next.programs_.end();++p)if(p.value().value(k("persist_on_owner_death")).toBool())for(const auto &raw:p.value().value(k("transitions")).toArray()) {
        const auto step=raw.toObject();auto definitions=step.value(k("counters")).toArray();if(step.value(k("counter")).isObject())definitions.append(step.value(k("counter")));
        for(const auto &effect:step.value(k("effects")).toArray())if(effect.toObject().contains(k("shared_key")))definitions.append(effect);
        for(const auto &definition:definitions)if(definition.toObject().contains(k("shared_key")))persistentShared.insert(definition.toObject().value(k("shared_key")).toString());
    }
    for(auto counter=next.sharedCounters_.begin();counter!=next.sharedCounters_.end();) {
        if(!persistentShared.contains(counter.key().section(k("|"),0,0))){next.sharedCounterExpiries_.remove(counter.key());counter=next.sharedCounters_.erase(counter);}else ++counter;
    }
    *this=next;return true;
}

bool StatefulSkillLedgerV7::onInterrupt(double at,QString *error) {
    auto next=*this;if(!next.advanceInPlace(at,error))return false;
    for(auto it=next.instances_.begin();it!=next.instances_.end();++it)if(next.programs_.value(it.key()).value(k("cancel_on_interrupt")).toBool()) {
        const auto clocks=it.value().value(k("clocks"));it.value()=fresh(next.programs_.value(it.key()),at);it.value()[k("clocks")]=clocks;
    }
    *this=next;return true;
}

bool StatefulSkillLedgerV7::onResourceExhausted(double at,const QString &resourceKind,QString *error) {
    auto next=*this;if(!next.advanceInPlace(at,error))return false;
    for(auto it=next.instances_.begin();it!=next.instances_.end();++it) {
        bool matches=false;
        for(const auto &raw:it.value().value(k("active_effects")).toArray()) {
            const auto effect=raw.toObject();if(effect.value(k("op")).toString()==k("resource_upkeep")&&effect.value(k("resource_kind")).toString(k("mana"))==resourceKind)matches=true;
        }
        if(matches) {const auto old=it.value();it.value()=fresh(next.programs_.value(it.key()),at);it.value()[k("clocks")]=old.value(k("clocks"));it.value()[k("stop_reason")]=k("resource_exhausted");it.value()[k("stopped_at")]=at;}
    }
    *this=next;return true;
}

double StatefulSkillLedgerV7::nextBoundary(double at,double fallback) const {
    double result=fallback;auto consider=[&](double value){if(value>at+eps)result=std::min(result,value);};
    for(auto it=instances_.begin();it!=instances_.end();++it) {
        const auto instance=it.value();consider(instance.value(k("expires_at")).toDouble(-1));
        const auto pools=instance.value(k("resource_pools")).toObject();const auto values=instance.value(k("counters")).toObject();
        for(auto pool=pools.begin();pool!=pools.end();++pool)if(poolActive(pool.value().toObject(),instance.value(k("state")).toString())) {
            const auto value=pool.value().toObject();consider(value.value(k("late_at")).toDouble(-1));
            consider(poolDepletionAt(value,values.value(pool.key()).toDouble(),value.value(k("last_integrated_at")).toDouble()));
        }
        const auto expiries=instance.value(k("counter_expiries")).toObject();for(auto e=expiries.begin();e!=expiries.end();++e)consider(e.value().toDouble());
        const auto entities=instance.value(k("entities")).toObject();for(auto e=entities.begin();e!=entities.end();++e)consider(e.value().toObject().value(k("expires_at")).toDouble(-1));
        for(const auto &raw:instance.value(k("active_effects")).toArray())consider(raw.toObject().value(k("expires_at")).toDouble(-1));
    }
    for(auto it=sharedCounterExpiries_.begin();it!=sharedCounterExpiries_.end();++it)consider(it.value().toDouble());
    return result;
}

QJsonArray StatefulSkillLedgerV7::activeEffects(double at) const {
    QJsonArray result;
    for(auto it=instances_.begin();it!=instances_.end();++it)for(const auto &raw:it.value().value(k("active_effects")).toArray()) {
        const auto effect=raw.toObject();const double start=effect.value(k("start_time")).toDouble(),end=effect.value(k("expires_at")).toDouble(-1);
        if(start<=at&&(end<0||at<end))result.append(effect);
    }
    return result;
}

QJsonObject StatefulSkillLedgerV7::snapshot() const {
    QJsonObject instances;for(auto it=instances_.begin();it!=instances_.end();++it)instances[it.key()]=it.value();
    auto ids=eventIds_.values();ids.sort();
    return {{k("schema_version"),7},{k("owner_key"),owner_},{k("team"),team_},{k("champion_id"),champion_},
        {k("combat_revision"),revision_},{k("programs_sha256"),programHash_},{k("time"),time_},
        {k("event_ids"),QJsonArray::fromStringList(ids)},{k("instances"),instances},
        {k("shared_counters"),sharedCounters_},{k("shared_counter_expiries"),sharedCounterExpiries_},{k("shared_clocks"),sharedClocks_}};
}

bool StatefulSkillLedgerV7::restore(const QJsonObject &checkpoint,QString *error) {
    if(!loaded_||checkpoint.value(k("schema_version")).toInt()!=7||checkpoint.value(k("owner_key")).toString()!=owner_||
        checkpoint.value(k("team")).toString()!=team_||checkpoint.value(k("champion_id")).toInt()!=champion_||
        checkpoint.value(k("combat_revision")).toString()!=revision_||checkpoint.value(k("programs_sha256")).toString()!=programHash_||
        !finite(checkpoint.value(k("time")),0,1e9)||!checkpoint.value(k("instances")).isObject()||!checkpoint.value(k("event_ids")).isArray())return fail(error,k("stateful_checkpoint_identity_mismatch"));
    auto next=*this;next.time_=checkpoint.value(k("time")).toDouble();next.eventIds_.clear();next.instances_.clear();
    const auto rows=checkpoint.value(k("instances")).toObject();if(rows.size()!=programs_.size())return fail(error,k("stateful_checkpoint_binding_mismatch"));
    for(const auto &raw:checkpoint.value(k("event_ids")).toArray()) {const auto id=raw.toString();if(!token(id)||next.eventIds_.contains(id))return fail(error,k("stateful_checkpoint_event_ids_invalid"));next.eventIds_.insert(id);}
    for(auto it=rows.begin();it!=rows.end();++it) {
        if(!programs_.contains(it.key())||!it.value().isObject())return fail(error,k("stateful_checkpoint_binding_mismatch"));
        const auto row=it.value().toObject();const auto program=programs_.value(it.key());QSet<QString> states{program.value(k("initial_state")).toString(k("idle")),program.value(k("expiry_state")).toString(k("idle"))};
        for(const auto &raw:program.value(k("transitions")).toArray()){const auto step=raw.toObject();states.insert(step.value(k("to")).toString());if(step.contains(k("expiry_state")))states.insert(step.value(k("expiry_state")).toString());for(const auto &state:step.value(k("from")).toArray())states.insert(state.toString());}
        for(const auto &raw:program.value(k("resource_pools")).toArray())states.insert(raw.toObject().value(k("on_empty_state")).toString(k("idle")));
        if(!states.contains(row.value(k("state")).toString())||!finite(row.value(k("entered_at")),0,next.time_)||!finite(row.value(k("started_at")),0,next.time_)||!finite(row.value(k("last_event_at")),0,next.time_)||
            !finite(row.value(k("expires_at")),-1,1e9)||!row.value(k("counters")).isObject()||!numericMap(row.value(k("counters")).toObject(),0)||
            !row.value(k("counter_expiries")).isObject()||!row.value(k("records")).isObject()||!row.value(k("clocks")).isObject()||
            !numericMap(row.value(k("counter_expiries")).toObject(),0,1e9)||!numericMap(row.value(k("records")).toObject(),0)||!numericMap(row.value(k("clocks")).toObject(),0,1e9)||
            !row.value(k("resource_pools")).isObject()||!row.value(k("entity_damage_receipts")).isObject()||!row.value(k("entities")).isObject()||!row.value(k("active_effects")).isArray()||!row.value(k("record_receipts")).isArray()||
            (row.contains(k("expiry_state"))&&!states.contains(row.value(k("expiry_state")).toString())))return fail(error,k("stateful_checkpoint_state_invalid"));
        if(row.contains(k("expiry_tombstone"))) {
            const auto tombstone=row.value(k("expiry_tombstone")).toObject();const double expiredAt=tombstone.value(k("expires_at")).toDouble(-1);
            if(!states.contains(tombstone.value(k("state")).toString())||!finite(expiredAt,0,next.time_)||
                !finite(tombstone.value(k("entered_at")),0,expiredAt)||!finite(tombstone.value(k("started_at")),0,expiredAt)||!finite(tombstone.value(k("last_event_at")),0,expiredAt)||
                !tombstone.value(k("target_key")).isString())return fail(error,k("stateful_checkpoint_expiry_tombstone_invalid"));
        }
        QSet<QString> receiptIds;
        for(const auto &receipt:row.value(k("record_receipts")).toArray()){if(!token(receipt.toString())||receiptIds.contains(receipt.toString()))return fail(error,k("stateful_checkpoint_receipt_invalid"));receiptIds.insert(receipt.toString());}
        if(row.contains(k("activation_cast_receipt"))) {
            const auto receipt=row.value(k("activation_cast_receipt")).toObject();
            if(receipt.value(k("binding_id")).toString()!=it.key()||receipt.value(k("owner_key")).toString()!=owner_||!token(receipt.value(k("event_id")).toString())||
                !token(receipt.value(k("slot")).toString())||!finite(receipt.value(k("time")),0,next.time_)||!finite(receipt.value(k("resource_cost")),0))return fail(error,k("stateful_checkpoint_cast_receipt_invalid"));
            if(program.value(k("hero_event_contract_v1")).toBool()&&
               (receipt.value(k("slot"))!=program.value(k("slot"))||!next.eventIds_.contains(receipt.value(k("event_id")).toString())))
                return fail(error,k("stateful_checkpoint_hero_cast_receipt_unowned"));
        }
        const auto observations=row.value(k("owned_timer_observations")).toObject();
        for(auto observation=observations.begin();observation!=observations.end();++observation) {
            const auto value=observation.value().toObject();bool declared=false;
            for(const auto &pool:program.value(k("owned_timer_pools")).toArray())if(pool.toObject().value(k("timer_key")).toString()==observation.key())declared=true;
            if(!declared||!token(value.value(k("observation_id")).toString())||!finite(value.value(k("time")),0,next.time_)||value.value(k("programs_sha256")).toString()!=programHash_||
                !row.value(k("clocks")).toObject().contains(k("owned:")+observation.key()))return fail(error,k("stateful_checkpoint_timer_observation_invalid"));
        }
        for(const auto &raw:row.value(k("entity_bridge_receipts")).toArray()) {
            const auto receipt=raw.toObject();const auto binding=receipt.value(k("source_binding_id")).toString();bool declared=false;
            for(const auto &step:program.value(k("transitions")).toArray()){const auto bridge=step.toObject().value(k("entity_bridge")).toObject();if(bridge.value(k("source_binding_id"))==receipt.value(k("source_binding_id"))&&bridge.value(k("source_entity_key"))==receipt.value(k("source_entity_key"))&&bridge.value(k("operation"))==receipt.value(k("operation")))declared=true;}
            if(!declared||!programs_.contains(binding)||receipt.value(k("source_program_sha256")).toString()!=hash(QJsonArray{programs_.value(binding)})||
                !next.eventIds_.contains(receipt.value(k("event_id")).toString())||!finite(receipt.value(k("time")),0,next.time_))return fail(error,k("stateful_checkpoint_entity_bridge_invalid"));
        }
        const auto entityReceipts=row.value(k("entity_damage_receipts")).toObject();
        for(auto receipt=entityReceipts.begin();receipt!=entityReceipts.end();++receipt) {
            const auto value=receipt.value().toObject();
            if(!token(receipt.key())||receipt.key()!=value.value(k("damage_receipt_id")).toString()||value.value(k("damage_target_owner_key")).toString()!=owner_||
                value.value(k("damage_target_binding_id")).toString()!=it.key()||!token(value.value(k("damage_target_key")).toString())||!token(value.value(k("damage_source_key")).toString())||
                !finite(value.value(k("actual_damage")),0)||!finite(value.value(k("damage_receipt_time")),0,next.time_)||!finite(value.value(k("hp_before")),0)||!finite(value.value(k("hp_after")),0)||
                std::abs(value.value(k("hp_before")).toDouble()-value.value(k("hp_after")).toDouble()-value.value(k("actual_damage")).toDouble())>eps)return fail(error,k("stateful_checkpoint_entity_receipt_invalid"));
        }
        QMap<QString,QJsonObject> poolDefinitions;
        for(const auto &pool:program.value(k("resource_pools")).toArray())poolDefinitions[pool.toObject().value(k("key")).toString()]=pool.toObject();
        const auto pools=row.value(k("resource_pools")).toObject();
        for(auto pool=pools.begin();pool!=pools.end();++pool) {
            const auto value=pool.value().toObject(),definition=poolDefinitions.value(pool.key());
            if(definition.isEmpty()||!finite(value.value(k("maximum")),eps)||!finite(value.value(k("decay_rate")),0)||!finite(value.value(k("late_decay_rate")),0)||
                !finite(value.value(k("late_at")),-1,1e9)||!finite(value.value(k("last_integrated_at")),0,next.time_)||
                value.value(k("active_states"))!=definition.value(k("active_states"))||value.value(k("on_empty_state")).toString()!=definition.value(k("on_empty_state")).toString(k("idle"))||
                row.value(k("counters")).toObject().value(pool.key()).toDouble()>value.value(k("maximum")).toDouble()+eps)return fail(error,k("stateful_checkpoint_resource_pool_invalid"));
        }
        const auto entities=row.value(k("entities")).toObject();
        for(auto entity=entities.begin();entity!=entities.end();++entity) {
            const auto object=entity.value().toObject();
            if(!token(entity.key())||object.value(k("entity_id")).toString()!=entity.key()||object.value(k("owner_key")).toString()!=owner_||object.value(k("binding_id")).toString()!=it.key()||
                !finite(object.value(k("hp")),0)||!finite(object.value(k("maximum_hp")),0)||object.value(k("hp")).toDouble()>object.value(k("maximum_hp")).toDouble()+eps||
                !finite(object.value(k("created_at")),0,next.time_)||!finite(object.value(k("expires_at")),-1,1e9)||!finite(object.value(k("attack_interval")),-1,1e6)||!finite(object.value(k("attack_ready_at")),0,1e9)||
                !numericMap(object.value(k("stats")).toObject()))return fail(error,k("stateful_checkpoint_entity_invalid"));
            if(object.value(k("attack_clock_basis")).toString()==k("owner_admitted_attack")) {
                const auto receiptId=object.value(k("last_owner_attack_receipt_id")).toString();
                bool declared=false;
                for(const auto &raw:program.value(k("transitions")).toArray()) {
                    const auto step=raw.toObject();
                    if(step.value(k("entity_attack_clock")).toString()==k("owner_admitted_attack")&&step.value(k("entity_attack")).toBool()&&
                        step.value(k("requires_admitted_attack")).toBool()&&step.value(k("attack_payload_policy")).toString()==k("replace_owner_base_attack"))declared=true;
                }
                if(!declared||!token(receiptId)||!finite(object.value(k("last_owner_attack_receipt_time")),0,next.time_)||
                    object.value(k("attack_ready_at")).toDouble()<object.value(k("last_owner_attack_receipt_time")).toDouble()||
                    !row.value(k("record_receipts")).toArray().contains(k("owner_attack|")+entity.key()+k("|")+receiptId))return fail(error,k("stateful_checkpoint_entity_owner_clock_invalid"));
            } else if(object.value(k("attack_clock_basis")).toString()==k("verified_observation")) {
                if(object.value(k("attack_interval_basis")).toString()!=k("observed")||!finite(object.value(k("attack_interval")),eps,3600)||
                    !token(object.value(k("clock_observation_id")).toString())||!finite(object.value(k("clock_observation_time")),0,next.time_)||
                    object.value(k("clock_observation_programs_sha256")).toString()!=programHash_||object.value(k("source_program_sha256")).toString()!=hash(QJsonArray{program}))return fail(error,k("stateful_checkpoint_entity_clock_observation_invalid"));
            } else if(object.contains(k("attack_clock_basis"))&&object.value(k("attack_clock_basis")).toString()!=k("independent"))return fail(error,k("stateful_checkpoint_entity_attack_clock_invalid"));
        }
        for(const auto &raw:row.value(k("active_effects")).toArray()) {
            const auto effect=raw.toObject();if(!effectOps.contains(effect.value(k("op")).toString())||effect.value(k("source_key")).toString()!=owner_||effect.value(k("source_binding_id")).toString()!=it.key()||
                !finite(effect.value(k("amount")))||!finite(effect.value(k("start_time")),0,next.time_)||
                (effect.contains(k("expires_at"))&&!finite(effect.value(k("expires_at")),0,1e9)))return fail(error,k("stateful_checkpoint_effect_invalid"));
        }
        next.instances_[it.key()]=row;
    }
    next.sharedCounters_=checkpoint.value(k("shared_counters")).toObject();next.sharedCounterExpiries_=checkpoint.value(k("shared_counter_expiries")).toObject();next.sharedClocks_=checkpoint.value(k("shared_clocks")).toObject();
    if(!numericMap(next.sharedCounters_,0)||!numericMap(next.sharedCounterExpiries_,0,1e9)||!numericMap(next.sharedClocks_,0,1e9))return fail(error,k("stateful_checkpoint_counter_invalid"));
    *this=next;return true;
}
