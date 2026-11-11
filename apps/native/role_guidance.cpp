#include "role_guidance.h"
#include "analysis.h"
#include "combat.h"
#include "growth_objective.h"
#include "external_strategy_reference.h"
#include "purchase_planner.h"
#include <algorithm>
#include <cmath>
#include <functional>

namespace {
QString k(const char *s) { return QString::fromLatin1(s); }
bool integer(const QJsonValue &v,int low,int high) { return v.isDouble()&&std::isfinite(v.toDouble())&&std::floor(v.toDouble())==v.toDouble()&&v.toDouble()>=low&&v.toDouble()<=high; }
QJsonObject self(const GameState &state) { QJsonObject row;int count=0;for(const auto &v:state.players)if(v.toObject().value(k("self")).toBool()){row=v.toObject();++count;}return count==1&&integer(row.value(k("champion_id")),1,100000)&&row.value(k("champion_id")).toInt()==state.championId?row:QJsonObject{}; }
bool positiveLiteral(const CombatEntry &entry,const QJsonValue &value) {
    if(value.isDouble())return std::isfinite(value.toDouble())&&value.toDouble()>0;
    // Only an exact pinned scalar/table is accepted here. Do not evaluate an
    // expression with invented counters to infer an offensive player intent.
    if(!value.isString())return false;const auto literal=entry.values.value(value.toString());
    if(literal.isDouble())return std::isfinite(literal.toDouble())&&literal.toDouble()>0;
    if(!literal.isArray()||literal.toArray().isEmpty())return false;
    for(const auto &v:literal.toArray())if(!v.isDouble()||!std::isfinite(v.toDouble())||v.toDouble()<=0)return false;
    return true;
}
bool offensive(const CombatEntry &entry) {
    bool healingCrit=false;
    for(const auto &v:entry.effects){const auto e=v.toObject();
        healingCrit|=e.value(k("op"))==k("flag")&&e.value(k("stat"))==k("heal_crit")&&positiveLiteral(entry,e.value(k("value")));
    }
    // Healing-critical chance is a support enabler, not evidence that this
    // player chose an attack build. Explicit attack effects still qualify.
    for(const auto stat:{Stat::Ad,Stat::BonusAs,Stat::As,Stat::Crit,Stat::CritDamage,Stat::AsMultiplier})
        if(entry.stats[stat]>0&&!(healingCrit&&(stat==Stat::Crit||stat==Stat::CritDamage)))return true;
    // Explicit attack scaling/damage rules qualify. AP and generic haste do not:
    // both can improve shields/heals and must not silently override protection.
    for(const auto &v:entry.effects){const auto e=v.toObject();const auto op=e.value(k("op")).toString(),stat=e.value(k("stat")).toString();
        if((op==k("add")||op==k("amp")||op==k("bonus_amp")||op==k("stack"))&&(stat==k("ad")||stat==k("as")||stat==k("bonus_as")||(!healingCrit&&(stat==k("crit")||stat==k("crit_damage"))))&&positiveLiteral(entry,e.value(k("value"))))return true;
        if((op==k("onhit")||op==k("extra_true"))&&positiveLiteral(entry,e.value(k("value"))))return true;
    }
    return false;
}
QJsonArray observedOffense(const Catalog &c,const GameState &state,bool &complete) {
    complete=false;QJsonArray out;const auto p=self(state);const auto raw=p.value(k("augment_ids"));
    if(!c.combat||!state.fresh()||!modelRulesUsable(c,state)||p.isEmpty()||!p.value(k("augments_complete")).isBool()||!p.value(k("augments_complete")).toBool()||!raw.isArray()||raw.toArray().size()>12)return out;
    QList<int> observed;QSet<int> seen;
    for(const auto &v:raw.toArray()){if(!integer(v,1,1000000)||seen.contains(v.toInt())||!c.combat->augments.contains(v.toInt())||!c.augments.contains(v.toInt()))return out;seen.insert(v.toInt());observed.append(v.toInt());}
    auto expected=state.selected;auto sorted=observed;std::sort(expected.begin(),expected.end());std::sort(sorted.begin(),sorted.end());if(expected!=sorted)return out;
    complete=true;for(int id:observed)if(offensive(c.combat->augments.value(id)))out.append(id);return out;
}
bool inventoryVerified(const Catalog &catalog,const GameState &state) {
    const auto p=self(state);if(!catalog.combat||!state.fresh()||state.level<1||state.level>18||p.isEmpty()||!p.value(k("items_complete")).isBool()||!p.value(k("items_complete")).toBool()||!p.value(k("items")).isArray()||!integer(p.value(k("level")),1,18)||p.value(k("level")).toInt()!=state.level)return false;
    QList<int> observed;for(const auto &v:p.value(k("items")).toArray()){if(!integer(v,1,10000000))return false;observed.append(v.toInt());}
    auto owned=state.owned;std::sort(owned.begin(),owned.end());std::sort(observed.begin(),observed.end());return observed==owned&&owned.size()<=6;
}
int referencePurchaseItem(const Catalog &catalog,int sourceItem) {
    // Terminal public builds can name an automatic upgrade. Follow only the
    // pinned acquisition link; buying the base never proves its trigger fired.
    QSet<int> visited;
    while(sourceItem>0&&visited.size()<12&&!visited.contains(sourceItem)) {
        visited.insert(sourceItem);
        if(!catalog.combat||!catalog.combat->items.contains(sourceItem))return 0;
        const auto raw=catalog.items.value(sourceItem);
        if(raw.value(k("item_id"))!=QJsonValue(sourceItem)||raw.value(k("patch"))!=catalog.patch
            ||raw.value(k("kiwi_resource_candidate"))!=QJsonValue(true))return 0;
        if(raw.value(k("in_store"))==QJsonValue(true))return sourceItem;
        int parent=0;
        for(const auto &reason:catalog.combat->items.value(sourceItem).purchaseExclusions) {
            if(!reason.startsWith(k("specialRecipe:")))continue;
            bool valid=false;const int id=reason.mid(14).toInt(&valid);
            if(!valid||id<=0||(parent&&parent!=id))return 0;
            parent=id;
        }
        if(!parent)return 0;
        sourceItem=parent;
    }
    return 0;
}
int observedReferenceItem(const Catalog &catalog,const GameState &state,int sourceItem) {
    if(state.owned.contains(sourceItem))return sourceItem;
    const int base=referencePurchaseItem(catalog,sourceItem);
    if(base&&state.owned.contains(base))return base;
    // A held transformed item also satisfies a public route naming its base.
    if(base)for(int held:state.owned)if(referencePurchaseItem(catalog,held)==base)return held;
    // Public core rows sometimes retain a starting component alongside its
    // completed upgrade. Its physical recipe credit is already in that item.
    for(int held:state.owned){
        const int purchase=referencePurchaseItem(catalog,held);QSet<int> visited;
        QList<int> pending=catalog.combat->items.value(purchase?purchase:held).from;
        while(!pending.isEmpty()&&visited.size()<4096){const int part=pending.takeLast();if(visited.contains(part))continue;visited.insert(part);
            if(part==sourceItem)return held;
            pending.append(catalog.combat->items.value(part).from);
        }
    }
    return 0;
}
bool supportReferenceSemantics(const CombatEntry &entry) {
    if(entry.stats[Stat::HealShield]>0)return true;
    const auto event=entry.triggerContract.value(k("event")).toString();
    return event.contains(k("ally"))&&(event.contains(k("heal"))||event.contains(k("shield")));
}
bool afterPurchase(const CombatDatabase &db,const GameState &state,int id,QList<int> &result) {
    result=state.owned;bool valid=true;QSet<int> visiting;
    std::function<void(int,int)> consume=[&](int part,int depth){if(depth>12||!db.items.contains(part)||visiting.contains(part)){valid=false;return;}if(result.removeOne(part))return;visiting.insert(part);for(int child:db.items.value(part).from)consume(child,depth+1);visiting.remove(part);};
    for(int part:db.items.value(id).from)consume(part,0);if(!valid)return false;result.append(id);if(result.size()>6)return false;
    QMap<QString,int> groups;QSet<int> unique;
    for(int held:result){const auto i=db.items.constFind(held);if(i==db.items.cend()||i->requiredLevel>state.level||(state.championId==69&&i->boots)||(i->eligible&&unique.contains(held)))return false;unique.insert(held);for(auto g=i->groups.cbegin();g!=i->groups.cend();++g)if(++groups[g.key()]>g.value())return false;}
    return true;
}
}

QJsonObject combatRoleIntent(const Catalog &catalog,const GameState &state,const QString &requestedStyle) {
    const auto roles=catalog.champion(state.championId).value(k("roles")).toArray();const bool support=roles.contains(k("support")),tank=roles.contains(k("tank"));
    const bool primarySupport=!roles.isEmpty()&&roles.first()==k("support");
    bool observed=false;auto offense=observedOffense(catalog,state,observed);bool scenarioComplete=false;
    if(state.source==GameState::Source::Offline&&catalog.combat){
        scenarioComplete=true;QSet<int> seen;
        for(int id:state.selected)if(id<=0||seen.contains(id)||!catalog.combat->augments.contains(id)){scenarioComplete=false;break;}else seen.insert(id);
        if(scenarioComplete)for(int id:state.selected)if(offensive(catalog.combat->augments.value(id)))offense.append(id);
    }
    // This is a missing-utility safeguard, not an offensive/defensive role
    // weight. An observed or explicitly assumed typed attack effect may be
    // compared; AP/haste/healing crit alone never proves ally utility is zero.
    const bool augmentDamage=primarySupport&&!offense.isEmpty();
    const bool protection=primarySupport&&!augmentDamage;
    const auto intent=protection?k("ally_protection"):k("growth");
    return {{k("schema_version"),2},{k("intent"),intent},{k("catalog_roles"),roles},{k("automatic_role_preference"),false},
        {k("role_matching_enabled"),false},{k("fixed_direction_matching_enabled"),false},{k("requested_direction_ignored"),!requestedStyle.isEmpty()},
        {k("support_role"),support},{k("primary_support_role"),primarySupport},{k("tank_role"),tank},{k("explicit_output_preference"),false},{k("explicit_protection_preference"),false},{k("observed_offensive_augment_route"),augmentDamage&&observed},
        {k("augment_observation_complete_and_matching"),observed},{k("offensive_augment_ids"),offense},
        {k("explicit_offline_augment_scenario_complete"),scenarioComplete},{k("selected_typed_offensive_effects_available"),augmentDamage},
        {k("reference_comparison_style"),growthCombatPreference()},
        {k("comparison_label"),protection?QStringLiteral("友军收益尚待量化"):QStringLiteral("成长联动候选")},
        {k("ally_utility_calculated"),false},{k("intent_source"),k("unquantified_ally_utility_guard_and_selected_typed_effects_not_direction_matching")},
        {k("offensive_augment_is_player_preference"),false},{k("full_hero_optimum"),false}};
}

QJsonObject roleAwareRecommendation(const Catalog &catalog,const GameState &state,const QString &requestedStyle,const QJsonObject &computedPlan) {
    const auto intent=combatRoleIntent(catalog,state,requestedStyle);const auto computed=computedPlan.value(k("purchase_sandbox")).toObject();
    QJsonObject out{{k("schema_version"),1},{k("role_intent"),intent},{k("comparison_label"),intent.value(k("comparison_label"))},{k("numeric_comparison_label"),intent.value(k("comparison_label"))},
        {k("computed_selected_item_id"),computed.value(k("selected_item_id"))},{k("computed_winner_changed"),false},
        {k("source_reference_is_model_winner"),false},{k("numeric_utility_added"),false},{k("real_win_rate"),QJsonValue::Null},
        {k("routes"),QJsonArray{}},{k("next_purchase_reference"),QJsonObject{}},{k("available"),false},{k("reference_only"),true},
        {k("purchase_order_verified"),false},{k("source_route_statistics_not_purchase_order"),true}};
    const auto reference=externalStrategyReference(catalog,state);out[k("external_status")]=reference.value(k("status"));out[k("revision")]=reference.value(k("revision"));
    if(!reference.value(k("available")).toBool()){out[k("reason")]=k("same_patch_mode_public_route_unavailable");return out;}
    QList<QJsonObject> builds;for(const auto &v:reference.value(k("records")).toArray()){const auto row=v.toObject(),data=row.value(k("data")).toObject();if(row.value(k("kind"))==k("item_build")&&data.value(k("category"))==k("core_items"))builds.append(row);}
    std::sort(builds.begin(),builds.end(),[](const QJsonObject &a,const QJsonObject &b){const auto ad=a.value(k("data")).toObject(),bd=b.value(k("data")).toObject();const int ai=ad.value(k("row_index")).toInt(1000000),bi=bd.value(k("row_index")).toInt(1000000);if(ai!=bi)return ai<bi;return a.value(k("id")).toString()<b.value(k("id")).toString();});
    QJsonArray routes;bool supportRoute=false;const bool protection=intent.value(k("intent"))==k("ally_protection");const bool ready=inventoryVerified(catalog,state);const bool budget=state.goldKnown&&std::isfinite(state.gold)&&state.gold>=0;
    for(const auto &row:builds){if(routes.size()>=5)break;auto route=row;route[k("provider_order_not_combat_rank")]=true;route[k("purchase_order_verified")]=false;route[k("legal_next_purchase_verified")]=false;
        bool supportsAllies=false;if(catalog.combat)for(const auto &v:row.value(k("data")).toObject().value(k("item_ids")).toArray())if(catalog.combat->items.contains(v.toInt())&&supportReferenceSemantics(catalog.combat->items.value(v.toInt())))supportsAllies=true;
        route[k("has_ally_support_reference_semantics")]=supportsAllies;route[k("role_reference_basis")]=k("source_route_items_with_pinned_heal_shield_stat_or_ally_heal_shield_contract_not_calculated_utility");supportRoute|=supportsAllies;
        for(const auto &v:row.value(k("data")).toObject().value(k("item_ids")).toArray()){
            const int id=v.toInt();if(!catalog.combat||!catalog.combat->items.contains(id)||state.owned.contains(id))continue;const auto item=catalog.combat->items.value(id);
            if(!item.eligible||!item.finished||item.price<=0||item.requiredLevel>state.level)continue;
            QList<int> result;if(!ready||!afterPurchase(*catalog.combat,state,id,result))continue;const int cost=remainingBuildCost(*catalog.combat,{id},state.owned);if(cost<=0)continue;
            // Terminal core-build rows do not establish a purchase sequence.
            // Keep one legal unowned element as a reference; do not reorder an
            // unaffordable row into an invented immediate shopping instruction.
            route[k("first_legal_unowned_core_item")]=QJsonObject{{k("item_id"),id},{k("name"),catalog.itemName(id)},{k("remaining_cost"),cost},{k("affordable"),budget?QJsonValue(cost<=state.gold):QJsonValue::Null}};
            if(budget&&cost<=state.gold){route[k("legal_next_purchase_verified")]=true;if((!protection||supportsAllies)&&out.value(k("next_purchase_reference")).toObject().isEmpty())out[k("next_purchase_reference")]=QJsonObject{{k("item_id"),id},{k("name"),catalog.itemName(id)},{k("remaining_cost"),cost},{k("affordable"),true},{k("source_record_id"),row.value(k("id"))},{k("source_url"),row.value(k("source_url"))},{k("source_sha256"),row.value(k("source_sha256"))},{k("region"),row.value(k("region"))},{k("scope"),k("legal_source_route_reference_not_sandbox_winner")}};}
            break;
        }
        routes.append(route);
    }
    out[k("routes")]=routes;out[k("available")]=!routes.isEmpty();out[k("inventory_verified")]=ready;out[k("budget_known")]=budget;out[k("sources")]=reference.value(k("sources"));
    out[k("reason")]=routes.isEmpty()?k("no_core_build_reference"):k("source_routes_separate_from_computed_winner");
    out[k("has_ally_support_reference_route")]=supportRoute;
    if(supportRoute&&protection)out[k("comparison_label")]=QStringLiteral("辅助路线参考");
    out[k("prefer_source_route_for_display")]=supportRoute&&protection;
    out[k("route_choice_is_objective_optimum")]=false;return out;
}

QJsonObject sourcePurchaseReference(const Catalog &catalog,const GameState &state){
    QJsonObject out{{k("available"),false},{k("reference_only"),true},{k("ranking_available"),false},
        {k("computed_winner_changed"),false},{k("actual_shop_price_verified"),false},
        {k("augments_complete_required"),false},{k("unknown_augments_assumed_empty"),false},
        {k("scope"),k("public_build_reference_and_observed_material_continuation")},
        {k("purchase_order_verified"),false},{k("components"),QJsonArray{}}};
    if(!catalog.combat||!inventoryVerified(catalog,state)||!purchaseReferenceReady(catalog,state)
        ||state.gameMode!=k("KIWI")||(state.queueId>0&&state.queueId!=2400))return out;
    if(state.source==GameState::Source::Live&&((state.phase!=k("InProgress")&&state.phase!=k("Reconnect"))
        ||!state.matchContext.value(k("active")).toBool()||state.matchContext.value(k("context_id")).toString().isEmpty()))return out;
    const auto reference=externalEquipmentReference(catalog,state);if(!reference.value(k("available")).toBool())return out;
    const auto &db=*catalog.combat;
    struct Target{int id=0,sourceId=0,credit=0,routeOwned=0,sourceOrder=0,position=0;bool sourceOrdered=false;QJsonObject quote,record;};
    QList<Target> targets;
    int strongestOwnedCore=0;
    const bool starting=state.owned.isEmpty()&&state.gameTime<120;
    for(const auto &v:reference.value(k("records")).toArray()){
        const auto row=v.toObject(),data=row.value(k("data")).toObject();
        if(row.value(k("kind"))!=k("item_build")||data.value(k("category"))!=(starting?k("starting_items"):k("core_items")))continue;
        const auto ids=data.value(k("item_ids")).toArray();int routeOwned=0,position=0;
        QSet<int> credited;
        for(const auto &id:ids){const int held=observedReferenceItem(catalog,state,id.toInt());if(held&&!credited.contains(held)){credited.insert(held);routeOwned+=std::max(0,db.items.value(held).price);}}
        strongestOwnedCore=std::max(strongestOwnedCore,routeOwned);
        for(const auto &id:ids){const int sourceItem=id.toInt();++position;if(observedReferenceItem(catalog,state,sourceItem))continue;
            const int item=referencePurchaseItem(catalog,sourceItem);if(!item)continue;
            if(!purchaseReferenceItemCompatible(catalog,state,item))continue;
            const auto quote=planInventoryRecipeReference(catalog,state.championId,state.level,state.owned,item);
            if(!quote.value(k("legal")).toBool()||quote.value(k("remaining_cost")).toInt()<=0)continue;
            int credit=0;for(const auto &part:quote.value(k("consume_item_ids")).toArray())credit+=std::max(0,db.items.value(part.toInt()).price);
            targets.append(Target{item,sourceItem,credit,routeOwned,data.value(k("row_index")).toInt(1000000),position,data.value(k("ordered")).toBool(),quote,row});
        }
    }
    // Completed source rows still anchor the observed loadout. Exhausting one
    // must not silently restart an unrelated combination with no held credit.
    if(strongestOwnedCore>0)targets.erase(std::remove_if(targets.begin(),targets.end(),[](const Target &target){return target.routeOwned==0&&target.credit==0;}),targets.end());
    if(targets.isEmpty()&&strongestOwnedCore>0)out[k("reason")]=k("observed_core_references_exhausted");
    if(targets.isEmpty())return out;
    std::sort(targets.begin(),targets.end(),[starting](const Target &a,const Target &b){
        if(a.routeOwned!=b.routeOwned)return a.routeOwned>b.routeOwned;
        if(a.credit!=b.credit)return a.credit>b.credit;
        if(a.sourceOrder!=b.sourceOrder)return a.sourceOrder<b.sourceOrder;
        // With equal observed inventory/material support, retain the source's
        // displayed sequence instead of silently choosing its cheapest core.
        // This is an explicit public ordering reference, never an optimality claim.
        if((starting||(a.sourceOrdered&&b.sourceOrdered))&&a.position!=b.position)return a.position<b.position;
        const int ac=a.quote.value(k("remaining_cost")).toInt(),bc=b.quote.value(k("remaining_cost")).toInt();
        if(ac!=bc)return ac<bc;return a.id<b.id;
    });
    const auto target=targets.first();const int targetCost=target.quote.value(k("remaining_cost")).toInt();
    const bool budget=state.goldKnown&&std::isfinite(state.gold)&&state.gold>=0;
    QJsonArray comparisons;QSet<QString> compared;
    for(const auto &candidate:targets){
        const auto record=candidate.record;const auto id=record.value(k("id")).toString();if(compared.contains(id))continue;compared.insert(id);
        const auto data=record.value(k("data")).toObject();
        comparisons.append(QJsonObject{{k("source_record_id"),id},{k("source_url"),record.value(k("source_url"))},
            {k("source_sha256"),record.value(k("source_sha256"))},{k("retrieved_at"),record.value(k("retrieved_at"))},
            {k("region"),record.value(k("region"))},{k("route_item_ids"),data.value(k("item_ids"))},
            {k("target_item_id"),candidate.id},{k("source_target_item_id"),candidate.sourceId},
            {k("observed_route_item_value"),candidate.routeOwned},{k("consumed_material_value"),candidate.credit},
            {k("remaining_cost"),candidate.quote.value(k("remaining_cost"))},{k("source_row_index"),candidate.sourceOrder},
            {k("source_item_position"),candidate.position},{k("source_order_reference"),candidate.sourceOrdered},
            {k("selected_for_reference"),id==target.record.value(k("id")).toString()},
            {k("published_win_rate"),data.value(k("win_rate")).isDouble()?data.value(k("win_rate")):QJsonValue(QJsonValue::Null)},
            {k("published_sample_size"),data.value(k("sample_size")).isDouble()?data.value(k("sample_size")):QJsonValue(QJsonValue::Null)}});
        if(comparisons.size()>=5)break;
    }
    struct Step{int id,cost,credit;};QList<Step> steps;
    QSet<int> visited;QList<int> pending=db.items.value(target.id).from;
    while(!pending.isEmpty()&&visited.size()<4096){const int part=pending.takeLast();if(visited.contains(part))continue;visited.insert(part);pending.append(db.items.value(part).from);
        const auto quote=planInventoryRecipeReference(catalog,state.championId,state.level,state.owned,part);const int cost=quote.value(k("remaining_cost")).toInt();
        if(!budget||!quote.value(k("legal")).toBool()||cost<=0||cost>state.gold)continue;
        QList<int> after;for(const auto &id:quote.value(k("inventory_after")).toArray())after.append(id.toInt());
        const auto completion=planInventoryRecipeReference(catalog,state.championId,state.level,after,target.id);
        // A quote must reduce this exact target by the paid amount, including
        // physical multiset consumption. Unrelated cheap items are excluded.
        if(!completion.value(k("legal")).toBool()||completion.value(k("remaining_cost")).toInt()+cost!=targetCost)continue;
        int credit=0;for(const auto &id:quote.value(k("consume_item_ids")).toArray())credit+=std::max(0,db.items.value(id.toInt()).price);
        steps.append(Step{part,cost,credit});
    }
    std::sort(steps.begin(),steps.end(),[](const Step &a,const Step &b){if(a.credit!=b.credit)return a.credit>b.credit;if(a.cost!=b.cost)return a.cost>b.cost;return a.id<b.id;});
    int action=target.id,actionCost=targetCost;
    if(budget&&targetCost>state.gold&&!steps.isEmpty()){action=steps.first().id;actionCost=steps.first().cost;}
    out[k("available")]=true;out[k("context_id")]=state.matchContext.value(k("context_id"));out[k("patch")]=catalog.patch;
    out[k("observed_patch")]=state.observedPatch;out[k("source_patch_matches_current")]=state.observedPatch==catalog.patch;
    out[k("combat_version_match")]=modelPatchMatches(catalog,state);
    out[k("target_item_id")]=target.id;out[k("target_remaining_cost")]=targetCost;
    out[k("source_target_item_id")]=target.sourceId;
    out[k("source_order_reference")]=target.sourceOrdered;
    out[k("source_item_position")]=target.position;
    out[k("route_comparison")]=comparisons;
    out[k("comparison_basis")]=k("observed_inventory_then_material_credit_then_public_display_order");
    out[k("statistical_ranking_available")]=false;
    out[k("requires_later_transformation")]=target.sourceId!=target.id;
    out[k("transformation_observed")]=false;
    out[k("action_item_id")]=action;out[k("action_cost")]=actionCost;
    out[k("budget_known")]=budget;out[k("within_catalog_budget")]=budget?QJsonValue(actionCost<=state.gold):QJsonValue::Null;
    out[k("current_gold")]=budget?QJsonValue(state.gold):QJsonValue::Null;
    out[k("basis")]=target.credit>0?k("continue_observed_materials"):target.routeOwned>0?k("continue_observed_core_combination"):starting?k("public_starting_item_reference"):k("public_core_combination_reference");
    out[k("route_item_ids")]=target.record.value(k("data")).toObject().value(k("item_ids"));
    out[k("source_record_id")]=target.record.value(k("id"));out[k("source_url")]=target.record.value(k("source_url"));
    out[k("source_sha256")]=target.record.value(k("source_sha256"));out[k("region")]=target.record.value(k("region"));out[k("revision")]=reference.value(k("revision"));
    if(action!=target.id&&budget&&actionCost<=state.gold)out[k("components")]=QJsonArray{QJsonObject{{k("item_id"),action},{k("remaining_cost"),actionCost}}};
    return out;
}
