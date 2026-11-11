#include "purchase_planner.h"
#include "chain_purchase_planner.h"
#include "combat_unknown_policy.h"
#include "combat_observation_scope.h"
#include "catalog.h"
#include "analysis.h"
#include "combat.h"
#include "combat_sandbox.h"
#include "decision_context.h"
#include "headliner.h"
#include "growth.h"
#include "growth_objective.h"
#include "mechanic_actions.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace {
QString q(const char *s) { return QString::fromUtf8(s); }
QJsonValue nil() { return QJsonValue(QJsonValue::Null); }
bool numeric(const QJsonValue &v) { return v.isDouble() && std::isfinite(v.toDouble()); }
QJsonArray ids(const QList<int> &values) { QJsonArray out; for (int id : values) out.append(id); return out; }
bool duplicateComponent(const CombatEntry &item) {
    return !item.eligible && !item.finished && !item.boots && item.effects.isEmpty()
        && item.actions.isEmpty() && item.eventRules.isEmpty()
        && item.growthStackInputs.isEmpty() && item.temporalBindingsV4.isEmpty();
}
bool validRecipe(const CombatDatabase &db,int id,QString *reason,bool requirePrice=true) {
    QSet<int> path;
    int visits=0;
    std::function<bool(int,int)> visit=[&](int part,int depth) {
        if(depth>12 || ++visits>4096){*reason=q("recipe_depth_or_size_exceeded");return false;}
        const auto it=db.items.constFind(part);
        if(it==db.items.cend()){*reason=q("recipe_contains_unknown_item");return false;}
        if(requirePrice&&it->price<0){*reason=q("recipe_price_unavailable");return false;}
        if(path.contains(part)){*reason=q("recipe_cycle");return false;}
        path.insert(part);
        qint64 childrenPrice=0;
        for(int child:it->from) {
            if(!visit(child,depth+1))return false;
            childrenPrice+=db.items.value(child).price;
            if(childrenPrice>std::numeric_limits<int>::max()){
                *reason=q("recipe_price_overflow");return false;
            }
        }
        path.remove(part);return true;
    };
    return visit(id,0);
}
bool shopCandidate(const CombatDatabase &db,int id) {
    if(db.items.value(id).eligible)return true;
    // `eligible` is the final-build search filter. Plain components are sold
    // too, but a price alone does not turn a reward/disabled item into a shop
    // item: it must occur in the recipe tree of an eligible finished item.
    QSet<int> visited;QList<int> pending;
    for(auto it=db.items.cbegin();it!=db.items.cend();++it)
        if(it->eligible && it->finished)pending.append(it->from);
    while(!pending.isEmpty()) {
        const int part=pending.takeLast();
        if(visited.contains(part))continue;
        visited.insert(part);
        if(part==id)return true;
        const auto it=db.items.constFind(part);
        if(it!=db.items.cend())pending.append(it->from);
    }
    return false;
}
QList<int> afterPurchase(const CombatDatabase &db, const QList<int> &owned, int id,QList<int> *consumed) {
    auto result = owned;
    // Credit and consume a real recipe multiset, including duplicate components.
    // The finished item must not be added beside components it has consumed.
    std::function<void(int,int)> consume = [&](int part,int depth) {
        // The recipe is validated before this traversal. Consume a held parent
        // before descending, so its children cannot receive a second credit.
        if (result.removeOne(part)) {consumed->append(part);return;}
        for (int child : db.items[part].from) consume(child,depth+1);
    };
    for (int part : db.items[id].from) consume(part,0);
    result.append(id); return result;
}
bool legal(const CombatDatabase &db, const QList<int> &inventory, int champion,int level,bool recipeReference=false) {
    if (inventory.size()>6) return false;
    QMap<QString,int> groups,limits; QSet<int> unique;
    for (int id : inventory) {
        const auto it=db.items.constFind(id);
        if(it==db.items.cend() || it->requiredLevel>level || (champion==69 && it->boots))return false;
        const bool physicalComponent=recipeReference&&!it->eligible&&!it->finished&&!it->boots;
        if(unique.contains(id) && !duplicateComponent(*it)&&!physicalComponent)return false; unique.insert(id);
        for(auto g=it->groups.cbegin();g!=it->groups.cend();++g) {
            ++groups[g.key()];
            limits[g.key()]=limits.contains(g.key())?std::min(limits.value(g.key()),g.value()):g.value();
        }
    }
    for(auto g=groups.cbegin();g!=groups.cend();++g)if(g.value()>limits.value(g.key()))return false;
    return true;
}
QJsonObject rejectedPurchase(const QString &reason) {
    return {{q("legal"),false},{q("reason"),reason},{q("consume_item_ids"),QJsonArray{}},
        {q("inventory_after"),QJsonArray{}},{q("remaining_cost"),nil()}};
}
QJsonObject purchaseAvailability(const CombatDatabase &db,const CombatEntry &item,int champion,int level,const QList<int> &owned,
                                 const PurchaseAvailabilityScenario *scenario) {
    QJsonObject out{{q("status"),q("not_actor_conditioned")},{q("allowed"),true},
        {q("static_candidate"),item.eligible},{q("conditional"),!item.conditionalPurchaseRestrictions.isEmpty()},
        {q("restrictions"),item.conditionalPurchaseRestrictions},{q("purchase_legal_verified"),false},
        {q("binding_is_authentication"),false},{q("model_only"),false}};
    if(item.conditionalPurchaseRestrictions.isEmpty())return out;
    auto fail=[&](const char *reason,const QString &message){out[q("allowed")]=false;out[q("reason")]=q(reason);
        out[q("status")]=q("conditional_purchase_unresolved");out[q("message")]=message;return out;};
    QSet<QString> required;
    for(const auto &raw:item.conditionalPurchaseRestrictions) {
        if(!raw.isObject())return fail("invalid_conditional_purchase_restriction",q("装备购买条件格式异常"));
        const auto row=raw.toObject();const auto buff=row.value(q("buff_name")).toString();
        const QSet<QString> restrictionFields{q("kind"),q("source_field"),q("buff_name"),q("source_pointer"),q("evaluation"),q("purchase_legal_verified")};
        bool shape=row.size()==restrictionFields.size();for(auto it=row.begin();it!=row.end();++it)shape=shape&&restrictionFields.contains(it.key());
        if(!shape||row.value(q("kind"))!=q("restricted_buff")||row.value(q("source_field"))!=q("RestrictedBuffName")
            ||row.value(q("source_pointer"))!=q("Items/")+QString::number(item.id)||buff.trimmed().isEmpty()
            ||required.contains(buff)||row.value(q("purchase_legal_verified"))!=QJsonValue(false)
            ||row.value(q("evaluation"))!=q("requires_current_actor_buff_or_authoritative_shop_state"))
            return fail("invalid_conditional_purchase_restriction",q("装备购买条件与来源不匹配"));
        required.insert(buff);
    }
    if(!scenario)return fail("conditional_purchase_observation_required",q("可作为出装候选；当前购买条件须由本局商店或完整状态确认"));
    out[q("model_only")]=true;out[q("scope")]=q("caller_declared_availability_scenario_not_live_observation");
    const auto &s=*scenario;const auto &o=s.observation;
    const QSet<QString> fields{q("schema_version"),q("kind"),q("source_id"),q("actor_key"),q("context_id"),q("champion_id"),q("level"),q("owned_item_ids"),q("item_id"),q("patch"),q("combat_revision"),q("observed_at"),q("purchasable"),q("complete"),q("buff_states")};
    for(auto it=o.begin();it!=o.end();++it)if(!fields.contains(it.key()))return fail("unsupported_purchase_availability_field",q("购买条件含未支持字段"));
    const auto source=o.value(q("source_id")).toString();
    QList<int> observedInventory;bool inventoryValid=o.value(q("owned_item_ids")).isArray();
    for(const auto &id:o.value(q("owned_item_ids")).toArray()) {
        if(!numeric(id)||id.toDouble()!=id.toInt()||id.toInt()<=0)inventoryValid=false;
        observedInventory.append(id.toInt());
    }
    auto expectedInventory=owned;std::sort(expectedInventory.begin(),expectedInventory.end());std::sort(observedInventory.begin(),observedInventory.end());
    if(o.value(q("schema_version"))!=QJsonValue(1)||s.actorKey.trimmed().isEmpty()||s.contextId.trimmed().isEmpty()
        ||s.championId!=champion||s.patch!=db.patch||s.combatRevision!=db.revision
        ||db.patch.isEmpty()||db.revision.isEmpty()||o.value(q("level"))!=QJsonValue(level)||!inventoryValid||observedInventory!=expectedInventory
        ||!std::isfinite(s.gameTime)||s.gameTime<0||!numeric(o.value(q("observed_at")))
        ||o.value(q("observed_at")).toDouble()!=s.gameTime||source.trimmed().isEmpty()||source.size()>256
        ||o.value(q("actor_key"))!=s.actorKey||o.value(q("context_id"))!=s.contextId
        ||o.value(q("champion_id"))!=QJsonValue(champion)||o.value(q("item_id"))!=QJsonValue(item.id)
        ||o.value(q("patch"))!=s.patch||o.value(q("combat_revision"))!=s.combatRevision)
        return fail("purchase_availability_binding_mismatch",q("购买条件与当前英雄、装备、版本或时刻不一致"));
    bool allowed=false;
    if(o.value(q("kind"))==q("caller_declared_shop_state")) {
        if(!o.value(q("purchasable")).isBool()||o.contains(q("buff_states"))||o.contains(q("complete")))
            return fail("invalid_declared_shop_state",q("商店条件声明不完整或含冲突输入"));
        allowed=o.value(q("purchasable")).toBool();
    } else if(o.value(q("kind"))==q("caller_declared_actor_buff_snapshot")) {
        if(o.value(q("complete"))!=QJsonValue(true)||!o.value(q("buff_states")).isArray()||o.contains(q("purchasable")))
            return fail("complete_restricted_buff_state_required",q("需逐项明确完整的购买限制状态"));
        QSet<QString> seen;allowed=true;
        for(const auto &raw:o.value(q("buff_states")).toArray()) {
            const auto row=raw.toObject();const auto buff=row.value(q("buff_name")).toString();
            if(!raw.isObject()||row.size()!=3||!row.contains(q("source_pointer"))||!row.contains(q("buff_name"))
                ||!row.value(q("present")).isBool()||!required.contains(buff)||seen.contains(buff)
                ||row.value(q("source_pointer"))!=q("Items/")+QString::number(item.id))
                return fail("restricted_buff_state_mismatch",q("购买限制状态与装备来源不一致"));
            seen.insert(buff);allowed=allowed&&!row.value(q("present")).toBool();
        }
        if(seen!=required)return fail("complete_restricted_buff_state_required",q("未完整声明所有购买限制状态"));
    } else return fail("unsupported_purchase_availability_kind",q("仅接受明确标注的离线购买条件"));
    out[q("allowed")]=allowed;out[q("status")]=allowed?q("conditionally_allowed_in_declared_scenario"):q("blocked_in_declared_scenario");
    out[q("reason")]=allowed?q("declared_purchase_conditions_passed"):q("declared_purchase_conditions_blocked");
    out[q("source_id")]=source;out[q("actor_key")]=s.actorKey;out[q("context_id")]=s.contextId;
    out[q("observed_at")]=s.gameTime;out[q("message")]=allowed?q("在明确声明的离线条件下可购买；尚非实盘商店认证"):q("本次离线声明的购买限制未通过");
    return out;
}
bool growthSensitive(const CombatEntry &item) {
    if(!item.growthStackInputs.isEmpty())return true;
    for(const auto &v:item.effects)if(v.toObject().value(q("op"))==q("stack"))return true;
    return false;
}
bool requiresItem(const CombatEntry &augment,int id) {
    return augment.triggerContract.value(q("requires")).toArray().contains(q("item:")+QString::number(id));
}
struct Candidate {
    int id=0,cost=0,unknown=0; bool affordable=false,growth=false,quest=false,growthTransitionVerified=true,numericComparisonComplete=false;
    double gain=0,score=0,currentScore=0,historicalScore=0;
    bool historicalAvailable=false,rosterHistoricalAvailable=false; int currentWindows=0,historicalWindows=0;
    QJsonObject rosterHistorySummary,scenarioSummary;QJsonArray comparisonCritical;
    QString scoreBasis=q("current_modeled_windows"); QList<int> inventory; QJsonObject detailed,growthTransition;
};
CombatContext purchaseContext(const Catalog &catalog,const GameState &state,const QString &style,QJsonObject *targets=nullptr,QJsonObject *audit=nullptr) {
    const auto &db=*catalog.combat;auto ctx=db.context(state.championId,state.level);
    ctx.discreteSkillCasts=true;
    ctx.ranks=selfSkillRankContext(db,state).context.ranks;
    ctx.counters=state.effectCounters;ctx.conditions=state.effectConditions;ctx.counters[q("observed_game_time")]=state.gameTime;
    if(std::isfinite(state.mechanicBaselineTime)&&state.mechanicBaselineTime>=0)ctx.counters[q("mechanic_baseline_time")]=state.mechanicBaselineTime;
    const auto target=applyOpponentLoadoutContext(catalog,state,ctx);if(targets)*targets=target;
    const auto selected=effectiveSelectedAugments(catalog,state);
    const auto ledger=replayCombatMechanics(db,ctx,state.owned,selected,state.mechanicObservations);
    applyMechanicLedgerContext(ledger,state.gameTime,ctx,true);
    if(audit) {
        double next=-1;
        auto boundary=[&](double time){if(std::isfinite(time)&&time>state.gameTime&&(next<0||time<next))next=time;};
        for(const auto &v:ledger.effects){const auto effect=v.toObject();boundary(effect.value(q("start")).toDouble(-1));boundary(effect.value(q("expires")).toDouble(-1));}
        for(const auto &v:state.mechanicObservations)boundary(v.toObject().value(q("time")).toDouble(-1));
        *audit=QJsonObject{{q("executed_mechanics"),ledger.json(state.gameTime)},
            {q("mechanic_snapshot_time"),state.gameTime},{q("mechanic_valid_until"),next}};
    }
    if(!state.liveStats.isEmpty())anchorObservedCombatStats(db,ctx,state.owned,selected,state.liveStats,resolvedCombatPreference(catalog,state,style));
    bindGrowthObjective(db,ctx,state.owned,selected);
    return ctx;
}
}

namespace {
QJsonObject inventoryPurchase(const CombatDatabase &db,int championId,int level,
                                 const QList<int> &owned,int targetItemId,const PurchaseAvailabilityScenario *scenario,bool recipeReference=false) {
    if(!db.champions.contains(championId))return rejectedPurchase(q("unknown_champion"));
    if(level<1 || level>18)return rejectedPurchase(q("unsupported_champion_level"));
    const auto item=db.items.constFind(targetItemId);
    if(item==db.items.cend())return rejectedPurchase(q("unknown_item"));
    for(const auto &reason:item->purchaseExclusions)
        if(reason!=q("not_finished_equipment"))return rejectedPurchase(q("source_item_excluded:")+reason);
    if(item->price<=0 || !shopCandidate(db,targetItemId))return rejectedPurchase(q("not_shop_eligible"));
    if(item->requiredLevel>level)return rejectedPurchase(q("item_required_level_not_reached"));
    if(championId==69 && item->boots)return rejectedPurchase(q("champion_cannot_buy_boots"));
    if(!legal(db,owned,championId,level,recipeReference))return rejectedPurchase(q("invalid_owned_inventory"));
    QString recipeReason;
    if(!validRecipe(db,targetItemId,&recipeReason))return rejectedPurchase(recipeReason);
    QList<int> consumed;
    const auto inventory=afterPurchase(db,owned,targetItemId,&consumed);
    if(!legal(db,inventory,championId,level,recipeReference))return rejectedPurchase(q("inventory_slots_groups_or_duplicate_conflict"));
    // remainingBuildCost also supports completed build plans and reserves an
    // exact held target. Here the request is always a NEW copy, so existing
    // copies of the target cannot make the new purchase free.
    auto creditOwned=owned;creditOwned.removeAll(targetItemId);
    const int cost=remainingBuildCost(db,{targetItemId},creditOwned);
    if(cost<0)return rejectedPurchase(q("recipe_cost_unavailable"));
    const auto availability=purchaseAvailability(db,*item,championId,level,owned,scenario);
    if(!availability.value(q("allowed")).toBool()) {
        auto rejected=rejectedPurchase(availability.value(q("reason")).toString());
        rejected[q("purchase_availability")]=availability;rejected[q("conditional_purchase")]=true;
        return rejected;
    }
    return {{q("legal"),true},{q("reason"),q("ok")},{q("consume_item_ids"),ids(consumed)},
        {q("inventory_after"),ids(inventory)},{q("remaining_cost"),cost},{q("purchase_availability"),availability}};
}

QJsonObject inventoryPurchase(const Catalog &catalog,int championId,int level,
                                 const QList<int> &owned,int targetItemId,const PurchaseAvailabilityScenario *scenario,bool recipeReference=false) {
    if(!catalog.combat)return rejectedPurchase(q("combat_catalog_unavailable"));
    if(catalog.patch.isEmpty() || catalog.combat->patch!=catalog.patch)return rejectedPurchase(q("catalog_patch_mismatch"));
    const auto raw=catalog.items.constFind(targetItemId);
    const auto entry=catalog.combat->items.constFind(targetItemId);
    if(raw==catalog.items.cend() || entry==catalog.combat->items.cend())return rejectedPurchase(q("unknown_catalog_item"));
    const auto identity=raw->value(q("item_id")),price=raw->value(q("price_total"));
    if(!numeric(identity) || identity.toDouble()!=targetItemId || raw->value(q("patch"))!=catalog.patch
        || !numeric(price) || price.toDouble()!=entry->price)return rejectedPurchase(q("catalog_item_source_mismatch"));
    const auto inStore=raw->value(q("in_store")),candidate=raw->value(q("kiwi_resource_candidate"));
    if(!inStore.isBool() || !inStore.toBool() || !candidate.isBool() || !candidate.toBool())
        return rejectedPurchase(q("not_in_current_mode_shop"));
    const auto recipe=raw->value(q("from_ids"));
    if(!recipe.isArray())return rejectedPurchase(q("catalog_recipe_mismatch"));
    QList<int> rawRecipe;
    for(const auto &part:recipe.toArray()) {
        if(!numeric(part) || part.toDouble()<1 || part.toDouble()>std::numeric_limits<int>::max()
            || part.toDouble()!=part.toInt())return rejectedPurchase(q("catalog_recipe_mismatch"));
        rawRecipe.append(part.toInt());
    }
    auto expected=entry->from;std::sort(rawRecipe.begin(),rawRecipe.end());std::sort(expected.begin(),expected.end());
    if(rawRecipe!=expected)return rejectedPurchase(q("catalog_recipe_mismatch"));
    return inventoryPurchase(*catalog.combat,championId,level,owned,targetItemId,scenario,recipeReference);
}
}
QJsonObject planInventoryPurchase(const CombatDatabase &db,int champion,int level,const QList<int> &owned,int item) {
    return inventoryPurchase(db,champion,level,owned,item,nullptr);
}
QJsonObject planInventoryPurchase(const Catalog &catalog,int champion,int level,const QList<int> &owned,int item) {
    return inventoryPurchase(catalog,champion,level,owned,item,nullptr);
}
QJsonObject planInventoryRecipeReference(const Catalog &catalog,int champion,int level,const QList<int> &owned,int item) {
    auto quote=inventoryPurchase(catalog,champion,level,owned,item,nullptr,true);
    quote[q("reference_only")]=true;quote[q("ranking_available")]=false;quote[q("combat_stack_semantics_verified")]=false;
    quote[q("scope")]=q("physical_component_slots_and_catalog_recipe_not_combat_execution");return quote;
}
QJsonObject planInventoryPurchase(const CombatDatabase &db,int champion,int level,const QList<int> &owned,int item,const PurchaseAvailabilityScenario &scenario) {
    return inventoryPurchase(db,champion,level,owned,item,&scenario);
}
QJsonObject planInventoryPurchase(const Catalog &catalog,int champion,int level,const QList<int> &owned,int item,const PurchaseAvailabilityScenario &scenario) {
    return inventoryPurchase(catalog,champion,level,owned,item,&scenario);
}
QJsonObject validateObservedPurchaseRecipe(const CombatDatabase &db,int champion,int level,const QList<int> &materials,int item) {
    QJsonObject out{{q("recipe_valid"),false},{q("purchase_legal_verified"),false},{q("allows_future_purchase"),false},
        {q("scope"),q("already_observed_transaction_recipe_and_physical_inventory_only")},
        {q("consume_item_ids"),QJsonArray{}},{q("inventory_after"),QJsonArray{}},{q("remaining_cost"),nil()}};
    auto fail=[&](const QString &why){out[q("reason")]=why;return out;};
    if(!db.champions.contains(champion)||level<1||level>18)return fail(q("invalid_actor"));
    if(!db.items.contains(item))return fail(q("unknown_item"));
    for(const auto &exclusion:db.items.value(item).purchaseExclusions)
        if(exclusion!=q("not_finished_equipment")&&!exclusion.startsWith(q("RestrictedBuffName:")))
            return fail(q("source_item_excluded:")+exclusion);
    if(!legal(db,materials,champion,level))return fail(q("invalid_material_inventory"));
    QString reason;if(!validRecipe(db,item,&reason,false))return fail(reason);
    QList<int> consumed;const auto after=afterPurchase(db,materials,item,&consumed);
    if(!legal(db,after,champion,level))return fail(q("inventory_slots_groups_or_duplicate_conflict"));
    out[q("recipe_valid")]=true;out[q("reason")]=q("observed_recipe_valid");out[q("consume_item_ids")]=ids(consumed);out[q("inventory_after")]=ids(after);
    auto credit=materials;credit.removeAll(item);const auto cost=remainingBuildCost(db,{item},credit);
    if(cost>=0)out[q("remaining_cost")]=cost;
    return out;
}

QList<int> nextPurchaseCandidateIds(const Catalog &catalog,const GameState &state) {
    QList<int> out;if(!catalog.combat)return out;const auto &db=*catalog.combat;
    QSet<int> scalingStats;
    std::function<void(const QJsonValue &,int)> collect=[&](const QJsonValue &value,int depth){
        if(depth>24)return;
        if(value.isArray()){for(const auto &child:value.toArray())collect(child,depth+1);return;}
        if(!value.isObject())return;const auto object=value.toObject();
        if(object.value(q("op"))==q("stat")&&object.value(q("part")).toInt()!=1)if(const auto stat=combatStat(object.value(q("stat")).toString()))scalingStats.insert(int(*stat));
        for(auto it=object.constBegin();it!=object.constEnd();++it)collect(it.value(),depth+1);
    };
    for(int id:effectiveSelectedAugments(catalog,state))if(db.augments.contains(id))collect(db.augments[id].calculations,0);
    for(auto it=db.items.cbegin();it!=db.items.cend();++it){
        if(it->price<=0||it->requiredLevel>state.level)continue;
        if(it->eligible&&it->finished&&!state.owned.contains(it.key())){out.append(it.key());continue;}
        if(!duplicateComponent(*it))continue;
        // A repeated plain component can be a complete immediate purchase.
        // Relevance expands the pool; it never supplies a score or a winner.
        for(int stat:scalingStats)if(it->stats[static_cast<Stat>(stat)]>0){out.append(it.key());break;}
    }
    return out;
}

QString purchaseBudgetBasis(const Catalog &catalog,const GameState &state) {
    if(!catalog.combat || !state.goldKnown || !std::isfinite(state.gold) || state.gold<0)return q("budget_unknown");
    QJsonArray affordable;
    for(int id:nextPurchaseCandidateIds(catalog,state)) {
        const auto transaction=planInventoryPurchase(catalog,state.championId,state.level,state.owned,id);
        if(!transaction.value(q("legal")).toBool())continue;
        const int cost=transaction.value(q("remaining_cost")).toInt();
        if(cost>0 && cost<=state.gold)affordable.append(id);
    }
    return q("budget_known:")+QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(affordable).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
}

bool purchaseReferenceReady(const Catalog &catalog,const GameState &state) {
    if(!catalog.combat)return false;
    if(modelRulesUsable(catalog,state))return true;
    const auto bridge=catalog.runtimeManifest.value(q("purchase_reference_continuity")).toObject();
    const auto items=bridge.value(q("reference_item_ids")).toArray();
    if(state.observedPatch.isEmpty()||state.observedPatch!=bridge.value(q("observed_patch")).toString()
        ||bridge.value(q("source_patch"))!=catalog.patch||bridge.value(q("combat_revision"))!=catalog.combat->revision||items.isEmpty())return false;
    for(int id:state.owned)if(!items.contains(id))return false;
    return true;
}
bool purchaseReferenceItemCompatible(const Catalog &catalog,const GameState &state,int id) {
    return purchaseReferenceReady(catalog,state)&&catalog.items.contains(id)
        &&(modelRulesUsable(catalog,state)||catalog.runtimeManifest.value(q("purchase_reference_continuity")).toObject().value(q("reference_item_ids")).toArray().contains(id));
}

QJsonObject observedPurchaseFacts(const Catalog &catalog,const GameState &state) {
    QJsonObject out{{q("available"),false},{q("ranking_available"),false},{q("selected_item_id"),0},
        {q("scope"),q("observed_inventory_gold_and_catalog_recipes_only")},
        {q("shop_price_verified"),false},{q("augment_price_modifiers_included"),false},
        {q("display_order"),q("remaining_cost_then_item_id_not_combat_priority")},
        {q("inventory_verified"),false},{q("upgrades"),QJsonArray{}},{q("current_gold"),nil()}};
    if(state.source!=GameState::Source::Live || !state.fresh() || state.gameMode!=q("KIWI")
        || (state.phase!=q("InProgress")&&state.phase!=q("Reconnect"))
        || !state.matchContext.value(q("active")).toBool()||state.matchContext.value(q("context_id")).toString().isEmpty()
        || !catalog.combat)return out;
    QJsonObject self;int selves=0;
    for(const auto &v:state.players)if(v.toObject().value(q("self"))==QJsonValue(true)){self=v.toObject();++selves;}
    if(selves!=1||self.value(q("champion_id")).toInt()!=state.championId)return out;
    const bool goldKnown=state.goldKnown&&std::isfinite(state.gold)&&state.gold>=0;
    out[q("available")]=true;out[q("budget_known")]=goldKnown;
    out[q("current_gold")]=goldKnown?QJsonValue(state.gold):nil();
    out[q("context_id")]=state.matchContext.value(q("context_id"));out[q("game_time")]=state.gameTime;out[q("patch")]=catalog.patch;
    out[q("observed_patch")]=state.observedPatch;out[q("combat_version_match")]=modelPatchMatches(catalog,state);
    if(!purchaseReferenceReady(catalog,state))return out;
    QList<int> inventory;
    for(const auto &v:self.value(q("items")).toArray()){
        if(!v.isDouble()||v.toInt()<1||v.toDouble()!=v.toInt())return out;
        inventory.append(v.toInt());
    }
    auto held=state.owned;std::sort(held.begin(),held.end());std::sort(inventory.begin(),inventory.end());
    if(selves!=1 || self.value(q("champion_id")).toInt()!=state.championId
        || self.value(q("items_complete"))!=QJsonValue(true) || !self.value(q("items")).isArray()
        || inventory!=held || !legal(*catalog.combat,held,state.championId,state.level))return out;
    out[q("inventory_verified")]=true;
    QList<QJsonObject> quotes;int affordable=0;
    for(auto it=catalog.combat->items.cbegin();it!=catalog.combat->items.cend();++it){
        if(!purchaseReferenceItemCompatible(catalog,state,it.key()))continue;
        if(held.isEmpty()||it->from.isEmpty()||held.contains(it.key()))continue;
        // Walk only to establish a material relationship; transaction validation
        // below decides source identity, current shop availability and legality.
        QSet<int> seen;QList<int> pending=it->from;bool related=false;
        while(!pending.isEmpty()&&seen.size()<4096){const int part=pending.takeLast();if(seen.contains(part))continue;seen.insert(part);
            if(held.contains(part)){related=true;break;}pending.append(catalog.combat->items.value(part).from);}
        if(!related)continue;
        const auto quote=planInventoryPurchase(catalog,state.championId,state.level,held,it.key());
        const int cost=quote.value(q("remaining_cost")).toInt(-1);
        if(!quote.value(q("legal")).toBool()||quote.value(q("consume_item_ids")).toArray().isEmpty()||cost<=0)continue;
        const bool canBuy=goldKnown&&cost<=state.gold;if(canBuy)++affordable;
        quotes.append(QJsonObject{{q("item_id"),it.key()},{q("name"),catalog.itemName(it.key())},
            {q("remaining_cost"),cost},{q("within_catalog_budget"),goldKnown?QJsonValue(canBuy):nil()},
            {q("consume_item_ids"),quote.value(q("consume_item_ids"))}});
    }
    std::sort(quotes.begin(),quotes.end(),[](const auto &a,const auto &b){const int ac=a.value(q("remaining_cost")).toInt(),bc=b.value(q("remaining_cost")).toInt();return ac==bc?a.value(q("item_id")).toInt()<b.value(q("item_id")).toInt():ac<bc;});
    QJsonArray rows;for(const auto &quote:quotes)rows.append(quote);
    out[q("upgrades")]=rows;out[q("within_catalog_budget_count")]=affordable;
    out[q("context_id")]=state.matchContext.value(q("context_id"));out[q("game_time")]=state.gameTime;
    return out;
}

QJsonObject buildPurchaseSandbox(const Catalog &catalog,const GameState &state,const QString &style,const RecommendationWorkPtr &work) {
    if(!state.explicitPurchaseChain.isEmpty())return buildExplicitChainPurchaseSandbox(catalog,state,work);
    QElapsedTimer timer; timer.start();
    QJsonObject out{{q("schema_version"),1},{q("policy"),q("budgeted_sandbox_v1")},
        {q("status"),q("unavailable")},{q("selected_item_id"),0},{q("all_effects_complete"),false},
        {q("actual_win_rate"),nil()},{q("candidates"),QJsonArray{}},{q("conditional_predictions"),QJsonArray{}},
        {q("conditional_purchase_candidates"),QJsonArray{}},
        {q("purchase_availability_scope"),q("ranked_candidates_require_current_purchase_conditions_passed")},
        {q("gaps"),QJsonArray{}},{q("context_id"),state.matchContext.value(q("context_id"))},
        {q("patch"),catalog.patch},{q("fresh"),state.fresh()},
        {q("enumeration_complete"),false},{q("screening_complete"),false},{q("detailed_comparisons_complete"),false},
        {q("ranking_available"),false},{q("globally_optimal_full_build"),false}};
    QString stage=q("validation");QElapsedTimer stageClock;stageClock.start();QJsonObject stages;
    int rejected=0,considered=0,unmodeled=0,legalCandidates=0,comparableCandidates=0,detailedCompleted=0;
    int currentWindowComplete=0,historicalWindowComplete=0,rosterHistoricalWindowComplete=0,detailedUnavailable=0;
    QList<int> detailedIds;QJsonArray candidateAudit;
    // The reuse object belongs only to this immutable purchase job. Candidate
    // effects are still evaluated independently on the complete shared model.
    auto sandboxReuse=std::make_shared<CombatSandboxReuse>(catalog,state);
    const auto health=combatHealthContract(catalog,state);
    out[q("health_basis")]=health;
    QStringList comparisonUnknowns=combatHealthUnknowns(health);
    QJsonObject diagnosticDetail;int diagnosticItemId=0;
    auto snapshotExpired=[&](){return state.source==GameState::Source::Live&&!state.fresh();};
    auto invalidate=[&](const QString &reason){
        out[q("status")]=q("interrupted");out[q("stop_reason")]=reason;
        out[q("unavailable_reason")]=reason;out[q("interrupted_stage")]=stage;
        out[q("enumeration_complete")]=false;out[q("ranking_available")]=false;out[q("selected_item_id")]=0;
        out.remove(q("selected_inventory"));out.remove(q("selected_sandbox"));out.remove(q("selected_score_basis"));out.remove(q("recommendation_kind"));
        // A prefix and a snapshot that expired mid-enumeration cannot rank
        // currently live purchases. Counts/IDs remain diagnostic only.
        out[q("candidates")]=QJsonArray{};out[q("conditional_predictions")]=QJsonArray{};out[q("conditional_purchase_candidates")]=QJsonArray{};
        candidateAudit=QJsonArray{};
    };
    auto nextStage=[&](const char *name){stages[stage]=stages.value(stage).toDouble()+double(stageClock.nsecsElapsed())/1e6;stage=q(name);stageClock.restart();};
    auto finish=[&](){
        // Recheck even for early unavailable exits and the final publication.
        // Replay snapshots deliberately have no wall-clock expiry.
        if(snapshotExpired())invalidate(q("snapshot_expired"));
        if(effectiveSelectedAugments(catalog,state).contains(3116)){
            out[q("ranking_available")]=false;out[q("selected_item_id")]=0;out[q("all_effects_complete")]=false;out[q("globally_optimal_full_build")]=false;
            out[q("unavailable_reason")]=q("3116_owned_stack_and_consume_policy_unverified");
            out[q("3116_comparison_scope")]=q("known_parts_only_unobserved_stack_order_and_heal_basis_may_reverse_ranking");
            out.remove(q("selected_inventory"));out.remove(q("selected_sandbox"));out.remove(q("selected_score_basis"));out.remove(q("recommendation_kind"));
            if(out.value(q("status"))!=q("interrupted"))out[q("status")]=q("conditional_comparison_without_selection");
        }
        const auto boundary=combatComparisonBoundary(comparisonUnknowns,!out.value(q("candidates")).toArray().isEmpty()&&out.value(q("status"))!=q("interrupted"));
        out[q("comparison_boundary")]=boundary;
        out[q("critical_unknowns")]=boundary.value(q("critical_unknowns"));
        out[q("unknowns")]=QJsonArray::fromStringList(comparisonUnknowns);
        out[q("diagnostic_comparison_available")]=boundary.value(q("numeric_comparison_available"));
        out[q("diagnostic_only")]=boundary.value(q("diagnostic_only"));out[q("live_certified")]=false;
        if(!boundary.value(q("critical_unknowns")).toArray().isEmpty()){
            out[q("ranking_available")]=false;out[q("selected_item_id")]=0;
            out.remove(q("selected_inventory"));out.remove(q("selected_sandbox"));out.remove(q("selected_score_basis"));out.remove(q("recommendation_kind"));
            if(out.value(q("status"))!=q("interrupted"))out[q("status")]=q("diagnostic_comparison_without_selection");
            if(out.value(q("unavailable_reason")).toString().isEmpty())out[q("unavailable_reason")]=q("critical_comparison_inputs_unverified");
        }
        auto rows=out.value(q("candidates")).toArray();
        for(int i=0;i<rows.size();++i){auto row=rows[i].toObject();
            const bool pairEligible=row.value(q("ranking_eligible")).toBool()&&row.value(q("growth_transition_verified")).toBool();
            row[q("pair_ranking_eligible")]=pairEligible;
            row[q("ranking_eligible")]=out.value(q("ranking_available")).toBool()&&pairEligible;
            row[q("diagnostic_only")]=!row.value(q("ranking_eligible")).toBool();rows[i]=row;}
        out[q("candidates")]=rows;
        out[q("diagnostic_only")]=boundary.value(q("numeric_comparison_available")).toBool()&&!out.value(q("ranking_available")).toBool();
        if(boundary.value(q("numeric_comparison_available")).toBool()&&!out.value(q("ranking_available")).toBool()&&!diagnosticDetail.isEmpty()){
            diagnosticDetail[q("comparison_boundary")]=boundary;
            diagnosticDetail[q("ranking_eligible")]=false;diagnosticDetail[q("diagnostic_only")]=true;
            diagnosticDetail[q("selection_status")]=q("unselected_diagnostic_reference");
            out[q("diagnostic_sandbox")]=diagnosticDetail;out[q("diagnostic_candidate_item_id")]=diagnosticItemId;
        }
        out[q("fresh")]=state.fresh();
        auto times=stages;times[stage]=times.value(stage).toDouble()+double(stageClock.nsecsElapsed())/1e6;
        out[q("elapsed_ms")]=double(timer.nsecsElapsed())/1e6;out[q("stage_timings_ms")]=times;
        out[q("screened_candidates")]=considered;out[q("rejected_candidates")]=rejected;out[q("unmodeled_legal_candidates")]=unmodeled;
        out[q("detailed_candidates")]=detailedCompleted;out[q("completed_detailed_item_ids")]=ids(detailedIds);
        int pairEligibleCount=0;for(const auto &v:candidateAudit)pairEligibleCount+=v.toObject().value(q("pair_ranking_eligible")).toBool();
        out[q("comparison_coverage")]=QJsonObject{{q("screened_candidate_count"),considered},{q("legal_candidate_count"),legalCandidates},
            {q("comparable_candidate_count"),comparableCandidates},{q("unmodeled_legal_candidates"),unmodeled},
            {q("completed_detailed_candidates"),detailedCompleted},{q("required_windows_per_candidate"),3},
            {q("complete_current_window_candidates"),currentWindowComplete},
            {q("complete_historical_window_candidates"),historicalWindowComplete},
            {q("complete_roster_historical_window_candidates"),rosterHistoricalWindowComplete},
            {q("unavailable_detailed_candidates"),detailedUnavailable},
            {q("pair_ranking_eligible_candidates"),pairEligibleCount},
            {q("all_legal_candidates_evaluated"),out.value(q("enumeration_complete")).toBool()&&unmodeled==0&&detailedCompleted==legalCandidates},
            {q("all_comparable_candidates_evaluated"),out.value(q("enumeration_complete")).toBool()&&detailedCompleted==comparableCandidates},
            {q("ranking_available"),out.value(q("ranking_available")).toBool()}};
        out[q("sandbox_reuse")]=sandboxReuse->stats();
        out[q("candidate_comparison_audit")]=candidateAudit;
        out[q("candidate_audit_scope")]=q("independent_candidate_pair_eligibility_not_overall_purchase_ranking");
        out[q("work_control")]=QJsonObject{{q("enabled"),bool(work)},{q("interrupted"),out.value(q("status"))==q("interrupted")},
            {q("job_elapsed_ms"),work?QJsonValue(double(work->elapsedMs())):nil()},
            {q("cancellation_boundary"),q("before_and_after_comparisons_and_inside_sandbox_stages")}};
        return out;
    };
    auto stop=[&](){return snapshotExpired()||(work&&work->stopped());};
    auto interrupted=[&](QString reason=QString()){
        if(snapshotExpired())reason=q("snapshot_expired");
        if(reason.isEmpty()&&work)reason=work->stopReason();
        if(reason.isEmpty())reason=q("cancelled");
        invalidate(reason);
        return finish();
    };
    if(stop())return interrupted();
    if(!catalog.combat || !state.fresh() || state.gameMode!=q("KIWI") || !modelRulesUsable(catalog,state))return finish();
    const auto &db=*catalog.combat;
    QJsonArray gaps;
    auto gap=[&](const char *code,int count){if(count>0)gaps.append(QJsonObject{{q("code"),q(code)},{q("count"),count}});};
    if(state.level<1 || state.level>18){gap("unsupported_level",1);out[q("gaps")]=gaps;return finish();}
    int selves=0;bool complete=false;
    for(const auto &v:state.players)if(v.toObject().value(q("self")).toBool()) {
        ++selves;const auto row=v.toObject();QList<int> observed;bool valid=row.value(q("items")).isArray();
        for(const auto &id:row.value(q("items")).toArray()){valid&=id.isDouble()&&id.toDouble()==id.toInt()&&db.items.contains(id.toInt());observed.append(id.toInt());}
        auto actual=state.owned;std::sort(observed.begin(),observed.end());std::sort(actual.begin(),actual.end());
        complete=row.value(q("items_complete")).toBool()&&valid&&observed==actual
            &&row.value(q("champion_id")).toInt()==state.championId;
    }
    if(selves!=1||!complete){out[q("unavailable_reason")]=q("self_inventory_unobserved_or_inconsistent");return finish();}
    if(!db.champions.contains(state.championId) || !legal(db,state.owned,state.championId,state.level))return finish();
    const auto skillRanks=selfSkillRankContext(db,state);out[q("skill_rank_observation")]=skillRanks.report;
    if(!skillRanks.available){out[q("unavailable_reason")]=q("skill_ranks_invalid");return finish();}
    if(stop())return interrupted();nextStage("current_baseline");
    QJsonObject targets,audit;auto context=purchaseContext(catalog,state,style,&targets,&audit);
    out[q("target_context")]=targets;
    for(auto it=audit.constBegin();it!=audit.constEnd();++it)out[it.key()]=it.value();
    if(stop())return interrupted();
    const auto selected=effectiveSelectedAugments(catalog,state);
    const auto preference=resolvedCombatPreference(catalog,state,style);
    out[q("equipment_growth_state")]=equipmentGrowthSelectionState(db,context,state.owned,selected,state.growthObservations);
    const auto teammates=evaluateTeammateContext(catalog,state,context,preference);
    if(stop())return interrupted();
    out[q("team_context")]=teammates.report;
    const auto base=evaluateCombat(db,context,state.owned,selected,false,preference);
    out[q("current_comparison")]=base.json();
    out[q("current_model_critical_unknowns")]=QJsonArray::fromStringList(combatCriticalUnknowns(base.unknowns));
    if(stop())return interrupted();
    if(!base.valid || !std::isfinite(base.score) || base.score<=0){
        comparisonUnknowns.append(base.unknowns);out[q("unavailable_reason")]=q("current_combat_model_unavailable");return finish();
    }
    QVector<Candidate> candidates;
    nextStage("screening");
    const auto candidateIds=nextPurchaseCandidateIds(catalog,state);
    out[q("candidate_universe")]=q("finished_items_and_selected_augment_stat_scaling_plain_components");
    for(int itemId:candidateIds) {const auto it=db.items.constFind(itemId);
        if(stop())return interrupted();
        ++considered;
        const auto transaction=planInventoryPurchase(catalog,state.championId,state.level,state.owned,it.key());
        if(!transaction.value(q("legal")).toBool()){
            ++rejected;
            if(transaction.value(q("conditional_purchase")).toBool()) {
                auto pending=out.value(q("conditional_purchase_candidates")).toArray();
                pending.append(QJsonObject{{q("item_id"),it.key()},{q("static_candidate"),true},{q("ranking_eligible"),false},
                    {q("purchase_availability"),transaction.value(q("purchase_availability"))}});
                out[q("conditional_purchase_candidates")]=pending;
            }
            continue;
        }
        QList<int> inventory;for(const auto &id:transaction.value(q("inventory_after")).toArray())inventory.append(id.toInt());
        const int cost=transaction.value(q("remaining_cost")).toInt();if(cost<=0)continue;
        ++legalCandidates;
        if(stop())return interrupted();
        const auto eval=evaluateCombat(db,context,inventory,selected,false,preference);
        if(stop())return interrupted();
        if(!eval.valid||!std::isfinite(eval.score)||!std::isfinite(eval.score/base.score)){
            ++unmodeled;const auto code=q("purchase_candidate_model_unavailable:")+QString::number(it.key());comparisonUnknowns.append(code);
            candidateAudit.append(QJsonObject{{q("item_id"),it.key()},{q("stage"),q("screening")},
                {q("pair_ranking_eligible"),false},{q("critical_unknowns"),QJsonArray{code}},
                {q("unknowns"),QJsonArray::fromStringList(eval.unknowns)}});continue;
        }
        Candidate c;c.id=it.key();c.cost=cost;c.inventory=inventory;c.gain=eval.score/base.score-1;
        c.growthTransition=equipmentGrowthInventoryTransition(db,context,state.owned,inventory,selected,state.growthObservations);
        c.growthTransitionVerified=c.growthTransition.value(q("numeric_transition_verified")).toBool();
        const auto teamGain=compareTeamObjective(teammates,base,eval,preference);
        c.score=teamGain.value(q("available")).toBool()?teamGain.value(q("score_gain")).toDouble():c.gain;
        c.unknown=eval.unknowns.size();c.affordable=state.goldKnown&&std::isfinite(state.gold)&&state.gold>=cost;
        c.growth=growthSensitive(*it);
        for(int aid:selected)if(db.augments.contains(aid)&&requiresItem(db.augments[aid],c.id))c.quest=true;
        candidates.append(c);
        comparableCandidates=candidates.size();
    }
    out[q("screening_complete")]=true;
    const auto ordering=[&](const Candidate &a,const Candidate &b) {
        // A quest dependency is an observed chosen objective, not fabricated
        // combat damage. It only guides an otherwise legal, positive purchase.
        const bool aq=a.quest&&a.gain>0,bq=b.quest&&b.gain>0;
        if(aq!=bq)return aq;
        if(state.goldKnown && a.affordable!=b.affordable)return a.affordable;
        const double av=a.affordable?a.score:a.score/std::max(1,a.cost);
        const double bv=b.affordable?b.score:b.score/std::max(1,b.cost);
        if(av!=bv)return av>bv;return a.id<b.id;
    };
    if(stop())return interrupted();
    std::sort(candidates.begin(),candidates.end(),ordering);
    // Enumerate the complete declared candidate pool on the same detailed model.
    // Screening alone cannot prove that an omitted item would lose.
    QList<int> enumerated;
    for(const auto &c:candidates)enumerated.append(c.id);
    out[q("enumerated_item_ids")]=ids(enumerated);
    QJsonArray predictions;
    int partial=0,stackMissing=0,triggerMissing=0,tactical=0,growthTransitionGaps=0;
    CombatSandboxOptions options;options.style=style;options.teamObjective=true;options.reuse=sandboxReuse;options.work=work;
    auto interruptedComparison=[&](int itemId,const QJsonObject &detail){
        out[q("interrupted_candidate_item_id")]=itemId;
        out[q("interrupted_candidate_stage")]=detail.value(q("interrupted_stage"));
        out[q("interrupted_candidate_coverage")]=detail.value(q("coverage"));
        return interrupted(detail.value(q("stop_reason")).toString());
    };
    nextStage("detailed_comparisons");
    QSet<QString> candidateScenarioBases;
    for(auto &c:candidates) {
        if(stop())return interrupted();
        // Only the first diagnostic and eventual winner can expose this projection.
        options.includeGrowthProjection=diagnosticDetail.isEmpty();
        c.detailed=evaluateCombatSandbox(catalog,state,c.inventory,options);
        if(diagnosticDetail.isEmpty()){diagnosticDetail=c.detailed;diagnosticItemId=c.id;}
        for(const auto &u:c.detailed.value(q("unknowns")).toArray())if(!comparisonUnknowns.contains(u.toString()))comparisonUnknowns.append(u.toString());
        c.detailed[q("inventory_growth_transition")]=c.growthTransition;
        growthTransitionGaps+=!c.growthTransitionVerified;
        if(c.detailed.value(q("status"))==q("interrupted"))return interruptedComparison(c.id,c.detailed);
        if(stop())return interrupted();
        ++detailedCompleted;detailedIds.append(c.id);
        // Compare the same explicit combat windows for every candidate.
        double gain=0,historicalGain=0;int n=0,historicalN=0;
        const auto windows=c.detailed.value(q("windows")).toArray();
        for(const auto &v:windows) {
            if(stop())return interrupted();
            const auto w=v.toObject();const auto value=w.value(q("team_score_gain")).isDouble()?w.value(q("team_score_gain")):w.value(q("score_gain"));
            if(numeric(value)){gain+=value.toDouble();++n;}
            const auto historical=w.value(q("historical_pressure_team_score_gain"));
            if(numeric(historical)){historicalGain+=historical.toDouble();++historicalN;}
        }
        if(n>0)c.score=gain/n;
        else c.score=c.detailed.value(q("available")).toBool()?c.gain:-1;
        c.currentScore=c.score;c.currentWindows=n;c.historicalWindows=historicalN;
        if(n==0)c.scoreBasis=c.detailed.value(q("available")).toBool()?q("reference_candidate_fallback"):q("model_unavailable");
        // Historical allocations are separate sensitivity scenarios. Each
        // complete scenario gets its own three-window mean before the minimum;
        // missing history never becomes a zero or individual output multiplier.
        c.historicalAvailable=windows.size()==3&&n==3&&historicalN==3
            &&std::isfinite(c.currentScore)&&std::isfinite(historicalGain/3.)
            &&c.detailed.value(q("historical_pressure_scenario")).toObject().value(q("available")).toBool();
        currentWindowComplete+=windows.size()==3&&n==3;
        historicalWindowComplete+=c.historicalAvailable;
        detailedUnavailable+=!c.detailed.value(q("available")).toBool();
        if(c.historicalAvailable){c.historicalScore=historicalGain/3.;c.score=std::min(c.currentScore,c.historicalScore);c.scoreBasis=q("minimum_current_and_historical_pressure_three_window_means");}
        c.rosterHistorySummary=summarizeRosterHistoryScenarios(c.detailed);
        c.rosterHistoricalAvailable=windows.size()==3&&n==3&&c.rosterHistorySummary.value(q("available")).toBool();
        rosterHistoricalWindowComplete+=c.rosterHistoricalAvailable;
        if(c.rosterHistoricalAvailable){
            c.score=std::min(c.currentScore,c.rosterHistorySummary.value(q("score_gain")).toDouble());
            if(c.rosterHistorySummary.value(q("scenarios")).toArray().size()>1||!c.historicalAvailable)
                c.scoreBasis=q("minimum_current_and_supported_roster_history_three_window_means");
        }
        c.scenarioSummary=summarizeCombatScenarios(c.detailed,state.players.size()>1);
        if(c.scenarioSummary.value(q("complete")).toBool()){
            c.score=c.scenarioSummary.value(q("score_gain")).toDouble();
            QStringList basis;for(const auto &v:c.scenarioSummary.value(q("scenarios")).toArray())basis.append(v.toObject().value(q("scenario")).toString());
            basis.sort();candidateScenarioBases.insert(basis.join(QLatin1Char('|')));
        }
        c.gain=c.score; // Public and positive-gain gates use the detailed basis.
        // A mean over the surviving opponents is not a complete ten-player
        // comparison. Do not let an unmodeled target disappear from the rank.
        bool fullTargetWindows=windows.size()==3;
        for(const auto &v:windows){const auto w=v.toObject();
            fullTargetWindows&=w.value(q("supported_targets")).toInt()==5
                &&w.value(q("team_supported_targets")).toInt()==5;}
        const bool numericComplete=c.detailed.value(q("available")).toBool()&&std::isfinite(c.score)
            &&(state.players.size()<=1||(n==3&&fullTargetWindows&&c.scenarioSummary.value(q("complete")).toBool()));
        c.numericComparisonComplete=numericComplete;
        auto candidateCritical=c.detailed.value(q("critical_unknowns")).toArray();
        if(!numericComplete){const auto code=q("purchase_candidate_comparison_incomplete:")+QString::number(c.id);
            comparisonUnknowns.append(code);candidateCritical.append(code);}
        c.comparisonCritical=candidateCritical;
        candidateAudit.append(QJsonObject{{q("item_id"),c.id},{q("stage"),q("detailed_comparison")},
            {q("numeric_comparison_complete"),numericComplete},{q("full_target_windows"),fullTargetWindows},
            {q("current_model_windows"),n},{q("score"),std::isfinite(c.score)?QJsonValue(c.score):nil()},
            {q("pair_ranking_eligible"),numericComplete&&c.growthTransitionVerified&&c.detailed.value(q("ranking_eligible")).toBool()},
            {q("critical_unknowns"),candidateCritical},
            {q("growth_transition_verified"),c.growthTransitionVerified}});
        if(!std::isfinite(c.score)){c.score=-1;c.gain=-1;c.currentScore=-1;}
        c.unknown=c.detailed.value(q("unknowns")).toArray().size();
        const auto &entry=db.items[c.id];
        if(entry.coverage!=q("modeled_numeric_effects"))++partial;
        for(const auto &u:c.detailed.value(q("unknowns")).toArray()) {
            const auto text=u.toString();stackMissing+=text.contains(q("stack"));triggerMissing+=text.contains(q("event"));
        }
        tactical+=!entry.actions.isEmpty()||!entry.eventRules.isEmpty();
        if(c.id==3084) {
            // A sensitivity grid tied to the verified 300-HP quest threshold.
            // These are hypotheses, never predicted hits or account history.
            for(int hp:{0,300,900}) {
                if(stop())return interrupted();
                auto scenario=context;scenario.counters[q("item:3084:permanent_hp")]=hp;
                scenario.counters[q("item:3084:hits")]=0;
                auto value=evaluateCombat(db,scenario,c.inventory,selected,false,preference);
                if(stop())return interrupted();
                predictions.append(QJsonObject{{q("condition_code"),q("heartsteel_growth")},{q("item_id"),3084},
                    {q("trigger_count"),nil()},{q("permanent_hp"),hp},
            {q("score_gain"),value.valid?QJsonValue(value.score/base.score-1):nil()},{q("evaluation_basis"),q("reference_target_8s")},{q("model_only"),true}});
            }
        }
    }
    // Do not rank one candidate's historical stress case against another's
    // easier current-only case. This is a required-input gap, not zero gain.
    if(candidateScenarioBases.size()>1)comparisonUnknowns.append(q("purchase_candidate_comparison_incomplete:scenario_axes_mismatch"));
    out[q("scenario_comparison_basis_consistent")]=candidateScenarioBases.size()<=1;
    out[q("detailed_comparisons_complete")]=true;
    if(stop())return interrupted();nextStage("ranking");
    // Every remaining candidate used the same opponent/window objective.
    std::sort(candidates.begin(),candidates.end(),ordering);
    if(stop())return interrupted();
    const Candidate *winner=nullptr;
    // A recipe diff cannot establish whether an already earned stack survives
    // an upgrade or sale. Keep numbers diagnostic until that lifecycle is bound.
    if(growthTransitionGaps==0)for(const auto &c:candidates)if(c.growthTransitionVerified&&c.numericComparisonComplete
        &&c.detailed.value(q("ranking_eligible")).toBool()&&c.gain>0&&c.score>0){winner=&c;break;}
    if(winner) {
        auto selectedDetail=winner->detailed;
        if(selectedDetail.value(q("growth")).toObject().isEmpty()) {
            options.includeGrowthProjection=true;
            const auto fullDetail=evaluateCombatSandbox(catalog,state,winner->inventory,options);
            if(fullDetail.value(q("status"))==q("interrupted"))return interruptedComparison(winner->id,fullDetail);
            if(stop())return interrupted();
            selectedDetail[q("growth")]=fullDetail.value(q("growth"));
        }
        out[q("selected_item_id")]=winner->id;
        out[q("selected_inventory")]=ids(winner->inventory);
        out[q("selected_sandbox")]=selectedDetail;
        out[q("selected_score_basis")]=winner->scoreBasis;
        out[q("recommendation_kind")]=winner->quest?q("quest_conditioned"):q("immediate");
    }
    QJsonArray publicCandidates;
    auto addCandidate=[&](const Candidate &c) {
        publicCandidates.append(QJsonObject{{q("item_id"),c.id},{q("remaining_cost"),c.cost},{q("affordable"),state.goldKnown?QJsonValue(c.affordable):nil()},
            {q("immediate_gain"),c.gain},{q("score"),c.score},{q("unknown_count"),c.unknown},
            {q("current_model_score_gain"),c.currentScore},{q("current_model_windows"),c.currentWindows},
            {q("historical_pressure_available"),c.historicalAvailable},{q("historical_pressure_score_gain"),c.historicalAvailable?QJsonValue(c.historicalScore):nil()},
            {q("historical_pressure_windows"),c.historicalWindows},{q("score_basis"),c.scoreBasis},
            {q("historical_roster_available"),c.rosterHistoricalAvailable},{q("historical_roster_summary"),c.rosterHistorySummary},
            {q("scenario_summary"),c.scenarioSummary},
            {q("growth_sensitive"),c.growth},{q("quest_dependency"),c.quest},
            {q("inventory_growth_transition"),c.growthTransition},{q("growth_transition_verified"),c.growthTransitionVerified}});
        auto row=publicCandidates.last().toObject();
        row[q("comparison_boundary")]=c.detailed.value(q("comparison_boundary"));
        row[q("critical_unknowns")]=c.comparisonCritical;
        row[q("numeric_comparison_complete")]=c.numericComparisonComplete;
        row[q("ranking_eligible")]=c.numericComparisonComplete&&c.detailed.value(q("ranking_eligible")).toBool();
        row[q("diagnostic_only")]=c.detailed.value(q("diagnostic_only"));
        publicCandidates[publicCandidates.size()-1]=row;
    };
    for(int i=0;i<std::min(7,int(candidates.size()));++i)addCandidate(candidates[i]);
    if(std::none_of(publicCandidates.cbegin(),publicCandidates.cend(),[](const QJsonValue &v){return v.toObject().value(q("item_id")).toInt()==3084;}))
        for(const auto &c:candidates)if(c.id==3084){addCandidate(c);break;}
    if(publicCandidates.size()<8 && candidates.size()>7 && std::none_of(publicCandidates.cbegin(),publicCandidates.cend(),[&](const QJsonValue &v){return v.toObject().value(q("item_id")).toInt()==candidates[7].id;}))addCandidate(candidates[7]);
    if(winner&&std::none_of(publicCandidates.cbegin(),publicCandidates.cend(),[&](const QJsonValue &v){return v.toObject().value(q("item_id")).toInt()==winner->id;}))addCandidate(*winner);
    QJsonArray reasons{q("model_partial"),q("scenario_not_forecast")};
    if(winner && winner->affordable)reasons.append(q("budget_affordable"));
    if(winner && winner->cost<db.items[winner->id].price)reasons.append(q("recipe_credit"));
    if(winner && winner->quest)reasons.append(q("quest_item_dependency"));
    if(!predictions.isEmpty())reasons.append(q("growth_sensitive"));
    if(targets.value(q("available")).toBool())reasons.append(q("target_specific"));else gap("incomplete_opponents",1);
    if(!state.goldKnown)reasons.append(q("gold_unobserved"));
    if(!winner && state.owned.size()>=6)reasons.append(q("inventory_full"));
    gap("partial_effects",partial);gap("unobserved_stacks",stackMissing);gap("unobserved_triggers",triggerMissing);gap("unmodeled_tactical_actions",tactical);
    gap("unverified_growth_inventory_transition",growthTransitionGaps);
    out[q("growth_transition_comparison_complete")]=growthTransitionGaps==0;
    if(growthTransitionGaps>0){out[q("unavailable_reason")]=q("growth_inventory_transition_unverified");reasons.append(q("growth_inventory_transition_unverified"));}
    out[q("status")]=winner?q("conditional"):q("unavailable");out[q("rationale_codes")]=reasons;
    out[q("candidates")]=publicCandidates;out[q("conditional_predictions")]=predictions;out[q("gaps")]=gaps;
    out[q("screened_candidates")]=considered;out[q("rejected_candidates")]=rejected;
    out[q("unmodeled_legal_candidates")]=unmodeled;
    out[q("detailed_candidates")]=enumerated.size();out[q("elapsed_ms")]=double(timer.nsecsElapsed())/1e6;
    out[q("search_scope")]=q("all_legal_catalog_finished_next_purchases_under_style_and_inventory_constraints");
    out[q("selection_policy")]=q("positive_quest_dependency_then_affordability_then_gain_or_unaffordable_gain_per_gold");
    out[q("enumeration_complete")]=true;out[q("globally_optimal_full_build")]=false;
    out[q("ranking_available")]=winner!=nullptr;
    out[q("objective_basis")]=teammates.available?q("marginal_team_output_and_durability"):q("individual_model_incomplete_team_fallback");
    out[q("budget_known")]=state.goldKnown;
    if(stop())return interrupted();
    return finish();
}

QJsonObject reconnectPurchasePlan(const Catalog &catalog,const GameState &state,QJsonObject plan,const QJsonObject &sandbox) {
    plan[q("purchase_sandbox")]=sandbox;
    plan[q("health_basis")]=sandbox.value(q("health_basis"));
    plan[q("diagnostic_comparison_available")]=sandbox.value(q("diagnostic_comparison_available"));
    plan[q("critical_unknowns")]=sandbox.value(q("critical_unknowns"));
    if(sandbox.contains(q("explicit_chain_condition_comparison")))plan[q("explicit_chain_condition_comparison")]=sandbox.value(q("explicit_chain_condition_comparison"));
    if(sandbox.value(q("equipment_growth_state")).isObject())plan[q("equipment_growth_state")]=sandbox.value(q("equipment_growth_state"));
    const int selected=sandbox.value(q("selected_item_id")).toInt();
    const bool interrupted=sandbox.value(q("status"))==q("interrupted");
    const bool incomplete=sandbox.contains(q("enumeration_complete"))&&!sandbox.value(q("enumeration_complete")).toBool();
    if(interrupted || incomplete || selected<=0 || !catalog.combat || !catalog.combat->items.contains(selected)) {
        // An unavailable current-purchase model cannot authorize a purchase
        // from the older final-build route, especially with a full/unknown bag.
        QJsonArray held;for(const auto &v:plan.value(q("items")).toArray())if(v.toObject().value(q("owned")).toBool())held.append(v);
        plan[q("items")]=held;plan[q("alternatives")]=QJsonArray{};plan.remove(q("offer_comparisons"));plan.remove(q("growth_projection"));plan.remove(q("comparison"));
        plan.remove(q("remaining_build_gold"));plan.remove(q("future_route_optimized_after_sandbox"));
        plan[q("route")]=interrupted?q("本次计算未完成，暂无当前购买排名"):
            sandbox.value(q("unavailable_reason"))==q("self_inventory_unobserved_or_inconsistent")?q("背包信息不完整，等待刷新"):
            sandbox.value(q("unavailable_reason"))==q("growth_inventory_transition_unverified")?q("成长装备的升级继承尚未核实，暂不生成购买排名"):
            sandbox.value(q("unavailable_reason"))==q("3116_owned_stack_and_consume_policy_unverified")?q("放倒的层数与消费顺序尚未核实，暂不生成购买排名"):
            sandbox.value(q("unavailable_reason"))==q("critical_comparison_inputs_unverified")?q("关键条件未核实，可查看估算表，暂不选出购买结论"):
            sandbox.value(q("unavailable_reason"))==q("skill_ranks_invalid")?q("技能等级信息异常，等待刷新"):q("当前没有可核实的购买方案");
        return plan;
    }
    const auto &db=*catalog.combat;
    const bool currentOnly=plan.value(q("current_purchase_only")).toBool();
    QList<int> route;QJsonArray result;
    auto append=[&](int id,bool held) {
        if((!currentOnly&&route.contains(id))||!db.items.contains(id))return;
        if(!held && !compatibleEquipment(db,route,id,state.championId,state.level))return;
        if(route.size()>=6)return;route.append(id);
        result.append(QJsonObject{{q("item_id"),id},{q("name"),catalog.itemName(id)},
            {q("price"),db.items[id].price},{q("owned"),held},{q("availability"),q("以本局商店为准")}});
    };
    // Recipe-consumed finished items cannot remain as a duplicate final slot.
    auto selectedInventory=sandbox.value(q("selected_inventory")).toArray();QSet<int> retained;
    for(const auto &v:selectedInventory)retained.insert(v.toInt());
    if(currentOnly) {
        auto heldInventory=selectedInventory;
        for(int i=0;i<heldInventory.size();++i)if(heldInventory[i].toInt()==selected){heldInventory.removeAt(i);break;}
        // Inventory is a multiset: two retained components remain two slots.
        for(const auto &v:heldInventory)append(v.toInt(),true);
    }else for(int id:state.owned)if(retained.contains(id)&&db.items.contains(id)&&db.items[id].finished)append(id,true);
    append(selected,false);
    if(!currentOnly)for(const auto &v:plan.value(q("items")).toArray()) {
        const int id=v.toObject().value(q("item_id")).toInt();
        if(state.owned.contains(id)&&!retained.contains(id))continue;
        append(id,state.owned.contains(id));
    }
    plan[q("items")]=result;
    if(sandbox.value(q("policy"))==q("explicit_chain_next_purchase_v1")) {
        plan[q("route")]=q("声明动作链下的下一件购买参考");
        plan[q("method")]=q("complete legal next-purchase transactions compared on one caller-declared chain");
        plan[q("comparison_scope")]=q("explicit_chain_condition_comparison");
        plan[q("explicit_chain_condition_comparison")]=sandbox.value(q("explicit_chain_condition_comparison"));
        plan[q("alternatives")]=QJsonArray{QJsonObject{{q("item_ids"),ids(route)},{q("policy"),q("explicit_chain_condition_comparison")},
            {q("chain_score_gain"),sandbox.value(q("selected_sandbox")).toObject().value(q("score_gain"))}}};
        plan[q("remaining_build_gold")]=sandbox.value(q("selected_sandbox")).toObject().value(q("remaining_cost"));
        plan[q("future_route_optimized_after_sandbox")]=false;
        plan.remove(q("comparison"));plan.remove(q("current_comparison"));plan.remove(q("growth_projection"));plan.remove(q("offer_comparisons"));
        return plan;
    }
    if(currentOnly) {
        // Realtime jobs already evaluated this exact inventory. They do not
        // authorize a future full-build route or another growth projection.
        plan[q("route")]=q("当前背包 · 下一件购买");
        plan[q("method")]=q("complete next-purchase enumeration on observed inventory");
        plan[q("comparison_scope")]=q("observed_inventory_plus_selected_next_purchase");
        plan[q("current_comparison")]=sandbox.value(q("current_comparison"));
        const auto candidate=sandbox.value(q("selected_sandbox")).toObject().value(q("candidate")).toObject().value(q("raw_eval")).toObject();
        if(candidate.isEmpty())plan.remove(q("comparison"));else plan[q("comparison")]=candidate;
        plan[q("alternatives")]=QJsonArray{QJsonObject{{q("item_ids"),ids(route)},
            {q("comparison_score"),candidate.value(q("score"))},
            {q("policy"),q("observed_inventory_plus_selected_next_purchase")}}};
        plan[q("remaining_build_gold")]=remainingBuildCost(db,route,state.owned);
        plan[q("future_route_optimized_after_sandbox")]=false;
        plan.remove(q("growth_projection"));plan.remove(q("offer_comparisons"));
        return plan;
    }
    plan[q("route")]=q("当前购买沙盘 · 后续路线参考");
    plan[q("method")]=q("budgeted next-purchase sandbox; conditional mechanics; reference future route");
    // The old six-item comparison must not be attributed to a different route.
    const auto preference=plan.value(q("resolved_preference")).toString(resolvedCombatPreference(catalog,state,q("按英雄定位")));
    auto context=purchaseContext(catalog,state,preference);
    plan[q("comparison")]=evaluateCombat(db,context,route,effectiveSelectedAugments(catalog,state),false,preference).json();
    plan[q("alternatives")]=QJsonArray{QJsonObject{{q("item_ids"),ids(route)},
        {q("comparison_score"),plan.value(q("comparison")).toObject().value(q("score"))},
        {q("policy"),q("sandbox_purchase_with_reference_future_route")}}};
    plan[q("remaining_build_gold")]=remainingBuildCost(db,route,state.owned);
    plan[q("future_route_optimized_after_sandbox")]=false;
    plan[q("growth_projection")]=predictCombatGrowth(db,context,route,effectiveSelectedAugments(catalog,state));
    plan.remove(q("offer_comparisons")); // These were evaluated on the previous route.
    return plan;
}
