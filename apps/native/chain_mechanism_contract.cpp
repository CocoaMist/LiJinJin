#include "chain_mechanism_contract.h"
#include "combat.h"
#include <QJsonArray>
#include <QSet>
#include <cmath>

namespace {
QString k(const char *v){return QString::fromLatin1(v);}
bool number(const QJsonValue &v,double low=0,double high=1e12){return v.isDouble()&&std::isfinite(v.toDouble())&&v.toDouble()>=low&&v.toDouble()<=high;}
bool token(const QJsonValue &v){return v.isString()&&!v.toString().trimmed().isEmpty()&&v.toString().size()<=256;}
bool nativeReason(const QJsonValue &v,const QString &reason){return v==reason||v==k("mechanic_observation_rejected:")+reason;}
QString playerKey(const QJsonObject &p){return p.value(k("player_key")).toString(p.value(k("team")).toString()+k(":")+QString::number(p.value(k("champion_id")).toInt())+k(":1"));}
QMap<QString,QJsonObject> actors(const QJsonObject &scenario){QMap<QString,QJsonObject> out;for(const auto &v:scenario.value(k("players")).toArray()){const auto p=v.toObject();if(p.value(k("augments")).toArray().contains(3049))out[playerKey(p)]=p;}return out;}
bool needsHit(const Catalog &c,const QJsonObject &e,const QJsonObject &p){const auto contract=chainActionContract(c,e,p);return contract.value(k("requires_admitted_attack")).toBool()||e.value(k("facts")).toObject().value(k("verified_skill_onhit_eligibility"))==QJsonValue(true)||(p.value(k("champion_id"))==QJsonValue(39)&&contract.value(k("requires_admitted_cast")).toBool()&&contract.value(k("slot"))==k("Q"));}
void error(QJsonArray &errors,const QString &reason,const QString &id=QString()){const QJsonObject row{{k("reason"),reason},{k("event_id"),id}};if(!errors.contains(row))errors.append(row);}
QJsonObject report(bool required,const QJsonArray &errors){return {{k("required"),required},{k("complete"),errors.isEmpty()},{k("errors"),errors},{k("source_scope"),k("caller_declared_conditional_reference_not_authenticated_live_observation")},{k("live_certified"),false}};}
}

QJsonObject chainActionContract(const Catalog &c,const QJsonObject &e,const QJsonObject &actor){
    const auto type=e.value(k("type")).toString();
    if(type!=k("skill_state"))return {{k("known"),true},{k("requires_admitted_cast"),type==k("cast")},{k("requires_admitted_attack"),type==k("attack")},{k("slot"),e.value(k("slot"))}};
    if(c.combat)for(const auto &v:c.combat->champions.value(actor.value(k("champion_id")).toInt()).value(k("stateful_bindings_v7")).toArray()){
        const auto p=v.toObject();if(p.value(k("binding_id"))!=e.value(k("binding_id")))continue;
        for(const auto &raw:p.value(k("transitions")).toArray()){const auto t=raw.toObject();if(t.value(k("id"))!=e.value(k("transition_id")))continue;
            auto slot=t.value(k("admission_slot")).toString(p.value(k("admission_slot")).toString(p.value(k("slot")).toString()));
            if(!t.value(k("admission_slots")).toArray().isEmpty())slot=e.value(k("admission_slot")).toString();
            return {{k("known"),true},{k("requires_admitted_cast"),t.value(k("requires_admitted_cast")).toBool()},
                {k("requires_admitted_attack"),t.value(k("requires_admitted_attack")).toBool()},{k("slot"),slot}};
        }
    }
    return {{k("known"),false},{k("requires_admitted_cast"),false},{k("requires_admitted_attack"),false},{k("slot"),e.value(k("slot"))}};
}

QJsonObject chainMechanismInputs(const Catalog &c,const QJsonObject &scenario){
    const auto owned=actors(scenario);QJsonArray errors;if(owned.isEmpty())return report(false,errors);
    const auto source=scenario.value(k("condition_source")).toObject();const double start=scenario.value(k("start_time")).toDouble(0),end=start+scenario.value(k("horizon")).toDouble(8);
    if(source.value(k("kind"))!=k("caller_declared_scenario")||!token(source.value(k("source_id")))||source.value(k("patch"))!=c.patch||!c.combat||source.value(k("combat_revision"))!=c.combat->revision)
        error(errors,k("3049_condition_source_identity_patch_revision_required"));
    if(!number(source.value(k("valid_from")))||!number(source.value(k("valid_until")))||source.value(k("valid_from")).toDouble()>start||source.value(k("valid_until")).toDouble()<end||end<=start)
        error(errors,k("3049_condition_source_validity_must_cover_explicit_window"));
    QMap<QString,QString> teams;for(const auto &v:scenario.value(k("players")).toArray()){const auto p=v.toObject();teams[playerKey(p)]=p.value(k("team")).toString();}
    for(const auto &v:scenario.value(k("action_events")).toArray()){
        const auto e=v.toObject();const auto who=e.value(k("player_key")).toString();if(!owned.contains(who))continue;
        const auto id=e.value(k("event_id")).toString(),type=e.value(k("type")).toString();const auto facts=e.value(k("facts")).toObject();
        if(type==k("mechanic_observation")&&e.value(k("observation")).toObject().value(k("id"))==QJsonValue(3049))error(errors,k("3049_comparison_requires_native_action_receipts_not_injected_mechanic_observation"),id);
        const auto contract=chainActionContract(c,e,owned.value(who));
        if(type==k("skill_state")&&!contract.value(k("known")).toBool())error(errors,k("3049_skill_state_source_transition_required"),id);
        if(contract.value(k("requires_admitted_cast")).toBool()){
            if(facts.value(k("verified_impact_timing_contract"))!=QJsonValue(true)||!number(facts.value(k("primed_window_seconds")),1e-6,120))error(errors,k("3049_explicit_primed_window_required"),id);
            if(!number(facts.value(k("internal_cooldown_seconds")),0,120))error(errors,k("3049_explicit_internal_cooldown_required"),id);
        }
        if(!needsHit(c,e,owned.value(who)))continue;
        if(facts.value(k("verified_current_hit_armor_basis"))!=QJsonValue(true))error(errors,k("3049_current_hit_bonus_armor_contract_required"),id);
        const auto targets=facts.value(k("verified_impact_target_keys"));QSet<QString> seen;bool valid=facts.value(k("verified_impact_target_set"))==QJsonValue(true)&&targets.isArray()&&!targets.toArray().isEmpty()&&targets.toArray().size()<=5;
        for(const auto &target:targets.toArray()){const auto key=target.toString();valid&=token(target)&&!seen.contains(key)&&teams.contains(key)&&teams.value(key)!=teams.value(who);seen.insert(key);}
        if(!valid)error(errors,k("3049_actual_unique_enemy_target_set_required"),id);
    }
    return report(true,errors);
}

QJsonObject chainMechanismExecution(const Catalog &c,const QJsonObject &scenario,const QJsonObject &result){
    auto out=chainMechanismInputs(c,scenario);if(!out.value(k("required")).toBool()){out[k("decisions")]=QJsonArray{};out[k("deliveries")]=QJsonArray{};return out;}
    auto errors=out.value(k("errors")).toArray();const auto owned=actors(scenario);const auto trace=result.value(k("trace")).toArray();
    QJsonArray decisions,delivered;QSet<QString> primes,hits,coveredInputHits,checkedDeliveries;QMap<QString,QJsonObject> requirements;
    for(const auto &v:scenario.value(k("action_events")).toArray()){const auto e=v.toObject();if(owned.contains(e.value(k("player_key")).toString()))requirements[e.value(k("event_id")).toString()]=e;}
    for(const auto &value:trace){const auto row=value.toObject();const auto who=row.value(k("player_key")).toString();if(!owned.contains(who))continue;
        const auto id=row.value(k("event_id")).toString();const auto prime=row.value(k("armor_impact_prime")).toObject(),hit=row.value(k("armor_impact_hit")).toObject();
        const auto primeKey=prime.value(k("event_id")).toString(id+k(":impact_prime"));
        if(!prime.isEmpty()&&!primes.contains(primeKey)){
            const auto pid=primeKey;primes.insert(pid);
            const bool accepted=prime.value(k("accepted"))==QJsonValue(true),knownCooldown=!accepted&&nativeReason(prime.value(k("reason")),k("armor_impact_internal_cooldown"));
            if(!prime.value(k("accepted")).isBool()||(!accepted&&!knownCooldown))error(errors,k("3049_native_prime_missing_or_rejected"),id);
            decisions.append(QJsonObject{{k("kind"),k("prime")},{k("event_id"),pid},{k("player_key"),who},{k("accepted"),accepted},{k("reason"),prime.value(k("reason"))}});
        }
        if(hit.isEmpty()||hits.contains(id))continue;hits.insert(id);
        QString inputId;for(auto it=requirements.begin();it!=requirements.end();++it)if(it.value().value(k("player_key"))==who&&(id==it.key()||id.startsWith(it.key()+k(":")))&&it.key().size()>inputId.size())inputId=it.key();
        if(inputId.isEmpty()){error(errors,k("3049_native_hit_not_bound_to_explicit_event"),id);continue;}coveredInputHits.insert(inputId);
        const bool accepted=hit.value(k("accepted"))==QJsonValue(true),knownInactive=!accepted&&nativeReason(hit.value(k("reason")),k("armor_impact_not_primed_or_expired"));
        if(!hit.value(k("accepted")).isBool()||(!accepted&&!knownInactive))error(errors,k("3049_native_hit_missing_or_rejected"),id);
        QJsonArray actualTargets;QSet<QString> uniqueTargets;
        if(accepted){
            const auto requested=requirements.value(inputId).value(k("facts")).toObject().value(k("verified_impact_target_keys")).toArray(),plans=hit.value(k("deliveries")).toArray();
            if(plans.size()!=requested.size()||plans.isEmpty())error(errors,k("3049_native_delivery_target_coverage_incomplete"),id);
            for(const auto &planValue:plans){const auto plan=planValue.toObject();const auto effect=plan.value(k("effect_id")).toString(),target=plan.value(k("recipient_key")).toString();
                if(!requested.contains(target)||uniqueTargets.contains(target)||effect.isEmpty()||plan.value(k("source"))!=k("augment:3049:"))error(errors,k("3049_native_delivery_target_or_origin_mismatch"),id);
                uniqueTargets.insert(target);actualTargets.append(target);int deliveryCount=0,receiptCount=0;QString deliveryId;QJsonValue amount=QJsonValue::Null;
                for(const auto &deliveryValue:trace){const auto delivery=deliveryValue.toObject();if(delivery.value(k("_delivery")).toObject().value(k("effect_id"))!=effect||delivery.value(k("type"))!=k("effect_delivery"))continue;
                    if(delivery.value(k("accepted"))==QJsonValue(true)&&delivery.value(k("player_key"))==who&&delivery.value(k("target_key"))==target){++deliveryCount;deliveryId=delivery.value(k("event_id")).toString();amount=delivery.value(k("_delivery")).toObject().value(k("amount"));}}
                for(const auto &receiptValue:trace){const auto receipt=receiptValue.toObject();if(receipt.value(k("type"))==k("damage_receipt")&&receipt.value(k("event_id"))==deliveryId&&receipt.value(k("source_key"))==who&&receipt.value(k("target_key"))==target&&number(receipt.value(k("actual_damage")))&&number(receipt.value(k("shield_damage"))))++receiptCount;}
                if(deliveryCount!=1||receiptCount!=1||!number(amount)||checkedDeliveries.contains(deliveryId))error(errors,k("3049_actual_delivery_and_damage_receipt_required_once"),id);
                checkedDeliveries.insert(deliveryId);delivered.append(QJsonObject{{k("input_event_id"),inputId},{k("native_hit_event_id"),id},{k("effect_id"),effect},{k("delivery_event_id"),deliveryId},{k("target_key"),target},{k("raw_amount"),amount},{k("delivery_count"),deliveryCount},{k("damage_receipt_count"),receiptCount}});
            }
        }
        decisions.append(QJsonObject{{k("kind"),k("hit")},{k("event_id"),id},{k("input_event_id"),inputId},{k("player_key"),who},{k("accepted"),accepted},{k("reason"),hit.value(k("reason"))},{k("delivered_targets"),actualTargets}});
    }
    for(auto it=requirements.begin();it!=requirements.end();++it){const auto e=it.value();
        const auto actor=owned.value(e.value(k("player_key")).toString());
        if(chainActionContract(c,e,actor).value(k("requires_admitted_cast")).toBool()&&!primes.contains(it.key()+k(":impact_prime")))error(errors,k("3049_required_native_prime_receipt_absent"),it.key());
        if(needsHit(c,e,actor)&&!coveredInputHits.contains(it.key()))error(errors,k("3049_required_native_hit_receipt_absent"),it.key());
    }
    for(const auto &v:trace){const auto row=v.toObject();if(row.value(k("type"))==k("effect_delivery")&&row.value(k("_delivery")).toObject().value(k("source"))==k("augment:3049:")&&!checkedDeliveries.contains(row.value(k("event_id")).toString()))error(errors,k("3049_unbound_or_extra_native_delivery"),row.value(k("event_id")).toString());}
    out[k("complete")]=errors.isEmpty();out[k("errors")]=errors;out[k("decisions")]=decisions;out[k("deliveries")]=delivered;out[k("numeric_amounts_in_decision_signature")]=false;return out;
}
