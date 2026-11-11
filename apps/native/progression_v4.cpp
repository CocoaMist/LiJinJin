#include "progression_v4.h"
#include "teamfight_sandbox.h"
#include "purchase_planner.h"
#include "hextech_mechanism_state.h"
#include "hextech_conversion.h"
#include "augment_3116.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>

namespace {
QString k(const char *s){return QString::fromLatin1(s);}
bool fail(QString *error,const QString &reason){if(error)*error=reason;return false;}
bool number(const QJsonValue &v,double low=0,double high=1e12){return v.isDouble()&&std::isfinite(v.toDouble())&&v.toDouble()>=low&&v.toDouble()<=high;}
QJsonArray strings(const QSet<QString> &set){auto values=set.values();std::sort(values.begin(),values.end());return QJsonArray::fromStringList(values);}
QJsonArray ids(const QSet<int> &set){auto values=set.values();std::sort(values.begin(),values.end());QJsonArray out;for(int id:values)out.append(id);return out;}
QJsonArray physicalIds(QList<int> values){std::sort(values.begin(),values.end());QJsonArray out;for(int id:values)out.append(id);return out;}
QSet<int> ownedSources(const QList<int> &items){QSet<int> out;for(int id:items)out.insert(id);return out;}
bool sameMultiset(QList<int> left,QList<int> right){std::sort(left.begin(),left.end());std::sort(right.begin(),right.end());return left==right;}
bool readPhysicalIds(const QJsonValue &value,QList<int> &out){if(!value.isArray()||value.toArray().size()>6)return false;for(const auto &v:value.toArray()){if(!number(v,1,2147483647)||v.toDouble()!=v.toInt())return false;out.append(v.toInt());}return true;}
QJsonArray effectDifference(const QJsonArray &before,const QJsonArray &after){QJsonArray out,unmatched=before;for(const auto &effect:after){qsizetype match=-1;for(qsizetype i=0;i<unmatched.size();++i)if(unmatched[i]==effect){match=i;break;}if(match>=0)unmatched.removeAt(match);else out.append(effect);}return out;}
QList<int> ordered(const QSet<int> &set){auto out=set.values();std::sort(out.begin(),out.end());return out;}
QJsonObject doubles(const QMap<QString,double> &map){QJsonObject out;for(auto it=map.cbegin();it!=map.cend();++it)out[it.key()]=it.value();return out;}
QJsonObject integers(const QMap<QString,int> &map){QJsonObject out;for(auto it=map.cbegin();it!=map.cend();++it)out[it.key()]=it.value();return out;}
bool readStats(const QJsonValue &value,CombatStats &out){if(!value.isObject())return false;const auto o=value.toObject();for(auto it=o.begin();it!=o.end();++it){auto stat=combatStat(it.key());if(!stat||!number(it.value(),-1e12))return false;out[*stat]=it.value().toDouble();}return true;}
QJsonObject packContext(const CombatContext &c){return {
    {k("champion_id"),c.championId},{k("level"),c.level},{k("ranks"),integers(c.ranks)},{k("counters"),doubles(c.counters)},{k("conditions"),strings(c.conditions)},
    {k("observed_offset"),c.observedOffset.json()},{k("observed_input_offset"),c.observedInputOffset.json()},{k("event_stats"),c.eventStats.json()},{k("permanent_event_stats"),c.permanentEventStats.json()},
    {k("target_armor"),c.targetArmor},{k("target_mr"),c.targetMr},{k("target_hp"),c.targetHp},{k("target_ms"),c.targetMs},{k("horizon"),c.horizon},
    {k("attack_speed_cap"),c.attackSpeedCap},{k("ability_haste_cap"),c.abilityHasteCap},{k("overflow_crit_ad"),c.overflowCritAd},{k("overflow_crit_ap"),c.overflowCritAp}};}
bool unpackContext(const CombatDatabase &db,const QJsonObject &o,CombatContext &out){
    const int id=o.value(k("champion_id")).toInt(),level=o.value(k("level")).toInt();if(!db.champions.contains(id)||level<1||level>18)return false;out=db.context(id,level);
    const auto ranks=o.value(k("ranks")).toObject();for(auto it=ranks.begin();it!=ranks.end();++it){if(!number(it.value(),0,6)||it.value().toDouble()!=it.value().toInt())return false;out.ranks[it.key()]=it.value().toInt();}
    if(!sandboxSkillRanksLegal(db,id,level,out.ranks))return false;
    const auto counters=o.value(k("counters")).toObject();for(auto it=counters.begin();it!=counters.end();++it){if(!number(it.value(),-1e12))return false;out.counters[it.key()]=it.value().toDouble();}
    for(const auto &v:o.value(k("conditions")).toArray()){if(!v.isString()||v.toString().isEmpty())return false;out.conditions.insert(v.toString());}
    if(!readStats(o.value(k("observed_offset")),out.observedOffset)||!readStats(o.value(k("observed_input_offset")),out.observedInputOffset)||!readStats(o.value(k("event_stats")),out.eventStats)||!readStats(o.value(k("permanent_event_stats")),out.permanentEventStats))return false;
    auto get=[&](const char *key,double &field){const auto v=o.value(k(key));if(!number(v,-1e12))return false;field=v.toDouble();return true;};
    return get("target_armor",out.targetArmor)&&get("target_mr",out.targetMr)&&get("target_hp",out.targetHp)&&get("target_ms",out.targetMs)&&get("horizon",out.horizon)&&get("attack_speed_cap",out.attackSpeedCap)&&get("ability_haste_cap",out.abilityHasteCap)&&get("overflow_crit_ad",out.overflowCritAd)&&get("overflow_crit_ap",out.overflowCritAp);
}
QSet<QString> stackKeys(const CombatDatabase &db,const CombatContext &ctx,const MechanicLedger &ledger){
    QSet<QString> out;auto append=[&](const QJsonArray &a){for(const auto &v:a)if(v.isString()&&!v.toString().isEmpty())out.insert(v.toString());};
    append(db.champions.value(ctx.championId).value(k("growth_stack_inputs")).toArray());
    auto entries=[&](const QMap<int,CombatEntry> &pool,const QSet<int> &owned){for(int id:owned){const auto it=pool.constFind(id);if(it==pool.cend())continue;append(it->growthStackInputs);for(const auto &v:it->effects){const auto e=v.toObject();if(e.value(k("op"))==k("stack")&&!e.value(k("counter")).toString().isEmpty())out.insert(e.value(k("counter")).toString());}}};
    entries(db.items,ledger.inventory);entries(db.augments,ledger.augments);return out;
}
bool rankRequirements(const CombatDatabase &db,const CombatContext &ctx,const QString &slot,int rank,const QJsonObject &event,QString *error){
    const auto rules=db.champions.value(ctx.championId).value(k("progression_rules_v4")).toObject();const auto slotRules=rules.value(k("slots")).toObject();const auto rows=slotRules.value(slot).toObject().value(k("ranks")).toArray();
    if(rows.isEmpty())return true; // The legacy guard and explicit observation still apply.
    if(rank>rows.size())return fail(error,k("rank_not_in_source_requirements"));
    for(const auto &v:rows[rank-1].toObject().value(k("requirements")).toArray()){
        const auto requirement=v.toObject();const auto type=requirement.value(k("__type")).toString();
        if(type==k("CharacterLevelRequirement")){
            if(!requirement.value(k("mLevel")).isDouble())return fail(error,k("source_level_requirement_unknown"));
            if(ctx.level<requirement.value(k("mLevel")).toInt())return fail(error,k("source_level_requirement_not_met"));
        } else if(type==k("HasBuffRequirement")){
            const auto name=requirement.value(k("mBuffName")).toString();if(name.isEmpty()||!event.value(k("verified_source_requirements")).toBool())return fail(error,k("source_buff_requirement_needs_verified_observation"));
        } else if(type!=k("HasSkillPointRequirement")&&!event.value(k("verified_source_requirements")).toBool())return fail(error,k("source_rank_requirement_unknown"));
    }
    return true;
}
bool item4005Attachment(const QJsonObject &effect){
    // The reviewed assignment is a property of the equipped Imperial Mandate,
    // unlike earned Heartsteel health. No other lifetime is inferred here.
    return effect.value(k("source"))==k("item:4005:")&&effect.value(k("op"))==k("buff")&&effect.value(k("target"))==k("self")&&!effect.value(k("growth_baseline")).toBool()&&effect.value(k("expires")).toDouble(-1)<0&&QStringList{k("q_haste"),k("w_haste"),k("e_haste"),k("r_haste")}.contains(effect.value(k("key")).toString());
}
QString checksum(QJsonObject o){o.remove(k("integrity_sha256"));return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(o).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());}
}

bool ProgressionLedger::load(const CombatDatabase &db,const CombatContext &context,const QList<int> &items,const QList<int> &augments,double time,QString *error){
    if(!std::isfinite(time)||time<0||!db.champions.contains(context.championId)||context.level<1||context.level>18)return fail(error,k("invalid_progression_baseline"));
    if(!sandboxSkillRanksLegal(db,context.championId,context.level,context.ranks))return fail(error,k("invalid_baseline_skill_ranks"));
    MechanicLedger next;QList<int> physical;for(int id:items){if(!compatibleEquipment(db,physical,id,context.championId,context.level))return fail(error,k("unknown_duplicate_or_incompatible_baseline_item"));physical.append(id);next.inventory.insert(id);}for(int id:augments){if(!db.augments.contains(id)||next.augments.contains(id))return fail(error,k("unknown_or_duplicate_baseline_augment"));next.augments.insert(id);}
    if(!evaluateCombat(db,context,items,augments).valid)return fail(error,k("invalid_baseline_equipment"));
    restoreMechanicGrowthModifiers(db,context,next);
    std::sort(physical.begin(),physical.end());context_=context;ledger_=next;inventoryItems_=physical;revision_=db.revision;time_=time;loaded_=true;consumedIds_.clear();decision_={};
    const double gameTime=context.counters.value(k("observed_game_time"),-1);gameTimeKnown_=std::isfinite(gameTime)&&gameTime>=0;gameTimeOffset_=gameTimeKnown_?gameTime-time:0;return true;
}
bool ProgressionLedger::advance(double time,QString *error){if(!loaded_||!std::isfinite(time)||time<time_)return fail(error,k("invalid_or_backward_progression_time"));advanceAugment3116State(ledger_,time);time_=time;return true;}
CombatContext ProgressionLedger::contextAt(double time) const {
    auto out=context_;if(!loaded_||!std::isfinite(time)||time<time_)return out;
    auto active=ledger_;if(!ledger_.inventory.contains(4005))for(qsizetype i=active.effects.size()-1;i>=0;--i)if(item4005Attachment(active.effects[i].toObject()))active.effects.removeAt(i);
    applyMechanicLedgerContext(active,time,out);
    if(gameTimeKnown_)out.counters[k("observed_game_time")]=time+gameTimeOffset_;else out.counters.remove(k("observed_game_time"));return out;
}
QList<int> ProgressionLedger::items() const{return inventoryItems_;}
QList<int> ProgressionLedger::augments() const{return ordered(ledger_.augments);}
double ProgressionLedger::nextBoundary(double at,double fallback) const {
    double next=fallback;for(const auto &v:ledger_.effects){const auto effect=v.toObject();for(const auto &field:{k("start"),k("expires")}){const auto value=effect.value(field);if(value.isDouble()&&std::isfinite(value.toDouble())&&value.toDouble()>at+1e-9&&value.toDouble()<next)next=value.toDouble();}}
    for(auto it=ledger_.values.begin();it!=ledger_.values.end();++it)if(it.key().startsWith(k("augment:3116:target:"))){const auto row=it.value().toObject();const double expiry=row.value(k("expires_at")).toDouble();if(row.value(k("stacks")).toInt()>0&&expiry>at+1e-9&&expiry<next)next=expiry;}return next;
}
bool ProgressionLedger::apply(const CombatDatabase &db,const QJsonObject &event,QString *error,const CombatContext *executionContext,bool native3116Receipt){
    if(!loaded_||revision_!=db.revision)return fail(error,k("progression_revision_mismatch"));
    const auto id=event.value(k("event_id")).toString(),type=event.value(k("type")).toString();const auto t=event.value(k("time"));
    if(id.isEmpty()||id.size()>512||consumedIds_.contains(id)||!number(t)||t.toDouble()<time_)return fail(error,k("duplicate_or_invalid_progression_event"));
    if(event.contains(k("event_stats"))||event.contains(k("permanent_event_stats")))return fail(error,k("unbound_event_stats_forbidden"));
    auto staged=*this;const double time=t.toDouble();auto &ctx=staged.context_;auto &ledger=staged.ledger_;const auto beforeEffects=ledger.effects;
    QJsonArray unknowns;QJsonObject detail;
    if(type==k("level_up")){
        const auto value=event.value(k("level"));if(!number(value,1,18)||value.toDouble()!=value.toInt()||value.toInt()!=ctx.level+1)return fail(error,k("level_up_must_advance_exactly_one_level"));
        const auto source=db.context(ctx.championId,value.toInt());ctx.level=source.level;ctx.base=source.base;ctx.baseAs=source.baseAs;ctx.asRatio=source.asRatio;ctx.ranged=source.ranged;ctx.usesMana=source.usesMana;
        detail={{k("new_level"),ctx.level},{k("resources_refilled"),false},{k("cooldowns_reset"),false},{k("health_policy"),k("preserve_absolute_current_health_clamp")}};
    } else if(type==k("skill_allocate")){
        const auto slot=event.value(k("slot")).toString();const auto value=event.value(k("rank"));
        if(!event.value(k("verified_allocation")).toBool()||!QStringList{k("Q"),k("W"),k("E"),k("R")}.contains(slot)||!number(value,1,6)||value.toDouble()!=value.toInt()||value.toInt()!=ctx.ranks.value(slot)+1)return fail(error,k("skill_allocation_needs_verified_next_rank"));
        auto ranks=ctx.ranks;ranks[slot]=value.toInt();QStringList missing;if(!sandboxSkillRanksLegal(db,ctx.championId,ctx.level,ranks,&missing))return fail(error,k("skill_rank_points_or_unlock_level_invalid"));
        if(!rankRequirements(db,ctx,slot,value.toInt(),event,error))return false;
        for(const auto &u:missing)unknowns.append(u);if(db.champions.value(ctx.championId).value(k("progression_rules_v4")).toObject().value(k("rank_requirements_status"))!=k("explicit_source_overrides"))unknowns.append(k("skill_rank_gate_uses_scenario_guard_and_verified_allocation"));
        ctx.ranks=ranks;detail={{k("slot"),slot},{k("rank"),value},{k("cooldowns_reset"),false},{k("charges_refilled"),false}};
    } else if(type==k("augment_select")){
        const auto value=event.value(k("augment_id")),verified=event.value(k("verified_selection")),player=event.value(k("player_key"));
        if(!verified.isBool()||!verified.toBool()||!player.isString()||player.toString().isEmpty()||player.toString().size()>512||!number(value,1,2147483647)||value.toDouble()!=value.toInt())return fail(error,k("augment_selection_requires_verified_choice"));
        const auto rawOffers=event.value(k("offered_augment_ids"));
        if(!rawOffers.isArray()||rawOffers.toArray().isEmpty()||rawOffers.toArray().size()>3)return fail(error,k("augment_offers_must_be_one_to_three"));
        QSet<int> offers;
        for(const auto &offer:rawOffers.toArray()){
            if(!number(offer,1,2147483647)||offer.toDouble()!=offer.toInt())return fail(error,k("augment_offer_unknown_duplicate_owned_or_level_invalid"));
            const int candidate=offer.toInt();const auto entry=db.augments.constFind(candidate);
            if(entry==db.augments.cend()||offers.contains(candidate)||ledger.augments.contains(candidate)||ctx.level<entry->requiredLevel)return fail(error,k("augment_offer_unknown_duplicate_owned_or_level_invalid"));
            offers.insert(candidate);
        }
        const int augment=value.toInt();if(!offers.contains(augment))return fail(error,k("augment_selection_not_offered"));
        if(ledger.augments.size()>=6)return fail(error,k("augment_selection_limit_reached"));
        // Admission to an ordinary offer is checked against Catalog by the
        // continuous executor. This transaction adds only an owned source;
        // passive numeric effects are evaluated normally, without replaying
        // any historical hits, quest rewards or resource restoration.
        ledger.augments.insert(augment);
        detail={{k("augment_id"),augment},{k("offered_augment_ids"),rawOffers},{k("selection_fact"),k("explicit_verified_scenario_choice")},{k("selection_economy"),k("not_inferred")},{k("historical_stacks_awarded"),false},{k("resources_refilled"),false},{k("cooldowns_reset"),false},{k("health_policy"),k("preserve_absolute_current_health_clamp")}};
        unknowns.append(k("augment_trigger_rewards_and_growth_require_explicit_observations"));
    } else if(type==k("observed_stack")){
        const auto key=event.value(k("counter")).toString();const auto value=event.value(k("value"));if(!event.value(k("verified_observation")).toBool()||!stackKeys(db,ctx,ledger).contains(key)||!number(value,0,1e9))return fail(error,k("stack_needs_source_bound_observation"));
        double absorbedGrowth=0;
        if(key==k("item:3084:permanent_hp")){
            // This is an absolute observed total, already including all prior
            // ordinary or tripled gains and any completed quest reward.
            for(qsizetype n=ledger.effects.size()-1;n>=0;--n){const auto effect=ledger.effects[n].toObject();const auto source=effect.value(k("source")).toString();
                if((source==k("item:3084:")||source==k("augment:1152:"))&&effect.value(k("op"))==k("buff")&&effect.value(k("key"))==k("hp")&&effect.value(k("target"))==k("self")&&effect.value(k("growth_baseline")).toBool()&&effect.value(k("expires")).toDouble(-1)<0){absorbedGrowth+=effect.value(k("value")).toDouble();ledger.effects.removeAt(n);}}
            ledger.values[k("item:3084:replay_permanent_hp")]=0;
        }
        ctx.counters[key]=value.toDouble();detail={{k("counter"),key},{k("observed_value"),value},{k("future_stacks_predicted"),0},{k("absorbed_replayed_permanent_hp"),absorbedGrowth}};
    } else if(type==k("purchase")||type==k("sell")){
        const auto value=event.value(k("item_id")),verified=event.value(k("verified_transaction"));if(!verified.isBool()||!verified.toBool()||!number(value,1,2147483647)||value.toDouble()!=value.toInt())return fail(error,k("purchase_requires_observed_transaction"));const int item=value.toInt();
        const auto inventoryBefore=staged.items();QList<int> consumed;QJsonValue cost(QJsonValue::Null);
        const int lockedItem=hextech40LockedItem(ledger);
        if(type==k("sell")&&hextech40ConversionLocksItem(ledger,item,time))return fail(error,k("conversion_source_item_removal_requires_expiry"));
        if(type==k("sell")&&item==lockedItem)return fail(error,k("destiny_locked_item_cannot_be_sold"));
        if(type==k("purchase")){
            if(event.contains(k("consume_item_ids"))){
                if(!readPhysicalIds(event.value(k("consume_item_ids")),consumed))return fail(error,k("purchase_consumption_requires_item_multiset"));
                for(int source:consumed)if(hextech40ConversionLocksItem(ledger,source,time))return fail(error,k("conversion_source_item_transform_unreviewed"));
                if(lockedItem>0&&consumed.contains(lockedItem))return fail(error,k("destiny_locked_item_transform_identity_unreviewed"));
                // A caller may keep a useful component and pay the remaining
                // price instead. Validate only the explicitly surrendered
                // material multiset; the shop quote chooses defaults upstream.
                const auto plan=validateObservedPurchaseRecipe(db,ctx.championId,ctx.level,consumed,item);
                if(!plan.value(k("recipe_valid")).toBool())return fail(error,k("purchase_recipe_invalid:")+plan.value(k("reason")).toString());
                QList<int> expected;if(!readPhysicalIds(plan.value(k("consume_item_ids")),expected)||!sameMultiset(consumed,expected))return fail(error,k("purchase_consumption_does_not_match_recipe"));
                auto after=inventoryBefore;for(int component:consumed)if(!after.removeOne(component))return fail(error,k("purchase_material_not_owned"));
                if(!compatibleEquipment(db,after,item,ctx.championId,ctx.level))return fail(error,k("incompatible_purchase"));
                after.append(item);staged.inventoryItems_=after;cost=plan.value(k("remaining_cost"));
            }else {
                // Legacy observed purchases remain append-only; an upgrade
                // requires the explicit material multiset above.
                if(!compatibleEquipment(db,inventoryBefore,item,ctx.championId,ctx.level))return fail(error,k("incompatible_purchase"));staged.inventoryItems_.append(item);
            }
        }else {
            if(event.contains(k("consume_item_ids")))return fail(error,k("sale_cannot_consume_recipe_materials"));
            if(!staged.inventoryItems_.removeOne(item))return fail(error,k("sold_item_not_owned"));
        }
        std::sort(staged.inventoryItems_.begin(),staged.inventoryItems_.end());ledger.inventory=ownedSources(staged.inventoryItems_);
        // This external counter described the prior inventory snapshot. A
        // verified inventory transaction invalidates it; derive the new bucket.
        ctx.counters.remove(k("augment:3059:item_mana"));
        // A checkpoint may have been taken while the required item was sold.
        // Preserve the committed quest receipt, but only reconstruct its
        // usable multiplier after actual ownership and current review agree.
        restoreMechanicGrowthModifiers(db,ctx,ledger);
        detail={{k("item_id"),item},{k("inventory_before"),physicalIds(inventoryBefore)},{k("consume_item_ids"),physicalIds(consumed)},{k("inventory_after"),physicalIds(staged.items())},{k("remaining_cost"),cost},{k("gold_debited"),false},{k("transaction_economy"),k("observed_transaction_only_gold_not_inferred")},{k("historical_stacks_awarded"),false},{k("resources_refilled"),false},{k("cooldowns_reset"),false}};
    } else if(type==k("mechanic_observation")){
        auto observation=event.value(k("observation")).isObject()?event.value(k("observation")).toObject():event;observation[k("time")]=time;
        const auto source=observation.value(k("source")).toString();const int sourceId=observation.value(k("id")).toInt();const CombatEntry *entry=nullptr;
        if(source==k("item")&&ledger.inventory.contains(sourceId)){const auto it=db.items.constFind(sourceId);if(it!=db.items.cend())entry=&*it;}
        else if(source==k("augment")&&ledger.augments.contains(sourceId)){const auto it=db.augments.constFind(sourceId);if(it!=db.augments.cend())entry=&*it;}
        if(!entry||(entry->actions.isEmpty()&&entry->eventRules.isEmpty()))return fail(error,k("mechanic_source_not_owned_or_without_executor"));
        auto active=staged.contextAt(time);
        if(executionContext){
            if(executionContext->championId!=active.championId||executionContext->level!=active.level||executionContext->ranks!=active.ranks||executionContext->permanentEventStats.values!=active.permanentEventStats.values)return fail(error,k("mechanic_execution_context_identity_or_growth_mismatch"));
            active=*executionContext;
        }
        const auto evaluated=evaluateCombat(db,active,staged.items(),staged.augments());if(!evaluated.valid)return fail(error,k("mechanic_pre_state_invalid"));
        const auto ownershipBefore=ledger.inventory;
        observation[k("event_id")]=id;
        QStringList missing;if(!executeMechanicActions(*entry,active,evaluated.stats,observation,ledger,&missing,&db,native3116Receipt))return fail(error,k("mechanic_observation_rejected:")+missing.join(QChar(u',')));
        // A committed observation may grow an owned item's mana affix or grant
        // an item. Its old aggregate counter has no authority over that change.
        ctx.counters.remove(k("augment:3059:item_mana"));
        for(int item:ledger.inventory)if(!db.items.contains(item))return fail(error,k("unbound_reward_item"));for(int augment:ledger.augments)if(!db.augments.contains(augment))return fail(error,k("unbound_reward_augment"));
        // Ownership remains a set, whereas each completed grant effect is a
        // physical acquisition receipt. A grant of an already owned plain
        // component must still occupy a slot and contribute its attributes.
        // Pending rewards and skipped once-actions emit no grant receipt.
        for(int item:ownershipBefore)if(!ledger.inventory.contains(item)&&!staged.inventoryItems_.removeOne(item))return fail(error,k("mechanic_inventory_source_not_in_physical_slots"));
        QSet<int> receiptedGrants;for(const auto &raw:effectDifference(beforeEffects,ledger.effects)){const auto receipt=raw.toObject();const auto op=receipt.value(k("op")).toString();if(op!=k("grant_item")&&op!=k("quest_item"))continue;const auto reward=receipt.value(k("value"));if(!number(reward,1,2147483647)||reward.toDouble()!=reward.toInt()||!db.items.contains(reward.toInt()))return fail(error,k("invalid_mechanic_inventory_receipt"));staged.inventoryItems_.append(reward.toInt());receiptedGrants.insert(reward.toInt());}
        for(int item:ledger.inventory)if(!ownershipBefore.contains(item)&&!receiptedGrants.contains(item))staged.inventoryItems_.append(item);
        std::sort(staged.inventoryItems_.begin(),staged.inventoryItems_.end());ledger.inventory=ownedSources(staged.inventoryItems_);
        QList<int> checkedInventory;for(int item:staged.inventoryItems_){if(!compatibleEquipment(db,checkedInventory,item,ctx.championId,ctx.level))return fail(error,k("mechanic_result_physical_inventory_invalid"));checkedInventory.append(item);}
        detail={{k("source"),source},{k("source_id"),sourceId},{k("event"),observation.value(k("event"))},{k("synthetic_future_events"),false}};
        if(source==k("augment")&&sourceId==3116){detail[k("receipt_path")]=native3116Receipt?k("native_admission_or_damage_settlement"):k("caller_trusted_external_observation");
            detail[k("source_authenticated")]=false;detail[k("live_certified")]=false;detail[k("policy_provenance")]=observation.value(k("receipt")).toObject().value(k("policy_provenance"));}
    } else return fail(error,k("unsupported_progression_event"));
    if(!ledger.inventory.contains(4005))for(qsizetype i=0;i<ledger.effects.size();++i){auto effect=ledger.effects[i].toObject();if(item4005Attachment(effect)){effect[k("expires")]=time;ledger.effects[i]=effect;}}
    if(!evaluateCombat(db,staged.contextAt(time),staged.items(),staged.augments()).valid)return fail(error,k("progression_result_equipment_or_stats_invalid"));
    staged.time_=time;staged.consumedIds_.insert(id);
    // Effects form a multiset. Two independent casts can produce byte-identical
    // payloads at the same instant, and each accepted occurrence must deliver.
    const auto newEffects=effectDifference(beforeEffects,ledger.effects);
    staged.decision_={{k("accepted"),true},{k("event_id"),id},{k("type"),type},{k("time"),time},{k("detail"),detail},{k("unknowns"),unknowns},{k("new_effects"),newEffects}};ledger.decisions.append(staged.decision_);
    *this=std::move(staged);return true;
}
QJsonObject ProgressionLedger::snapshot() const {
    if(!loaded_)return {};QJsonObject out{{k("schema_version"),4},{k("data_revision"),revision_},{k("as_of_time"),time_},{k("context"),packContext(context_)},{k("consumed_event_ids"),strings(consumedIds_)},
        {k("mechanic_values"),ledger_.values},{k("mechanic_effects"),ledger_.effects},{k("mechanic_decisions"),ledger_.decisions},{k("mechanic_consumed"),strings(ledger_.consumed)},{k("mechanic_clocks"),doubles(ledger_.clocks)},
        {k("inventory"),ids(ledger_.inventory)},{k("inventory_items"),physicalIds(inventoryItems_)},{k("augments"),ids(ledger_.augments)},{k("mechanic_last_time"),ledger_.lastTime},{k("observed_game_time_known"),gameTimeKnown_},{k("observed_game_time_offset"),gameTimeOffset_}};out[k("integrity_sha256")]=checksum(out);return out;
}
bool ProgressionLedger::restore(const CombatDatabase &db,const QJsonObject &state,QString *error){
    if(state.value(k("schema_version")).toInt()!=4||state.value(k("data_revision")).toString()!=db.revision||state.value(k("integrity_sha256")).toString()!=checksum(state)||!number(state.value(k("as_of_time"))))return fail(error,k("invalid_progression_checkpoint_identity_or_integrity"));
    ProgressionLedger staged;CombatContext context;if(!unpackContext(db,state.value(k("context")).toObject(),context))return fail(error,k("invalid_progression_checkpoint_context"));
    if(loaded_&&context.championId!=context_.championId)return fail(error,k("progression_checkpoint_actor_mismatch"));
    QList<int> ownedIds,itemIds,augmentIds;if(!readPhysicalIds(state.value(k("inventory")),ownedIds)||ownedSources(ownedIds).size()!=ownedIds.size())return fail(error,k("invalid_progression_inventory"));
    if(state.contains(k("inventory_items"))){if(!readPhysicalIds(state.value(k("inventory_items")),itemIds)||ownedSources(itemIds)!=ownedSources(ownedIds))return fail(error,k("invalid_progression_physical_inventory"));}else itemIds=ownedIds;
    for(const auto &v:state.value(k("augments")).toArray()){if(!number(v,1,2147483647)||v.toDouble()!=v.toInt())return fail(error,k("invalid_progression_augments"));augmentIds.append(v.toInt());}
    if(!staged.load(db,context,itemIds,augmentIds,state.value(k("as_of_time")).toDouble(),error))return false;
    if(!state.value(k("observed_game_time_known")).isBool()||!number(state.value(k("observed_game_time_offset")),-1e12))return fail(error,k("invalid_progression_game_time_baseline"));
    staged.gameTimeKnown_=state.value(k("observed_game_time_known")).toBool();staged.gameTimeOffset_=state.value(k("observed_game_time_offset")).toDouble();
    if(staged.gameTimeKnown_&&staged.time_+staged.gameTimeOffset_<0)return fail(error,k("invalid_progression_game_time_offset"));
    for(const auto &v:state.value(k("consumed_event_ids")).toArray()){if(!v.isString()||v.toString().isEmpty()||staged.consumedIds_.contains(v.toString()))return fail(error,k("invalid_progression_event_ids"));staged.consumedIds_.insert(v.toString());}
    staged.ledger_.values=state.value(k("mechanic_values")).toObject();staged.ledger_.effects=state.value(k("mechanic_effects")).toArray();staged.ledger_.decisions=state.value(k("mechanic_decisions")).toArray();
    const auto clocks=state.value(k("mechanic_clocks")).toObject();for(auto it=clocks.begin();it!=clocks.end();++it){if(!number(it.value())||it.value().toDouble()>staged.time_)return fail(error,k("invalid_progression_mechanic_clock"));staged.ledger_.clocks[it.key()]=it.value().toDouble();}
    for(const auto &v:state.value(k("mechanic_consumed")).toArray()){if(!v.isString())return fail(error,k("invalid_progression_mechanic_consumed"));staged.ledger_.consumed.insert(v.toString());}
    const auto last=state.value(k("mechanic_last_time"));if(!number(last,-1)||last.toDouble()>staged.time_)return fail(error,k("invalid_progression_mechanic_time"));staged.ledger_.lastTime=last.toDouble();
    for(const auto &v:staged.ledger_.effects){if(!v.isObject())return fail(error,k("invalid_progression_effect"));const auto e=v.toObject();if(!number(e.value(k("start")))||!number(e.value(k("expires")),-1)|| (e.value(k("expires")).toDouble()>=0&&e.value(k("expires")).toDouble()<e.value(k("start")).toDouble()))return fail(error,k("invalid_progression_effect_lifetime"));}
    if(!validateAugment3116State(staged.ledger_,error))return false;
    restoreMechanicGrowthModifiers(db,staged.context_,staged.ledger_);
    *this=std::move(staged);return true;
}
