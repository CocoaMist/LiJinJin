#include "augment_3116.h"
#include <QRegularExpression>
#include <cmath>

namespace {
QString k(const char *s){return QString::fromLatin1(s);}
const QString prefix=QStringLiteral("augment:3116:");
bool id(const QJsonValue &v){return v.isString()&&!v.toString().isEmpty()&&v.toString().size()<=256;}
bool num(const QJsonValue &v,double lo=0,double hi=1e12){return v.isDouble()&&std::isfinite(v.toDouble())&&v.toDouble()>=lo&&v.toDouble()<=hi;}
bool no(QStringList *out,const char *why){if(out)out->append(k(why));return false;}
QString identity(const QString &kind,const QJsonObject &r){return prefix+kind+k(":")+r.value(k("actor_key")).toString()+k("|")+r.value(k("target_key")).toString()+k("|")+r.value(k("cycle_id")).toString();}
std::optional<double> calc(const CombatEntry &e,const char *name,const CombatContext &ctx,const CombatStats &stats){return compileCombatFormula(e.calculations.value(k(name)).toObject(),ctx).evaluate(ctx,stats);}
double value(const CombatEntry &e,const char *name){const auto v=e.values.value(k(name));return v.isArray()?v.toArray().first().toDouble(-1):v.toDouble(-1);}
QJsonObject effect(const char *op,double amount,const QString &target,const QJsonObject &r,double at){
 return {{k("op"),k(op)},{k("key"),k(op)==k("damage")?k("magic"):k("health")},{k("value"),amount},
  {k("source"),prefix},{k("target"),target.isEmpty()?k("self"):k("enemy")},{k("target_id"),target},
  {k("start"),at},{k("expires"),-1},{k("causal_3116"),r}};
}
bool damageBounds(const QJsonObject &r){
 for(const auto &key:{k("raw_damage"),k("post_mitigation_damage"),k("actual_hp_damage"),k("shield_damage"),k("target_hp_before")})if(!num(r.value(key)))return false;
 return r.value(k("actual_hp_damage")).toDouble()+r.value(k("shield_damage")).toDouble()<=r.value(k("post_mitigation_damage")).toDouble()+1e-7&&
  r.value(k("actual_hp_damage")).toDouble()<=r.value(k("target_hp_before")).toDouble()+1e-7;
}
}

bool executeAugment3116(const CombatEntry &entry,const CombatContext &ctx,const CombatStats &stats,
 const QJsonObject &obs,MechanicLedger &ledger,QStringList *missing,bool nativeReceipt){
 static const QRegularExpression digest(k("^[0-9a-f]{64}$"));
 if(entry.id!=3116||entry.mechanismStateV1.value(k("review_id"))!=k("augment_3116_causal_v1")||
  !digest.match(entry.mechanismStateV1.value(k("review_sha256")).toString()).hasMatch()||
  !digest.match(entry.mechanismStateV1.value(k("source_projection_sha256")).toString()).hasMatch())return no(missing,"3116_source_review_required");
 if(obs.value(k("source"))!=k("augment")||obs.value(k("id")).toInt()!=3116||!ledger.augments.contains(3116))return no(missing,"3116_source_not_owned");
 if(!id(obs.value(k("event_id")))||!num(obs.value(k("time")),0,1e7)||obs.value(k("time")).toDouble()<ledger.lastTime)return no(missing,"3116_event_identity_or_time_invalid");
 const auto event=obs.value(k("event")).toString(),eventId=obs.value(k("event_id")).toString();const double at=obs.value(k("time")).toDouble();
 const auto facts=obs.value(k("facts")).toObject();auto receipt=obs.value(k("receipt")).toObject();
 auto staged=ledger;const auto occurrence=prefix+k("event:")+eventId;
 if(staged.consumed.contains(occurrence))return no(missing,"3116_duplicate_event");
 if(!nativeReceipt){
  if(obs.value(k("observation_mode"))!=k("trusted_external")||receipt.value(k("verified"))!=QJsonValue(true)||
   !id(receipt.value(k("provider")))||!id(receipt.value(k("observation_id")))||receipt.value(k("augment_id")).toInt()!=3116||
   receipt.value(k("event"))!=event||receipt.value(k("time"))!=obs.value(k("time")))return no(missing,"3116_trusted_external_receipt_required");
 }
 for(const auto &key:{k("actor_key"),k("target_key"),k("actor_life_id"),k("target_life_id"),k("cycle_id")})if(!id(receipt.value(key)))return no(missing,"3116_receipt_participant_or_cycle_required");
 if(receipt.value(k("actor_key"))!=obs.value(k("actor_key"))||receipt.value(k("actor_key"))==receipt.value(k("target_key"))||receipt.value(k("target_key"))!=obs.value(k("target"))||
  receipt.value(k("target_type"))!=k("champion"))return no(missing,"3116_receipt_target_mismatch");
 const auto actor=receipt.value(k("actor_key")).toString(),target=receipt.value(k("target_key")).toString();
 const auto consumeKey=identity(k("consumed_cycle"),receipt),healKey=identity(k("healed_cycle"),receipt);
 if(event==k("native_qualified_hit")){
  if(!nativeReceipt||receipt.value(k("verified_contact"))!=QJsonValue(true)||receipt.value(k("time"))!=obs.value(k("time"))||
   !id(receipt.value(k("hit_event_id")))||!id(receipt.value(k("parent_event_id")))||!id(receipt.value(k("cast_instance_id")))||
   !id(receipt.value(k("origin_ability_id")))||!QStringList{k("basic_attack"),k("bound_spell")}.contains(receipt.value(k("application_origin_family")).toString()))return no(missing,"3116_native_qualified_contact_required");
  if(value(entry,"Stacks_Max")!=5||value(entry,"Stack_Duration")!=5||value(entry,"Attack_Stacks")!=1||value(entry,"Ability_Stacks")!=1)return no(missing,"3116_source_stack_parameters_changed");
  if(facts.value(k("verified_3116_stack_refresh_policy"))!=QJsonValue(true)||facts.value(k("stack_refresh_policy"))!=k("refresh_target_on_qualified_hit"))return no(missing,"3116_stack_refresh_policy_unverified");
  const auto policy=facts.value(k("3116_policy_provenance")).toObject();
  if(policy.value(k("kind"))!=k("hypothetical_scenario")||!id(policy.value(k("policy_id")))||policy.value(k("live_certified"))!=QJsonValue(false))return no(missing,"3116_hypothetical_policy_provenance_required");
  receipt[k("policy_provenance")]=policy;
  const auto hitKey=prefix+k("hit:")+actor+k("|")+receipt.value(k("hit_event_id")).toString();
  if(staged.consumed.contains(hitKey))return no(missing,"3116_duplicate_qualified_hit");
  const auto stackKey=prefix+k("target:")+actor+k("|")+target;auto stack=staged.values.value(stackKey).toObject();
  int before=stack.value(k("stacks")).toInt();
  if(stack.isEmpty()||at>=stack.value(k("expires_at")).toDouble()||stack.value(k("target_life_id"))!=receipt.value(k("target_life_id"))||stack.value(k("actor_life_id"))!=receipt.value(k("actor_life_id"))) {before=0;stack={{k("actor_key"),actor},{k("target_key"),target},{k("actor_life_id"),receipt.value(k("actor_life_id"))},{k("target_life_id"),receipt.value(k("target_life_id"))},{k("cycle_id"),receipt.value(k("cycle_id"))},{k("stacks"),0}};}
  receipt[k("cycle_id")]=stack.value(k("cycle_id"));receipt[k("review_sha256")]=entry.mechanismStateV1.value(k("review_sha256"));
  if(before<5){
   const auto base=calc(entry,"{c75ec8fe}",ctx,stats),extra=calc(entry,"{25caecdf}",ctx,stats);if(!base||!extra)return no(missing,"3116_stack_hit_formula_unavailable");
   const double amount=*base+before**extra;if(!std::isfinite(amount)||amount<0)return no(missing,"3116_invalid_stack_hit_amount");
   receipt[k("kind")]=k("stack_hit");receipt[k("previous_target_stacks")]=before;staged.effects.append(effect("damage",amount,target,receipt,at));
   stack[k("stacks")]=before+1;stack[k("expires_at")]=at+value(entry,"Stack_Duration");
  }else{
   if(facts.value(k("verified_3116_consume_order"))!=QJsonValue(true)||facts.value(k("consume_order"))!=k("consume_preexisting_max_without_same_hit_reapply")||
    facts.value(k("missing_hp_snapshot"))!=k("pre_simultaneous_qualified_hit")||!num(facts.value(k("target_missing_hp"))))return no(missing,"3116_consume_edge_or_snapshot_unverified");
   const auto cycle=identity(k("consumed_cycle"),receipt);if(staged.consumed.contains(cycle))return no(missing,"3116_cycle_already_consumed");
   const auto ratio=calc(entry,"{0c0f49f5}",ctx,stats);if(!ratio)return no(missing,"3116_consume_formula_unavailable");
   const double raw=facts.value(k("target_missing_hp")).toDouble()**ratio;if(!std::isfinite(raw)||raw<0)return no(missing,"3116_invalid_consume_amount");
   receipt[k("kind")]=k("consume");receipt[k("raw_damage")]=raw;receipt[k("damage_packet_id")]=eventId+k(":consume");
   receipt[k("heal_basis_verified")]=facts.value(k("verified_3116_heal_damage_basis"))==QJsonValue(true)&&facts.value(k("heal_damage_basis"))==k("actual_hp_damage");
   staged.values[identity(k("pending_consume"),receipt)]=receipt;staged.consumed.insert(cycle);
   staged.effects.append(effect("damage",raw,target,receipt,at));stack[k("stacks")]=0;stack[k("expires_at")]=at;
  }
  stack[k("last_hit_at")]=at;stack[k("review_sha256")]=entry.mechanismStateV1.value(k("review_sha256"));staged.values[stackKey]=stack;staged.consumed.insert(hitKey);
 }else if(event==k("champion_stack_hit")){
  if(nativeReceipt)return no(missing,"3116_external_observation_mode_required");
  const auto n=facts.value(k("previous_target_stacks"));if(facts.value(k("enemy_champion"))!=QJsonValue(true)||!num(n,0,4)||std::floor(n.toDouble())!=n.toDouble()||!id(receipt.value(k("hit_event_id"))))return no(missing,"3116_external_stack_observation_invalid");
  const auto key=prefix+k("external_hit:")+actor+k("|")+receipt.value(k("hit_event_id")).toString();if(staged.consumed.contains(key))return no(missing,"3116_duplicate_qualified_hit");
  const auto base=calc(entry,"{c75ec8fe}",ctx,stats),extra=calc(entry,"{25caecdf}",ctx,stats);if(!base||!extra)return no(missing,"3116_stack_hit_formula_unavailable");
  receipt[k("kind")]=k("external_stack_hit");staged.effects.append(effect("damage",*base+n.toDouble()**extra,target,receipt,at));staged.consumed.insert(key);
 }else if(event==k("target_stacks_consumed")){
  if(nativeReceipt||facts.value(k("target_stacks_before"))!=QJsonValue(5)||!num(facts.value(k("target_missing_hp"))))return no(missing,"3116_external_consume_observation_invalid");
  if(staged.consumed.contains(consumeKey))return no(missing,"3116_cycle_already_consumed");
  const auto ratio=calc(entry,"{0c0f49f5}",ctx,stats);if(!ratio)return no(missing,"3116_consume_formula_unavailable");
  receipt[k("kind")]=k("external_consume");staged.effects.append(effect("damage",facts.value(k("target_missing_hp")).toDouble()**ratio,target,receipt,at));staged.consumed.insert(consumeKey);
 }else if(event==k("consume_damage_resolved")){
  if(facts.value(k("verified_consume_damage_identity"))!=QJsonValue(true)||!id(receipt.value(k("damage_receipt_id")))||!id(receipt.value(k("damage_packet_id")))||
   receipt.value(k("damage_source"))!=k("augment:3116:consume")||!damageBounds(receipt)||facts.value(k("actual_damage_dealt"))!=receipt.value(k("actual_hp_damage")))return no(missing,"3116_damage_receipt_identity_or_bounds_invalid");
  for(const auto &key:{k("hit_event_id"),k("parent_event_id"),k("cast_instance_id"),k("origin_ability_id"),k("damage_event_id")})if(!id(receipt.value(key)))return no(missing,"3116_damage_source_provenance_required");
  if(!num(receipt.value(k("damage_time")),0,at)||receipt.value(k("heal_damage_basis"))!=k("actual_hp_damage"))return no(missing,"3116_damage_time_or_heal_basis_invalid");
  const auto packetKey=prefix+k("healed_packet:")+actor+k("|")+receipt.value(k("damage_packet_id")).toString();
  if(staged.consumed.contains(healKey)||staged.consumed.contains(packetKey)||staged.consumed.contains(prefix+k("healed_damage:")+receipt.value(k("damage_receipt_id")).toString()))return no(missing,"3116_consume_damage_already_healed");
  if(nativeReceipt){
   const auto pending=staged.values.value(identity(k("pending_consume"),receipt)).toObject();
   if(pending.isEmpty()||pending.value(k("heal_basis_verified"))!=QJsonValue(true)||pending.value(k("damage_packet_id"))!=receipt.value(k("damage_packet_id"))||
    pending.value(k("hit_event_id"))!=receipt.value(k("hit_event_id"))||pending.value(k("cast_instance_id"))!=receipt.value(k("cast_instance_id"))||
    pending.value(k("raw_damage"))!=receipt.value(k("raw_damage"))||pending.value(k("time"))!=receipt.value(k("time")))return no(missing,"3116_native_consume_receipt_unowned_or_basis_unverified");
   for(const auto &key:{k("actor_life_id"),k("target_life_id"),k("parent_event_id"),k("origin_ability_id"),k("application_origin_family")})if(pending.value(key)!=receipt.value(key))return no(missing,"3116_native_consume_provenance_mismatch");
   if(pending.value(k("time"))!=receipt.value(k("damage_time")))return no(missing,"3116_native_consume_damage_time_mismatch");
  }
  const auto ratio=calc(entry,"{c1fb65e1}",ctx,stats);if(!ratio)return no(missing,"3116_heal_formula_unavailable");
  receipt[k("kind")]=nativeReceipt?k("native_consume_heal"):k("trusted_external_consume_heal");staged.effects.append(effect("heal",receipt.value(k("actual_hp_damage")).toDouble()**ratio,QString(),receipt,at));
  staged.consumed.insert(healKey);staged.consumed.insert(prefix+k("healed_damage:")+receipt.value(k("damage_receipt_id")).toString());
  staged.consumed.insert(packetKey);staged.consumed.insert(consumeKey);
 }else return no(missing,"3116_event_not_supported");
 staged.lastTime=at;staged.consumed.insert(occurrence);ledger=std::move(staged);return true;
}

bool validateAugment3116State(const MechanicLedger &ledger,QString *error){
 for(auto it=ledger.values.begin();it!=ledger.values.end();++it)if(it.key().startsWith(prefix+k("target:"))){
  const auto row=it.value().toObject();const auto n=row.value(k("stacks"));
  if(!ledger.augments.contains(3116)||!id(row.value(k("actor_key")))||!id(row.value(k("target_key")))||!id(row.value(k("cycle_id")))||
   !num(n,0,5)||std::floor(n.toDouble())!=n.toDouble()||!num(row.value(k("expires_at")),0,1e7)||!num(row.value(k("last_hit_at")),0,ledger.lastTime)){
   if(error)*error=k("3116_checkpoint_stack_state_invalid");return false;
  }
 }
 return true;
}
void advanceAugment3116State(MechanicLedger &ledger,double at){
 for(auto it=ledger.values.begin();it!=ledger.values.end();++it)if(it.key().startsWith(prefix+k("target:"))){
  auto row=it.value().toObject();if(row.value(k("stacks")).toInt()>0&&at>=row.value(k("expires_at")).toDouble()){
   row[k("stacks")]=0;row[k("expired_at")]=row.value(k("expires_at"));it.value()=row;
  }
 }
}
