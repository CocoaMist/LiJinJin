#include "growth.h"
#include "growth_objective.h"
#include <algorithm>
#include <cmath>

namespace {
QString k(const char *s){return QString::fromLatin1(s);}
CombatStats difference(CombatStats after,const CombatStats &before){
    for(int n=0;n<int(Stat::Count);++n)after.values[n]-=before.values[n];
    return after;
}
QSet<QString> growthKeys(const CombatEntry &entry,int id,bool item){
    QSet<QString> keys;for(const auto &v:entry.growthStackInputs)if(v.isString()&&!v.toString().isEmpty())keys.insert(v.toString());
    for(const auto &v:entry.effects){const auto e=v.toObject();if(e.value(k("op"))==k("stack")&&!e.value(k("counter")).toString().isEmpty())keys.insert(e.value(k("counter")).toString());}
    // These typed observation keys add no numeric adapter. In particular a
    // tooltip resource value is not inferred from the final character panel.
    if(item&&(id==3070||id==3003||id==3004))keys.insert(k("item:")+QString::number(id)+k(":mana_charge"));
    if(item&&(id==1082||id==3041))keys.insert(k("item:")+QString::number(id)+k(":glory_stacks"));
    if(!item&&id==1152)keys.insert(k("augment:1152:heartsteel_progress"));
    return keys;
}
QJsonObject matchingObservation(const QJsonObject &report,const QString &source,int id,const QString &key){
    for(const auto &v:report.value(k("accepted")).toArray()){
        const auto r=v.toObject();const auto value=r.value(k("value"));
        if(r.value(k("source"))==source&&r.value(k("id"))==QJsonValue(id)&&r.value(k("counter_key"))==key
            &&value.isDouble()&&std::isfinite(value.toDouble())&&value.toDouble()>=0)return r;
    }
    return {};
}
QJsonObject publicObservation(const QJsonObject &row){
    // Validation retains original provider evidence. A selection explanation
    // only receives typed facts; no record JSON, file path or capture payload.
    QJsonObject out;
    for(const auto &name:{k("source"),k("id"),k("counter_key"),k("value"),k("unit"),k("counter_applied"),k("disposition"),
            k("measurement_kind"),k("game_time"),k("captured_at_utc"),k("expires_at_ms"),k("counter_semantics"),k("selection_observed_here")})
        if(row.contains(name))out[name]=row.value(name);
    return out;
}
std::optional<double> sourceScalar(const CombatEntry &entry,const QJsonValue &value){
    auto v=value;if(v.isString())v=entry.values.value(v.toString());if(v.isArray()&&!v.toArray().isEmpty())v=v.toArray().first();
    if(v.isDouble()&&std::isfinite(v.toDouble())&&v.toDouble()>=0)return v.toDouble();return {};
}
bool permanentGrowthCounter(const CombatEntry &entry,int id,const QString &key,bool item){
    // The reviewed Heartsteel contract explicitly adds permanent HP after a
    // verified charged attack. Other stack counters can be combat-local,
    // death-sensitive or unreviewed and are not promoted to permanent state.
    if(!item||id!=3084||key!=k("item:3084:permanent_hp"))return false;
    for(const auto &v:entry.actions){const auto a=v.toObject();if(a.value(k("op"))==k("buff")&&a.value(k("key"))==k("hp")
        &&a.value(k("growth_baseline"))==QJsonValue(true)&&a.value(k("duration"))==QJsonValue(-1))return true;}
    return false;
}
}

QJsonObject equipmentGrowthSelectionState(const CombatDatabase &db,const CombatContext &ctx,const QList<int> &items,const QList<int> &augments,const QJsonObject &observations){
    QJsonArray entries,quests;QJsonObject quantified,unquantified;int observedCount=0;
    auto inspect=[&](const QMap<int,CombatEntry> &table,const QList<int> &ids,bool item){
        const auto source=item?k("item"):k("augment");
        for(int id:ids){const auto found=table.constFind(id);if(found==table.cend())continue;const auto &entry=*found;auto keys=growthKeys(entry,id,item).values();keys.sort();
            const bool gloryItem=item&&(id==1082||id==3041);const auto gloryKey=k("item:")+QString::number(id)+k(":glory_stacks");
            if(gloryItem){
                const auto usableCounter=[&](const QString &key){const auto it=ctx.counters.constFind(key);return it!=ctx.counters.cend()&&std::isfinite(*it)&&*it>=0;};
                const bool namespacedKnown=usableCounter(gloryKey)||!matchingObservation(observations,source,id,gloryKey).isEmpty();
                const bool legacyKnown=usableCounter(k("buff:{22eeba1f}"))||!matchingObservation(observations,source,id,k("buff:{22eeba1f}")).isEmpty();
                // Present one source identity once. This display preference is
                // not a counter alias or a permission to execute the raw buff.
                if(namespacedKnown||!legacyKnown)keys.removeAll(k("buff:{22eeba1f}"));else keys.removeAll(gloryKey);
            }
            for(const auto &key:keys){const auto observation=matchingObservation(observations,source,id,key);const auto supplied=ctx.counters.constFind(key);
                const bool counterKnown=supplied!=ctx.counters.cend()&&std::isfinite(*supplied)&&*supplied>=0;
                const bool visibleKnown=!observation.isEmpty();const bool known=counterKnown||visibleKnown;
                const double value=counterKnown?*supplied:observation.value(k("value")).toDouble();
                bool stackBinding=false;QJsonArray bindings;
                for(const auto &v:entry.effects){const auto e=v.toObject();if(e.value(k("op"))==k("stack")&&e.value(k("counter"))==key){stackBinding=true;bindings.append(e);}}
                const bool observed=visibleKnown&&(!counterKnown||std::abs(value-observation.value(k("value")).toDouble())<=1e-9);
                const bool namespacedGlory=gloryItem&&key==gloryKey;
                const bool glory=gloryItem&&(namespacedGlory||key==k("buff:{22eeba1f}"));
                const bool eventDerived=observation.value(k("measurement_kind"))==k("event_derived_counter");
                const bool applied=counterKnown&&stackBinding&&!namespacedGlory;observedCount+=observed;
                const bool permanent=permanentGrowthCounter(entry,id,key,item);
                const auto capacity=sourceScalar(entry,glory?QJsonValue(k("MaxGloryStacks"))
                    : key.endsWith(k(":mana_charge"))?QJsonValue(k("MaxMana")):QJsonValue());
                const auto deathLoss=glory?sourceScalar(entry,QJsonValue(k("GloryLossOnDeath"))):std::optional<double>{};
                const QString kind=permanent?k("permanent_health_while_item_held")
                    : glory?k("death_sensitive_glory")
                    : key.endsWith(k(":mana_charge"))?k("mana_charge")
                    : !item&&id==1152?k("quest_progress_only")
                    : entry.growthStackInputs.contains(key)?k("declared_growth_persistence_unverified"):k("combat_state_persistence_unverified");
                entries.append(QJsonObject{{k("source"),source},{k("id"),id},{k("counter_key"),key},{k("kind"),kind},
                    {k("value_supplied"),known},{k("value"),known?QJsonValue(value):QJsonValue(QJsonValue::Null)},
                    {k("actual_observation_attested"),observed},{k("observation"),publicObservation(observation)},
                    {k("death_sensitive"),glory},{k("event_derived"),eventDerived},
                    {k("active_stack_binding_present"),stackBinding},{k("numeric_input_available"),applied},
                    {k("numeric_scope"),stackBinding&&!namespacedGlory?k("existing_adapter_counter_input_no_additional_stats_injected"):k("observation_only_active_stack_adapter_missing")},
                    {k("observation_counter_alias_applied"),false},
                    {k("capacity"),capacity?QJsonValue(*capacity):QJsonValue(QJsonValue::Null)},
                    {k("remaining_capacity"),known&&capacity?QJsonValue(std::max(0.,*capacity-value)):QJsonValue(QJsonValue::Null)},
                    {k("source_loss_on_death"),deathLoss?QJsonValue(*deathLoss):QJsonValue(QJsonValue::Null)},
                    {k("loss_or_reset_applied_from_observation"),false},
                    {k("full_effect_execution_verified_here"),false},{k("active_bindings"),bindings},
                    {k("permanence_verified"),permanent},{k("fixed_counter_projection_is_scenario_only"),!permanent},
                    {k("future_growth_assumed"),false},
                    {k("lifecycle_transfer_verified"),false},{k("source_pointer"),entry.triggerContract.value(k("source_pointer"))},
                    {k("source_url"),entry.triggerContract.value(k("source_url"))}});
                if(known){if(applied)quantified[key]=value;else unquantified[key]=value;}
            }
            if(!item&&id==1152){
                const auto progressKey=k("augment:1152:heartsteel_progress");const auto observation=matchingObservation(observations,source,id,progressKey);
                const auto threshold=sourceScalar(entry,entry.triggerContract.value(k("parameters")).toObject().value(k("threshold")));
                const auto multiplier=sourceScalar(entry,entry.triggerContract.value(k("parameters")).toObject().value(k("multiplier")));
                const bool progressKnown=!observation.isEmpty();const double progress=observation.value(k("value")).toDouble();
                const bool sourceBound=entry.triggerContract.value(k("event"))==k("quest_progress")
                    &&entry.triggerContract.value(k("requires")).toArray().contains(k("item:3084"))&&threshold&&multiplier&&*multiplier>=1;
                QJsonObject reward{{k("available"),sourceBound},{k("applied"),false},{k("completion_observed"),false},
                    {k("requires_owned_item_id"),3084},{k("required_event"),k("quest_progress")},
                    {k("required_event_fact"),k("heartsteel_stacks")},{k("progress_is_reward_event_fact"),false}};
                if(sourceBound){reward[k("stack_multiplier")]=*multiplier;reward[k("threshold_total_after_reward")]=*threshold * *multiplier;
                    reward[k("additional_health_if_completed_at_threshold")]=*threshold*(*multiplier-1);}
                quests.append(QJsonObject{{k("augment_id"),id},{k("progress_key"),progressKey},{k("progress_observed"),progressKnown},
                    {k("progress"),progressKnown?QJsonValue(progress):QJsonValue(QJsonValue::Null)},
                    {k("threshold"),threshold?QJsonValue(*threshold):QJsonValue(QJsonValue::Null)},
                    {k("remaining_to_threshold"),progressKnown&&threshold?QJsonValue(std::max(0.,*threshold-progress)):QJsonValue(QJsonValue::Null)},
                    {k("required_item_owned"),items.contains(3084)},{k("conditional_reward"),reward},
                    {k("time_to_completion"),QJsonValue::Null},{k("future_hit_rate_known"),false},{k("reward_claimed_from_progress"),false}});
            }
        }
    };
    inspect(db.items,items,true);inspect(db.augments,augments,false);
    return {{k("schema_version"),1},{k("entries"),entries},{k("observed_input_count"),observedCount},
        {k("active_numeric_inputs"),quantified},{k("unquantified_supplied_inputs"),unquantified},{k("quest_progress"),quests},
        {k("missing_is_zero"),false},{k("additional_stats_injected"),false},{k("panel_residual_used_as_stack"),false},
        {k("counter_then_fresh_panel_anchor_required"),true},{k("future_growth_assumed"),false},
        {k("scope"),k("held_inventory_growth_evidence_and_active_adapter_inputs_not_future_growth_or_lifecycle_inference")}};
}

QJsonObject equipmentGrowthInventoryTransition(const CombatDatabase &db,const CombatContext &ctx,const QList<int> &before,const QList<int> &after,const QList<int> &augments,const QJsonObject &observations){
    auto removed=before;for(int id:after)removed.removeOne(id);auto added=after;for(int id:before)added.removeOne(id);
    QJsonArray removedIds,addedIds,unresolved;for(int id:removed)removedIds.append(id);for(int id:added)addedIds.append(id);
    const auto baseline=equipmentGrowthSelectionState(db,ctx,before,augments,observations);
    for(const auto &v:baseline.value(k("entries")).toArray()){const auto r=v.toObject();
        if(r.value(k("source"))!=k("item")||!removed.contains(r.value(k("id")).toInt()))continue;
        if(r.value(k("value_supplied")).toBool()&&r.value(k("value")).toDouble()==0)continue;
        unresolved.append(QJsonObject{{k("item_id"),r.value(k("id"))},{k("counter_key"),r.value(k("counter_key"))},
            {k("value"),r.value(k("value"))},{k("reason"),k("stack_transfer_sale_reset_or_loss_requires_explicit_source_and_observation")}});
    }
    return {{k("removed_item_ids"),removedIds},{k("added_item_ids"),addedIds},{k("unresolved_growth_transitions"),unresolved},
        {k("numeric_transition_verified"),unresolved.isEmpty()},{k("stack_transfer_assumed"),false},
        {k("sale_inferred"),false},{k("stack_reset_or_loss_inferred"),false},{k("future_growth_assumed"),false}};
}

QJsonObject predictCombatGrowth(const CombatDatabase &db,const CombatContext &current,const QList<int> &items,const QList<int> &augments,int endLevel){
    if(!db.champions.contains(current.championId))return {};
    const int start=std::clamp(current.level,1,18),end=std::clamp(endLevel,start,18);
    QMap<QString,double> held;QJsonArray excluded;
    // A supplied stack counter is a scenario baseline. Do not extrapolate it,
    // and do not carry target HP, temporary damage totals or cooldown snapshots.
    auto keepStackInputs=[&](const QJsonArray &inputs){
        for(const auto &v:inputs){const auto key=v.toString();if(current.counters.contains(key)&&std::isfinite(current.counters[key])&&current.counters[key]>=0)held[key]=current.counters[key];}
    };
    keepStackInputs(db.champions[current.championId].value(QStringLiteral("growth_stack_inputs")).toArray());
    auto inspect=[&](const QMap<int,CombatEntry> &entries,const QList<int> &ids,const QString &source){for(int id:ids){const auto it=entries.constFind(id);if(it==entries.cend())continue;
        for(const auto &v:it->effects){const auto e=v.toObject();const auto counter=e.value(QStringLiteral("counter")).toString();
            if(e.value(QStringLiteral("op"))==QStringLiteral("stack")&&!counter.isEmpty()&&current.counters.contains(counter)&&std::isfinite(current.counters[counter])&&current.counters[counter]>=0)held[counter]=current.counters[counter];
        }
        bool eventDependent=false;for(const auto &v:it->actions){const auto op=v.toObject().value(QStringLiteral("op")).toString();eventDependent|=op!=QStringLiteral("base_stats")&&op!=QStringLiteral("configure");}
        keepStackInputs(it->growthStackInputs);
        if(eventDependent)excluded.append(QJsonObject{{QStringLiteral("source"),source},{QStringLiteral("id"),id},{QStringLiteral("reason"),QStringLiteral("Event actions require actual triggers; no future event or reward projected.")}});
    }};
    inspect(db.items,items,QStringLiteral("item"));inspect(db.augments,augments,QStringLiteral("augment"));
    QSet<QString> forms;for(const auto &condition:current.conditions)if(condition.startsWith(QStringLiteral("jayce_"))||condition.startsWith(QStringLiteral("nidalee_"))||condition.startsWith(QStringLiteral("elise_"))||condition.startsWith(QStringLiteral("aphelios_")))forms.insert(condition);
    const auto hero=db.champions[current.championId];QJsonArray rows;CombatStats baseline;bool baselineAvailable=false;QSet<QString> unknowns;QJsonObject heldJson;
    for(auto it=held.cbegin();it!=held.cend();++it)heldJson[it.key()]=it.value();
    for(int level=start;level<=end;++level){auto ctx=db.context(current.championId,level);ctx.ranks=current.ranks;ctx.counters=held;ctx.conditions=forms;
        ctx.eventStats=current.permanentEventStats;
        ctx.attackSpeedCap=current.attackSpeedCap;ctx.abilityHasteCap=current.abilityHasteCap;
        ctx.overflowCritAd=current.overflowCritAd;ctx.overflowCritAp=current.overflowCritAp;
        // Only explicitly certified permanent event stats are retained. No
        // live calibration, temporary buff, target-hit counter or elapsed
        // game-time input is carried forward into a level-growth scenario.
        const auto result=evaluateCombat(db,ctx,items,augments);
        for(const auto &u:result.unknowns)unknowns.insert(u);
        if(!result.valid){rows.append(QJsonObject{{QStringLiteral("level"),level},{QStringLiteral("valid"),false},{QStringLiteral("unknowns"),QJsonArray::fromStringList(result.unknowns)}});continue;}
        if(level==start){baseline=result.stats;baselineAvailable=true;}
        const auto delta=baselineAvailable?QJsonValue(difference(result.stats,baseline).json()):QJsonValue(QJsonValue::Null);
        const auto withoutEquipment=evaluateCombat(db,ctx,{},augments);
        const auto heroOnly=evaluateCombat(db,ctx,{},{});
        QJsonObject layers{{QStringLiteral("hero_and_confirmed_permanent_stats"),heroOnly.stats.json()},
            {QStringLiteral("augment_delta_given_hero"),difference(withoutEquipment.stats,heroOnly.stats).json()},
            {QStringLiteral("equipment_delta_given_augments"),difference(result.stats,withoutEquipment.stats).json()},
            {QStringLiteral("valid"),heroOnly.valid&&withoutEquipment.valid},
            {QStringLiteral("order"),QStringLiteral("hero and held permanent state -> selected augments -> held equipment; deltas include conversions and caps, not order-independent attribution")}};
        QJsonArray skills;
        for(const auto &v:hero.value(QStringLiteral("components")).toArray()){const auto c=v.toObject();
            if(level<c.value(QStringLiteral("min_level")).toInt(1)||level>c.value(QStringLiteral("max_level")).toInt(18))continue;
            const auto slot=c.value(QStringLiteral("slot")).toString(),rankSlot=c.value(QStringLiteral("rank_slot")).toString(slot),condition=c.value(QStringLiteral("condition")).toString();
            if(rankSlot!=QStringLiteral("P")&&rankSlot!=QStringLiteral("EFFECT")&&!ctx.ranks.contains(rankSlot)){unknowns.insert(QStringLiteral("skill_rank_unobserved:")+rankSlot);continue;}
            const int rank=rankSlot==QStringLiteral("P")||rankSlot==QStringLiteral("EFFECT")?1:ctx.ranks.value(rankSlot,0);
            if(rank<=0)continue;
            if(rank>c.value(QStringLiteral("max_rank")).toInt()){unknowns.insert(QStringLiteral("invalid_skill_rank:")+slot);continue;}
            if(!condition.isEmpty()&&level<c.value(QStringLiteral("condition_below_level")).toInt(99)&&!forms.contains(condition)){unknowns.insert(QStringLiteral("skill_condition:")+condition);continue;}
            bool ready=true;for(const auto &value:c.value(QStringLiteral("requires_conditions")).toArray())if(!forms.contains(value.toString())){unknowns.insert(QStringLiteral("skill_condition:")+value.toString());ready=false;}
            if(!ready)continue;
            const auto raw=compileCombatFormula(c.value(QStringLiteral("expression")).toObject(),ctx,rank).evaluate(ctx,result.stats);
            QJsonObject row{{QStringLiteral("slot"),slot},{QStringLiteral("rank"),rank},{QStringLiteral("calculation"),c.value(QStringLiteral("calculation"))},{QStringLiteral("damage_type"),c.value(QStringLiteral("damage_type"))},{QStringLiteral("source_pointer"),c.value(QStringLiteral("source_pointer"))}};
            const auto cds=c.value(QStringLiteral("cooldown")).toArray();
            if(rank<cds.size()&&cds[rank].toDouble()>0){
                const bool hasteApplies=combatSkillCooldownHasteApplies(hero,c);
                const double cooldown=growthEffectiveSkillCooldown(db,ctx,augments,result.stats,slot,cds[rank].toDouble(),hasteApplies);
                row[QStringLiteral("generic_haste_cooldown_component")]=cooldown;
                row[QStringLiteral("cooldown_haste_applies")]=hasteApplies;
                row[QStringLiteral("cooldown_scope")]=QStringLiteral("Shared combat comparison interval with source-bound haste eligibility; recast, refunds, ammo and other spell-specific timing remain outside this value.");
            }
            const auto rate=c.value(QStringLiteral("rate")).toString();
            const bool fraction=rate==QStringLiteral("target_missing_hp_fraction")||rate==QStringLiteral("target_hp_fraction");
            if(raw&&std::isfinite(*raw)){
                row[fraction?QStringLiteral("raw_fraction"):QStringLiteral("raw_component")]=*raw;
                row[QStringLiteral("value_kind")]=fraction?rate:QStringLiteral("raw_formula_component_not_full_cast_damage");
            }else {row[QStringLiteral("status")]=QStringLiteral("requires_unobserved_state");unknowns.insert(QStringLiteral("skill_formula:")+slot);}
            skills.append(row);
        }
        for(const auto &u:result.unknowns)unknowns.insert(u);
        rows.append(QJsonObject{{QStringLiteral("level"),level},{QStringLiteral("valid"),true},{QStringLiteral("stats"),result.stats.json()},{QStringLiteral("delta_from_start"),delta},{QStringLiteral("reasoning_layers"),layers},{QStringLiteral("skill_components_at_fixed_ranks"),skills}});
    }
    QJsonArray itemIds,augmentIds;for(int id:items)itemIds.append(id);for(int id:augments)augmentIds.append(id);auto missing=unknowns.values();std::sort(missing.begin(),missing.end());
    QJsonObject ranks;for(auto it=current.ranks.cbegin();it!=current.ranks.cend();++it)ranks[it.key()]=it.value();
    if(!baselineAvailable)unknowns.insert(QStringLiteral("growth_start_baseline_unavailable"));
    missing=unknowns.values();std::sort(missing.begin(),missing.end());
    return {{QStringLiteral("champion_id"),current.championId},{QStringLiteral("baseline_available"),baselineAvailable},{QStringLiteral("start_level"),start},{QStringLiteral("end_level"),end},{QStringLiteral("items"),itemIds},{QStringLiteral("selected_augments"),augmentIds},{QStringLiteral("fixed_skill_ranks"),ranks},{QStringLiteral("held_observed_stacks"),heldJson},{QStringLiteral("confirmed_permanent_event_stats"),current.permanentEventStats.json()},{QStringLiteral("predicted_additional_stacks"),0},{QStringLiteral("rows"),rows},{QStringLiteral("excluded_future_events"),excluded},{QStringLiteral("unknowns"),QJsonArray::fromStringList(missing)},{QStringLiteral("data_revision"),db.revision},{QStringLiteral("scope"),QStringLiteral("Relative fixed-loadout level growth, including verified item/augment formulas and conversions. Supplied champion/item/augment stack inputs are held as scenario baselines, never forecast. Only identity-reviewed passive-stat bindings enter final stats; other passive parameters remain partial. Skill ranks stay fixed until an explicit allocation plan exists. No future triggers, purchases, kills, hit rates, time-to-level, mode-balance guarantee or actual-health-loss prediction.")}};
}
