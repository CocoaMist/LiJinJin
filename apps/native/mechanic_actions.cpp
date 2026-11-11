#include "mechanic_actions.h"
#include "hextech_conversion.h"
#include "hextech_mechanism_state.h"
#include "augment_3116.h"
#include <QJsonDocument>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace {
struct Inputs {
    const CombatEntry &entry;const CombatContext &ctx;const CombatStats &stats;
    QJsonObject facts;MechanicLedger &ledger;double time;QStringList missing;
    QJsonValue eval(const QJsonValue &input,int depth=0) {
        if(depth>32){missing.append(QStringLiteral("expression_depth"));return {};}
        if(input.isDouble()||input.isBool()||input.isArray())return input;
        if(input.isObject()){
            const auto o=input.toObject();const auto op=o.value(QStringLiteral("expr")).toString();const auto args=o.value(QStringLiteral("args")).toArray();
            if(op==QStringLiteral("if")&&args.size()==3){const auto test=eval(args[0],depth+1);if(test.isUndefined())return {};return eval(args[test.toBool(test.toDouble()!=0)?1:2],depth+1);}
            if(op==QStringLiteral("eq")&&args.size()==2){const auto left=eval(args[0],depth+1),right=eval(args[1],depth+1);if(left.isUndefined()||right.isUndefined())return {};return left==right;}
            QList<double> nums;for(const auto &a:args){const auto v=eval(a,depth+1);if(!v.isDouble()&&!v.isBool()){missing.append(QStringLiteral("numeric_expression"));return {};}nums.append(v.isBool()?double(v.toBool()):v.toDouble());}
            if(nums.isEmpty()){missing.append(QStringLiteral("empty_expression"));return {};}double n=nums[0];
            if(op==QStringLiteral("add")){n=0;for(double v:nums)n+=v;}
            else if(op==QStringLiteral("mul")){n=1;for(double v:nums)n*=v;}
            else if(op==QStringLiteral("min"))n=*std::min_element(nums.cbegin(),nums.cend());
            else if(op==QStringLiteral("max"))n=*std::max_element(nums.cbegin(),nums.cend());
            else if(nums.size()==2){if(op==QStringLiteral("sub"))n-=nums[1];else if(op==QStringLiteral("div")&&nums[1]!=0)n/=nums[1];else if(op==QStringLiteral("lt"))n=n<nums[1];else if(op==QStringLiteral("eq"))n=n==nums[1];else{missing.append(QStringLiteral("expression_operator"));return {};}}
            else {missing.append(QStringLiteral("expression_operator"));return {};}
            if(!std::isfinite(n)){missing.append(QStringLiteral("nonfinite_expression"));return {};}return n;
        }
        if(!input.isString()){missing.append(QStringLiteral("missing_expression"));return {};}
        const auto key=input.toString();
        if(key.startsWith(QStringLiteral("literal:")))return key.mid(8);
        if(key==QStringLiteral("time"))return time;
        if(key==QStringLiteral("ranged"))return ctx.ranged;
        if(key.startsWith(QStringLiteral("fact:"))){const auto name=key.mid(5);if(facts.contains(name))return facts.value(name);if(ctx.counters.contains(name))return ctx.counters.value(name);missing.append(key);return {};}
        if(key.startsWith(QStringLiteral("state:"))){const auto name=key.mid(6);if(ledger.values.contains(name))return ledger.values.value(name);missing.append(key);return {};}
        if(key.startsWith(QStringLiteral("stat:"))||key.startsWith(QStringLiteral("base:"))){if(auto stat=combatStat(key.mid(5)))return key.startsWith(QStringLiteral("stat:"))?stats[*stat]:ctx.base[*stat];missing.append(key);return {};}
        if(key.startsWith(QStringLiteral("ref:"))){const auto ref=key.mid(4);std::optional<double> n;
            if(ref.startsWith(QStringLiteral("calc:")))n=compileCombatFormula(entry.calculations.value(ref.mid(5)).toObject(),ctx).evaluate(ctx,stats);
            else {const auto v=entry.values.value(ref);if(v.isDouble())n=v.toDouble();else if(v.isArray()&&!v.toArray().isEmpty()&&v.toArray().first().isDouble())n=v.toArray().first().toDouble();}
            if(n&&std::isfinite(*n))return *n;missing.append(key);return {};
        }
        return key;
    }
    QString dynamicKey(const QString &key){
        if(key.startsWith(QStringLiteral("fact:"))){const auto v=eval(key);const auto name=v.toString();if(QStringList{QStringLiteral("physical"),QStringLiteral("magic"),QStringLiteral("true")}.contains(name))return name;missing.append(QStringLiteral("invalid_damage_type"));return {};}
        if(!key.startsWith(QChar(u'@')))return key;
        QString fact=key.mid(1),suffix;
        if(fact.endsWith(QStringLiteral("_damage_amp"))){fact.chop(11);suffix=QStringLiteral("_damage_amp");}
        else if(fact.endsWith(QStringLiteral("_haste"))){fact.chop(6);suffix=QStringLiteral("_haste");}
        const auto name=facts.value(fact).toString();
        if(fact==QStringLiteral("permanent_stack_key")){if(name.startsWith(QStringLiteral("buff:"))||name.startsWith(QStringLiteral("permanent:")))return name;}
        else if(QStringList{QStringLiteral("Q"),QStringLiteral("W"),QStringLiteral("E"),QStringLiteral("R")}.contains(name))return suffix.isEmpty()?name:name.toLower()+suffix;
        missing.append(QStringLiteral("invalid_key:")+key);return {};
    }
};
QJsonArray ids(const QSet<int> &set){auto list=set.values();std::sort(list.begin(),list.end());QJsonArray a;for(int id:list)a.append(id);return a;}
bool validIds(const QJsonValue &v,QList<int> &out){if(!v.isArray())return false;for(const auto &id:v.toArray()){if(!id.isDouble()||id.toDouble()!=id.toInt()||id.toInt()<=0||out.contains(id.toInt()))return false;out.append(id.toInt());}return true;}
QString heartsteelMultiplierKey(){return QStringLiteral("persistent_growth_multiplier:item:3084:permanent_hp");}
QString heartsteelRewardKey(){return QStringLiteral("augment:1152:heartsteel_quest_reward");}
QJsonObject reviewedHeartsteelMultiplier(const CombatEntry &entry){
    const auto contract=entry.triggerContract.value(QStringLiteral("persistent_growth_multiplier")).toObject();
    static const QRegularExpression digest(QStringLiteral("^[0-9a-f]{64}$"));
    if(entry.id!=1152||contract.value(QStringLiteral("item_id")).toInt()!=3084||
       contract.value(QStringLiteral("counter"))!=QStringLiteral("permanent_hp")||
       contract.value(QStringLiteral("multiplier_value"))!=QStringLiteral("StackMultiplication")||
       contract.value(QStringLiteral("activation"))!=QStringLiteral("quest_reward_committed")||
       contract.value(QStringLiteral("source_review"))!=QStringLiteral("steel_your_heart_patch_26_1_review")||
       !digest.match(contract.value(QStringLiteral("review_sha256")).toString()).hasMatch())return {};
    const auto value=entry.values.value(QStringLiteral("StackMultiplication"));
    const auto number=value.isArray()&&!value.toArray().isEmpty()?value.toArray().first():value;
    if(!number.isDouble()||!std::isfinite(number.toDouble())||number.toDouble()<1||number.toDouble()>100)return {};
    auto out=contract;out[QStringLiteral("multiplier")]=number;out[QStringLiteral("source")]=QStringLiteral("augment:1152:");return out;
}
void installHeartsteelMultiplier(const CombatEntry &entry,MechanicLedger &ledger){
    if(!ledger.inventory.contains(3084)||!ledger.augments.contains(1152)||!ledger.values.value(heartsteelRewardKey()).toBool())return;
    const auto modifier=reviewedHeartsteelMultiplier(entry);if(!modifier.isEmpty())ledger.values[heartsteelMultiplierKey()]=modifier;
}
double heartsteelGrowthMultiplier(const MechanicLedger &ledger){
    const auto modifier=ledger.values.value(heartsteelMultiplierKey()).toObject();
    static const QRegularExpression digest(QStringLiteral("^[0-9a-f]{64}$"));
    const double factor=modifier.value(QStringLiteral("multiplier")).toDouble(1);
    if(!ledger.inventory.contains(3084)||!ledger.augments.contains(1152)||!ledger.values.value(heartsteelRewardKey()).toBool()||
       modifier.value(QStringLiteral("source"))!=QStringLiteral("augment:1152:")||modifier.value(QStringLiteral("source_review"))!=QStringLiteral("steel_your_heart_patch_26_1_review")||
       !digest.match(modifier.value(QStringLiteral("review_sha256")).toString()).hasMatch()||!std::isfinite(factor)||factor<1||factor>100)return 1;
    return factor;
}
}

void restoreMechanicGrowthModifiers(const CombatDatabase &db,const CombatContext &ctx,MechanicLedger &ledger){
    // Do not trust an old serialized multiplier after its source contract was
    // removed or changed; reconstruct it from the currently reviewed entry.
    ledger.values.remove(heartsteelMultiplierKey());
    if(!ledger.augments.contains(1152)||!ledger.inventory.contains(3084))return;
    if(ctx.conditions.contains(QStringLiteral("augment:1152:heartsteel_quest_reward_committed")))ledger.values[heartsteelRewardKey()]=true;
    const auto entry=db.augments.constFind(1152);if(entry!=db.augments.cend())installHeartsteelMultiplier(*entry,ledger);
}

CombatStats MechanicLedger::statsAt(double time,bool growthBaselineOnly) const {
    CombatStats out;
    for(const auto &v:effects){const auto e=v.toObject();if(e.value(QStringLiteral("op"))!=QStringLiteral("buff")||e.value(QStringLiteral("target"))!=QStringLiteral("self"))continue;
        if(e.value(QStringLiteral("source"))==QStringLiteral("augment:3081:")&&e.contains(QStringLiteral("item_instance_id"))){
            const auto locked=values.value(QStringLiteral("augment:3081:locked_item")).toObject();
            if(!inventory.contains(e.value(QStringLiteral("item_id")).toInt())||e.value(QStringLiteral("item_id"))!=locked.value(QStringLiteral("item_id"))||
               e.value(QStringLiteral("item_instance_id"))!=locked.value(QStringLiteral("instance_id")))continue;
        }
        if(e.contains(QStringLiteral("conversion_pair"))){bool owned=true;for(const auto &id:e.value(QStringLiteral("source_item_ids")).toArray())owned&=inventory.contains(id.toInt());if(!owned)continue;}
        const double start=e.value(QStringLiteral("start")).toDouble(),end=e.value(QStringLiteral("expires")).toDouble(-1);
        if(time<start||(end>=0&&time>=end))continue;
        if(growthBaselineOnly&&(!e.value(QStringLiteral("growth_baseline")).toBool()||end>=0))continue;
        if(auto stat=combatStat(e.value(QStringLiteral("key")).toString()))out[*stat]+=e.value(QStringLiteral("value")).toDouble();
    }
    return out;
}
QMap<int,CombatStats> MechanicLedger::itemStatsAt(double time) const {
    QMap<int,CombatStats> out;
    for(const auto &v:effects){const auto e=v.toObject();const int item=e.value(QStringLiteral("item_id")).toInt();
        if(item<=0||!inventory.contains(item)||!e.contains(QStringLiteral("item_instance_id")))continue;
        auto one=*this;one.effects=QJsonArray{e};out[item]+=one.statsAt(time);
    }return out;
}
QJsonArray MechanicLedger::itemEffectsAt(double time) const {
    QJsonArray out;
    for(const auto &v:effects){const auto e=v.toObject();
        if(!e.contains(QStringLiteral("item_instance_id"))&&!e.contains(QStringLiteral("conversion_pair")))continue;
        if(e.value(QStringLiteral("op"))!=QStringLiteral("buff")||e.value(QStringLiteral("target"))!=QStringLiteral("self"))continue;
        const double start=e.value(QStringLiteral("start")).toDouble(),end=e.value(QStringLiteral("expires")).toDouble(-1);
        if(time<start||(end>=0&&time>=end))continue;
        auto one=*this;one.effects=QJsonArray{e};const auto stat=combatStat(e.value(QStringLiteral("key")).toString());
        if(stat&&one.statsAt(time)[*stat]!=0)out.append(e);
    }return out;
}
void applyMechanicLedgerContext(const MechanicLedger &l,double time,CombatContext &c,bool replace){
    if(replace){c.eventStats={};c.permanentEventStats={};c.itemAttachedStats.clear();c.itemAttachedEffects={};}
    c.eventStats+=l.statsAt(time);c.permanentEventStats+=l.statsAt(time,true);
    const auto attached=l.itemStatsAt(time);for(auto it=attached.cbegin();it!=attached.cend();++it)c.itemAttachedStats[it.key()]+=it.value();
    for(const auto &v:l.itemEffectsAt(time))c.itemAttachedEffects.append(v);
    // A replay receipt may supersede only the matching, previously valid
    // snapshot. Failed observations and arbitrary external counters retain
    // their original authority and still fail the owned-bucket comparison.
    const auto invalidation=l.values.value(QStringLiteral("augment:3059:item_mana_snapshot_superseded")).toObject();
    const auto counter=QStringLiteral("augment:3059:item_mana");
    const int item=invalidation.value(QStringLiteral("item_id")).toInt();
    const auto locked=l.values.value(QStringLiteral("augment:3081:locked_item")).toObject();
    if(c.counters.contains(counter)&&invalidation.value(QStringLiteral("previous_snapshot")).isDouble()&&
       c.counters.value(counter)==invalidation.value(QStringLiteral("previous_snapshot")).toDouble()&&
       time>=invalidation.value(QStringLiteral("committed_at")).toDouble(1e12)&&attached.value(item)[Stat::Mana]>0&&
       item==locked.value(QStringLiteral("item_id")).toInt()&&
       invalidation.value(QStringLiteral("item_instance_id"))==locked.value(QStringLiteral("instance_id")))c.counters.remove(counter);
    projectHextech40ConversionState(l,c);
}
QJsonObject MechanicLedger::json(double time) const {
    QJsonArray active,pending;QJsonObject totals;
    for(const auto &v:effects){const auto e=v.toObject();const auto op=e.value(QStringLiteral("op")).toString();const double start=e.value(QStringLiteral("start")).toDouble(),end=e.value(QStringLiteral("expires")).toDouble(-1);
        if(start>time){pending.append(e);continue;}if(op==QStringLiteral("buff")||op==QStringLiteral("shield")){if(end<0||time<end)active.append(e);}
        else {const auto key=op+QChar(u':')+e.value(QStringLiteral("key")).toString();totals[key]=totals.value(key).toDouble()+e.value(QStringLiteral("value")).toDouble();}
    }
    return {{QStringLiteral("values"),values},{QStringLiteral("inventory"),ids(inventory)},{QStringLiteral("augments"),ids(augments)},{QStringLiteral("active_effects"),active},{QStringLiteral("scheduled_effects"),pending},{QStringLiteral("raw_event_totals"),totals},{QStringLiteral("self_stat_deltas"),statsAt(time).json()},{QStringLiteral("decisions"),decisions},{QStringLiteral("scope"),QStringLiteral("Explicit-observation replay; raw damage/heal/shield before recipient defenses and map modifiers. No game commands or invented future hits.")}};
}

bool executeMechanicActions(const CombatEntry &entry,const CombatContext &ctx,const CombatStats &stats,const QJsonObject &obs,MechanicLedger &ledger,QStringList *missing,const CombatDatabase *database,bool native3116Receipt) {
    if(entry.id==3116&&obs.value(QStringLiteral("source"))==QStringLiteral("augment"))
        return executeAugment3116(entry,ctx,stats,obs,ledger,missing,native3116Receipt);
    if(hasHextech40Conversion(entry)&&(entry.id==3018||(entry.id==3132&&obs.value(QStringLiteral("event"))==QStringLiteral("observed_crown_state"))))
        return executeHextech40Conversion(database,entry,ctx,obs,ledger,missing);
    if(entry.id==1058&&!entry.mechanismStateV1.isEmpty()&&obs.value(QStringLiteral("event"))==QStringLiteral("skill_on_hit")) {
        const auto facts=obs.value(QStringLiteral("facts")).toObject();
        if(facts.value(QStringLiteral("verified_skill_onhit_eligibility"))!=QJsonValue(true)||
           facts.value(QStringLiteral("verified_mystic_punch_skill_on_hit"))!=QJsonValue(true)||
           facts.value(QStringLiteral("verified_mystic_punch_on_hit"))!=QJsonValue(true)||
           facts.value(QStringLiteral("mystic_punch_slot_contract"))!=QStringLiteral("compiled_slots")) {
            if(missing)missing->append(QStringLiteral("mystic_punch_skill_onhit_contract_unverified"));return false;
        }
        auto canonical=obs;canonical[QStringLiteral("event")]=QStringLiteral("attack_hit");
        canonical[QStringLiteral("origin_event")]=QStringLiteral("skill_on_hit");
        return executeMechanicActions(entry,ctx,stats,canonical,ledger,missing,database);
    }
    if(!entry.mechanismStateV1.isEmpty()&&(entry.id==3049||entry.id==3081))
        return executeHextech40State(database,entry,ctx,stats,obs,ledger,missing);
    if(obs.value(QStringLiteral("id")).toInt()!=entry.id)return false;
    const auto serialized=QString::fromUtf8(QJsonDocument(obs).toJson(QJsonDocument::Compact));
    if(ledger.consumed.contains(serialized))return false;
    const double time=obs.value(QStringLiteral("time")).toDouble(-1);
    if(!std::isfinite(time)||time<0||time<ledger.lastTime){if(missing)missing->append(QStringLiteral("invalid_or_out_of_order_time"));return false;}
    CombatEntry candidate=entry;QJsonArray actions=entry.actions;
    if(obs.value(QStringLiteral("event"))!=entry.triggerContract.value(QStringLiteral("event"))) {
        bool found=false;for(const auto &v:entry.eventRules){const auto rule=v.toObject();if(rule.value(QStringLiteral("event"))==obs.value(QStringLiteral("event"))){candidate.triggerContract=rule;actions=rule.value(QStringLiteral("actions")).toArray();found=true;break;}}
        if(!found)return false;
    }
    if(actions.isEmpty())return false;
    // Inventory requirements are authoritative ledger state. An observation's
    // facts/flags can describe a trigger, but cannot manufacture an owned item.
    // Other condition namespaces keep their existing explicit-input contract.
    static const QRegularExpression itemRequirement(QStringLiteral("^item:([1-9][0-9]*)$"));
    for(const auto &v:candidate.triggerContract.value(QStringLiteral("requires")).toArray()){
        const auto match=itemRequirement.match(v.toString());if(!match.hasMatch())continue;
        bool valid=false;const int id=match.captured(1).toInt(&valid);
        if(!valid||!ledger.inventory.contains(id)){if(missing)missing->append(QStringLiteral("item_not_owned:")+match.captured(1));return false;}
    }
    auto staged=ledger;auto decision=evaluateMechanicTrigger(candidate,ctx,stats,obs,staged.clocks);
    if(!decision.matched){if(missing)*missing=decision.missing;return false;}
    const auto facts=obs.value(QStringLiteral("facts")).toObject();
    for(auto it=facts.begin();it!=facts.end();++it)if(it.value().isDouble()&&!std::isfinite(it.value().toDouble()))return false;
    for(const auto &v:candidate.triggerContract.value(QStringLiteral("distinct_slots")).toArray()){
        const auto name=v.toString();if(!facts.contains(name)){if(missing)missing->append(name);return false;}
    }
    const auto distinct=candidate.triggerContract.value(QStringLiteral("distinct_slots")).toArray();
    if(distinct.size()==2&&facts.value(distinct[0].toString())==facts.value(distinct[1].toString()))return false;
    Inputs in{entry,ctx,stats,facts,staged,time,{}};
    const auto source=obs.value(QStringLiteral("source")).toString()+QChar(u':')+QString::number(entry.id)+QChar(u':');
    const auto sequence=candidate.triggerContract.value(QStringLiteral("sequence")).toObject();
    if(!sequence.isEmpty()){const auto key=sequence.value(QStringLiteral("key")).toString();const auto expected=staged.values.contains(key)?staged.values.value(key):sequence.value(QStringLiteral("initial"));if(facts.value(sequence.value(QStringLiteral("fact")).toString())!=expected){if(missing)missing->append(QStringLiteral("invalid_sequence"));return false;}}
    const auto monotonic=candidate.triggerContract.value(QStringLiteral("monotonic_fact")).toString();
    if(!monotonic.isEmpty()){const auto value=facts.value(monotonic);const auto key=source+QStringLiteral("observed:")+monotonic;if(!value.isDouble()||(staged.values.contains(key)&&value.toDouble()<staged.values.value(key).toDouble())){if(missing)missing->append(QStringLiteral("nonmonotonic_progress"));return false;}staged.values[key]=value;}
    bool heartsteelRewardCommitted=false;
    for(const auto &v:actions){const auto a=v.toObject();const auto op=a.value(QStringLiteral("op")).toString();
        const auto once=a.value(QStringLiteral("once")).toString();if(!once.isEmpty()&&staged.values.value(source+once).toBool())continue;
        if(a.contains(QStringLiteral("when"))){const auto test=in.eval(a.value(QStringLiteral("when")));if(test.isUndefined())break;if(!test.toBool(test.toDouble()!=0))continue;}
        const auto key=in.dynamicKey(a.value(QStringLiteral("key")).toString());auto value=in.eval(a.value(QStringLiteral("value")));
        if(value.isUndefined()||key.isEmpty()||!in.missing.isEmpty())break;
        double growthMultiplier=1;
        // Only a fresh source-formula growth delta is amplified. The damage
        // packet and an already amplified absolute observed counter are not.
        if(source==QStringLiteral("item:3084:")&&obs.value(QStringLiteral("event"))==QStringLiteral("charged_attack")&&op==QStringLiteral("add")&&
           key==QStringLiteral("item:3084:replay_permanent_hp")&&a.value(QStringLiteral("value"))==QStringLiteral("ref:calc:ProcHealthGain")&&value.isDouble()){
            growthMultiplier=heartsteelGrowthMultiplier(staged);value=value.toDouble()*growthMultiplier;
        }
        const double amount=value.toDouble();if(value.isDouble()&&(!std::isfinite(amount)||std::abs(amount)>1e12)){in.missing.append(QStringLiteral("invalid_amount"));break;}
        if(op!=QStringLiteral("set")&&op!=QStringLiteral("configure")&&op!=QStringLiteral("grant_list")&&op!=QStringLiteral("apply_anvil")&&!value.isDouble()){in.missing.append(QStringLiteral("numeric_action_value_required"));break;}
        if((op==QStringLiteral("damage")||op==QStringLiteral("heal")||op==QStringLiteral("shield")||op==QStringLiteral("health_cost")||op==QStringLiteral("execute"))&&amount<0){in.missing.append(QStringLiteral("negative_amount"));break;}
        double duration=-1,delay=0;
        if(a.contains(QStringLiteral("duration")))duration=in.eval(a.value(QStringLiteral("duration"))).toDouble(-2);
        if(a.contains(QStringLiteral("delay")))delay=in.eval(a.value(QStringLiteral("delay"))).toDouble(-1);
        if(duration < -1||delay<0){in.missing.append(QStringLiteral("invalid_time_interval"));break;}
        QJsonObject effect{{QStringLiteral("op"),op},{QStringLiteral("key"),key},{QStringLiteral("value"),value},{QStringLiteral("target"),a.value(QStringLiteral("target")).toString(QStringLiteral("self"))},{QStringLiteral("start"),time+delay},{QStringLiteral("expires"),duration<0?-1:time+delay+duration},{QStringLiteral("source"),source}};
        if(growthMultiplier!=1){effect[QStringLiteral("growth_multiplier")]=growthMultiplier;effect[QStringLiteral("unmodified_growth_value")]=amount/growthMultiplier;effect[QStringLiteral("growth_multiplier_source")]=QStringLiteral("augment:1152:");effect[QStringLiteral("growth_multiplier_review")]=staged.values.value(heartsteelMultiplierKey());}
        if(a.value(QStringLiteral("growth_baseline")).toBool())effect[QStringLiteral("growth_baseline")]=true;
        // An enemy hit identifies the trigger recipient, not the owner of a
        // self buff. Otherwise changing targets incorrectly stacks self buffs.
        if(effect.value(QStringLiteral("target"))!=QStringLiteral("self")&&obs.contains(QStringLiteral("target")))effect[QStringLiteral("target_id")]=obs.value(QStringLiteral("target"));
        if(a.contains(QStringLiteral("damage_ratio")))effect.insert(QStringLiteral("damage_ratio"),in.eval(a.value(QStringLiteral("damage_ratio"))));
        if(op==QStringLiteral("set")||op==QStringLiteral("configure"))staged.values[key]=value;
        else if(op==QStringLiteral("add")||op==QStringLiteral("add_capped")){double n=staged.values.value(key).toDouble()+amount;if(op==QStringLiteral("add_capped"))n=std::min(n,in.eval(a.value(QStringLiteral("cap"))).toDouble());staged.values[key]=n;}
        else if(op==QStringLiteral("reset"))staged.values[QStringLiteral("ready:")+key]=time;
        else if(op==QStringLiteral("reset_source_cooldown")){const auto prefix=source;for(auto it=staged.clocks.begin();it!=staged.clocks.end();)if(it.key().startsWith(prefix))it=staged.clocks.erase(it);else ++it;}
        else if(op==QStringLiteral("expire_buff")){const auto minimum=in.eval(a.value(QStringLiteral("min_lifetime")));if(!minimum.isDouble()||minimum.toDouble()<0){in.missing.append(QStringLiteral("invalid_minimum_lifetime"));break;}for(int n=0;n<staged.effects.size();++n){auto old=staged.effects[n].toObject();const double start=old.value(QStringLiteral("start")).toDouble(),expires=old.value(QStringLiteral("expires")).toDouble(-1);if(old.value(QStringLiteral("op"))==QStringLiteral("buff")&&old.value(QStringLiteral("source"))==source&&old.value(QStringLiteral("key"))==key&&old.value(QStringLiteral("target"))==QStringLiteral("self")&&time>=start&&(expires<0||time<expires)){old[QStringLiteral("expires")]=expires<0?std::max(time,start+minimum.toDouble()):std::min(expires,std::max(time,start+minimum.toDouble()));staged.effects[n]=old;}}}
        else if(op==QStringLiteral("refund")){const auto remain=in.eval(a.value(QStringLiteral("remaining")));if(!remain.isDouble()||remain.toDouble()<0||amount<0){in.missing.append(QStringLiteral("invalid_remaining_cooldown"));break;}staged.values[QStringLiteral("ready:")+key]=time+std::max(0.,remain.toDouble()-amount);effect[QStringLiteral("value")]=std::min(amount,remain.toDouble());}
        else if(op==QStringLiteral("consume_charge")){const double n=in.eval(a.value(QStringLiteral("available"))).toDouble(-1);if(n<amount||amount<0){in.missing.append(QStringLiteral("insufficient_charges"));break;}staged.values[key+QStringLiteral("_charges")]=n-amount;}
        else if(op==QStringLiteral("slot_haste")){const auto eligibleSlots=in.eval(a.value(QStringLiteral("slots")));if(!eligibleSlots.isArray()){in.missing.append(QStringLiteral("invalid_slots"));break;}if(!eligibleSlots.toArray().contains(key))continue;effect[QStringLiteral("op")]=QStringLiteral("buff");effect[QStringLiteral("key")]=key.toLower()+QStringLiteral("_haste");}
        else if(op==QStringLiteral("quest_buff")||op==QStringLiteral("quest_item")){
            const auto progress=in.eval(a.value(QStringLiteral("progress"))),threshold=in.eval(a.value(QStringLiteral("threshold")));
            if(!progress.isDouble()||!threshold.isDouble()||threshold.toDouble()<=0||progress.toDouble()<0){in.missing.append(QStringLiteral("invalid_quest_progress"));break;}
            staged.values[source+QStringLiteral("quest_progress")]=progress;if(progress.toDouble()<threshold.toDouble())continue;
            const auto completed=source+QStringLiteral("quest_completed:")+key;if(staged.values.value(completed).toBool())continue;
            if(op==QStringLiteral("quest_item")){if(amount<=0||amount!=value.toInt()){in.missing.append(QStringLiteral("invalid_reward_item"));break;}if(staged.inventory.size()>=6&&!staged.inventory.contains(value.toInt())){staged.values[source+QStringLiteral("pending_reward")]=value;continue;}staged.inventory.insert(value.toInt());}
            else {effect[QStringLiteral("op")]=QStringLiteral("buff");if(a.contains(QStringLiteral("cap")))effect[QStringLiteral("value")]=std::min(amount,in.eval(a.value(QStringLiteral("cap"))).toDouble());}
            staged.values[completed]=true;
        }
        else if(op==QStringLiteral("grant_item")){if(amount<=0||amount!=value.toInt()){in.missing.append(QStringLiteral("invalid_item"));break;}if(staged.inventory.size()>=6&&!staged.inventory.contains(value.toInt())){staged.values[source+QStringLiteral("pending_reward")]=value;continue;}staged.inventory.insert(value.toInt());}
        else if(op==QStringLiteral("transform_items")){QList<int> required;const auto items=a.value(QStringLiteral("items"));if(!validIds(items.isArray()?items:in.eval(items),required)||required.isEmpty()){in.missing.append(QStringLiteral("invalid_recipe"));break;}bool owns=true;for(int id:required)owns&=staged.inventory.contains(id);if(!owns){in.missing.append(QStringLiteral("recipe_not_owned"));break;}if(amount<=0||amount!=value.toInt()){in.missing.append(QStringLiteral("invalid_reward_item"));break;}for(int id:required)staged.inventory.remove(id);staged.inventory.insert(value.toInt());}
        else if(op==QStringLiteral("grant_list")){QList<int> list;if(!validIds(value,list)){in.missing.append(QStringLiteral("invalid_granted_ids"));break;}const int count=in.eval(a.value(QStringLiteral("count"))).toInt(-1);if(count<0||list.size()!=count){in.missing.append(QStringLiteral("unexpected_grant_count"));break;}if(key==QStringLiteral("augments")){if(a.value(QStringLiteral("replace")).toBool())staged.augments.clear();for(int id:list)staged.augments.insert(id);}else for(int id:list)staged.values[key+QChar(u':')+QString::number(id)]=true;}
        else if(op==QStringLiteral("apply_anvil")){if(!value.isArray()||(a.contains(QStringLiteral("count"))&&value.toArray().size()!=in.eval(a.value(QStringLiteral("count"))).toInt(-1))){in.missing.append(QStringLiteral("invalid_anvil_choices"));break;}for(const auto &choice:value.toArray()){const auto c=choice.toObject();const auto stat=c.value(QStringLiteral("stat")).toString();if(!combatStat(stat)||!c.value(QStringLiteral("value")).isDouble()||!std::isfinite(c.value(QStringLiteral("value")).toDouble())){in.missing.append(QStringLiteral("invalid_anvil_stat"));break;}auto e=effect;e[QStringLiteral("op")]=QStringLiteral("buff");e[QStringLiteral("key")]=stat;e[QStringLiteral("value")]=c.value(QStringLiteral("value"));staged.effects.append(e);}}
        else if(op==QStringLiteral("summon_scale")){const auto base=in.eval(a.value(QStringLiteral("base")));if(!base.isDouble()||base.toDouble()<0){in.missing.append(QStringLiteral("invalid_summon_base"));break;}staged.values[QStringLiteral("summon:")+key]=base.toDouble()*(1+amount);}
        else if(op==QStringLiteral("base_stats")){staged.values[source+QStringLiteral("base_stats_included")]=true;}
        else if(op==QStringLiteral("block_spell")){staged.values[source+QStringLiteral("blocked_spells")]=staged.values.value(source+QStringLiteral("blocked_spells")).toDouble()+1;}
        else if(op==QStringLiteral("cleanse")){staged.values[QStringLiteral("cleansed:")+key]=time;}
        else if(op==QStringLiteral("health_cost")||op==QStringLiteral("redirect_damage")||op==QStringLiteral("shield_reduce")||op==QStringLiteral("incoming_reduction")||op==QStringLiteral("execute")){staged.values[source+op]=staged.values.value(source+op).toDouble()+amount;}
        else if(op==QStringLiteral("grant_anvils")||op==QStringLiteral("cast")||op==QStringLiteral("summon")||op==QStringLiteral("drop")||op==QStringLiteral("dash")){staged.values[source+key]=staged.values.value(source+key).toDouble()+amount;}
        else if(op!=QStringLiteral("buff")&&op!=QStringLiteral("damage")&&op!=QStringLiteral("heal")&&op!=QStringLiteral("shield")){in.missing.append(QStringLiteral("unsupported_action:")+op);break;}
        if(!once.isEmpty())staged.values[source+once]=true;
        // Re-observing a refreshable effect replaces its previous lifetime;
        // stackable bonuses must use an explicit counter/add action instead.
        if(effect.value(QStringLiteral("op"))==QStringLiteral("buff")&&delay==0){for(int n=staged.effects.size()-1;n>=0;--n){auto previous=staged.effects[n].toObject();if(previous.value(QStringLiteral("op"))==QStringLiteral("buff")&&previous.value(QStringLiteral("source"))==effect.value(QStringLiteral("source"))&&previous.value(QStringLiteral("key"))==effect.value(QStringLiteral("key"))&&previous.value(QStringLiteral("target"))==effect.value(QStringLiteral("target"))&&previous.value(QStringLiteral("target_id"))==effect.value(QStringLiteral("target_id")))staged.effects.removeAt(n);}}
        staged.effects.append(effect);
        if(source==QStringLiteral("augment:1152:")&&once==QStringLiteral("heartsteel_quest_reward")&&op==QStringLiteral("buff")&&key==QStringLiteral("hp")&&effect.value(QStringLiteral("target"))==QStringLiteral("self")&&a.value(QStringLiteral("growth_baseline")).toBool()&&duration<0&&delay==0)heartsteelRewardCommitted=true;
    }
    if(!in.missing.isEmpty()){in.missing.removeDuplicates();if(missing)*missing=in.missing;return false;}
    const int lockedItem=hextech40LockedItem(ledger);
    if(lockedItem>0&&!staged.inventory.contains(lockedItem)){if(missing)missing->append(QStringLiteral("destiny_locked_item_transform_identity_unreviewed"));return false;}
    if(entry.id==3132&&hasHextech40Conversion(entry)&&
       (obs.value(QStringLiteral("event"))==QStringLiteral("takedown")||obs.value(QStringLiteral("event"))==QStringLiteral("death"))){
        staged.values.remove(QStringLiteral("augment:3132:observed_state"));
        staged.values[QStringLiteral("augment:3132:state_invalidated")]=true;
    }
    if(heartsteelRewardCommitted)installHeartsteelMultiplier(entry,staged);
    staged.consumed.insert(serialized);staged.lastTime=time;ledger=std::move(staged);return true;
}

MechanicLedger replayCombatMechanics(const CombatDatabase &db,const CombatContext &ctx,const QList<int> &items,const QList<int> &augments,const QJsonArray &observations){
    MechanicLedger ledger;for(int id:items)ledger.inventory.insert(id);for(int id:augments)ledger.augments.insert(id);
    restoreMechanicGrowthModifiers(db,ctx,ledger);
    const auto evaluation=evaluateCombat(db,ctx,items,augments);const auto stats=evaluation.valid?evaluation.stats:ctx.base;
    const double baseline=ctx.counters.value(QStringLiteral("mechanic_baseline_time"),-1);
    const bool hasBaseline=std::isfinite(baseline)&&baseline>=0;
    for(const auto &v:observations){const auto obs=v.toObject();const auto source=obs.value(QStringLiteral("source")).toString();const int id=obs.value(QStringLiteral("id")).toInt();
        // The snapshot already contains changes through the baseline instant.
        // Only strictly later events can contribute replay deltas a second time.
        if(hasBaseline&&obs.value(QStringLiteral("time")).toDouble(-1)<=baseline)continue;
        const CombatEntry *entry=nullptr;if(source==QStringLiteral("item")&&items.contains(id)){const auto it=db.items.constFind(id);if(it!=db.items.cend())entry=&*it;}
        if(source==QStringLiteral("augment")&&augments.contains(id)){const auto it=db.augments.constFind(id);if(it!=db.augments.cend())entry=&*it;}
        if(!entry||(entry->actions.isEmpty()&&entry->eventRules.isEmpty())||obs.value(QStringLiteral("time")).toDouble(-1)>ctx.counters.value(QStringLiteral("observed_game_time"),1e12))continue;
        auto active=ctx;applyMechanicLedgerContext(ledger,obs.value(QStringLiteral("time")).toDouble(),active);
        const auto recalculated=evaluateCombat(db,active,items,augments);const auto currentStats=recalculated.valid?recalculated.stats:stats;
        const double eventTime=obs.value(QStringLiteral("time")).toDouble();
        const auto itemManaBefore=ledger.itemStatsAt(eventTime);
        QStringList missing;const bool applied=executeMechanicActions(*entry,active,currentStats,obs,ledger,&missing,&db);
        const auto snapshot=QStringLiteral("augment:3059:item_mana");
        const auto receiptKey=QStringLiteral("augment:3059:item_mana_snapshot_superseded");
        // Admit this invalidation only after a real mana-affix commit whose
        // input comparison was valid. A stale external snapshot cannot become
        // valid merely because a later growth observation was accepted.
        if(applied&&evaluation.valid&&recalculated.valid&&augments.contains(3059)&&
           source==QStringLiteral("augment")&&id==3081&&obs.value(QStringLiteral("event"))==QStringLiteral("destiny_item_takedown")&&
           ctx.counters.contains(snapshot)&&!ledger.values.contains(receiptKey)){
            const auto after=ledger.itemStatsAt(eventTime);const auto locked=ledger.values.value(QStringLiteral("augment:3081:locked_item")).toObject();
            const int item=locked.value(QStringLiteral("item_id")).toInt();
            if(after.value(item)[Stat::Mana]>itemManaBefore.value(item)[Stat::Mana])ledger.values[receiptKey]=QJsonObject{
                {QStringLiteral("previous_snapshot"),ctx.counters.value(snapshot)},{QStringLiteral("committed_at"),eventTime},
                {QStringLiteral("item_id"),item},{QStringLiteral("item_instance_id"),locked.value(QStringLiteral("instance_id"))},
                {QStringLiteral("event_id"),obs.value(QStringLiteral("event_id"))}};
        }
        ledger.decisions.append(QJsonObject{{QStringLiteral("source"),source},{QStringLiteral("id"),id},{QStringLiteral("event"),obs.value(QStringLiteral("event"))},{QStringLiteral("applied"),applied},{QStringLiteral("missing"),QJsonArray::fromStringList(missing)}});
    }
    return ledger;
}
