#include "ally_utility.h"
#include "continuous_control_state.h"
#include "growth_objective.h"
#include <algorithm>
#include <cmath>

namespace {
QString k(const char *s){return QString::fromLatin1(s);}
struct Event {double at=0,amount=0,duration=0;QString kind,key;};
std::optional<double> formula(const QJsonObject &row,const QString &name,const CombatContext &ctx,const CombatStats &stats,int rank){
    const auto value=compileCombatFormula(row.value(name).toObject(),ctx,rank).evaluate(ctx,stats);
    return value&&std::isfinite(*value)&&*value>=0?value:std::nullopt;
}
QJsonArray reviewedAlistar(const CombatDatabase &db,const CombatContext &ctx,bool controls){
    QJsonArray out;if(db.patch!=k("16.21")||ctx.championId!=12)return out;
    for(const auto &v:db.champions.value(12).value(k("spells")).toArray()){
        const auto s=v.toObject();const auto path=s.value(k("path")).toString();
        if(!controls&&path==k("Characters/Alistar/Spells/AlistarPassiveAbility/AlistarPassive")){
            const auto expr=s.value(k("calculations")).toObject().value(k("AllyHeal")).toObject();if(expr.isEmpty())continue;
            out.append(QJsonObject{{k("slot"),k("P")},{k("kind"),k("heal")},{k("target"),k("ally_other")},{k("rate"),k("cast")},
                {k("expression"),expr},{k("source_pointer"),path},{k("max_rank"),5},{k("reference_one_passive_trigger"),true}});
        }
        if(controls&&path==k("Characters/Alistar/Spells/PulverizeAbility/Pulverize")){
            const auto values=s.value(k("data_values")).toObject().value(k("KnockupDuration")).toArray();if(values.isEmpty())continue;
            out.append(QJsonObject{{k("slot"),k("Q")},{k("type"),k("knockup")},{k("source_pointer"),path},{k("cooldown"),s.value(k("cooldown"))},
                {k("duration_expression"),QJsonObject{{k("op"),k("rank")},{k("values"),values}}}});
        }
    }return out;
}
}

QJsonObject evaluateAllyUtility(const CombatDatabase &db,const CombatContext &ctx,const CombatStats &stats,const QList<int> &augments){
    const auto hero=db.champions.value(ctx.championId);auto support=hero.value(k("support_components")).toArray(),controls=hero.value(k("control_bindings_v3")).toArray();
    for(const auto &v:reviewedAlistar(db,ctx,false))support.append(v);for(const auto &v:reviewedAlistar(db,ctx,true))controls.append(v);
    const double horizon=std::clamp(ctx.horizon,0.,30.),maximum=2000,initial=1000,attackDps=100,spellDps=100;
    if(horizon<=0||!std::isfinite(ctx.horizon))return {{k("available"),false}};
    QVector<Event> events;QJsonArray sources;QStringList unknowns;ContinuousControlState control;
    QMap<QString,int> supportSlots,controlGroups;bool sealed=false;
    for(int id:augments)for(const auto &v:db.augments.value(id).effects){const auto e=v.toObject();if(e.value(k("op"))==k("flag")&&e.value(k("stat"))==k("seal_r"))sealed=true;}
    for(const auto &v:support){const auto r=v.toObject();if(r.value(k("target"))!=k("self"))++supportSlots[r.value(k("slot")).toString()];}
    const auto cooldown=[&](const QJsonObject &r,const QString &slot,int rank){auto cds=r.value(k("cooldown")).toArray();if(cds.isEmpty())for(const auto &raw:hero.value(k("spells")).toArray()){const auto s=raw.toObject();if(s.value(k("slot"))==slot)cds=s.value(k("cooldown")).toArray();}
        return rank<cds.size()&&cds[rank].isDouble()?growthEffectiveSkillCooldown(db,ctx,augments,stats,slot,cds[rank].toDouble(),slot!=k("P")):0.;};
    for(const auto &v:support){const auto row=v.toObject();const auto slot=row.value(k("slot")).toString(),target=row.value(k("target")).toString(),kind=row.value(k("kind")).toString();
        if(target==k("self"))continue;
        if(!QStringList{k("ally_other"),k("self_or_ally"),k("self_and_ally_champion"),k("all_allies_including_self")}.contains(target)||supportSlots.value(slot)!=1){unknowns.append(k("ambiguous_ally_support_source:")+slot);continue;}
        const bool passive=row.value(k("reference_one_passive_trigger")).toBool();const int rank=passive?1:ctx.ranks.value(slot);
        const auto condition=row.value(k("condition")).toString();
        if(rank<=0||(slot==k("R")&&sealed))continue;
        if((!condition.isEmpty()&&!ctx.conditions.contains(condition))||row.value(k("rate")).toString(k("cast"))!=k("cast")||!row.value(k("damage_filter")).toString().isEmpty()){
            unknowns.append(k("ally_support_condition_or_typed_effect_unmodeled:")+slot);continue;}
        const auto amount=formula(row,k("expression"),ctx,stats,rank);const auto duration=kind==k("shield")?formula(row,k("duration_expression"),ctx,stats,rank):std::optional<double>(0);
        if(!amount||!duration||row.value(k("source_pointer")).toString().isEmpty()||!QStringList{k("heal"),k("shield")}.contains(kind)||(kind==k("shield")&&*duration<=0)){unknowns.append(k("ally_support_formula_unavailable:")+slot);continue;}
        const double cd=passive?0:cooldown(row,slot,rank);const double first=std::max(horizon*.25,ctx.skillReadyIn.value(slot,0.));
        for(double t=first;t<horizon;t+=cd>0?cd:horizon+1){events.append({t,*amount*std::max(0.,1+stats[Stat::HealShield]),*duration,kind,slot});if(events.size()>=128)break;}
        auto source=row;source[k("reference_casts_are_assumed")]=true;sources.append(source);
        if(passive)unknowns.append(k("one_alistar_passive_proc_is_explicit_reference_assumption_not_observed_stacks"));
        if(row.contains(k("health_cost_fraction_expression")))unknowns.append(k("support_caster_cost_and_survival_not_in_ally_reference"));
    }
    for(const auto &v:controls){const auto r=v.toObject();const auto group=r.value(k("exclusive_group")).toString();if(!group.isEmpty())++controlGroups[group];}
    int controlCount=0;
    for(const auto &v:controls){const auto row=v.toObject();const auto slot=row.value(k("slot")).toString(),type=row.value(k("type")).toString();const int rank=ctx.ranks.value(slot);
        if(rank<=0||(slot==k("R")&&sealed))continue;
        bool ready=!row.value(k("source_pointer")).toString().isEmpty();const auto condition=row.value(k("condition")).toString();ready&=condition.isEmpty()||ctx.conditions.contains(condition);
        for(const auto &c:row.value(k("required_conditions")).toArray())ready&=ctx.conditions.contains(c.toString());
        ready&=row.value(k("required_facts")).toArray().isEmpty()&&row.value(k("consumes_fact")).toString().isEmpty()&&row.value(k("application_phase")).toString(k("hit"))!=k("expiration");
        const auto group=row.value(k("exclusive_group")).toString();ready&=group.isEmpty()||controlGroups.value(group)==1;
        if(!ready){unknowns.append(k("control_requires_additional_hit_or_phase_facts:")+slot);continue;}
        const auto duration=formula(row,k("duration_expression"),ctx,stats,rank);if(!duration||*duration<=0||*duration>120)continue;
        const double cd=cooldown(row,slot,rank),first=std::max(0.,ctx.skillReadyIn.value(slot,0.));
        for(double t=first;t<horizon;t+=cd>0?cd:horizon+1){
            QJsonObject event{{k("event_id"),k("utility-")+QString::number(controlCount++)},{k("source"),k("self")},{k("target"),k("reference_enemy")},
                {k("type"),type},{k("time"),t},{k("duration"),*duration}};
            if(type==k("slow")){const auto slow=formula(row,k("slow_fraction_expression"),ctx,stats,rank);if(!slow||*slow>1)continue;event[k("slow_fraction")]=*slow;}
            CombatStats enemy;enemy[Stat::Tenacity]=std::clamp(ctx.counters.value(k("reference_enemy_tenacity"),0.),0.,1.);QString error;
            if(!control.apply(event,enemy,&error))unknowns.append(error);
            if(controlCount>=128)break;
        }sources.append(row);
    }
    if(sources.isEmpty())return {{k("available"),false},{k("unknowns"),QJsonArray::fromStringList(unknowns)}};
    std::stable_sort(events.begin(),events.end(),[](const Event &a,const Event &b){return a.at<b.at;});
    struct Shield{QString key;double remaining=0,expires=0;};QVector<Shield> shields;
    double hp=initial,baseline=initial,healed=0,absorbed=0,prevented=0;int cursor=0;
    for(double t=0;t<horizon-1e-9;){
        for(auto &s:shields)if(s.expires<=t+1e-9)s.remaining=0;
        if(hp<=0)break;
        while(cursor<events.size()&&events[cursor].at<=t+1e-9){const auto e=events[cursor++];if(e.kind==k("heal")){const double effective=std::min(maximum-hp,e.amount);hp+=effective;healed+=effective;}else{for(auto &s:shields)if(s.key==e.key)s.remaining=0;shields.append({e.key,e.amount,t+e.duration});}}
        double next=control.nextBoundary(t,std::min(horizon,t+.1));if(cursor<events.size()&&events[cursor].at>t+1e-9)next=std::min(next,events[cursor].at);
        for(const auto &s:shields)if(s.remaining>0&&s.expires>t+1e-9)next=std::min(next,s.expires);if(next<=t)break;
        const double dt=next-t,raw=(attackDps+spellDps)*dt,damage=((control.canAttackTarget(t,k("reference_ally"))?attackDps:0)+(control.canCast(t)?spellDps:0))*dt;
        prevented+=raw-damage;double remaining=damage;
        for(auto &s:shields){const double used=std::min(s.remaining,remaining);s.remaining-=used;remaining-=used;absorbed+=used;}
        hp=std::max(0.,hp-remaining);baseline=std::max(0.,baseline-raw);t=next;
    }
    // Counterfactual baseline continues to the same horizon after an early
    // death. No healing event may revive the reference ally.
    baseline=std::max(0.,initial-(attackDps+spellDps)*horizon);
    unknowns.append(k("movement_range_target_access_resources_and_hit_success_are_not_observed"));unknowns.append(k("displacement_position_utility_not_modeled"));unknowns.removeDuplicates();
    return {{k("available"),!sources.isEmpty()},{k("scope"),k("conditional_one_ally_health_preserved_not_observed_utility")},
        {k("scenario"),QJsonObject{{k("horizon"),horizon},{k("ally_maximum_health"),maximum},{k("ally_initial_health"),initial},
            {k("incoming_attack_dps"),attackDps},{k("incoming_spell_dps"),spellDps},{k("support_first_cast_at"),horizon*.25},
            {k("one_ally_in_range_assumed"),true},{k("source_eligible_control_hit_assumed"),true},{k("source_alive_and_resources_available_assumed"),true},{k("enemy_tenacity"),std::clamp(ctx.counters.value(k("reference_enemy_tenacity"),0.),0.,1.)},{k("actual_casts_or_hits_observed"),false}}},
        {k("effective_healing"),healed},{k("shield_absorbed"),absorbed},{k("control_prevented_incoming_damage"),prevented},
        {k("baseline_remaining_health"),baseline},{k("remaining_health"),hp},{k("ally_health_preserved"),std::max(0.,hp-baseline)},
        {k("normalized_capacity"),1+std::max(0.,hp-baseline)/maximum},{k("sources"),sources},{k("unknowns"),QJsonArray::fromStringList(unknowns)},
        {k("root_or_slow_awarded_as_damage_prevention"),false},{k("overlap_control_summed_twice"),false},{k("ranking_certified"),false}};
}
