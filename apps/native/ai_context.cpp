#include "ai_context.h"
#include "external_strategy_reference.h"
#include "semantic_evidence.h"
#include "headliner.h"
#include "mechanic_triggers.h"
#include "coach_knowledge.h"
#include <QRegularExpression>
#include <QSet>
namespace {
QJsonObject compactGrowth(const QJsonObject &full){
    auto out=full;QJsonArray rows;const int start=full.value(QStringLiteral("start_level")).toInt();
    for(const auto &v:full.value(QStringLiteral("rows")).toArray()){const auto row=v.toObject();const int level=row.value(QStringLiteral("level")).toInt();if(level==start||level==start+1||QList<int>{7,11,15,18}.contains(level))rows.append(row);}
    out[QStringLiteral("rows")]=rows;out[QStringLiteral("presentation")]=QStringLiteral("Current, next and milestone levels; complete per-level rows remain in the local result.");return out;
}
}
QJsonObject lockOverlayAiContext(const QJsonObject &context) {
    // A public build can be explained without becoming a locked current-game
    // action. Budget, legal acquisition and live objectives are separate.
    if(context.value(QStringLiteral("reference_only")).toBool()
        ||context.value(QStringLiteral("conditional_reference")).toBool()
        ||context.value(QStringLiteral("growth_conditioned_candidates")).toBool())return {};
    auto out=context;
    const auto routes=context.value(QStringLiteral("candidate_loadouts")).toArray();
    QJsonObject route;
    for(const auto &value:routes){const auto candidate=value.toObject();if(!candidate.value(QStringLiteral("reference_only")).toBool()&&candidate.value(QStringLiteral("requires_augment_id")).toInt()==0&&!candidate.value(QStringLiteral("item_ids")).toArray().isEmpty()){route=candidate;break;}}
    if(route.isEmpty())return {};
    const auto next=context.value(QStringLiteral("coach_economy")).toObject().value(QStringLiteral("next_purchase")).toObject();
    const auto routeItems=route.value(QStringLiteral("item_ids")).toArray();
    if(next.value(QStringLiteral("item_id")).toInt()>0&&!routeItems.contains(next.value(QStringLiteral("item_id"))))return {};
    QJsonArray selected,items,mechanics;QSet<int> selectedIds;QStringList forbidden;
    for(const auto &value:context.value(QStringLiteral("augments")).toArray()){
        const auto card=value.toObject();
        if(card.value(QStringLiteral("selected")).toBool()){selected.append(card);selectedIds.insert(card.value(QStringLiteral("id")).toInt());}
        else if(card.value(QStringLiteral("currently_offered")).toBool()&&!card.value(QStringLiteral("name")).toString().isEmpty())forbidden.append(card.value(QStringLiteral("name")).toString());
    }
    for(const auto &value:context.value(QStringLiteral("candidate_mechanic_contracts")).toArray()){
        const auto mechanic=value.toObject();const int id=mechanic.value(QStringLiteral("id")).toInt();
        if((mechanic.value(QStringLiteral("source")).toString()==QStringLiteral("item")&&routeItems.contains(id))||(mechanic.value(QStringLiteral("source")).toString()==QStringLiteral("augment")&&selectedIds.contains(id)))mechanics.append(mechanic);
    }
    out[QStringLiteral("candidate_mechanic_contracts")]=mechanics;
    for(const auto &value:context.value(QStringLiteral("candidate_items")).toArray()){
        const auto item=value.toObject();const int id=item.value(QStringLiteral("item_id")).toInt();
        if(routeItems.contains(id))items.append(item);
        if(id!=next.value(QStringLiteral("item_id")).toInt()&&!item.value(QStringLiteral("name")).toString().isEmpty())forbidden.append(item.value(QStringLiteral("name")).toString());
    }
    out[QStringLiteral("candidate_loadouts")]=QJsonArray{route};out[QStringLiteral("candidate_items")]=items;out[QStringLiteral("augments")]=selected;
    QSet<int> lockedItemIds;for(const auto &id:routeItems)lockedItemIds.insert(id.toInt());
    out[QStringLiteral("candidate_semantic_evidence")]=filterCandidateSemanticEvidence(
        context.value(QStringLiteral("candidate_semantic_evidence")).toObject(),
        context.value(QStringLiteral("champion_id")).toInt(),lockedItemIds,selectedIds);
    for(const auto &key:{QStringLiteral("offer_comparisons"),QStringLiteral("augment_decision"),QStringLiteral("growth_projection"),QStringLiteral("teamfight_scenario")})out.remove(key);
    out[QStringLiteral("advice_contract")]=QJsonObject{{QStringLiteral("kind"),QStringLiteral("local_locked")},{QStringLiteral("allowed_loadout_index"),0},
        {QStringLiteral("next_item_id"),next.value(QStringLiteral("item_id"))},{QStringLiteral("forbidden_advice_names"),QJsonArray::fromStringList(forbidden)},
        {QStringLiteral("action_title"),next.value(QStringLiteral("item_id")).toInt()>0?QStringLiteral("下一件：%1").arg(next.value(QStringLiteral("name")).toString()):QStringLiteral("保持当前配装")}};
    return out;
}
bool validateOverlayAiAdvice(QJsonObject &plan,const QJsonObject &context,QString *error) {
    const auto contract=context.value(QStringLiteral("advice_contract")).toObject();
    if(contract.value(QStringLiteral("kind")).toString()!=QStringLiteral("local_locked"))return true;
    const auto choices=plan.value(QStringLiteral("choices")).toArray();
    if(choices.size()!=1){*error=QStringLiteral("只允许同一条本地路线");return false;}
    auto choice=choices.first().toObject();
    if(!choice.value(QStringLiteral("loadout_index")).isDouble()||choice.value(QStringLiteral("loadout_index")).toDouble()!=0){*error=QStringLiteral("路线与本地结果冲突");return false;}
    const auto reason=choice.value(QStringLiteral("reason")).toString().trimmed();
    const auto tradeoff=choice.value(QStringLiteral("tradeoffs")).toString().trimmed();
    const auto text=reason+tradeoff;
    static const QRegularExpression unsafe(QStringLiteral("[0-9%％\\r\\n]|不建议|不推荐|不要|别买|别出|放弃|重随|必选|最强|选左|选中|选右|胜率|评分|回城|回泉水"));
    if(reason.isEmpty()||reason.size()>28||tradeoff.size()>16||unsafe.match(text).hasMatch()){*error=QStringLiteral("提示超长或改变本地结论");return false;}
    for(const auto &value:contract.value(QStringLiteral("forbidden_advice_names")).toArray())if(text.contains(value.toString())){*error=QStringLiteral("提示混入另一张牌或装备");return false;}
    const QJsonObject clean{{QStringLiteral("title"),contract.value(QStringLiteral("action_title"))},{QStringLiteral("reason"),reason},{QStringLiteral("tradeoffs"),tradeoff},
        {QStringLiteral("loadout_index"),0},{QStringLiteral("item_ids"),choice.value(QStringLiteral("item_ids"))},{QStringLiteral("augment_ids"),choice.value(QStringLiteral("augment_ids"))}};
    plan=QJsonObject{{QStringLiteral("choices"),QJsonArray{clean}},{QStringLiteral("decision_source"),QStringLiteral("local_locked")},{QStringLiteral("action_item_id"),contract.value(QStringLiteral("next_item_id"))}};return true;
}
bool overlayAdviceMatchesAction(const QJsonObject &plan,const QJsonObject &coach){
    if(coach.value(QStringLiteral("plan")).toObject().value(QStringLiteral("reference_only")).toBool())return false;
    const auto choices=plan.value(QStringLiteral("choices")).toArray();
    if(plan.value(QStringLiteral("decision_source"))!=QStringLiteral("local_locked")||choices.size()!=1)return false;
    const auto next=coach.value(QStringLiteral("economy")).toObject().value(QStringLiteral("next_purchase")).toObject();
    if(!plan.value(QStringLiteral("action_item_id")).isDouble()||plan.value(QStringLiteral("action_item_id")).toInt()!=next.value(QStringLiteral("item_id")).toInt())return false;
    QSet<int> local,received;for(const auto &v:coach.value(QStringLiteral("plan")).toObject().value(QStringLiteral("items")).toArray())local.insert(v.toObject().value(QStringLiteral("item_id")).toInt());
    for(const auto &v:choices.first().toObject().value(QStringLiteral("item_ids")).toArray())received.insert(v.toInt());
    return !local.isEmpty()&&local==received;
}
AiCandidateContext makeAiCandidateContext(const Catalog &catalog,const GameState &state,int championId,const QString &preference,const QJsonObject *computedPlan) {
    AiCandidateContext result;
    const auto plan=computedPlan?*computedPlan:recommendations(catalog,state,championId,preference);QList<int> &items=result.items;QJsonArray candidates;
    for(const auto &v:plan.value(QStringLiteral("items")).toArray()){auto item=v.toObject();int id=item.value(QStringLiteral("item_id")).toInt();items.append(id);item.insert(QStringLiteral("description"),plainText(catalog.items.value(id).value(QStringLiteral("description_html")).toString()));candidates.append(item);}
    for(const auto &v:plan.value(QStringLiteral("offer_comparisons")).toArray())for(const auto &value:v.toObject().value(QStringLiteral("adapted_item_ids")).toArray()){
        const int id=value.toInt();if(items.contains(id))continue;items.append(id);const auto item=catalog.items.value(id);
        candidates.append(QJsonObject{{QStringLiteral("item_id"),id},{QStringLiteral("name"),catalog.itemName(id)},{QStringLiteral("price"),item.value(QStringLiteral("price_total"))},{QStringLiteral("description"),plainText(item.value(QStringLiteral("description_html")).toString())}});
    }
    for(const auto &v:plan.value(QStringLiteral("alternatives")).toArray())for(const auto &value:v.toObject().value(QStringLiteral("item_ids")).toArray()){
        const int id=value.toInt();if(items.contains(id))continue;items.append(id);const auto item=catalog.items.value(id);
        candidates.append(QJsonObject{{QStringLiteral("item_id"),id},{QStringLiteral("name"),catalog.itemName(id)},{QStringLiteral("price"),item.value(QStringLiteral("price_total"))},{QStringLiteral("description"),plainText(item.value(QStringLiteral("description_html")).toString())}});
    }
    const bool referencePlan=plan.value(QStringLiteral("reference_only")).toBool();
    const bool growthCandidates=plan.value(QStringLiteral("growth_conditioned_candidates")).toBool();
    const auto reasoning=plan.value(QStringLiteral("build_reasoning")).toObject();
    const auto referenceRoutes=plan.contains(QStringLiteral("reference_routes"))?plan.value(QStringLiteral("reference_routes")).toArray()
        :plan.value(QStringLiteral("role_aware_recommendation")).toObject().value(QStringLiteral("routes")).toArray();
    auto appendReferenceItem=[&](int id){
        if(id<=0||!catalog.items.contains(id)||items.contains(id)||items.size()>=96)return;
        const auto source=catalog.items.value(id);QJsonObject explanation;
        for(const auto &value:reasoning.value(QStringLiteral("items")).toArray())if(value.toObject().value(QStringLiteral("item_id")).toInt()==id){explanation=value.toObject();break;}
        auto availability=explanation.value(QStringLiteral("availability")).toObject();
        if(!availability.contains(QStringLiteral("acquisition_kind")))availability[QStringLiteral("acquisition_kind")]=source.value(QStringLiteral("in_store")).toBool()?QStringLiteral("resource_shop_candidate"):QStringLiteral("upgrade_or_other_acquisition_unverified");
        availability[QStringLiteral("in_store_resource_flag")]=source.value(QStringLiteral("in_store"));
        availability[QStringLiteral("direct_purchase_verified")]=false;
        items.append(id);candidates.append(QJsonObject{{QStringLiteral("item_id"),id},{QStringLiteral("name"),catalog.itemName(id)},
            {QStringLiteral("price"),source.value(QStringLiteral("price_total"))},{QStringLiteral("description"),plainText(source.value(QStringLiteral("description_html")).toString())},
            {QStringLiteral("reference_only"),true},{QStringLiteral("legal_purchase_verified"),false},{QStringLiteral("numeric_winner"),false},
            {QStringLiteral("availability"),availability},{QStringLiteral("build_reasoning"),explanation}});
    };
    if(referencePlan){
        if(growthCandidates)for(const auto &route:plan.value(QStringLiteral("candidate_loadouts")).toArray())
            for(const auto &id:route.toObject().value(QStringLiteral("item_ids")).toArray())appendReferenceItem(id.toInt());
        if(!growthCandidates){
            for(const auto &id:plan.value(QStringLiteral("reference_item_ids")).toArray())appendReferenceItem(id.toInt());
            for(const auto &route:referenceRoutes)for(const auto &id:route.toObject().value(QStringLiteral("data")).toObject().value(QStringLiteral("item_ids")).toArray())appendReferenceItem(id.toInt());
        }
        for(qsizetype index=0;index<candidates.size();++index){auto item=candidates[index].toObject();const int id=item.value(QStringLiteral("item_id")).toInt();
            QJsonObject explanation;for(const auto &value:reasoning.value(QStringLiteral("items")).toArray())if(value.toObject().value(QStringLiteral("item_id")).toInt()==id){explanation=value.toObject();break;}
            auto availability=explanation.value(QStringLiteral("availability")).toObject();
            if(!availability.contains(QStringLiteral("acquisition_kind")))availability[QStringLiteral("acquisition_kind")]=catalog.items.value(id).value(QStringLiteral("in_store")).toBool()?QStringLiteral("resource_shop_candidate"):QStringLiteral("upgrade_or_other_acquisition_unverified");
            availability[QStringLiteral("in_store_resource_flag")]=catalog.items.value(id).value(QStringLiteral("in_store"));availability[QStringLiteral("direct_purchase_verified")]=false;
            item[QStringLiteral("availability")]=availability;item[QStringLiteral("build_reasoning")]=explanation;
            item[QStringLiteral("reference_only")]=true;item[QStringLiteral("legal_purchase_verified")]=false;item[QStringLiteral("numeric_winner")]=false;candidates[index]=item;
        }
    }
    QList<int> &augments=result.augments;augments=state.offers;for(int id:state.selected)if(!augments.contains(id))augments.append(id);QJsonArray cardsData;
    for(int id:augments){auto a=catalog.augments.value(id);cardsData.append(QJsonObject{{QStringLiteral("id"),id},{QStringLiteral("name"),a.value(QStringLiteral("name_zh_cn"))},{QStringLiteral("description"),a.value(QStringLiteral("description"))},{QStringLiteral("currently_offered"),state.offers.contains(id)},{QStringLiteral("selected"),state.selected.contains(id)},{QStringLiteral("unresolved_values"),a.value(QStringLiteral("unresolved_template_tokens"))}});}
    QJsonArray abilityData;for(const auto &v:catalog.champion(championId).value(QStringLiteral("abilities")).toArray()){auto a=v.toObject();auto slot=a.value(QStringLiteral("spellKey")).toString().toUpper();if(slot==QStringLiteral("PASSIVE"))slot=QStringLiteral("P");abilityData.append(QJsonObject{{QStringLiteral("key"),slot},{QStringLiteral("name"),a.value(QStringLiteral("name"))},{QStringLiteral("description"),a.value(QStringLiteral("description"))},{QStringLiteral("catalog_cost_text"),a.value(QStringLiteral("cost"))},{QStringLiteral("catalog_cooldown_text"),a.value(QStringLiteral("cooldown"))},{QStringLiteral("catalog_range_text"),a.value(QStringLiteral("range"))}});}
    QJsonObject heroProfile;QJsonArray skillParameters;
    if(catalog.combat){const auto hero=catalog.combat->champions.value(championId);for(const auto &key:{QStringLiteral("base"),QStringLiteral("growth"),QStringLiteral("ranged"),QStringLiteral("primary_resource"),QStringLiteral("passive_stat_effects"),QStringLiteral("uses_mana"),QStringLiteral("roles"),QStringLiteral("title")})heroProfile[key]=hero.value(key);
        for(const auto &v:hero.value(QStringLiteral("spells")).toArray()){const auto spell=v.toObject();if(spell.value(QStringLiteral("slot"))!=QStringLiteral("extra"))skillParameters.append(spell);}}
    if(catalog.combat)heroProfile[QStringLiteral("admitted_stat_reasoning")]=catalog.combat->champions.value(championId).value(QStringLiteral("hero_growth_equipment_reasoning_v1"));
    QJsonArray loadouts=growthCandidates?plan.value(QStringLiteral("candidate_loadouts")).toArray():plan.value(QStringLiteral("alternatives")).toArray();
    for(const auto &v:plan.value(QStringLiteral("offer_comparisons")).toArray()){auto o=v.toObject();if(!o.value(QStringLiteral("adapted_item_ids")).toArray().isEmpty())loadouts.append(QJsonObject{{QStringLiteral("item_ids"),o.value(QStringLiteral("adapted_item_ids"))},{QStringLiteral("requires_augment_id"),o.value(QStringLiteral("augment_id"))}});}
    if(growthCandidates){
        for(qsizetype index=0;index<loadouts.size();++index){auto route=loadouts[index].toObject();
            route[QStringLiteral("reference_only")]=true;route[QStringLiteral("growth_conditioned_candidates")]=true;
            route[QStringLiteral("numeric_winner")]=false;route[QStringLiteral("legal_purchase_verified")]=false;
            route[QStringLiteral("purchase_order_verified")]=false;loadouts[index]=route;
        }
    }else if(referencePlan){
        loadouts=QJsonArray{};
        for(const auto &value:referenceRoutes){if(loadouts.size()>=5)break;const auto route=value.toObject();QJsonArray ids;
            for(const auto &id:route.value(QStringLiteral("data")).toObject().value(QStringLiteral("item_ids")).toArray())if(items.contains(id.toInt())&&!ids.contains(id)&&ids.size()<6)ids.append(id);
            if(ids.isEmpty())continue;
            loadouts.append(QJsonObject{{QStringLiteral("item_ids"),ids},{QStringLiteral("reference_only"),true},{QStringLiteral("requires_augment_id"),0},
                {QStringLiteral("numeric_winner"),false},{QStringLiteral("legal_purchase_verified"),false},{QStringLiteral("purchase_order_verified"),false},
                {QStringLiteral("source_record_id"),route.value(QStringLiteral("id"))},{QStringLiteral("source_url"),route.value(QStringLiteral("source_url"))},
                {QStringLiteral("source_sha256"),route.value(QStringLiteral("source_sha256"))}});
        }
    }
    result.context=QJsonObject{{QStringLiteral("champion"),catalog.championName(championId)},{QStringLiteral("champion_id"),championId},{QStringLiteral("patch"),catalog.patch},{QStringLiteral("preference"),preference},
        {QStringLiteral("source"),state.source==GameState::Source::Live?QStringLiteral("live_read_only"):state.source==GameState::Source::Replay?QStringLiteral("official_sample_not_live"):QStringLiteral("manual_scenario")},
        {QStringLiteral("candidate_items"),candidates},{QStringLiteral("augments"),cardsData},{QStringLiteral("abilities"),abilityData},{QStringLiteral("champion_profile"),heroProfile},{QStringLiteral("skill_parameters"),skillParameters},{QStringLiteral("visible_players_no_identity"),state.players},{QStringLiteral("verified_numeric_components"),visibleNumericComponents(catalog,state,championId)},
        {QStringLiteral("candidate_loadouts"),loadouts},
        {QStringLiteral("local_comparison"),plan.value(QStringLiteral("comparison"))},
        {QStringLiteral("version_status"),catalog.combat?catalog.combat->audit.value(QStringLiteral("source_status")):QJsonValue()},
        {QStringLiteral("comparison_assumptions"),plan.value(QStringLiteral("assumptions"))},
        {QStringLiteral("offer_comparisons"),plan.value(QStringLiteral("offer_comparisons"))},
        {QStringLiteral("observed_trigger_decisions"),plan.value(QStringLiteral("trigger_decisions"))},
        {QStringLiteral("executed_mechanics"),plan.value(QStringLiteral("executed_mechanics"))},
        {QStringLiteral("growth_projection"),compactGrowth(plan.value(QStringLiteral("growth_projection")).toObject())},
        {QStringLiteral("source_conflicts"),catalog.combat?catalog.combat->audit.value(QStringLiteral("verified_description_overrides")):QJsonValue()},
        {QStringLiteral("headliner"),evaluateHeadliner(catalog,state)},{QStringLiteral("observed_patch"),state.observedPatch},
        {QStringLiteral("game_rules"),QJsonObject{{QStringLiteral("priority"),QStringLiteral("Base League rules, then ARAM inheritance, then Mayhem overrides, then verified patch and observed event conditions.")},{QStringLiteral("attack_speed_cap"),5.0},{QStringLiteral("ability_haste_cap"),500.0},{QStringLiteral("stack_policy"),QStringLiteral("Hold observed stacks constant; no future stack, hit or kill inference.")},{QStringLiteral("patch"),catalog.patch}}},
        {QStringLiteral("limits"),QStringLiteral("local_comparison是情景比较而非实际掉血或全局最优；unknowns中的机制未纳入。模式平衡系数、敌方精确抗性和隐藏状态未知。不得把offer_comparisons当作已选强化；装备以本局商店为准。")}};
    if(catalog.combat){auto ctx=catalog.combat->context(championId,state.fresh()?state.level:18);ctx.counters=state.effectCounters;ctx.conditions=state.effectConditions;result.context.insert(QStringLiteral("candidate_mechanic_contracts"),describeCombatMechanics(*catalog.combat,ctx,items,augments));}
    result.context[QStringLiteral("game_rules")]=coachRuleContract(catalog);
    result.context[QStringLiteral("self_hero_execution")]=boundedHeroKnowledgeReference(catalog,championId);
    result.context[QStringLiteral("match_context")]=state.matchContext;
    result.context[QStringLiteral("reference_only")]=referencePlan;
    result.context[QStringLiteral("growth_conditioned_candidates")]=growthCandidates;
    result.context[QStringLiteral("ranking_available")]=plan.value(QStringLiteral("ranking_available"));
    result.context[QStringLiteral("conditional_reference")]=plan.value(QStringLiteral("conditional_reference"));
    result.context[QStringLiteral("build_reasoning")]=reasoning;
    result.context[QStringLiteral("preference")]=QStringLiteral("成长联动");
    result.context[QStringLiteral("fixed_direction_matching_enabled")]=false;
    result.context[QStringLiteral("growth_reasoning")]=plan.value(QStringLiteral("growth_reasoning"));
    result.context[QStringLiteral("growth_objective")]=plan.value(QStringLiteral("growth_objective"));
    QJsonArray growthFacts;
    for(const auto &value:state.growthObservations.value(QStringLiteral("accepted")).toArray()){
        const auto row=value.toObject();growthFacts.append(QJsonObject{{QStringLiteral("source"),row.value(QStringLiteral("source"))},
            {QStringLiteral("id"),row.value(QStringLiteral("id"))},{QStringLiteral("counter_key"),row.value(QStringLiteral("counter_key"))},
            {QStringLiteral("value"),row.value(QStringLiteral("value"))},{QStringLiteral("unit"),row.value(QStringLiteral("unit"))},
            {QStringLiteral("measurement_kind"),row.value(QStringLiteral("measurement_kind"))},{QStringLiteral("counter_applied"),row.value(QStringLiteral("counter_applied"))}});
    }
    result.context[QStringLiteral("current_growth_observations")]=QJsonObject{{QStringLiteral("accepted"),growthFacts},
        {QStringLiteral("missing_is_zero"),false},{QStringLiteral("future_stacks_predicted"),false}};
    if(referencePlan||growthCandidates)result.context[QStringLiteral("reference_contract")]=QJsonObject{{QStringLiteral("kind"),growthCandidates?QStringLiteral("conditional_growth_model_candidates"):QStringLiteral("mechanism_and_public_reference")},
        {QStringLiteral("can_explain_items"),true},{QStringLiteral("can_issue_purchase_instruction"),false},{QStringLiteral("source_order_is_purchase_order"),false},
        {QStringLiteral("unknown_state_can_be_assumed"),false},{QStringLiteral("upgrade_target_is_direct_purchase"),false}};
    // Keep the next action and its explicit uncertainty on the same basis as
    // the local recommendation; omit the much larger per-target raw traces.
    auto purchase=plan.value(QStringLiteral("purchase_sandbox")).toObject();
    purchase.remove(QStringLiteral("selected_sandbox"));purchase.remove(QStringLiteral("elapsed_ms"));
    result.context[QStringLiteral("purchase_sandbox")]=purchase;
    // Re-filter cached public references to the very same local candidates.
    // An external guide cannot introduce another action or an unseen card.
    result.context[QStringLiteral("external_strategy_reference")]=externalStrategyReference(catalog,state,
        QSet<int>(items.cbegin(),items.cend()),QSet<int>(augments.cbegin(),augments.cend()),0,true);
    result.context[QStringLiteral("candidate_semantic_evidence")]=candidateSemanticEvidence(catalog,championId,
        QSet<int>(items.cbegin(),items.cend()),QSet<int>(augments.cbegin(),augments.cend()));
    result.context=sanitizedMatchAiContext(result.context);
    return result;
}
