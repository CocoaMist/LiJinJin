#include "mechanic_triggers.h"
#include <cmath>

namespace {
std::optional<double> parameter(const CombatEntry &entry,const QJsonValue &ref,const CombatContext &ctx,const CombatStats &stats) {
    if(ref.isDouble())return ref.toDouble();
    if(!ref.isString())return {};
    const auto key=ref.toString();
    if(key.startsWith(QStringLiteral("calc:")))return compileCombatFormula(entry.calculations.value(key.mid(5)).toObject(),ctx).evaluate(ctx,stats);
    auto value=entry.values.value(key);
    if(value.isDouble())return value.toDouble();
    if(value.isArray()&&!value.toArray().isEmpty()&&value.toArray().first().isDouble())return value.toArray().first().toDouble();
    return {};
}
}
QJsonObject TriggerDecision::json() const {return {{QStringLiteral("matched"),matched},{QStringLiteral("reason"),reason},{QStringLiteral("missing"),QJsonArray::fromStringList(missing)},{QStringLiteral("parameters"),parameters}};}
TriggerDecision evaluateMechanicTrigger(const CombatEntry &entry,const CombatContext &ctx,const CombatStats &stats,const QJsonObject &observation,QMap<QString,double> &lastTriggers) {
    TriggerDecision out;const auto &contract=entry.triggerContract;
    if(contract.isEmpty()){out.reason=QStringLiteral("no_verified_contract");return out;}
    const auto event=contract.value(QStringLiteral("event")).toString();
    if(observation.value(QStringLiteral("event")).toString()!=event){out.reason=QStringLiteral("different_event");return out;}
    const auto facts=observation.value(QStringLiteral("facts")).toObject();
    const auto flags=observation.value(QStringLiteral("flags")).toArray();
    for(const auto &v:contract.value(QStringLiteral("requires")).toArray()){
        const auto key=v.toString();
        if(facts.value(key).isBool()&&!facts.value(key).toBool()&&!contract.value(QStringLiteral("presence_only")).toArray().contains(key)){out.reason=QStringLiteral("condition_not_met:")+key;return out;}
        if(!flags.contains(key)&&!facts.contains(key)&&!ctx.conditions.contains(key)&&!ctx.counters.contains(key))out.missing.append(key);
    }
    const auto allowedFacts=contract.value(QStringLiteral("fact_values")).toObject();
    for(auto it=allowedFacts.begin();it!=allowedFacts.end();++it){
        if(!facts.contains(it.key()))out.missing.append(it.key());
        else if(!it.value().toArray().contains(facts.value(it.key()))){out.reason=QStringLiteral("invalid_fact:")+it.key();return out;}
    }
    const auto bounds=contract.value(QStringLiteral("input_bounds")).toObject();
    for(auto it=bounds.begin();it!=bounds.end();++it){const auto value=facts.value(it.key());const auto rule=it.value().toObject();
        if(!value.isDouble()||!std::isfinite(value.toDouble())||(rule.contains(QStringLiteral("min"))&&value.toDouble()<rule.value(QStringLiteral("min")).toDouble())||(rule.contains(QStringLiteral("max"))&&value.toDouble()>rule.value(QStringLiteral("max")).toDouble())||(rule.value(QStringLiteral("integer")).toBool()&&std::floor(value.toDouble())!=value.toDouble())){out.missing.append(QStringLiteral("invalid_fact:")+it.key());out.reason=QStringLiteral("invalid_observed_input");return out;}
    }
    // Skill-specific augments must match the actual assigned slot. Knowing
    // merely that an assignment exists does not make every spell eligible.
    if(contract.value(QStringLiteral("requires")).toArray().contains(QStringLiteral("designated_slot"))){
        if(!facts.contains(QStringLiteral("designated_slot"))||!facts.contains(QStringLiteral("cast_slot")))out.missing.append(QStringLiteral("designated_slot_and_cast_slot"));
        else if(facts.value(QStringLiteral("designated_slot"))!=facts.value(QStringLiteral("cast_slot"))){out.reason=QStringLiteral("different_skill");return out;}
    }
    const auto params=contract.value(QStringLiteral("parameters")).toObject();
    for(auto it=params.begin();it!=params.end();++it){if(auto value=parameter(entry,it.value(),ctx,stats);value&&std::isfinite(*value))out.parameters.insert(it.key(),*value);else if(it.value().isString())out.missing.append(QStringLiteral("parameter:")+it.key());}
    for(const auto &v:contract.value(QStringLiteral("predicates")).toArray()){
        const auto p=v.toObject();const auto key=p.value(QStringLiteral("fact")).toString();
        auto bound=parameter(entry,p.value(QStringLiteral("bound")),ctx,stats);
        if(!facts.value(key).isDouble()||!bound){out.missing.append(key);continue;}
        double x=facts.value(key).toDouble();if(p.value(QStringLiteral("precision"))==QStringLiteral("float32")){x=static_cast<float>(x);*bound=static_cast<float>(*bound);}const auto op=p.value(QStringLiteral("op")).toString();
        if(!std::isfinite(x)||!((op==QStringLiteral("lt")&&x<*bound)||(op==QStringLiteral("le")&&x<=*bound)||(op==QStringLiteral("ge")&&x>=*bound)||(op==QStringLiteral("gt")&&x>*bound))){out.reason=QStringLiteral("threshold_not_met:")+key;return out;}
    }
    if(!out.missing.isEmpty()){out.missing.removeDuplicates();out.reason=QStringLiteral("unobserved_requirements");return out;}
    if(!observation.value(QStringLiteral("time")).isDouble()||!std::isfinite(observation.value(QStringLiteral("time")).toDouble())){out.reason=QStringLiteral("missing_time");return out;}
    const double now=observation.value(QStringLiteral("time")).toDouble();if(now<0){out.reason=QStringLiteral("invalid_time");return out;}
    const auto scope=contract.value(QStringLiteral("cooldown_scope")).toString(QStringLiteral("global"));
    const QString scopeId=scope==QStringLiteral("target")?observation.value(QStringLiteral("target")).toString():scope==QStringLiteral("cast")?observation.value(QStringLiteral("cast_id")).toString():QStringLiteral("global");
    if(scopeId.isEmpty()){out.reason=QStringLiteral("missing_scope_identity");return out;}
    const auto source=observation.value(QStringLiteral("source")).toString();
    if(source!=QStringLiteral("item")&&source!=QStringLiteral("augment")){out.reason=QStringLiteral("missing_source_identity");return out;}
    const auto key=source+QStringLiteral(":%1:").arg(entry.id)+scopeId;
    const double cd=out.parameters.value(QStringLiteral("cooldown")).toDouble();
    if(lastTriggers.contains(key)&&(now<lastTriggers.value(key)||now-lastTriggers.value(key)<cd)){out.reason=QStringLiteral("cooldown");return out;}
    lastTriggers[key]=now;out.matched=true;out.reason=QStringLiteral("observed_trigger_only");return out;
}
QJsonArray describeCombatMechanics(const CombatDatabase &db,const CombatContext &ctx,const QList<int> &items,const QList<int> &augments){
    QJsonArray out;
    auto append=[&](const QMap<int,CombatEntry> &entries,const QList<int> &ids,const QString &source){for(int id:ids){auto it=entries.constFind(id);if(it==entries.cend())continue;QJsonObject parameters;QStringList missing;
        const auto refs=it->triggerContract.value(QStringLiteral("parameters")).toObject();
        for(auto p=refs.begin();p!=refs.end();++p)if(auto value=parameter(*it,p.value(),ctx,ctx.base);value)parameters.insert(p.key(),*value);else if(p.value().isString())missing.append(p.key());
        out.append(QJsonObject{{QStringLiteral("source"),source},{QStringLiteral("id"),id},{QStringLiteral("contract"),it->triggerContract},{QStringLiteral("base_state_parameters"),parameters},{QStringLiteral("unresolved_parameters"),QJsonArray::fromStringList(missing)},{QStringLiteral("effects"),it->effects},{QStringLiteral("event_actions"),it->actions},{QStringLiteral("additional_events"),it->eventRules},{QStringLiteral("coverage"),it->coverage}});
    }};
    append(db.items,items,QStringLiteral("item"));append(db.augments,augments,QStringLiteral("augment"));return out;
}
