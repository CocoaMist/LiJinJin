#include "growth_objective.h"
#include "growth.h"
#include "ally_utility.h"
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <optional>

namespace {
QString k(const char *s){return QString::fromLatin1(s);}
QJsonArray ids(const QList<int> &values){QJsonArray out;for(int id:values)out.append(id);return out;}
double output(const CombatEvaluation &e){return std::max(1.,e.attackDamage+e.skillDamage+e.procDamage);}
std::optional<double> scalar(const CombatEntry &entry,const QJsonValue &value,const CombatContext &ctx,const CombatStats &stats){
    if(value.isDouble())return std::isfinite(value.toDouble())?std::optional<double>(value.toDouble()):std::nullopt;
    if(value.isObject())return compileCombatFormula(value.toObject(),ctx).evaluate(ctx,stats);
    const auto name=value.toString();if(name.startsWith(k("calc:")))return compileCombatFormula(entry.calculations.value(name.mid(5)).toObject(),ctx).evaluate(ctx,stats);
    const auto literal=entry.values.value(name);if(literal.isDouble())return scalar(entry,literal,ctx,stats);
    if(literal.isArray()&&!literal.toArray().isEmpty())return scalar(entry,literal.toArray().first(),ctx,stats);
    return {};
}
struct Timing {bool onlyUltimate=false,sealed=false;double multiplier=1;};
Timing timing(const CombatDatabase &db,const CombatContext &ctx,const QList<int> &augments,const CombatStats &stats){
    Timing out;
    for(int id:augments){const auto it=db.augments.constFind(id);if(it==db.augments.cend())continue;
        for(const auto &v:it->effects){const auto e=v.toObject();const auto condition=e.value(k("condition")).toString();
            if(e.value(k("op"))!=k("flag")||(!condition.isEmpty()&&!ctx.conditions.contains(condition)))continue;
            const auto value=scalar(*it,e.value(k("value")),ctx,stats);if(!value||*value<=0)continue;
            if(e.value(k("stat"))==k("ultimate_haste_only")){out.onlyUltimate=true;out.multiplier=1+*value;}
            if(e.value(k("stat"))==k("seal_r"))out.sealed=true;
        }
    }return out;
}
QJsonArray spellSources(const CombatDatabase &db,const CombatContext &ctx,bool ultimateOnly,bool sealed,QStringList &unknowns){
    const auto hero=db.champions.value(ctx.championId),rules=hero.value(k("action_rules_v3")).toObject().value(k("slots")).toObject();
    QMap<QString,QJsonArray> bySlot;
    for(const auto &v:hero.value(k("spells")).toArray()){const auto s=v.toObject();const auto slot=s.value(k("slot")).toString();
        if(!QStringList{k("Q"),k("W"),k("E"),k("R")}.contains(slot)||(ultimateOnly&&slot!=k("R"))||(sealed&&slot==k("R")))continue;
        const int rank=ctx.ranks.value(slot,0);if(rank<=0)continue;
        const auto cd=s.value(k("cooldown")).toArray();if(rank>=cd.size()||!cd[rank].isDouble()||cd[rank].toDouble()<=0)continue;
        const auto source=s.value(k("path")).toString();if(source.isEmpty())continue;
        const auto rule=rules.value(slot).toObject();const auto canonical=rule.value(k("source_pointer")).toString();
        if(!canonical.isEmpty()&&source!=canonical)continue;
        bySlot[slot].append(QJsonObject{{k("slot"),slot},{k("rank"),rank},{k("spell_path"),source},{k("base_cooldown"),cd[rank]},
            {k("haste_applies"),rule.value(k("cooldown_haste_applies")).toBool(true)}});
    }
    QJsonArray out;for(auto it=bySlot.cbegin();it!=bySlot.cend();++it){
        if(it->size()!=1){unknowns.append(k("ambiguous_skill_cycle_source:")+it.key());continue;}out.append(it->first());
    }
    if(ultimateOnly&&out.isEmpty())unknowns.append(k("ultimate_cycle_source_unavailable_or_unlearned"));
    return out;
}
double cycleCapacity(const CombatDatabase &db,const CombatContext &ctx,const QList<int> &augments,const CombatStats &stats,const QJsonArray &sources,QJsonArray *report=nullptr){
    const auto t=timing(db,ctx,augments,stats);
    double cyclesPerSecond=0;
    for(const auto &v:sources){
        auto row=v.toObject();
        const auto slot=row.value(k("slot")).toString();
        const bool sealed=t.sealed&&slot==k("R");
        const double cd=sealed?0:effectiveCombatSkillCooldown(row.value(k("base_cooldown")).toDouble(),slot,row.value(k("haste_applies")).toBool(),stats,ctx,t.onlyUltimate,t.multiplier);
        if(!sealed&&(!std::isfinite(cd)||cd<=0))continue;
        const double rate=sealed?0:1/cd;
        cyclesPerSecond+=rate;
        if(report){
            row[k("effective_cooldown")]=sealed?QJsonValue(QJsonValue::Null):QJsonValue(cd);
            row[k("reference_cycles_per_60s")]=60*rate;
            row[k("sealed")]=sealed;
            row[k("actual_cast_count_known")]=false;
            row[k("hit_or_control_success_assumed")]=false;
            row[k("resource_and_safe_cast_opportunities_known")]=false;
            row[k("scope")]=k("steady_cycle_capacity_not_cast_damage_or_actual_cast_count");
            report->append(row);
        }
    }
    // The source set is fixed before scoring. Sealing a slow skill contributes
    // zero cycles, while the surviving skills keep their original denominator.
    return sources.isEmpty()?0:cyclesPerSecond/sources.size();
}
QJsonObject components(const QJsonObject &basis,const CombatEvaluation &e,double cycle){
    const auto n=basis.value(k("normalizers")).toObject();QJsonObject out;
    const auto row=[&](const QString &name,double value){const double base=n.value(name).toDouble();if(std::isfinite(value)&&value>=0&&base>0)out[name]=QJsonObject{{k("value"),value},{k("baseline"),base},{k("ratio"),value/base}};};
    row(k("modeled_output"),output(e));row(k("mixed_effective_hp"),e.survival);
    if(basis.value(k("metric_weights")).toObject().contains(k("skill_cycle_capacity")))row(k("skill_cycle_capacity"),cycle);
    return out;
}
}

QString growthCombatPreference(){return QStringLiteral("成长联动");}

void bindGrowthObjective(const CombatDatabase &db,CombatContext &ctx,const QList<int> &baselineItems,const QList<int> &selectedAugments){
    auto raw=ctx;raw.growthObjectiveBasis=QJsonObject{{k("binding_in_progress"),true}};
    const auto baseline=evaluateCombat(db,raw,baselineItems,selectedAugments,false,growthCombatPreference());
    QJsonArray selected,dependencies;QJsonObject held;QStringList unknowns;
    const auto hero=db.champions.value(ctx.championId);
    auto hold=[&](const QJsonArray &inputs){for(const auto &v:inputs){const auto name=v.toString();auto it=ctx.counters.constFind(name);if(it!=ctx.counters.cend()&&std::isfinite(*it)&&*it>=0)held[name]=*it;else unknowns.append(k("growth_input_unobserved:")+name);}};
    hold(hero.value(k("growth_stack_inputs")).toArray());
    for(int id:baselineItems){const auto it=db.items.constFind(id);if(it==db.items.cend())continue;hold(it->growthStackInputs);for(const auto &v:it->effects){const auto e=v.toObject();if(e.value(k("op"))==k("stack")&&!e.value(k("counter")).toString().isEmpty())hold(QJsonArray{e.value(k("counter"))});}}
    for(int id:selectedAugments){QJsonObject s{{k("augment_id"),id},{k("selection_source"),k("bound_caller_selection")},{k("selection_observation_verified_here"),false}};const auto it=db.augments.constFind(id);
        if(it==db.augments.cend()){s[k("available")]=false;s[k("reason")]=k("same_patch_augment_rule_missing");unknowns.append(k("selected_augment_rule_missing:")+QString::number(id));selected.append(s);continue;}
        s[k("available")]=true;s[k("effect_count")]=it->effects.size();s[k("trigger_contract")]=it->triggerContract;s[k("event_dependent")]=!it->actions.isEmpty()||!it->eventRules.isEmpty();
        s[k("growth_stack_inputs")]=it->growthStackInputs;hold(it->growthStackInputs);
        for(const auto &v:it->triggerContract.value(k("requires")).toArray()){const auto value=v.toString();if(!value.startsWith(k("item:")))continue;bool valid=false;const int item=value.mid(5).toInt(&valid);if(!valid||!db.items.contains(item))continue;
            dependencies.append(QJsonObject{{k("augment_id"),id},{k("item_id"),item},{k("source_field"),k("trigger_contract.requires")},{k("future_growth_assumed"),false},
                {k("progress_or_completion_inferred_from_dependency"),false}});
            unknowns.append(k("growth_dependency_does_not_verify_stacks_or_quest_progress:augment:")+QString::number(id)+k(":item:")+QString::number(item));}
        for(const auto &v:it->effects){const auto e=v.toObject();if(e.value(k("op"))==k("stack")){const auto counter=e.value(k("counter")).toString();if(!counter.isEmpty())hold(QJsonArray{counter});}}
        selected.append(s);
    }
    const auto t=timing(db,ctx,selectedAugments,baseline.stats);const auto sources=spellSources(db,ctx,t.onlyUltimate,t.sealed,unknowns);
    const double cycles=cycleCapacity(db,ctx,selectedAugments,baseline.stats,sources);QJsonObject normalizers{{k("modeled_output"),output(baseline)},{k("mixed_effective_hp"),std::max(1.,baseline.survival)}};
    const auto utility=ctx.explicitActionCounts?QJsonObject{}:evaluateAllyUtility(db,ctx,baseline.stats,selectedAugments);
    // An explicit ultimate-only rule makes total auto-attack output the wrong
    // criterion. Compare the verified loop and ability to survive for it; do
    // not fabricate R damage or control value when those components are absent.
    QStringList metrics{k("mixed_effective_hp")};if(!t.onlyUltimate)metrics.append(k("modeled_output"));
    if(cycles>0){normalizers[k("skill_cycle_capacity")]=cycles;metrics.append(k("skill_cycle_capacity"));}
    if(utility.value(k("available")).toBool()){normalizers[k("ally_protection_reference")]=utility.value(k("normalized_capacity"));metrics.append(k("ally_protection_reference"));}
    QJsonObject weights;for(const auto &name:metrics)weights[name]=1./metrics.size();
    unknowns.append(k("ally_heal_shield_control_utility_not_fully_quantified"));unknowns.append(k("future_stacks_hits_kills_and_time_to_growth_not_predicted"));unknowns.removeDuplicates();unknowns.sort();
    QString summary=selectedAugments.isEmpty()?QStringLiteral("按英雄已绑定机制比较成长，等待已选海克斯补充联动条件"):
        !dependencies.isEmpty()?QStringLiteral("已选海克斯存在装备前置；成长入口可核对，层数和完成进度仍未核实"):
        t.onlyUltimate?QStringLiteral("已选规则仅让急速作用于大招，单独比较大招循环与承受能力"):
        QStringLiteral("结合已选海克斯的已绑定效果、成长输入和装备转换比较收益");
    ctx.growthObjectiveBasis=QJsonObject{{k("schema_version"),1},{k("summary"),summary},{k("champion_id"),ctx.championId},{k("level"),ctx.level},
        {k("data_revision"),db.revision},{k("baseline_items"),ids(baselineItems)},{k("selected_augments"),selected},{k("required_item_dependencies"),dependencies},
        {k("held_growth_inputs"),held},{k("equipment_growth_state"),equipmentGrowthSelectionState(db,ctx,baselineItems,selectedAugments)},
        {k("predicted_additional_stacks"),0},{k("normalizers"),normalizers},{k("metric_weights"),weights},{k("skill_sources"),sources},{k("ally_utility_reference"),utility},
        {k("baseline_valid"),baseline.valid},{k("basis_frozen_before_candidates"),true},{k("role_matching_enabled"),false},{k("fixed_direction_matching_enabled"),false},
        {k("aggregation"),k("equal_geometric_mean_of_same_baseline_normalized_metrics")},{k("unknowns"),QJsonArray::fromStringList(unknowns)},
        {k("skill_cycle_aggregation"),k("mean_reference_cycles_per_second_over_frozen_skill_sources")},
        {k("selected_ultimate_only_rule"),t.onlyUltimate},{k("ranking_available"),false},{k("full_build_optimum_claimed"),false},
        {k("total_output_used_as_ultimate_utility"),false},{k("candidate_filter_not_complete_growth_value"),true},
        {k("numeric_scope"),k("partial_model_comparison_not_actual_utility_or_win_rate")},
        {k("self_sustain_contract"),QJsonObject{{k("self_shield_measure"),k("single_available_pool_not_actual_absorption")},
            {k("shield_recasts_summed"),false},{k("drain_procs_generated_from_casts"),false},
            {k("effective_healing_requires_missing_health"),true},
            {k("modeled_drain_effectiveness_explicitly_assumed"),ctx.conditions.contains(k("comparison_drain_damage_is_effective"))},
            {k("baseline_self_sustain"),baseline.selfSustain}}}};
}

void applyGrowthObjective(const CombatDatabase &db,const CombatContext &ctx,const QList<int> &items,const QList<int> &augments,CombatEvaluation &e,bool details){
    const auto basis=ctx.growthObjectiveBasis;
    if(basis.value(k("binding_in_progress")).toBool()){e.score=1;return;}
    if(!basis.value(k("baseline_valid")).toBool()){e.score=0;if(details)e.unknowns.append(k("growth_objective_baseline_unavailable"));return;}
    QJsonArray cycles;const double capacity=cycleCapacity(db,ctx,augments,e.stats,basis.value(k("skill_sources")).toArray(),details?&cycles:nullptr);
    auto values=components(basis,e,capacity);auto weights=basis.value(k("metric_weights")).toObject();double sum=0,total=0;bool zeroCycleCapacity=false;
    if(ctx.explicitActionCounts&&weights.contains(k("ally_protection_reference"))){weights.remove(k("ally_protection_reference"));double remaining=0;for(const auto &v:weights)remaining+=v.toDouble();if(remaining>0)for(auto it=weights.begin();it!=weights.end();++it)it.value()=it.value().toDouble()/remaining;}
    QJsonObject utility;
    if(weights.contains(k("ally_protection_reference"))){utility=evaluateAllyUtility(db,ctx,e.stats,augments);
        const double value=utility.value(k("normalized_capacity")).toDouble(),baseline=basis.value(k("normalizers")).toObject().value(k("ally_protection_reference")).toDouble();
        if(utility.value(k("available")).toBool()&&value>0&&baseline>0)values[k("ally_protection_reference")]=QJsonObject{{k("value"),value},{k("baseline"),baseline},{k("ratio"),value/baseline}};
    }
    for(auto it=weights.constBegin();it!=weights.constEnd();++it){
        const double ratio=values.value(it.key()).toObject().value(k("ratio")).toDouble(-1);
        if(it.key()==k("skill_cycle_capacity")&&ratio==0){zeroCycleCapacity=true;continue;}
        if(!std::isfinite(ratio)||ratio<=0){e.score=0;return;}
        sum+=it.value().toDouble()*std::log(ratio);total+=it.value().toDouble();
    }
    e.score=!zeroCycleCapacity&&total>0?std::exp(sum/total):0;
    // The same metric weights are needed by the team and opponent comparators.
    e.growthObjective=QJsonObject{{k("metric_weights"),weights},{k("metrics"),values},{k("role_matching_enabled"),false},{k("fixed_direction_matching_enabled"),false}};
    if(details){e.growthObjective=basis;e.growthObjective[k("metric_weights")]=weights;e.growthObjective[k("metrics")]=values;e.growthObjective[k("skill_cycles")]=cycles;
        e.growthObjective[k("ally_utility_reference")]=utility;
        e.growthObjective[k("candidate_self_sustain")]=e.selfSustain;
        QJsonArray equipmentGrowth;for(int id:items){const auto it=db.items.constFind(id);if(it==db.items.cend()||it->growthStackInputs.isEmpty())continue;QJsonObject supplied;
            for(const auto &v:it->growthStackInputs){const auto name=v.toString();auto counter=ctx.counters.constFind(name);if(counter!=ctx.counters.cend()&&std::isfinite(*counter)&&*counter>=0)supplied[name]=*counter;}
            equipmentGrowth.append(QJsonObject{{k("item_id"),id},{k("required_inputs"),it->growthStackInputs},{k("supplied_baseline_inputs"),supplied},{k("future_growth_assumed"),false}});}
        e.growthObjective[k("equipment_growth_inputs")]=equipmentGrowth;
        e.growthObjective[k("equipment_growth_state")]=equipmentGrowthSelectionState(db,ctx,items,augments);
        QList<int> baselineItems;for(const auto &v:basis.value(k("baseline_items")).toArray())baselineItems.append(v.toInt());
        e.growthObjective[k("inventory_growth_transition")]=equipmentGrowthInventoryTransition(db,ctx,baselineItems,items,augments);
        e.growthObjective[k("required_items_satisfied")]=growthRequiredItemCoverage(ctx,items);
        e.growthObjective[k("actual_casts_or_hits_inferred")]=false;
        for(const auto &v:basis.value(k("unknowns")).toArray())if(v.toString().startsWith(k("selected_augment_rule_missing:")))e.unknowns.append(k("growth_objective_")+v.toString());
    }
}

QJsonObject describeGrowthObjective(const CombatDatabase &db,const CombatContext &context,const QList<int> &items,const QList<int> &augments){
    auto ctx=context;if(ctx.growthObjectiveBasis.isEmpty())bindGrowthObjective(db,ctx,items,augments);
    return evaluateCombat(db,ctx,items,augments,false,growthCombatPreference()).growthObjective;
}
double growthObjectiveMetricWeight(const CombatEvaluation &e,const QString &metric){const auto weights=e.growthObjective.value(k("metric_weights")).toObject();if(!weights.isEmpty())return weights.value(metric).toDouble(0);return metric==k("modeled_output")||metric==k("mixed_effective_hp")?.5:0;}
int growthRequiredItemCoverage(const CombatContext &ctx,const QList<int> &items){QSet<int> found;for(const auto &v:ctx.growthObjectiveBasis.value(k("required_item_dependencies")).toArray()){const int id=v.toObject().value(k("item_id")).toInt();if(items.contains(id))found.insert(id);}return found.size();}
double growthEffectiveSkillCooldown(const CombatDatabase &db,const CombatContext &ctx,const QList<int> &augments,const CombatStats &stats,const QString &slot,double baseCooldown,bool hasteApplies){
    const auto t=timing(db,ctx,augments,stats);if(t.sealed&&slot==k("R"))return 0;
    return effectiveCombatSkillCooldown(baseCooldown,slot,hasteApplies,stats,ctx,t.onlyUltimate,t.multiplier);
}
