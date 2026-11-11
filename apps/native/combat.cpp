#include "combat.h"
#include "combat_unknown_policy.h"
#include "hextech_conversion.h"
#include "combat_sustain.h"
#include "mechanic_actions.h"
#include "growth.h"
#include "growth_objective.h"
#include "analysis.h"
#include <QRegularExpression>
#include "headliner.h"
#include "decision_context.h"
#include "mechanic_triggers.h"
#include "role_guidance.h"
#include "build_reasoning.h"
#include "mode_context_rules.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QFile>
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include <limits>
#include <functional>
#include <QHash>

namespace {
const QStringList statNames={QStringLiteral("ap"),QStringLiteral("ad"),QStringLiteral("hp"),QStringLiteral("mana"),QStringLiteral("armor"),QStringLiteral("mr"),QStringLiteral("bonus_as"),QStringLiteral("as"),QStringLiteral("crit"),QStringLiteral("crit_damage"),QStringLiteral("haste"),QStringLiteral("ms"),QStringLiteral("ms_percent"),QStringLiteral("magic_pen"),QStringLiteral("magic_pen_percent"),QStringLiteral("armor_pen"),QStringLiteral("armor_pen_percent"),QStringLiteral("lethality"),QStringLiteral("lifesteal"),QStringLiteral("omnivamp"),QStringLiteral("heal_shield"),QStringLiteral("tenacity"),QStringLiteral("slow_resist"),QStringLiteral("range"),QStringLiteral("hp_regen"),QStringLiteral("mana_regen"),QStringLiteral("hp_regen_percent"),QStringLiteral("mana_regen_percent"),QStringLiteral("cooldown_reduction"),QStringLiteral("as_multiplier"),QStringLiteral("item_haste"),QStringLiteral("q_haste"),QStringLiteral("w_haste"),QStringLiteral("e_haste"),QStringLiteral("r_haste"),QStringLiteral("spell_amp"),QStringLiteral("damage_amp"),QStringLiteral("shield"),QStringLiteral("shred"),QStringLiteral("armor_shred"),QStringLiteral("magic_shred"),QStringLiteral("magic_amp")};
CombatStats readStats(const QJsonObject &o) {CombatStats s;for(auto it=o.begin();it!=o.end();++it)if(auto k=combatStat(it.key());k&&it.value().isDouble())s[*k]=it.value().toDouble();return s;}
double penMultiplier(double r,double percent,double flat,double shred=0) {return resistanceMultiplier(r>0?std::max(0.0,r*(1-std::clamp(shred,0.,1.))*(1-std::clamp(percent,0.,1.))-flat):r);}
double damageMultiplier(const QString &type,const CombatStats &s,const CombatContext &c,const QMap<QString,double> &typeAmps={}) {
    const double amp=typeAmps.value(type,1);
    if(type==QStringLiteral("true"))return amp;
    if(type==QStringLiteral("physical"))return amp*penMultiplier(c.targetArmor,s[Stat::ArmorPenPercent],s[Stat::ArmorPen]+s[Stat::Lethality],1-(1-s[Stat::Shred])*(1-s[Stat::ArmorShred]));
    if(type==QStringLiteral("magic"))return amp*(1+s[Stat::MagicAmp])*penMultiplier(c.targetMr,s[Stat::MagicPenPercent],s[Stat::MagicPen],1-(1-s[Stat::Shred])*(1-s[Stat::MagicShred]));
    if(type==QStringLiteral("adaptive"))return s[Stat::Ap]>s[Stat::Ad]-c.base[Stat::Ad]?damageMultiplier(QStringLiteral("magic"),s,c,typeAmps):damageMultiplier(QStringLiteral("physical"),s,c,typeAmps);
    return 0;
}
enum Op {Constant,StatValue,Add,Multiply,Clamp,HealthValue,Minimum,Maximum};
std::optional<bool> castRequirement(const QJsonObject &o,const CombatContext &ctx,int rank,int depth=0) {
    if(depth>12)return {};std::optional<bool> result;
    const auto kind=o.value(QStringLiteral("__type")).toString();
    if(kind==QStringLiteral("IsRangedCastRequirement"))result=ctx.ranged;
    else if(kind==QStringLiteral("HasBuffCastRequirement")||kind==QStringLiteral("AboveHealthPercentCastRequirement")) {
        const auto key=o.value(QStringLiteral("input")).toString();if(!ctx.counters.contains(key))return {};
        const double value=ctx.counters.value(key);if(!std::isfinite(value)||value<0||value>1)return {};
        if(kind==QStringLiteral("HasBuffCastRequirement")){if(value!=0&&value!=1)return {};result=value==1;}
        else {const auto threshold=o.value(QStringLiteral("threshold")).toObject();double limit=std::numeric_limits<double>::quiet_NaN();
            if(threshold.value(QStringLiteral("op"))==QStringLiteral("const"))limit=threshold.value(QStringLiteral("value")).toDouble();
            else {const auto a=threshold.value(QStringLiteral("values")).toArray();if(rank>=0&&rank<a.size()&&a[rank].isDouble())limit=a[rank].toDouble();}
            if(!std::isfinite(limit)||limit<0||limit>1)return {};result=value>limit;}
    } else if(kind==QStringLiteral("HasAllSubRequirementsCastRequirement")) {
        const auto rows=o.value(QStringLiteral("mSubRequirements")).toArray();if(rows.isEmpty())return {};bool missing=false;result=true;
        for(const auto &row:rows){const auto value=castRequirement(row.toObject(),ctx,rank,depth+1);if(value&&!*value){result=false;missing=false;break;}if(!value)missing=true;}
        if(missing)return {};
    } else return {};
    if(o.value(QStringLiteral("mInvertResult")).toBool())return !*result;return result;
}
bool compileNode(const QJsonObject &o,const CombatContext &ctx,int rank,QVector<FormulaInstruction> &code,int depth=0) {
    if(depth>48)return false;
    const auto op=o.value(QStringLiteral("op")).toString();FormulaInstruction n;
    if(op==QStringLiteral("const")) {if(!o.value(QStringLiteral("value")).isDouble())return false;n.a=o.value(QStringLiteral("value")).toDouble();}
    else if(op==QStringLiteral("rank_level_table")) {
        const auto rows=o.value(QStringLiteral("values")).toArray();if(rank<0||rank>=rows.size())return false;
        const auto a=rows[rank].toArray();if(ctx.level<1||ctx.level>=a.size()||!a[ctx.level].isDouble())return false;n.a=a[ctx.level].toDouble();
    } else if(op==QStringLiteral("fixed_rank")||op==QStringLiteral("source_rank")) {
        int fixed=o.value(QStringLiteral("rank")).toInt();if(op==QStringLiteral("source_rank")){
            const auto key=o.value(QStringLiteral("key")).toString();if(!ctx.counters.contains(key))return false;
            const double v=ctx.counters.value(key);if(!std::isfinite(v)||std::floor(v)!=v||v<0||v>6)return false;fixed=int(v);
        }
        if(fixed<0||fixed>6)return false;return compileNode(o.value(QStringLiteral("arg")).toObject(),ctx,fixed,code,depth+1);
    } else if(op==QStringLiteral("rank")||op==QStringLiteral("level_table")) {
        const auto a=o.value(QStringLiteral("values")).toArray();int ix=op==QStringLiteral("rank")?rank:ctx.level;
        if(ix<0||ix>=a.size()||!a[ix].isDouble())return false;n.a=a[ix].toDouble();
    } else if(op==QStringLiteral("level_linear")) {
        const auto policy=o.value(QStringLiteral("scale_past_default_max_level"));
        if(!policy.isUndefined()&&!policy.isBool())return false;
        // Legacy compiled expressions remain valid at 1..18. Missing metadata
        // must not become permission to extrapolate if that range is extended.
        if(ctx.level>18&&policy.isUndefined())return false;
        const int formulaLevel=policy.isBool()&&!policy.toBool()?std::min(ctx.level,18):ctx.level;
        n.a=o.value(QStringLiteral("start")).toDouble()+(o.value(QStringLiteral("end")).toDouble()-o.value(QStringLiteral("start")).toDouble())*(formulaLevel-1)/17.;
    } else if(op==QStringLiteral("context")) {
        const auto integer=o.value(QStringLiteral("integer"));
        if(!integer.isUndefined()&&!integer.isBool())return false;
        const auto key=o.value(QStringLiteral("key")).toString();
        if(key==QStringLiteral("target_hp"))n.a=ctx.targetHp;
        else if(ctx.counters.contains(key))n.a=ctx.counters.value(key);
        else return false;
        if(integer.toBool()&&(!std::isfinite(n.a)||std::floor(n.a)!=n.a))return false;
        if((o.contains(QStringLiteral("min"))&&n.a<o.value(QStringLiteral("min")).toDouble())||(o.contains(QStringLiteral("max"))&&n.a>o.value(QStringLiteral("max")).toDouble()))return false;
    } else if(op==QStringLiteral("health")) {
        const auto subject=o.value(QStringLiteral("subject")).toString();if(subject!=QStringLiteral("self")&&subject!=QStringLiteral("target"))return false;
        n.part=subject==QStringLiteral("target");const auto measure=o.value(QStringLiteral("measure")).toString();
        n.stat=QStringList{QStringLiteral("max"),QStringLiteral("current"),QStringLiteral("current_fraction"),QStringLiteral("missing"),QStringLiteral("missing_fraction")}.indexOf(measure);if(n.stat<0)return false;n.op=HealthValue;
        if(n.stat){const auto fraction=subject+QStringLiteral("_health_fraction"),raw=subject+QStringLiteral("_current_hp");
            if(ctx.counters.contains(fraction)){n.a=ctx.counters.value(fraction);if(!std::isfinite(n.a)||n.a<0||n.a>1)return false;
                if(ctx.counters.contains(raw)){n.count=2;n.b=ctx.counters.value(raw);if(!std::isfinite(n.b)||n.b<0)return false;}}
            else if(ctx.counters.contains(raw)){n.count=1;n.a=ctx.counters.value(raw);if(!std::isfinite(n.a)||n.a<0)return false;}
            else return false;
        }
    } else if(op==QStringLiteral("requirement_select")) {
        const auto condition=castRequirement(o.value(QStringLiteral("requirement")).toObject(),ctx,rank);if(!condition)return false;
        return compileNode(o.value(*condition?QStringLiteral("yes"):QStringLiteral("no")).toObject(),ctx,rank,code,depth+1);
    } else if(op==QStringLiteral("stat")) {
        const auto name=o.value(QStringLiteral("stat")).toString();n.op=StatValue;n.part=o.value(QStringLiteral("part")).toInt();
        if(name==QStringLiteral("cooldown_multiplier"))n.stat=-1;
        else if(name==QStringLiteral("attack_speed_multiplier"))n.stat=-2;
        else {auto s=combatStat(name);if(!s)return false;n.stat=static_cast<int>(*s);}
    } else if(op==QStringLiteral("ranged"))return compileNode(o.value(ctx.ranged?QStringLiteral("yes"):QStringLiteral("no")).toObject(),ctx,rank,code,depth+1);
    else if(op==QStringLiteral("add")||op==QStringLiteral("mul")||op==QStringLiteral("min")||op==QStringLiteral("max")) {
        const auto args=o.value(QStringLiteral("args")).toArray();if(args.isEmpty())return false;
        for(const auto &a:args)if(!compileNode(a.toObject(),ctx,rank,code,depth+1))return false;
        n.op=op==QStringLiteral("add")?Add:op==QStringLiteral("mul")?Multiply:op==QStringLiteral("min")?Minimum:Maximum;n.count=args.size();
    } else if(op==QStringLiteral("clamp")) {
        if(!compileNode(o.value(QStringLiteral("arg")).toObject(),ctx,rank,code,depth+1))return false;
        n.op=Clamp;n.a=o.value(QStringLiteral("min")).toDouble();n.b=o.value(QStringLiteral("max")).toDouble();
    } else return false;
    code.append(n);return std::isfinite(n.a)&&std::isfinite(n.b);
}
enum class EffectOp {Unknown,Add,Transform,Convert,Amp,BonusAmp,Penetration,Resist,Range,Derived,MagicDamageAmp,DamageTypeAmp,Flag,ExtraTrue,SpeedDamage,SpellCrit,CritDefense,ObservedDamage,ObservedHeal,Shield,Onhit,CurrentHpOnhit,NthOnhit,BurnOnhit,Proc,Spellblade,SpellPercentHp,Stack,Mechanic};
EffectOp effectOp(const QString &name) {
    static const QHash<QString,EffectOp> kinds{
        {QStringLiteral("add"),EffectOp::Add},{QStringLiteral("transform"),EffectOp::Transform},{QStringLiteral("convert"),EffectOp::Convert},{QStringLiteral("amp"),EffectOp::Amp},{QStringLiteral("bonus_amp"),EffectOp::BonusAmp},
        {QStringLiteral("penetration"),EffectOp::Penetration},{QStringLiteral("resist"),EffectOp::Resist},{QStringLiteral("range"),EffectOp::Range},{QStringLiteral("derived"),EffectOp::Derived},{QStringLiteral("magic_damage_amp"),EffectOp::MagicDamageAmp},{QStringLiteral("damage_type_amp"),EffectOp::DamageTypeAmp},
        {QStringLiteral("flag"),EffectOp::Flag},{QStringLiteral("extra_true"),EffectOp::ExtraTrue},{QStringLiteral("speed_damage"),EffectOp::SpeedDamage},{QStringLiteral("spell_crit"),EffectOp::SpellCrit},{QStringLiteral("crit_defense"),EffectOp::CritDefense},
        {QStringLiteral("observed_damage"),EffectOp::ObservedDamage},{QStringLiteral("observed_heal"),EffectOp::ObservedHeal},{QStringLiteral("shield"),EffectOp::Shield},{QStringLiteral("onhit"),EffectOp::Onhit},{QStringLiteral("current_hp_onhit"),EffectOp::CurrentHpOnhit},
        {QStringLiteral("nth_onhit"),EffectOp::NthOnhit},{QStringLiteral("burn_onhit"),EffectOp::BurnOnhit},{QStringLiteral("proc"),EffectOp::Proc},{QStringLiteral("spellblade"),EffectOp::Spellblade},{QStringLiteral("spell_percent_hp"),EffectOp::SpellPercentHp},
        {QStringLiteral("stack"),EffectOp::Stack},{QStringLiteral("mechanic"),EffectOp::Mechanic}};
    return kinds.value(name,EffectOp::Unknown);
}
struct Effect {
    EffectOp op=EffectOp::Unknown;QString type,trigger,counter,ownerKey;
    Stat stat=Stat::Ap,source=Stat::Ap;
    bool hasStat=false,hasSource=false,consume=false,percentHp=false,targetHp=false,targetCurrentHp=false,canCrit=false,critBonusScaled=false;
    double scale=1,cap=std::numeric_limits<double>::infinity(),cooldown=0,apRatio=0,heal=0,duration=0,count=1;
    int every=1;
    CombatFormula formula,targetHealth;
};
struct PreparedEntry {const CombatEntry *entry=nullptr;QVector<Effect> effects;QStringList unknowns;};
CombatFormula scalar(double value) {CombatFormula f;f.code.append(FormulaInstruction{Constant,0,0,0,value,0});f.valid=std::isfinite(value);return f;}
CombatFormula valueFormula(const CombatEntry &entry,const QJsonValue &value,const CombatContext &c) {
    if(value.isDouble())return scalar(value.toDouble());
    if(value.isObject())return compileCombatFormula(value.toObject(),c,1);
    const auto name=value.toString();
    if(name.startsWith(QStringLiteral("calc:")))return compileCombatFormula(entry.calculations.value(name.mid(5)).toObject(),c,1);
    const auto v=entry.values.value(name);
    if(v.isDouble())return scalar(v.toDouble());
    if(v.isArray()){const auto a=v.toArray();if(!a.isEmpty()&&a[0].isDouble())return scalar(a[0].toDouble());}
    return {};
}
double scalarValue(const CombatEntry &entry,const QJsonValue &v,const CombatContext &c,double fallback=0) {
    return valueFormula(entry,v,c).evaluate(c,c.base).value_or(fallback);
}
PreparedEntry prepare(const CombatEntry &entry,const CombatContext &c,bool augment) {
    PreparedEntry p;p.entry=&entry;
    if(entry.coverage!=QStringLiteral("modeled_numeric_effects"))p.unknowns.append(QStringLiteral("%1:%2:partial_mechanics").arg(augment?QStringLiteral("augment"):QStringLiteral("item")).arg(entry.id));
    if(entry.triggerContract.value(QStringLiteral("activation_status"))==QStringLiteral("unverified"))p.unknowns.append(QStringLiteral("%1:%2:activation_unverified").arg(augment?QStringLiteral("augment"):QStringLiteral("item")).arg(entry.id));
    if(augment&&entry.id==3132&&hasHextech40Conversion(entry)&&!hextech40CrownReady(c))return p;
    for(const auto &v:entry.effects) {
        const auto o=v.toObject();
        if(augment&&hasHextech40Conversion(entry)&&(entry.id==3059||entry.id==3058)&&o.value(QStringLiteral("op"))==QStringLiteral("add"))continue;
        const auto condition=o.value(QStringLiteral("condition")).toString();
        // A documented utility mechanic has no executable numeric value.
        if(o.value(QStringLiteral("op")).toString()==QStringLiteral("mechanic"))continue;
        if(!condition.isEmpty()&&!c.conditions.contains(condition)){p.unknowns.append(QStringLiteral("condition:%1:%2").arg(entry.id).arg(condition));continue;}
        bool incomplete=false;
        for(const auto &field:{QStringLiteral("cooldown"),QStringLiteral("ap_ratio"),QStringLiteral("cap"),QStringLiteral("ranged_value"),QStringLiteral("duration"),QStringLiteral("count"),QStringLiteral("heal_multiplier")})
            if(o.contains(field)&&!valueFormula(entry,o.value(field),c).valid){p.unknowns.append(QStringLiteral("effect_parameter:%1:%2").arg(entry.id).arg(field));incomplete=true;}
        if(incomplete)continue;
        Effect ef;ef.op=effectOp(o.value(QStringLiteral("op")).toString());ef.type=o.value(QStringLiteral("stat")).toString();ef.trigger=o.value(QStringLiteral("trigger")).toString();
        ef.ownerKey=(augment?QStringLiteral("augment:"):QStringLiteral("item:"))+QString::number(entry.id);
        if(ef.op==EffectOp::DamageTypeAmp&&!QStringList{QStringLiteral("physical"),QStringLiteral("magic"),QStringLiteral("true")}.contains(ef.type)){p.unknowns.append(QStringLiteral("effect_damage_type:%1").arg(entry.id));continue;}
        auto st=combatStat(ef.type);ef.hasStat=bool(st);if(st)ef.stat=*st;
        auto source=o.value(QStringLiteral("source")).toString();
        if(source==QStringLiteral("base_ad")||source==QStringLiteral("bonus_ad")) {ef.source=Stat::Ad;ef.hasSource=true;}
        else if(source==QStringLiteral("bonus_hp")){ef.source=Stat::Hp;ef.hasSource=true;}
        else if(source==QStringLiteral("bonus_mana")){ef.source=Stat::Mana;ef.hasSource=true;}
        else if(auto s=combatStat(source)){ef.source=*s;ef.hasSource=true;}
        ef.counter=source; // source part retained for base/bonus transforms.
        QJsonValue val=o.value(QStringLiteral("value"));
        if(c.ranged&&o.contains(QStringLiteral("ranged_value")))val=o.value(QStringLiteral("ranged_value"));
        ef.formula=valueFormula(entry,val,c);ef.scale=o.value(QStringLiteral("scale")).toDouble(1)*(o.value(QStringLiteral("negative")).toBool()?-1:1);
        ef.percentHp=o.value(QStringLiteral("percent_hp")).toBool();ef.consume=o.value(QStringLiteral("consume")).toBool();
        ef.canCrit=o.value(QStringLiteral("can_crit")).toBool();
        ef.cooldown=scalarValue(entry,o.value(QStringLiteral("cooldown")),c,0);ef.apRatio=scalarValue(entry,o.value(QStringLiteral("ap_ratio")),c,0);ef.heal=scalarValue(entry,o.value(QStringLiteral("heal_multiplier")),c,0);
        ef.duration=scalarValue(entry,o.value(QStringLiteral("duration")),c,0);ef.count=scalarValue(entry,o.value(QStringLiteral("count")),c,1);
        ef.every=o.value(QStringLiteral("every")).toInt(1);
        if(o.contains(QStringLiteral("cap")))ef.cap=scalarValue(entry,o.value(QStringLiteral("cap")),c,0);
        ef.targetHp=o.value(QStringLiteral("target_hp")).toBool();ef.targetCurrentHp=o.value(QStringLiteral("target_current_hp")).toBool();
        if(ef.op==EffectOp::CurrentHpOnhit||ef.targetCurrentHp){
            ef.targetHealth=compileCombatFormula(QJsonObject{{QStringLiteral("op"),QStringLiteral("health")},{QStringLiteral("subject"),QStringLiteral("target")},{QStringLiteral("measure"),QStringLiteral("current")}},c);
            if(!ef.targetHealth.evaluate(c,c.base)){p.unknowns.append(QStringLiteral("target_current_hp:%1").arg(entry.id));continue;}
        }
        if(o.contains(QStringLiteral("below_target_health"))){
            const auto threshold=valueFormula(entry,o.value(QStringLiteral("below_target_health")),c).evaluate(c,c.base);
            const auto fraction=compileCombatFormula(QJsonObject{{QStringLiteral("op"),QStringLiteral("health")},{QStringLiteral("subject"),QStringLiteral("target")},{QStringLiteral("measure"),QStringLiteral("current_fraction")}},c).evaluate(c,c.base);
            if(!threshold||*threshold<0||*threshold>1){p.unknowns.append(QStringLiteral("effect_parameter:%1:below_target_health").arg(entry.id));continue;}
            if(!fraction){p.unknowns.append(QStringLiteral("target_current_hp:%1").arg(entry.id));continue;}
            // BIN thresholds and the game's health comparison are float32.
            if(!(static_cast<float>(*fraction)<static_cast<float>(*threshold)))continue;
        }
        ef.critBonusScaled=o.value(QStringLiteral("crit_bonus_scaled")).toBool();
        if(ef.critBonusScaled){
            const auto key=QStringLiteral("target_crit_damage_multiplier");
            if(!c.counters.contains(key)||!std::isfinite(c.counters.value(key))||c.counters.value(key)<0||c.counters.value(key)>1){p.unknowns.append(key);continue;}
            ef.scale*=c.counters.value(key);
        }
        if(c.ranged)ef.scale*=o.value(QStringLiteral("ranged_scale")).toDouble(1);
        if(ef.op==EffectOp::ObservedDamage||ef.op==EffectOp::ObservedHeal) {
            const auto counter=o.value(QStringLiteral("counter")).toString();
            if(!c.counters.contains(counter)){p.unknowns.append(QStringLiteral("unobserved_event:%1").arg(counter));continue;}
            const double count=c.counters.value(counter);
            if(!std::isfinite(count)||count<0){p.unknowns.append(QStringLiteral("invalid_event:%1").arg(counter));continue;}
            ef.scale*=count;
        }
        if(ef.op==EffectOp::Stack) {
            const auto counter=o.value(QStringLiteral("counter")).toString();
            if(!c.counters.contains(counter)){p.unknowns.append(QStringLiteral("unobserved_stack:%1").arg(counter));continue;}
            ef.scale*=std::clamp(c.counters.value(counter),0.,ef.cap);ef.op=EffectOp::Add;
            if(ef.type==QStringLiteral("ap_amp")){ef.op=EffectOp::Amp;ef.stat=Stat::Ap;ef.hasStat=true;}
            else if(ef.type==QStringLiteral("hp_amp")){ef.op=EffectOp::Amp;ef.stat=Stat::Hp;ef.hasStat=true;}
            else if(ef.type==QStringLiteral("damage_amp")||ef.type==QStringLiteral("spell_amp"))ef.op=EffectOp::Amp;
        }
        if(!ef.formula.valid){p.unknowns.append(QStringLiteral("effect_value:%1:%2").arg(entry.id).arg(val.toString()));continue;}
        if(ef.op!=EffectOp::Mechanic&&ef.op!=EffectOp::Unknown)p.effects.append(ef);
    }
    return p;
}
bool skillCooldownHasteApplies(const QJsonObject &hero,const QJsonObject &component) {
    const auto source=component.value(QStringLiteral("source_pointer")).toString();
    if(source.isEmpty())return true;
    const auto rules=hero.value(QStringLiteral("action_rules_v3")).toObject();
    QList<QJsonObject> slotSets{rules.value(QStringLiteral("slots")).toObject()};
    const auto forms=rules.value(QStringLiteral("forms")).toObject();
    for(auto it=forms.constBegin();it!=forms.constEnd();++it)slotSets.append(it.value().toObject().value(QStringLiteral("slots")).toObject());
    // Bind by spell source, since a shared slot can represent distinct forms.
    for(const auto &slotRows:slotSets)for(auto it=slotRows.constBegin();it!=slotRows.constEnd();++it){
        const auto spec=it.value().toObject();
        if(spec.value(QStringLiteral("source_pointer")).toString()==source)return spec.value(QStringLiteral("cooldown_haste_applies")).toBool(spec.value(QStringLiteral("haste_applies")).toBool(true));
    }
    return true;
}
struct Skill {CombatFormula f;QString type,slot,rate;double cooldown=0;bool canCrit=false;double scale=1,scenarioAttackCount=0;bool cooldownHasteApplies=true;};
struct InnateBonus {Stat stat;CombatFormula formula;};
struct Solver {
    const CombatDatabase &db;CombatContext c;QVector<PreparedEntry> items,augments;QHash<int,int> byId;
    QVector<Skill> skills;QVector<InnateBonus> innate;QStringList unknowns,skillUnknowns,innateUnknowns;QString style;
    bool frostShot=false;CombatFormula frostShotDamage;
    Solver(const CombatDatabase &data,const CombatContext &ctx,const QList<int> &selected,const QString &preference,const QList<int> &extra={},const Solver *reuse=nullptr,bool restrictToExtra=false):db(data),c(ctx),style(growthCombatPreference()) {
        Q_UNUSED(preference);
        // Low-level callers without a bound comparison get a role-neutral
        // naked-hero reference. Public comparisons bind their actual current
        // inventory and chosen augments once before evaluating candidates.
        if(c.growthObjectiveBasis.isEmpty())bindGrowthObjective(db,c,{},{});
        if(reuse){items=reuse->items;byId=reuse->byId;skills=reuse->skills;skillUnknowns=reuse->skillUnknowns;}
        else if(restrictToExtra){
            // Sandbox evaluations bind only the supplied inventory. Retain
            // the catalog's ascending order and prepare each entry once.
            auto ids=extra;
            std::sort(ids.begin(),ids.end());
            ids.erase(std::unique(ids.begin(),ids.end()),ids.end());
            for(int id:ids){
                const auto it=db.items.constFind(id);
                if(it==db.items.cend())continue;
                byId[id]=items.size();
                items.append(prepare(it.value(),c,false));
            }
        }else for(auto it=db.items.cbegin();it!=db.items.cend();++it)if(it->eligible||extra.contains(it.key())){byId[it.key()]=items.size();items.append(prepare(it.value(),c,false));}
        QSet<int> seen;for(int id:selected)if(!seen.contains(id)){seen.insert(id);const auto it=db.augments.constFind(id);if(it!=db.augments.cend())augments.append(prepare(it.value(),c,true));else unknowns.append(QStringLiteral("unknown_augment:%1").arg(id));}
        if(seen.contains(1205)&&seen.contains(1206))unknowns.append(QStringLiteral("conflicting_conversions"));
        const auto h=db.champions.value(c.championId);
        // Frost Shot replaces ordinary critical bonus damage. Its numeric
        // multiplier comes from the pinned passive resource, not a web value.
        // A full-window qualified-target condition is an explicit scenario,
        // never inferred from merely owning crit or having learned Q.
        frostShot=c.championId==22;
        if(frostShot){
            for(const auto &raw:h.value(QStringLiteral("spells")).toArray()){
                const auto spell=raw.toObject();
                if(spell.value(QStringLiteral("slot"))!=QStringLiteral("P"))continue;
                const auto formula=spell.value(QStringLiteral("calculations")).toObject().value(QStringLiteral("DamageBonus")).toObject();
                if(!formula.isEmpty()){frostShotDamage=compileCombatFormula(formula,c,1);break;}
            }
            if(!frostShotDamage.valid)innateUnknowns.append(QStringLiteral("skill_formula_or_state:22:FrostShot_damage_bonus"));
            if(!c.conditions.contains(QStringLiteral("ashe_frost_shot_target_frosted_entire_window")))
                innateUnknowns.append(QStringLiteral("skill_condition:ashe_frost_shot_target_frosted_entire_window"));
            if(!c.explicitActionCounts)
                innateUnknowns.append(QStringLiteral("skill_condition:ashe_frost_shot_target_bound_attack_scenario"));
            if(c.conditions.contains(QStringLiteral("ashe_q_active_entire_window")))
                innateUnknowns.append(QStringLiteral("skill_formula_or_state:22:Q_bonus_attack_speed_and_window_uptime"));
        }
        for(const auto &v:h.value(QStringLiteral("passive_stat_effects")).toArray()){
            const auto o=v.toObject();if(c.level<o.value(QStringLiteral("min_level")).toInt(1)||c.level>o.value(QStringLiteral("max_level")).toInt(18))continue;
            const auto slot=o.value(QStringLiteral("slot")).toString();const int rank=slot==QStringLiteral("P")?1:c.ranks.value(slot,0);
            if(rank==0&&c.ranks.contains(slot))continue;
            const auto counter=o.value(QStringLiteral("counter")).toString();
            if(!counter.isEmpty()){
                const auto bounds=o.value(QStringLiteral("counter_bounds")).toArray();const auto found=c.counters.constFind(counter);
                if(found==c.counters.cend()||!std::isfinite(*found)||(bounds.size()==2&&(*found<bounds[0].toDouble()||*found>bounds[1].toDouble()))||(o.value(QStringLiteral("counter_integer")).toBool()&&std::floor(*found)!=*found)){
                    innateUnknowns.append(QStringLiteral("unobserved_passive_counter:%1").arg(counter));continue;
                }
            }
            const auto condition=o.value(QStringLiteral("condition")).toString();if(!condition.isEmpty()&&!c.conditions.contains(condition)){innateUnknowns.append(QStringLiteral("passive_condition:%1").arg(condition));continue;}
            if(o.contains(QStringLiteral("max_rank"))&&rank>o.value(QStringLiteral("max_rank")).toInt()){innateUnknowns.append(QStringLiteral("invalid_passive_rank:")+slot);continue;}
            const auto stat=combatStat(o.value(QStringLiteral("stat")).toString());auto f=compileCombatFormula(o.value(QStringLiteral("expression")).toObject(),c,rank);
            if(stat&&rank>0&&f.valid)innate.append({*stat,f});else innateUnknowns.append(QStringLiteral("unobserved_passive_stats:%1:%2").arg(c.championId).arg(o.value(QStringLiteral("calculation")).toString()));
        }
        unknowns.append(innateUnknowns);
        if(!reuse)for(const auto &value:h.value(QStringLiteral("skill_mechanism_gaps_v10")).toArray())
            if(value.isString()&&!value.toString().isEmpty())skillUnknowns.append(value.toString());
        // Removed dimensionally wrong or unqualified components are an explicit
        // coverage gap, never evidence that the skill has zero combat value.
        if(!reuse)for(const auto &v:h.value(QStringLiteral("damage_safety_review_v2")).toArray()) {
            const auto review=v.toObject();
            if(review.value(QStringLiteral("action")).toString()!=QStringLiteral("remove"))continue;
            const auto slot=review.value(QStringLiteral("slot")).toString();
            // A known unlearned ability cannot contribute in this comparison.
            // A missing rank still needs the quarantine, including passive rows.
            if(c.ranks.contains(slot)&&c.ranks.value(slot)==0)continue;
            skillUnknowns.append(QStringLiteral("skill_semantic_quarantine:%1:%2:%3")
                .arg(review.value(QStringLiteral("slot")).toString(),review.value(QStringLiteral("calculation")).toString(),review.value(QStringLiteral("reason_code")).toString()));
        }
        int knownUnlearnedComponents=0;
        if(!reuse)for(const auto &v:h.value(QStringLiteral("components")).toArray()) {
            const auto o=v.toObject();if(c.level<o.value(QStringLiteral("min_level")).toInt(1))continue;
            const auto componentKey=combatComponentIdentity(o);
            if(c.explicitActionCounts&&(c.restrictExplicitComponents||!c.explicitComponentKeys.isEmpty())&&!c.explicitComponentKeys.contains(componentKey))continue;
            const double componentScale=c.explicitActionCounts?c.explicitComponentScales.value(componentKey,1):1;
            if(componentScale==0)continue;
            const auto slot=o.value(QStringLiteral("slot")).toString(),rankSlot=o.value(QStringLiteral("rank_slot")).toString(slot);
            const int rank=rankSlot==QStringLiteral("P")||rankSlot==QStringLiteral("EFFECT")?1:c.ranks.value(rankSlot,0);
            if(rank==0&&!o.value(QStringLiteral("allow_rank_zero")).toBool()){
                if(c.ranks.contains(rankSlot))++knownUnlearnedComponents;
                continue;
            }
            if(rank<0||rank>o.value(QStringLiteral("max_rank")).toInt()){skillUnknowns.append(QStringLiteral("invalid_skill_rank:%1").arg(slot));continue;}
            const auto condition=o.value(QStringLiteral("condition")).toString();
            if(!condition.isEmpty()&&c.level<o.value(QStringLiteral("condition_below_level")).toInt(99)&&!c.conditions.contains(condition)){skillUnknowns.append(QStringLiteral("skill_condition:%1").arg(condition));continue;}
            bool requiredConditionsKnown=true;
            for(const auto &value:o.value(QStringLiteral("requires_conditions")).toArray()){
                if(!value.isString()||value.toString().isEmpty()||!c.conditions.contains(value.toString())){
                    requiredConditionsKnown=false;skillUnknowns.append(QStringLiteral("skill_condition:%1").arg(value.toString()));
                }
            }
            if(!requiredConditionsKnown)continue;
            const auto rate=o.value(QStringLiteral("rate")).toString();auto expression=o.value(QStringLiteral("expression")).toObject();
            double scenarioAttackCount=0;
            if(rate==QStringLiteral("explicit_scenario_attack_count")){
                const auto counter=o.value(QStringLiteral("counter")).toString();
                const auto group=o.value(QStringLiteral("counter_group")).toArray();
                const double limit=o.value(QStringLiteral("counter_limit")).toDouble(-1);
                bool known=!counter.isEmpty()&&!group.isEmpty()&&std::isfinite(limit)&&limit>=0;
                double total=0;int activeBranches=0;QSet<QString> distinct;
                for(const auto &value:group){
                    const auto key=value.toString();
                    if(!value.isString()||key.isEmpty()||distinct.contains(key)){known=false;continue;}
                    distinct.insert(key);const auto found=c.counters.constFind(key);
                    if(found==c.counters.cend()||!std::isfinite(*found)||*found<0||*found>limit||std::floor(*found)!=*found){known=false;continue;}
                    total+=*found;activeBranches+=*found>0;
                }
                known=known&&distinct.contains(counter)&&total<=limit
                    &&(!o.value(QStringLiteral("counter_mutually_exclusive")).toBool()||activeBranches<=1);
                if(c.explicitActionCounts)known=known&&std::isfinite(c.explicitAttackCount)&&total<=c.explicitAttackCount;
                if(!known){skillUnknowns.append(QStringLiteral("skill_explicit_scenario_attack_count_unknown_or_invalid:%1").arg(slot));continue;}
                scenarioAttackCount=c.counters.value(counter);
            }
            if(rate==QStringLiteral("target_missing_hp_fraction")){
                const QJsonObject missingHealth{{QStringLiteral("op"),QStringLiteral("health")},{QStringLiteral("subject"),QStringLiteral("target")},{QStringLiteral("measure"),QStringLiteral("missing")}};
                expression=QJsonObject{{QStringLiteral("op"),QStringLiteral("mul")},{QStringLiteral("args"),QJsonArray{expression,missingHealth}}};
            }
            auto f=compileCombatFormula(expression,c,rank);const auto cd=o.value(QStringLiteral("cooldown")).toArray();
            if(f.valid)skills.append(Skill{f,o.value(QStringLiteral("damage_type")).toString(),slot,rate,rank<cd.size()?cd[rank].toDouble():0,o.value(QStringLiteral("can_crit")).toBool(),componentScale,scenarioAttackCount,skillCooldownHasteApplies(h,o)});
            else skillUnknowns.append(QStringLiteral("skill_formula_or_state:%1:%2").arg(c.championId).arg(o.value(QStringLiteral("calculation")).toString()));
        }
        if(!reuse&&skills.size()+knownUnlearnedComponents<3)skillUnknowns.append(QStringLiteral("incomplete_skill_cast_model"));
        unknowns.append(skillUnknowns);
        if(c.conditions.contains(QStringLiteral("jayce_hammer"))&&c.conditions.contains(QStringLiteral("jayce_cannon")))unknowns.append(QStringLiteral("conflicting_champion_forms"));
        if(c.conditions.contains(QStringLiteral("jayce_hammer_form"))&&c.conditions.contains(QStringLiteral("jayce_cannon_form")))unknowns.append(QStringLiteral("conflicting_champion_forms"));
        if(c.conditions.contains(QStringLiteral("nidalee_human_form"))&&c.conditions.contains(QStringLiteral("nidalee_cougar_form")))unknowns.append(QStringLiteral("conflicting_champion_forms"));
        if(c.conditions.contains(QStringLiteral("gnar_mini_form"))&&c.conditions.contains(QStringLiteral("gnar_mega_form")))unknowns.append(QStringLiteral("conflicting_champion_forms"));
        int weapons=0;for(const auto &key:{QStringLiteral("aphelios_calibrum"),QStringLiteral("aphelios_gravitum"),QStringLiteral("aphelios_infernum")})weapons+=c.conditions.contains(key);
        if(weapons>1)unknowns.append(QStringLiteral("conflicting_champion_forms"));
        unknowns.append(QStringLiteral("champion_passives_mode_balance_and_hit_conditions_not_fully_simulated"));
        if(c.discreteSkillCasts)unknowns.append(QStringLiteral("initial_skill_readiness_is_explicit_scenario_not_live_observation"));
    }
    CombatEvaluation eval(const QList<int> &ids,bool details=false) const {
        CombatEvaluation r;r.stats=c.base;auto &s=r.stats;
        for(const auto &bonus:innate){const auto amount=bonus.formula.evaluate(c,c.base);if(amount&&std::isfinite(*amount)&&*amount>=0)s[bonus.stat]+=*amount;else if(details)r.unknowns.append(QStringLiteral("unobserved_passive_stat_value"));}
        if(unknowns.contains(QStringLiteral("conflicting_conversions"))||unknowns.contains(QStringLiteral("conflicting_champion_forms"))) {r.unknowns=unknowns;return r;}
        double ownedItemMana=0;
        QVector<const Effect *> effects;effects.reserve(32);
        if(details)r.unknowns.append(unknowns);
        for(int id:ids) {
            if(!byId.contains(id))return r;
            const auto &p=items[byId[id]];
            const double magicPen=s[Stat::MagicPenPercent],armorPen=s[Stat::ArmorPenPercent];
            s+=p.entry->stats;ownedItemMana+=p.entry->stats[Stat::Mana];
            // Each independent percentage penetration source applies to the
            // remaining resistance, including a learned champion passive.
            s[Stat::MagicPenPercent]=1-(1-magicPen)*(1-p.entry->stats[Stat::MagicPenPercent]);
            s[Stat::ArmorPenPercent]=1-(1-armorPen)*(1-p.entry->stats[Stat::ArmorPenPercent]);
            for(const auto &ef:p.effects)effects.append(&ef);
            if(details)r.unknowns.append(p.unknowns);
        }
        for(const auto &p:augments){for(const auto &ef:p.effects)effects.append(&ef);if(details)r.unknowns.append(p.unknowns);}
        if(!c.usesMana){s[Stat::Mana]=0;s[Stat::ManaRegen]=0;}
        auto value=[&](const Effect &ef)->double {auto v=ef.formula.evaluate(c,s);return v?*v*ef.scale:std::numeric_limits<double>::quiet_NaN();};
        auto add=[&](const Effect &ef,double v){
            if(ef.hasStat)s[ef.stat]+=v;
            else if(ef.type==QStringLiteral("resists")){s[Stat::Armor]+=v;s[Stat::Mr]+=v;}
            else if(ef.type==QStringLiteral("adaptive")){if(s[Stat::Ap]>s[Stat::Ad]-c.base[Stat::Ad])s[Stat::Ap]+=v;else s[Stat::Ad]+=.6*v;}
            else if(details)r.unknowns.append(QStringLiteral("unsupported_stat:%1").arg(ef.type));
        };
        auto eventStats=c.eventStats;
        // A candidate that drops a source item loses the entire paired effect
        // and its attached growth. Do not retain a credit after removing cost.
        for(const auto &v:c.itemAttachedEffects){const auto e=v.toObject();bool owned=true;
            if(e.contains(QStringLiteral("item_id")))owned=ids.contains(e.value(QStringLiteral("item_id")).toInt());
            for(const auto &id:e.value(QStringLiteral("source_item_ids")).toArray())owned&=ids.contains(id.toInt());
            if(!owned)if(auto stat=combatStat(e.value(QStringLiteral("key")).toString()))eventStats[*stat]-=e.value(QStringLiteral("value")).toDouble();
        }
        s+=eventStats;
        s+=c.observedInputOffset;
        // Flat effects before transforms, conversions and multiplicative stats.
        for(const auto *p:effects)if(p->op==EffectOp::Add) {double v=value(*p);if(!std::isfinite(v))return r;add(*p,v);if(p->ownerKey.startsWith(QStringLiteral("item:"))&&p->hasStat&&p->stat==Stat::Mana)ownedItemMana+=v;}
        auto formTransforms=[&](const QString &stage){for(const auto &entry:c.formStatTransforms){
            const auto row=entry.toObject();if(row.value(QStringLiteral("stage")).toString()!=stage)continue;
            const auto stat=*combatStat(row.value(QStringLiteral("stat")).toString());const double multiplier=row.value(QStringLiteral("multiplier")).toDouble();
            if(row.value(QStringLiteral("basis")).toString()==QStringLiteral("total"))s[stat]*=multiplier;
            else s[stat]+=std::max(0.,s[stat]-c.base[stat])*(multiplier-1);
        }};
        formTransforms(QStringLiteral("before_conversions"));
        for(const auto *p:effects)if(p->op==EffectOp::Transform&&p->hasSource&&p->hasStat) {
            double source=s[p->source];if(p->counter==QStringLiteral("bonus_ad"))source-=c.base[Stat::Ad];
            const double v=value(*p);if(!std::isfinite(v))return r;s[p->stat]+=std::max(0.,source)*v;
            if(p->consume)s[p->source]-=std::max(0.,source);
        }
        QList<int> selectedAugments;for(const auto &p:augments)selectedAugments.append(p.entry->id);
        if(!applyHextech40Conversions(db,c,ids,selectedAugments,ownedItemMana,r))return r;
        auto convert=[&](Stat dest){for(const auto *p:effects)if(p->op==EffectOp::Convert&&p->hasSource&&p->hasStat&&p->stat==dest){const double v=value(*p);if(std::isfinite(v))s[p->stat]+=(p->counter==QStringLiteral("bonus_hp")||p->counter==QStringLiteral("bonus_ad")||p->counter==QStringLiteral("bonus_mana")?std::max(0.,s[p->source]-c.base[p->source]):s[p->source])*v;else if(details)r.unknowns.append(QStringLiteral("conversion_value"));}};
        auto amp=[&](Stat dest){double sum=0;for(const auto *p:effects)if(p->op==EffectOp::Amp&&p->hasStat&&p->stat==dest){const double v=value(*p);if(std::isfinite(v))sum+=v;}s[dest]*=1+sum;};
        convert(Stat::Hp);amp(Stat::Hp);convert(Stat::Ap);amp(Stat::Ap);convert(Stat::Ad);amp(Stat::Ad);
        for(const auto *p:effects)if(p->op==EffectOp::BonusAmp&&p->hasStat)s[p->stat]+=std::max(0.,s[p->stat]-c.base[p->stat])*value(*p);
        convert(Stat::Crit);s[Stat::Crit]=std::max(0.,s[Stat::Crit]);
        convert(Stat::Haste);convert(Stat::HealShield);convert(Stat::Ms);convert(Stat::BonusAs);
        for(const auto *p:effects){const auto &ef=*p;const double v=value(ef);if(!std::isfinite(v))continue;
            if(ef.op==EffectOp::Penetration||ef.op==EffectOp::Resist)s[ef.stat]=1-(1-s[ef.stat])*(1-v);
            else if(ef.op==EffectOp::Range)s[Stat::Range]+=v;
        }
        s[Stat::As]=(c.baseAs+c.observedInputOffset[Stat::As]+c.asRatio*s[Stat::BonusAs])*(1+s[Stat::AsMultiplier]);amp(Stat::As);s[Stat::As]=std::min(c.attackSpeedCap,s[Stat::As]);
        s[Stat::Ms]=(s[Stat::Ms])*(1+s[Stat::MsPercent]);
        const bool crown=selectedAugments.contains(3132)&&hasHextech40Conversion(db.augments[3132])&&hextech40CrownReady(c);
        const double crownFactor=crown?hextech40CrownMovement(db.augments[3132],c):1;
        if(crown&&c.conditions.contains(QStringLiteral("verified_hextech40_3132_ms_before_softcap")))s[Stat::Ms]*=crownFactor;
        // Standard movement soft caps; special champion movement is unknown.
        if(s[Stat::Ms]>490)s[Stat::Ms]=s[Stat::Ms]*.5+230;else if(s[Stat::Ms]>415)s[Stat::Ms]=s[Stat::Ms]*.8+83;
        if(crown&&c.conditions.contains(QStringLiteral("verified_hextech40_3132_ms_after_softcap")))s[Stat::Ms]*=crownFactor;
        s+=c.observedOffset;
        // Derived sustain reads completed offensive/defensive stats, including
        // amplifiers and observed calibration, rather than selected-card order.
        for(const auto *p:effects)if(p->op==EffectOp::Derived) {
            const double v=value(*p);if(std::isfinite(v))add(*p,std::clamp(v,0.,p->cap));
            else if(details)r.unknowns.append(QStringLiteral("derived_value:%1").arg(p->type));
        }
        // Item base-mana-regeneration bonuses scale the champion's base
        // per-second regeneration, not flat item regeneration or each other.
        if(c.usesMana)s[Stat::ManaRegen]+=std::max(0.,c.base[Stat::ManaRegen])*std::max(0.,s[Stat::ManaRegenPercent]);
        else {s[Stat::Mana]=0;s[Stat::ManaRegen]=0;}
        // Excess crit is converted once, after all flat and derived crit. The
        // granted adaptive stat does not recursively re-run conversions.
        const double excessCritPoints=100*std::max(0.,s[Stat::Crit]-1.);
        // Their local passive templates define an exclusive fixed-AD overflow
        // conversion. The complete champion critical contract is not admitted
        // here, so the generic adaptive fallback must not pay it a second way.
        const bool ownCriticalConversion=c.championId==157||c.championId==777;
        const bool adaptiveAp=s[Stat::Ap]>s[Stat::Ad]-c.base[Stat::Ad];double overflowAmplifier=1;
        for(const auto *p:effects)if(p->op==EffectOp::Amp&&p->hasStat&&p->stat==(adaptiveAp?Stat::Ap:Stat::Ad))overflowAmplifier+=value(*p);
        if(ownCriticalConversion&&excessCritPoints>0)
            r.unknowns.append(QStringLiteral("champion_crit_conversion:%1:exclusive_passive_contract_not_fully_simulated").arg(c.championId));
        else if(adaptiveAp)s[Stat::Ap]+=excessCritPoints*c.overflowCritAp*overflowAmplifier;
        else s[Stat::Ad]+=excessCritPoints*c.overflowCritAd*overflowAmplifier;
        s[Stat::Crit]=std::clamp(s[Stat::Crit],0.,1.);
        formTransforms(QStringLiteral("after_derived_and_overflow"));
        double spellAmp=1,damageAmp=1,extraTrue=0;bool sealR=false,ultimateHasteOnly=false;double ultimateHasteMultiplier=1,spellCrit=1,defense=1,onhitRepeats=1;QMap<QString,double> typeAmps;
        double potentialDrainHealing=0, admittedDrainHealing=0;
        QJsonArray drainReports;
        for(const auto *p:effects){const auto &ef=*p;double v=value(ef);if(!std::isfinite(v))continue;
            if(ef.op==EffectOp::MagicDamageAmp)s[Stat::MagicAmp]+=v;
            if(ef.op==EffectOp::DamageTypeAmp){
                // Cinderbloom's crit modifiers multiply only its bonus portion,
                // rather than multiplying the entire underlying damage again.
                const double modifier=ef.critBonusScaled?1+s[Stat::CritDamage]-c.base[Stat::CritDamage]:1;
                if(modifier<0){if(details)r.unknowns.append(QStringLiteral("invalid_crit_damage_modifier"));continue;}
                typeAmps[ef.type]=typeAmps.value(ef.type,1)*(1+v*modifier);
            }
            if(ef.op==EffectOp::Flag&&ef.type==QStringLiteral("seal_r"))sealR=true;
            if(ef.op==EffectOp::Flag&&ef.type==QStringLiteral("ultimate_haste_only")){ultimateHasteOnly=true;ultimateHasteMultiplier=1+v;}
            if(ef.op==EffectOp::Amp&&ef.stat==Stat::SpellAmp)spellAmp*=1+v;
            if(ef.op==EffectOp::Amp&&ef.stat==Stat::DamageAmp)damageAmp*=1+v;
            if(ef.op==EffectOp::ExtraTrue)extraTrue+=v;
            if(ef.op==EffectOp::SpeedDamage)damageAmp*=1+std::max(0.,s[Stat::Ms]-c.targetMs)*v;
            if(ef.op==EffectOp::SpellCrit)spellCrit=1+s[Stat::Crit]*std::max(0.,v-1);
            if(ef.op==EffectOp::CritDefense)defense*=1-std::min(s[Stat::Crit],ef.cap)*v;
            if(ef.op==EffectOp::Flag&&ef.type==QStringLiteral("secondary_onhit"))onhitRepeats+=v;
            if(ef.op==EffectOp::Flag&&ef.type==QStringLiteral("crit_onhit"))onhitRepeats+=s[Stat::Crit]*v;
        }
        if(details)for(auto it=typeAmps.cbegin();it!=typeAmps.cend();++it)r.damageTypeMultipliers.insert(it.key(),it.value());
        r.globalDamageMultiplier=damageAmp;r.copiedTrueDamageRatio=extraTrue;
        const double attacks=c.explicitActionCounts?c.explicitAttackCount:c.horizon*std::max(0.,s[Stat::As]);
        const bool attackReplacement=std::any_of(skills.cbegin(),skills.cend(),[](const Skill &sk){return sk.rate==QStringLiteral("attack_replacement");});
        double attackModifier=1+s[Stat::Crit]*std::max(0.,s[Stat::CritDamage]-1);
        if(frostShot){
            attackModifier=1; // Unknown qualified hits remain diagnostic.
            if(c.conditions.contains(QStringLiteral("ashe_frost_shot_target_frosted_entire_window"))&&frostShotDamage.valid){
                const auto value=frostShotDamage.evaluate(c,s);
                if(value&&std::isfinite(*value)&&*value>=1)attackModifier=*value;
                else if(details)r.unknowns.append(QStringLiteral("skill_formula_or_state:22:FrostShot_damage_bonus"));
            }
        }
        r.attackDamage=attackReplacement?0:attacks*s[Stat::Ad]*attackModifier*damageMultiplier(QStringLiteral("physical"),s,c,typeAmps);
        double casts=0;int magicCasts=0;
        if(c.explicitActionCounts)for(auto it=c.explicitCastCounts.cbegin();it!=c.explicitCastCounts.cend();++it)casts+=it.value();
        for(const auto &sk:skills){if(sealR&&sk.slot==QStringLiteral("R"))continue;auto v=sk.f.evaluate(c,s);if(!v||*v<0||*v>1e7)continue;*v*=sk.scale;
            if(sk.rate==QStringLiteral("explicit_scenario_attack_count")){
                // Already-qualified attacks from one source-bound skill window.
                // Neither attack-speed/window nor cooldown generates extra hits;
                // generic on-hit repeats and spell crit are not silently applied.
                const double damage=*v*sk.scenarioAttackCount*damageMultiplier(sk.type,s,c,typeAmps);
                r.procDamage+=damage;r.onhitDamage+=damage;continue;
            }
            if(sk.rate==QStringLiteral("proc_phase")) {
                const double count=c.explicitActionCounts?c.explicitCastCounts.value(sk.slot,0):0;
                r.procDamage+=*v*count*damageMultiplier(sk.type,s,c,typeAmps);continue;
            }
            if(sk.rate==QStringLiteral("onhit")||sk.rate==QStringLiteral("attack_wave")){
                const bool onhit=sk.rate==QStringLiteral("onhit");const double n=attacks*(onhit?onhitRepeats:1);
                const double crit=sk.canCrit?1+s[Stat::Crit]*std::max(0.,s[Stat::CritDamage]-1):1;
                const double damage=*v*n*damageMultiplier(sk.type,s,c,typeAmps)*crit*(onhit?1:spellAmp);
                r.procDamage+=damage;r.onhitDamage+=damage;continue;
            }
            const double interval=effectiveCombatSkillCooldown(sk.cooldown,sk.slot,sk.cooldownHasteApplies,s,c,ultimateHasteOnly,ultimateHasteMultiplier);
            // One resource component per cast; channel/charge/recast timing isn't
            // implied. Continuous cast rate is a comparison objective only.
            double n=sk.rate==QStringLiteral("per_second")?c.horizon:sk.rate==QStringLiteral("attack_replacement")?attacks:interval>0?1+c.horizon/interval:1;
            if(c.explicitActionCounts) {
                n=sk.rate==QStringLiteral("per_second")?c.explicitActiveSeconds.value(sk.slot,0):sk.rate==QStringLiteral("attack_replacement")?attacks:c.explicitCastCounts.value(sk.slot,0);
            } else if(c.discreteSkillCasts && sk.rate!=QStringLiteral("per_second") && sk.rate!=QStringLiteral("attack_replacement")){
                const double ready=c.skillReadyIn.value(sk.slot,0),remaining=c.horizon-ready;
                n=!std::isfinite(ready)||ready<0||remaining<=0?0:interval>0?std::ceil(std::max(0.,remaining/interval-1e-10)):1;
            }
            if(sk.rate==QStringLiteral("target_missing_hp_fraction")&&!c.explicitActionCounts)n=std::min(n,attacks);
            if(sk.rate==QStringLiteral("target_hp_fraction"))*v*=c.targetHp;
            const bool frostReplacement=frostShot&&sk.rate==QStringLiteral("attack_replacement");
            const double damage=*v*n*damageMultiplier(sk.type,s,c,typeAmps)*(frostReplacement?attackModifier:spellAmp*spellCrit);
            r.skillDamage+=damage;if(sk.rate==QStringLiteral("attack_replacement"))r.replacementAttackDamage+=damage;
            if(!c.explicitActionCounts)casts+=n;magicCasts+=sk.type==QStringLiteral("magic");
        }
        for(const auto *p:effects){const auto &ef=*p;double v=value(ef);if(!std::isfinite(v))continue;
            if(ef.targetHp)v*=c.targetHp;
            if(ef.targetCurrentHp)v*=ef.targetHealth.evaluate(c,s).value_or(0);
            if(ef.op==EffectOp::ObservedDamage)r.procDamage+=v*(c.explicitActionCounts?c.explicitProcCounts.value(ef.ownerKey,0):1)*damageMultiplier(ef.type,s,c,typeAmps);
            if(ef.op==EffectOp::ObservedHeal)r.healing+=v*(c.explicitActionCounts?c.explicitProcCounts.value(ef.ownerKey,0):1)*(1+s[Stat::HealShield]);
            if(ef.op==EffectOp::Shield)s[Stat::Shield]+=ef.percentHp?v*s[Stat::Hp]:v;
            if(ef.op==EffectOp::Onhit||ef.op==EffectOp::CurrentHpOnhit||ef.op==EffectOp::NthOnhit||ef.op==EffectOp::BurnOnhit){
                double hits=attacks*onhitRepeats;
                if(ef.op==EffectOp::NthOnhit)hits=std::floor(attacks/std::max(1,ef.every));
                if(c.explicitActionCounts&&ef.op==EffectOp::NthOnhit){hits=c.explicitProcCounts.value(ef.ownerKey,0);if(details&&!c.explicitProcCounts.contains(ef.ownerKey))r.unknowns.append(QStringLiteral("explicit_nth_hit_state_missing:")+ef.ownerKey);}
                if(ef.op==EffectOp::CurrentHpOnhit)v*=ef.targetHealth.evaluate(c,s).value_or(0);
                if(ef.op==EffectOp::BurnOnhit){
                    v*=c.targetHp;
                    if(ef.duration>0)hits=s[Stat::As]*(c.horizon>=ef.duration?c.horizon-ef.duration*.5:c.horizon*c.horizon/(2*ef.duration));
                    if(c.explicitActionCounts){hits=c.explicitProcCounts.value(ef.ownerKey,0);if(details&&!c.explicitProcCounts.contains(ef.ownerKey))r.unknowns.append(QStringLiteral("explicit_burn_tick_state_missing:")+ef.ownerKey);}
                }
                if(ef.hasSource&&ef.op==EffectOp::Onhit)v*=s[ef.source];
                const double critMultiplier=ef.canCrit?1+s[Stat::Crit]*std::max(0.,s[Stat::CritDamage]-1):1;
                const double damage=(v+ef.apRatio*s[Stat::Ap])*hits*critMultiplier*damageMultiplier(ef.type,s,c,typeAmps);
                r.procDamage+=damage;r.onhitDamage+=damage;
            } else if(ef.op==EffectOp::Proc||ef.op==EffectOp::Spellblade||ef.op==EffectOp::SpellPercentHp) {
                double triggers=ef.trigger==QStringLiteral("crit")?attacks*s[Stat::Crit]:ef.trigger==QStringLiteral("periodic")?c.horizon:ef.trigger==QStringLiteral("hit")?attacks+casts:casts;
                if(c.explicitActionCounts){triggers=c.explicitProcCounts.value(ef.ownerKey,0);if(details&&!c.explicitProcCounts.contains(ef.ownerKey))r.unknowns.append(QStringLiteral("explicit_item_or_augment_proc_state_missing:")+ef.ownerKey);}
                // Nearby champion combat is its own admitted periodic event.
                // A spell count or elapsed window never proves that it occurred.
                else if(ef.trigger==QStringLiteral("melee_combat")){triggers=0;if(details)r.unknowns.append(QStringLiteral("explicit_melee_combat_proc_count_required:")+ef.ownerKey);}
                else if(ef.cooldown>0)triggers=std::min(triggers,1+std::floor((c.horizon-1e-6)/(ef.cooldown/(1+s[Stat::ItemHaste]/100.))));
                if(ef.op==EffectOp::Spellblade&&ef.hasSource)v*=c.base[ef.source];
                if(ef.op==EffectOp::SpellPercentHp)v*=c.targetHp*ef.count;
                if(ef.hasSource&&ef.op==EffectOp::Proc)v*=ef.counter==QStringLiteral("bonus_hp")?s[Stat::Hp]-c.base[Stat::Hp]:s[ef.source];
                const double procDamage=v*std::max(0.,triggers)*damageMultiplier(ef.type,s,c,typeAmps);
                r.procDamage+=procDamage;
                if(ef.heal>0){
                    // The item's tooltip says healing from DAMAGE DEALT. The
                    // tooltip's pre-resistance HealCalc is not a damage receipt.
                    const double generated=procDamage*damageAmp*ef.heal;
                    potentialDrainHealing+=generated;
                    const bool admitted=c.explicitActionCounts&&c.conditions.contains(QStringLiteral("comparison_drain_damage_is_effective"));
                    if(admitted)admittedDrainHealing+=generated;
                    if(details){
                        drainReports.append(QJsonObject{{QStringLiteral("source"),ef.ownerKey},{QStringLiteral("admitted_proc_count"),std::max(0.,triggers)},
                            {QStringLiteral("post_resistance_modeled_damage"),procDamage*damageAmp},{QStringLiteral("heal_from_damage_multiplier"),ef.heal},
                            {QStringLiteral("raw_self_heal"),generated},{QStringLiteral("damage_effectiveness_explicitly_assumed"),admitted},
                            {QStringLiteral("actual_damage_receipt"),false},{QStringLiteral("scope"),QStringLiteral("explicit_conditional_proc_model_not_observed_damage")}});
                        if(triggers>0&&!admitted)r.unknowns.append(QStringLiteral("drain_actual_damage_or_explicit_effectiveness_contract_required:")+ef.ownerKey);
                    }
                }
            }
        }
        const double raw=r.attackDamage+r.skillDamage+r.procDamage;
        if(details&&extraTrue>0&&typeAmps.contains(QStringLiteral("true")))r.unknowns.append(QStringLiteral("copied_true_damage_amplifier_order"));
        r.procDamage+=raw*extraTrue;
        r.attackDamage*=damageAmp;r.skillDamage*=damageAmp;r.procDamage*=damageAmp;
        r.replacementAttackDamage*=damageAmp;
        r.onhitDamage=r.onhitDamage*damageAmp*(1+extraTrue)+(r.attackDamage+r.replacementAttackDamage)*extraTrue;
        QList<int> selectedIds;for(const auto &entry:augments)if(entry.entry)selectedIds.append(entry.entry->id);
        const auto sustainModifiers=combatSelfSustainModifiers(db,c,ids,selectedIds,s);
        const auto selfShield=combatSelfShieldCapacity(db,c,s,sustainModifiers);
        const double outgoingPower=std::max(0.,1+s[Stat::HealShield]);
        // An observed shield is already a final received pool. Its source is
        // not identified by a scalar stat, so do not amplify it again or add a
        // second hypothetical W pool on top of an unidentified active shield.
        const double observedShield=std::max(0.,c.observedOffset[Stat::Shield]+c.observedInputOffset[Stat::Shield]);
        const double additionalSelfShield=observedShield>0?0:selfShield.amplified;
        const double generatedSelfHealing=potentialDrainHealing*outgoingPower*sustainModifiers.healingReceived;
        const auto missing=compileCombatFormula(QJsonObject{{QStringLiteral("op"),QStringLiteral("health")},{QStringLiteral("subject"),QStringLiteral("self")},{QStringLiteral("measure"),QStringLiteral("missing")}},c).evaluate(c,s);
        const auto settled=missing?settleCombatSelfHeal(admittedDrainHealing*outgoingPower,*missing,sustainModifiers):CombatHealSettlement{};
        const bool effectiveKnown=settled.valid&&(potentialDrainHealing<=0||std::abs(admittedDrainHealing-potentialDrainHealing)<1e-8);
        const double effectiveSelfHealing=settled.valid?settled.effective:0;
        if(details){
            r.unknowns.append(sustainModifiers.unknowns);r.unknowns.append(selfShield.unknowns);
            if(observedShield>0&&selfShield.amplified>0)r.unknowns.append(QStringLiteral("active_observed_shield_source_unidentified_conditional_w_pool_not_added_twice"));
            if(potentialDrainHealing>0&&!missing)r.unknowns.append(QStringLiteral("self_missing_health_required_for_effective_drain_healing"));
            r.selfSustain=QJsonObject{{QStringLiteral("received_modifiers"),sustainModifiers.sources},
                {QStringLiteral("healing_received_multiplier"),sustainModifiers.healingReceived},{QStringLiteral("shield_received_multiplier"),sustainModifiers.shieldingReceived},
                {QStringLiteral("conditional_raw_healing"),potentialDrainHealing},{QStringLiteral("conditional_amplified_healing"),generatedSelfHealing},
                {QStringLiteral("effective_healing"),effectiveKnown?QJsonValue(effectiveSelfHealing):QJsonValue(QJsonValue::Null)},
                {QStringLiteral("effective_healing_added_to_survival"),effectiveSelfHealing},
                {QStringLiteral("overheal"),effectiveKnown?QJsonValue(settled.overheal):QJsonValue(QJsonValue::Null)},
                {QStringLiteral("missing_health_known"),bool(missing)},{QStringLiteral("drain_procs"),drainReports},
                {QStringLiteral("raw_self_shield_capacity"),selfShield.raw},{QStringLiteral("conditional_self_shield_capacity"),selfShield.amplified},
                {QStringLiteral("observed_final_shield_pool"),observedShield},{QStringLiteral("additional_self_shield_capacity_added_to_survival"),additionalSelfShield},
                {QStringLiteral("shield_components"),selfShield.components},{QStringLiteral("actual_shield_absorption"),QJsonValue(QJsonValue::Null)},
                {QStringLiteral("repeated_shields_summed_as_absorption"),false},{QStringLiteral("scope"),QStringLiteral("single_available_shield_and_explicitly_admitted_capped_self_healing_not_live_sustain")}};
        }
        const double itemShield=observedShield+amplifyCombatSelfShield(std::max(0.,s[Stat::Shield]-observedShield)*outgoingPower,sustainModifiers).value_or(0);
        const double ehp=(s[Stat::Hp]+itemShield+additionalSelfShield+effectiveSelfHealing)/(.5*resistanceMultiplier(s[Stat::Armor])+.5*resistanceMultiplier(s[Stat::Mr]))/std::max(.01,defense);
        r.survival=ehp;
        // The selected rule set, including a candidate augment, supplies its
        // actual cooldown behavior while the comparison basis stays frozen.
        applyGrowthObjective(db,c,ids,selectedIds,r,details);
        const bool finiteStats=std::all_of(s.values.cbegin(),s.values.cend(),[](double v){return std::isfinite(v);});
        const double metrics[]{r.attackDamage,r.skillDamage,r.procDamage,r.onhitDamage,r.replacementAttackDamage,r.healing,r.survival,r.score};
        const bool finiteMetrics=std::all_of(std::begin(metrics),std::end(metrics),[](double v){return std::isfinite(v);});
        r.valid=finiteStats&&finiteMetrics&&r.score>0&&s[Stat::Hp]>0;
        if(details&&(!finiteStats||!finiteMetrics))r.unknowns.append(QStringLiteral("invalid_combat_numeric_result"));
        if(details)r.unknowns.removeDuplicates();return r;
    }
};
}

std::optional<Stat> combatStat(const QString &name){const int n=statNames.indexOf(name);if(n<0)return {};return static_cast<Stat>(n);}
bool combatSkillCooldownHasteApplies(const QJsonObject &hero,const QJsonObject &component){return skillCooldownHasteApplies(hero,component);}
double effectiveCombatSkillCooldown(double base,const QString &slot,bool hasteApplies,const CombatStats &stats,const CombatContext &ctx,bool ultimateHasteOnly,double ultimateHasteMultiplier){
    if(!std::isfinite(base)||base<=0)return 0;
    double haste=ultimateHasteOnly&&slot!=QStringLiteral("R")?0:stats[Stat::Haste];
    if(auto stat=combatStat(slot.toLower()+QStringLiteral("_haste")))haste+=stats[*stat];
    if(slot==QStringLiteral("R"))haste*=ultimateHasteMultiplier;
    haste=hasteApplies?std::clamp(haste,0.,ctx.abilityHasteCap):0;
    return base*(1-std::clamp(stats[Stat::CooldownReduction],0.,.99))*100/(100+haste);
}
CombatStats &CombatStats::operator+=(const CombatStats &s){for(size_t i=0;i<values.size();++i)values[i]+=s.values[i];return *this;}
QJsonObject CombatStats::json() const {QJsonObject o;for(int i=0;i<statNames.size();++i)o.insert(statNames[i],values[i]);return o;}
QString combatComponentIdentity(const QJsonObject &o) {
    return QStringList{o.value(QStringLiteral("slot")).toString(),o.value(QStringLiteral("source_pointer")).toString(),o.value(QStringLiteral("calculation")).toString(),o.value(QStringLiteral("damage_type")).toString(),o.value(QStringLiteral("phase_id")).toString()}.join(QStringLiteral("|"));
}
CombatFormula compileCombatFormula(const QJsonObject &o,const CombatContext &ctx,int rank) {CombatFormula f;f.valid=ctx.level>=1&&ctx.level<=18&&rank>=1&&rank<=6&&compileNode(o,ctx,rank,f.code)&&f.code.size()<192;return f;}
std::optional<double> CombatFormula::evaluate(const CombatContext &c,const CombatStats &s) const {
    if(!valid)return {};
    if(code.size()==1&&code.first().op==Constant)return code.first().a;
    std::array<double,192> stack;int top=0;
    for(const auto &n:code){double v=n.a;
        if(n.op==StatValue){if(n.stat==-1)v=100/(100+std::clamp(s[Stat::Haste],-99.,c.abilityHasteCap));
            else if(n.stat==-2)v=n.part==1?1:n.part==2?s[Stat::BonusAs]:1+s[Stat::BonusAs];
            else {auto k=static_cast<Stat>(n.stat);v=n.part==1?c.base[k]:n.part==2?s[k]-c.base[k]:s[k];}}
        else if(n.op==HealthValue){const double maximum=n.part?c.targetHp:s[Stat::Hp];if(!std::isfinite(maximum)||maximum<=0)return {};
            const double current=n.count==1?n.a:maximum*n.a;if(current<0||current>maximum)return {};
            if(n.count==2&&(n.b>maximum||std::abs(current-n.b)>1e-6*std::max(1.,maximum)))return {};
            v=n.stat==0?maximum:n.stat==1?current:n.stat==2?current/maximum:n.stat==3?maximum-current:1-current/maximum;}
        else if(n.op==Add||n.op==Multiply){if(top<n.count)return {};v=n.op==Add?0:1;for(int i=0;i<n.count;++i){const double x=stack[--top];if(n.op==Add)v+=x;else v*=x;}}
        else if(n.op==Minimum||n.op==Maximum){if(n.count<1||top<n.count)return {};v=stack[--top];for(int i=1;i<n.count;++i){const double x=stack[--top];v=n.op==Minimum?std::min(v,x):std::max(v,x);}}
        else if(n.op==Clamp){if(top<1||n.a>n.b)return {};v=std::clamp(stack[--top],n.a,n.b);}
        if(top>=static_cast<int>(stack.size())||!std::isfinite(v))return {};stack[top++]=v;
    }
    if(top!=1)return {};return stack[0];
}
bool CombatDatabase::load(const QString &root,QString *error) {
    champions.clear();items.clear();augments.clear();revision.clear();patch.clear();
    const auto path=root+QStringLiteral("/data/combat.v1.json");QFile file(path);if(!file.open(QIODevice::ReadOnly)){if(error)*error=QStringLiteral("缺少数值资料，请先编译数据");return false;}
    const auto bytes=file.readAll();const auto o=QJsonDocument::fromJson(bytes).object();revision=QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
    patch=o.value(QStringLiteral("patch")).toString();
    if(!QRegularExpression(QStringLiteral("\\A[0-9]{1,3}\\.[0-9]{1,3}\\z")).match(patch).hasMatch()||o.value(QStringLiteral("schema")).toInt()!=1||o.value(QStringLiteral("mode"))!=QStringLiteral("KIWI"))return false;
    const auto modeRules=validatedCombatModeRules(o.value(QStringLiteral("mode_context_rules")),patch);
    if(!modeRules){if(error)*error=QStringLiteral("模式参数的版本或来源校验失败");return false;}
    audit[QStringLiteral("mode_context_rules")]=*modeRules;
    for(const auto &v:o.value(QStringLiteral("champions")).toArray()){const auto h=v.toObject();const auto value=h.value(QStringLiteral("id"));const int id=value.toInt();
        if(!value.isDouble()||value.toDouble()!=id||id<=0||champions.contains(id))return false;champions[id]=h;}
    auto rules=o.value(QStringLiteral("effect_rules")).toObject();
    if((patch==QStringLiteral("16.21")||!rules.value(QStringLiteral("patch")).toString().isEmpty())&&rules.value(QStringLiteral("patch")).toString()!=patch)rules={};
    audit[QStringLiteral("verified_description_overrides")]=o.value(QStringLiteral("verified_description_overrides"));
    auto loadEntries=[&](const QString &key,QMap<int,CombatEntry> &out){const auto adapters=rules.value(key).toObject();for(const auto &v:o.value(key).toArray()){
        const auto identity=v.toObject().value(QStringLiteral("id"));const int id=identity.toInt();if(!identity.isDouble()||identity.toDouble()!=id||id<=0||out.contains(id))return false;
        const auto x=v.toObject();CombatEntry e;e.id=x.value(QStringLiteral("id")).toInt();const auto price=x.value(QStringLiteral("price"));e.price=price.isDouble()&&std::isfinite(price.toDouble())&&price.toDouble()==price.toInt()&&price.toInt()>=0?price.toInt():-1;e.requiredLevel=x.value(QStringLiteral("required_level")).toInt(1);e.eligible=x.value(QStringLiteral("eligible")).toBool()&&e.price>=0;e.boots=x.value(QStringLiteral("boots")).toBool();e.finished=x.value(QStringLiteral("epicness")).toInt()>=5||(e.boots&&x.value(QStringLiteral("epicness")).toInt()==4);
        e.stats=readStats(x.value(QStringLiteral("stats")).toObject());e.values=x.value(QStringLiteral("values")).toObject();e.calculations=x.value(QStringLiteral("calculations")).toObject();e.growthStackInputs=x.value(QStringLiteral("growth_stack_inputs")).toArray();const auto adapter=adapters.value(QString::number(e.id)).toObject();e.effects=adapter.value(QStringLiteral("effects")).toArray();e.actions=adapter.value(QStringLiteral("actions")).toArray();e.eventRules=adapter.value(QStringLiteral("event_rules")).toArray();e.triggerContract=adapter.value(QStringLiteral("trigger_contract")).toObject();e.coverage=adapter.value(QStringLiteral("coverage")).toString();
        e.temporalBindingsV4=x.value(QStringLiteral("temporal_bindings_v4")).toArray();
        e.mechanismStateV1=x.value(QStringLiteral("mechanism_state_v1")).toObject();
        e.epicness=x.value(QStringLiteral("epicness")).toInt();
        if(x.contains(QStringLiteral("conditional_purchase_restrictions"))) {
            const auto restrictions=x.value(QStringLiteral("conditional_purchase_restrictions"));
            if(key!=QStringLiteral("items")||!restrictions.isArray())return false;
            QSet<QString> seen;
            for(const auto &value:restrictions.toArray()) {
                if(!value.isObject())return false;const auto restriction=value.toObject();
                const QSet<QString> fields{QStringLiteral("kind"),QStringLiteral("source_field"),QStringLiteral("buff_name"),QStringLiteral("source_pointer"),QStringLiteral("evaluation"),QStringLiteral("purchase_legal_verified")};
                if(restriction.size()!=fields.size())return false;
                for(auto field=restriction.begin();field!=restriction.end();++field)if(!fields.contains(field.key()))return false;
                const auto buff=restriction.value(QStringLiteral("buff_name")).toString();
                if(restriction.value(QStringLiteral("kind"))!=QStringLiteral("restricted_buff")
                    ||restriction.value(QStringLiteral("source_field"))!=QStringLiteral("RestrictedBuffName")
                    ||buff.trimmed().isEmpty()||buff.size()>256||seen.contains(buff)
                    ||restriction.value(QStringLiteral("source_pointer"))!=QStringLiteral("Items/")+QString::number(e.id)
                    ||restriction.value(QStringLiteral("evaluation"))!=QStringLiteral("requires_current_actor_buff_or_authoritative_shop_state")
                    ||restriction.value(QStringLiteral("purchase_legal_verified"))!=QJsonValue(false))return false;
                seen.insert(buff);e.conditionalPurchaseRestrictions.append(restriction);
            }
        }
        if(e.id==3049&&!e.mechanismStateV1.isEmpty()) {
            // The raw name Cooldown does not establish an ICD. The reviewed
            // state consumer requires an explicit timing contract instead.
            auto parameters=e.triggerContract.value(QStringLiteral("parameters")).toObject();
            parameters.remove(QStringLiteral("cooldown"));e.triggerContract[QStringLiteral("parameters")]=parameters;
        }
        const auto groups=x.value(QStringLiteral("groups")).toObject();for(auto i=groups.begin();i!=groups.end();++i)e.groups[i.key()]=i.value().toInt();
        for(const auto &id:x.value(QStringLiteral("from_ids")).toArray())e.from.append(id.toInt());
        for(const auto &reason:x.value(QStringLiteral("exclusions")).toArray())e.purchaseExclusions.append(reason.toString());out[e.id]=e;
    }return true;};
    if(!loadEntries(QStringLiteral("items"),items)||!loadEntries(QStringLiteral("augments"),augments))return false;
    // The hashed catalog manifest owns exact ID/count validation. Database
    // loading itself must not silently retain entries from a previous patch.
    return !champions.isEmpty()&&!items.isEmpty()&&!augments.isEmpty();
}
CombatContext CombatDatabase::context(int id,int level) const {
    CombatContext c;c.championId=id;c.level=level;const auto hero=champions.value(id);const auto levels=hero.value(QStringLiteral("levels")).toArray();
    applyCombatModeRules(c,audit.value(QStringLiteral("mode_context_rules")).toObject());
    if(level<1||level>18||levels.size()!=18)return c;
    c.base=readStats(levels[level-1].toObject());const auto base=hero.value(QStringLiteral("base")).toObject();c.baseAs=base.value(QStringLiteral("as")).toDouble();c.asRatio=base.value(QStringLiteral("as_ratio")).toDouble();c.ranged=hero.value(QStringLiteral("ranged")).toBool();
    int activeStage=0;for(const auto &value:hero.value(QStringLiteral("attack_range_stages")).toArray()){
        const auto stage=value.toObject();const int minimum=stage.value(QStringLiteral("min_level")).toInt();
        if(minimum>activeStage&&minimum<=level&&stage.value(QStringLiteral("ranged")).isBool()&&stage.value(QStringLiteral("range")).isDouble()){
            c.ranged=stage.value(QStringLiteral("ranged")).toBool();c.base[Stat::Range]=stage.value(QStringLiteral("range")).toDouble();activeStage=minimum;
        }
    }
    c.usesMana=hero.value(QStringLiteral("uses_mana")).toBool();return c;
}
std::optional<double> mitigateCombatDamage(const QString &type,double raw,const CombatStats &stats,const CombatContext &context,const QMap<QString,double> &typeAmps){
    if(!QStringList{QStringLiteral("physical"),QStringLiteral("magic"),QStringLiteral("true"),QStringLiteral("adaptive")}.contains(type)||!std::isfinite(raw)||raw<0||raw>1e9||!std::isfinite(context.targetArmor)||!std::isfinite(context.targetMr))return {};
    for(const double value:stats.values)if(!std::isfinite(value))return {};
    for(auto it=typeAmps.cbegin();it!=typeAmps.cend();++it)if(!std::isfinite(it.value())||it.value()<0||it.value()>1e5)return {};
    const double result=raw*damageMultiplier(type,stats,context,typeAmps);if(!std::isfinite(result)||result<0||result>1e12)return {};return result;
}
QJsonObject CombatEvaluation::json() const{
    QJsonObject out{{QStringLiteral("valid"),valid},{QStringLiteral("stats"),stats.json()},{QStringLiteral("damage_type_multipliers"),damageTypeMultipliers},{QStringLiteral("conversion_audit"),conversionAudit},{QStringLiteral("attack_component"),attackDamage},{QStringLiteral("skill_components"),skillDamage},{QStringLiteral("effect_components"),procDamage},{QStringLiteral("attack_effect_subset"),onhitDamage},{QStringLiteral("attack_replacement_subset"),replacementAttackDamage},{QStringLiteral("observed_healing"),healing},{QStringLiteral("self_sustain"),selfSustain},{QStringLiteral("mixed_effective_hp"),survival},{QStringLiteral("growth_objective"),growthObjective},{QStringLiteral("comparison_score"),score},{QStringLiteral("unknowns"),QJsonArray::fromStringList(unknowns)}};
    const auto boundary=combatComparisonBoundary(unknowns,valid);
    for(auto it=boundary.begin();it!=boundary.end();++it)out[it.key()]=it.value();
    return out;
}
CombatEvaluation evaluateCombat(const CombatDatabase &db,const CombatContext &c,const QList<int> &items,const QList<int> &augments,bool prepareWholePool,const QString &style){
    if(!c.formStatTransforms.isEmpty()){
        QSet<QString> transformedStats;bool valid=c.conditions.contains(QStringLiteral("form_stat_transform_order_verified"));
        const bool before=c.conditions.contains(QStringLiteral("form_stat_transform_before_conversions")),after=c.conditions.contains(QStringLiteral("form_stat_transform_after_derived_and_overflow"));
        valid=valid&&(before!=after);
        for(const auto &entry:c.formStatTransforms){const auto row=entry.toObject();const auto stat=row.value(QStringLiteral("stat")).toString(),basis=row.value(QStringLiteral("basis")).toString(),stage=row.value(QStringLiteral("stage")).toString();const double multiplier=row.value(QStringLiteral("multiplier")).toDouble(-1);
            valid=valid&&entry.isObject()&&QSet<QString>{QStringLiteral("ad"),QStringLiteral("hp"),QStringLiteral("armor"),QStringLiteral("mr")}.contains(stat)&&!transformedStats.contains(stat)&&combatStat(stat).has_value()&&(basis==QStringLiteral("total")||basis==QStringLiteral("bonus"))&&std::isfinite(multiplier)&&multiplier>=0&&multiplier<=10&&!row.value(QStringLiteral("source_pointer")).toString().isEmpty()&&!row.value(QStringLiteral("source_field")).toString().isEmpty()&&stage==(before?QStringLiteral("before_conversions"):QStringLiteral("after_derived_and_overflow"));
            transformedStats.insert(stat);
        }
        if(!valid){CombatEvaluation invalid;invalid.unknowns.append(QStringLiteral("invalid_or_unverified_form_stat_transforms"));return invalid;}
    }
    if(c.explicitActionCounts){
        auto validCounts=[](const QMap<QString,double> &values,bool integral){for(auto it=values.cbegin();it!=values.cend();++it)if(!std::isfinite(it.value())||it.value()<0||it.value()>1e5||(integral&&std::floor(it.value())!=it.value()))return false;return true;};
        if(!std::isfinite(c.explicitAttackCount)||c.explicitAttackCount<0||c.explicitAttackCount>1e5||std::floor(c.explicitAttackCount)!=c.explicitAttackCount||!validCounts(c.explicitCastCounts,true)||!validCounts(c.explicitActiveSeconds,false)||!validCounts(c.explicitProcCounts,false)||!validCounts(c.explicitComponentScales,false)){
            CombatEvaluation invalid;invalid.unknowns.append(QStringLiteral("invalid_explicit_action_counts"));return invalid;
        }
    }
    if(c.level<1||c.level>18){CombatEvaluation unavailable;unavailable.unknowns.append(QStringLiteral("unsupported_champion_level:%1:verified_range_1_18").arg(c.level));return unavailable;}
    if(!db.champions.contains(c.championId)||items.size()>6)return {};
    QMap<QString,int> counts;QSet<int> seen;
    for(int id:items){auto item=db.items.constFind(id);if(item==db.items.cend()||item->requiredLevel>c.level)return {};
        if(item->eligible&&seen.contains(id))return {};seen.insert(id);
        if(c.championId==69&&item->boots)return {};
        for(auto g=item->groups.begin();g!=item->groups.end();++g)if(++counts[g.key()]>g.value())return {};
    }
    return Solver(db,c,augments,style,items,nullptr,!prepareWholePool).eval(items,true);
}
QJsonObject anchorObservedCombatStats(const CombatDatabase &db,CombatContext &ctx,const QList<int> &items,
                                     const QList<int> &augments,const QJsonObject &observed,const QString &style) {
    const QMap<QString,Stat> fields{{QStringLiteral("abilityPower"),Stat::Ap},{QStringLiteral("attackDamage"),Stat::Ad},
        {QStringLiteral("maxHealth"),Stat::Hp},{QStringLiteral("armor"),Stat::Armor},{QStringLiteral("magicResist"),Stat::Mr},
        {QStringLiteral("attackSpeed"),Stat::As},{QStringLiteral("moveSpeed"),Stat::Ms}};
    QVector<Stat> stats;QVector<double> targets;
    for(auto it=fields.cbegin();it!=fields.cend();++it){const auto v=observed.value(it.key());if(v.isDouble()&&std::isfinite(v.toDouble())&&v.toDouble()>=0){stats.append(it.value());targets.append(v.toDouble());}}
    QJsonObject report{{QStringLiteral("available"),false},{QStringLiteral("causal_identification"),false}};
    if(stats.isEmpty())return report;
    auto raw=ctx;raw.observedOffset={};raw.observedInputOffset={};
    const auto baseline=evaluateCombat(db,raw,items,augments,false,style);if(!baseline.valid)return report;
    auto error=[&](const CombatEvaluation &v){double result=0;for(int i=0;i<stats.size();++i)result=std::max(result,std::abs(v.stats[stats[i]]-targets[i])/std::max(1.,std::abs(targets[i])));return result;};
    auto trial=raw;auto trialValue=baseline;QString failure;int evaluations=1,iterations=0;bool matched=false;
    // Seven inputs at most; four bounded corrections also handle MS soft-cap
    // branch changes. No correction is accepted unless the baseline matches.
    for(int iteration=0;iteration<=4;++iteration){
        const auto &value=trialValue;
        if(!value.valid){failure=QStringLiteral("invalid_trial");break;}
        if(error(value)<=1e-6){matched=true;break;}
        if(iteration==4){failure=QStringLiteral("iteration_limit");break;}
        QVector<int> active;
        for(int i=0;i<stats.size();++i){
            if(stats[i]==Stat::As&&targets[i]>ctx.attackSpeedCap+1e-6){failure=QStringLiteral("observed_attack_speed_exceeds_cap");break;}
            if(stats[i]==Stat::As&&targets[i]>=ctx.attackSpeedCap-1e-6&&value.stats[Stat::As]>=ctx.attackSpeedCap-1e-6)continue;
            active.append(i);
        }
        if(!failure.isEmpty()||active.isEmpty())break;
        std::array<std::array<double,8>,7> matrix{};const int n=int(active.size());
        for(int j=0;j<n;++j){const int index=active[j];auto probe=trial;
            const double step=stats[index]==Stat::As?.001:std::max(.01,std::abs(targets[index])*1e-4);
            probe.observedInputOffset[stats[index]]+=step;
            const auto next=evaluateCombat(db,probe,items,augments,false,style);++evaluations;
            if(!next.valid){failure=QStringLiteral("invalid_probe");break;}
            for(int i=0;i<n;++i)matrix[i][j]=(next.stats[stats[active[i]]]-value.stats[stats[active[i]]])/step;
        }
        if(!failure.isEmpty())break;
        for(int i=0;i<n;++i)matrix[i][n]=targets[active[i]]-value.stats[stats[active[i]]];
        for(int j=0;j<n;++j){int pivot=j;for(int i=j+1;i<n;++i)if(std::abs(matrix[i][j])>std::abs(matrix[pivot][j]))pivot=i;
            if(!std::isfinite(matrix[pivot][j])||std::abs(matrix[pivot][j])<1e-9){failure=QStringLiteral("singular_anchor");break;}
            std::swap(matrix[pivot],matrix[j]);const double divisor=matrix[j][j];for(int k=j;k<=n;++k)matrix[j][k]/=divisor;
            for(int i=0;i<n;++i)if(i!=j){const double scale=matrix[i][j];for(int k=j;k<=n;++k)matrix[i][k]-=scale*matrix[j][k];}
        }
        if(!failure.isEmpty())break;
        for(int i=0;i<n;++i){const double delta=matrix[i][n];if(!std::isfinite(delta)||std::abs(delta)>1e7){failure=QStringLiteral("unbounded_correction");break;}trial.observedInputOffset[stats[active[i]]]+=delta;}
        if(!failure.isEmpty())break;
        ++iterations;
        trialValue=evaluateCombat(db,trial,items,augments,false,style);++evaluations;
    }
    report[QStringLiteral("available")]=true;report[QStringLiteral("iterations")]=iterations;
    if(matched){ctx=trial;report[QStringLiteral("method")]=QStringLiteral("pre_transform_residual");}
    else{
        ctx=raw;for(int i=0;i<stats.size();++i)ctx.observedOffset[stats[i]]=targets[i]-baseline.stats[stats[i]];
        report[QStringLiteral("method")]=QStringLiteral("legacy_additive_fallback");report[QStringLiteral("fallback_reason")]=failure.isEmpty()?QStringLiteral("anchor_not_identifiable"):failure;
        trialValue=evaluateCombat(db,ctx,items,augments,false,style);++evaluations;
    }
    const double residual=trialValue.valid?error(trialValue):1.;
    report[QStringLiteral("evaluations")]=evaluations;
    report[QStringLiteral("maximum_relative_error")]=residual;report[QStringLiteral("baseline_matched")]=trialValue.valid&&residual<=1e-6;
    return report;
}
bool compatibleEquipment(const CombatDatabase &db,const QList<int> &ids,int id,int cid,int level) {
    if(ids.size()>=6||!db.items.contains(id))return false;
    const auto &item=db.items[id];if(item.requiredLevel>level||(cid==69&&item.boots))return false;
    // Physical copies of plain recipe components stack their base attributes.
    // Unique/passive-bearing equipment keeps the original one-copy contract.
    const bool duplicateComponent=!item.eligible&&!item.finished&&!item.boots
        &&item.effects.isEmpty()&&item.actions.isEmpty()&&item.eventRules.isEmpty()
        &&item.growthStackInputs.isEmpty()&&item.temporalBindingsV4.isEmpty();
    if(ids.contains(id)&&!duplicateComponent)return false;
    QMap<QString,int> counts;for(int held:ids){const auto entry=db.items.constFind(held);if(entry==db.items.cend())return false;for(auto i=entry->groups.cbegin();i!=entry->groups.cend();++i)++counts[i.key()];}
    for(auto i=item.groups.cbegin();i!=item.groups.cend();++i)if(counts.value(i.key())>=i.value())return false;
    return true;
}
int remainingBuildCost(const CombatDatabase &db,const QList<int> &build,const QList<int> &owned) {
    QMap<int,int> stock;for(int id:owned)++stock[id];
    std::function<int(int,int)> credit=[&](int id,int depth){
        if(depth>12||!db.items.contains(id))return -1;const auto &item=db.items[id];
        if(stock.value(id)>0){if(item.price<0)return -1;--stock[id];return item.price;}
        int total=0;for(int part:item.from){const int value=credit(part,depth+1);if(value<0)return -1;total+=value;}
        if(item.price<0)return total==0?0:-1;return std::min(item.price,total);
    };
    auto cost=[&](int id){if(!db.items.contains(id)||db.items[id].price<0)return -1;
        const auto &item=db.items[id];int amount=0;for(int part:item.from){const int value=credit(part,0);if(value<0)return -1;amount+=value;}return std::max(0,item.price-amount);};
    // Reserve exact held finished items before consuming their recipe parts.
    QList<int> pending;for(int id:build){if(stock.value(id)>0)--stock[id];else pending.append(id);}
    int total=0;for(int id:pending){int n=cost(id);if(n<0)return -1;total+=n;}return total;
}
QJsonObject optimizeCombatBuild(const Catalog &catalog,const GameState &state,int cid,const QString &style,int beamWidth) {
    if(!modelRulesUsable(catalog,state))return {{QStringLiteral("route"),QStringLiteral("等待本地规则")},{QStringLiteral("items"),QJsonArray{}},{QStringLiteral("version_match"),false}};
    if(!catalog.combat||!catalog.combat->champions.contains(cid))return {{QStringLiteral("route"),QStringLiteral("等待数值资料")},{QStringLiteral("items"),QJsonArray{}}};
    auto subject=state;subject.championId=cid;
    if(combatRoleIntent(catalog,subject,style).value(QStringLiteral("intent"))==QStringLiteral("ally_protection")){
        auto reference=protectionEquipmentReference(catalog,state,cid,growthCombatPreference());
        auto context=catalog.combat->context(cid,subject.level>=1&&subject.level<=18?subject.level:18);
        for(const auto &slot:{QStringLiteral("Q"),QStringLiteral("W"),QStringLiteral("E"),QStringLiteral("R")})context.ranks[slot]=state.fresh()?state.abilities.value(slot).toObject().value(QStringLiteral("abilityLevel")).toInt():slot==QStringLiteral("R")?3:5;
        context.counters=state.effectCounters;context.conditions=state.effectConditions;
        const auto selected=effectiveSelectedAugments(catalog,state);bindGrowthObjective(*catalog.combat,context,state.owned,selected);
        reference[QStringLiteral("resolved_preference")]=growthCombatPreference();reference[QStringLiteral("growth_objective")]=describeGrowthObjective(*catalog.combat,context,state.owned,selected);
        reference[QStringLiteral("role_matching_enabled")]=false;reference[QStringLiteral("fixed_direction_matching_enabled")]=false;
        return reference;
    }
    const auto &db=*catalog.combat;const bool same=state.fresh()&&state.championId==cid;
    const bool explicitScenario=state.source==GameState::Source::Offline&&state.championId==cid;
    const bool supplied=same||explicitScenario;
    if(same&&(state.level<1||state.level>18))return {
        {QStringLiteral("route"),QStringLiteral("当前等级成长规则尚未核实")},{QStringLiteral("items"),QJsonArray{}},
        {QStringLiteral("observed_level"),state.level},{QStringLiteral("supported_level_range"),QJsonArray{1,18}},
        {QStringLiteral("comparison"),evaluateCombat(db,db.context(cid,state.level),{},{}).json()},
        {QStringLiteral("advice"),QJsonArray{QStringLiteral("已保留真实等级；该等级的基础属性成长尚未核实，暂不生成数值配装。")}},
        {QStringLiteral("method"),QStringLiteral("unsupported_champion_level")}};
    auto ctx=db.context(cid,supplied?state.level:18);
    const auto targetContext=applyOpponentLoadoutContext(catalog,state,ctx);
    QStringList assumptions{targetContext.value(QStringLiteral("available")).toBool()?QStringLiteral("比较目标按当前可见对手装备和等级估计；未知海克斯与隐藏抗性仍未知"):QStringLiteral("未取得对手背包；使用参考目标，不代表本局实际抗性")};
    for(const QString slot:{QStringLiteral("Q"),QStringLiteral("W"),QStringLiteral("E"),QStringLiteral("R")})ctx.ranks[slot]=same?state.abilities.value(slot).toObject().value(QStringLiteral("abilityLevel")).toInt():slot==QStringLiteral("R")?3:5;
    if(same){assumptions.append(QStringLiteral("使用当前等级与技能等级；基础属性加规划装备，当前面板不会重复叠加"));ctx.counters=state.effectCounters;ctx.conditions=state.effectConditions;}
    else if(explicitScenario){ctx.counters=state.effectCounters;ctx.conditions=state.effectConditions;assumptions.append(QStringLiteral("背包、层数和状态为离线显式假设，不是局内观测；不推算后续触发或成长层数"));}
    else assumptions.append(QStringLiteral("离线按 18 级参考加点比较；自动升级技能和特殊加点仍需本局确认"));
    if(ctx.counters.contains(QStringLiteral("target_current_hp"))||ctx.counters.contains(QStringLiteral("target_health_fraction")))assumptions.append(QStringLiteral("目标血量按传入状态固定比较；窗口内受伤、回复及跨血线时刻仍未知"));
    QList<int> selected=effectiveSelectedAugments(catalog,state);
    QJsonArray triggerDecisions;QMap<QString,double> triggerTimes;QSet<QString> observedEvents;
    if(same)for(const auto &v:state.mechanicObservations){const auto observation=v.toObject();const int id=observation.value(QStringLiteral("id")).toInt();
        const bool augment=observation.value(QStringLiteral("source")).toString()==QStringLiteral("augment");
        if(augment?!selected.contains(id):!state.owned.contains(id))continue;
        const auto &entries=augment?db.augments:db.items;const auto entry=entries.constFind(id);if(entry==entries.cend())continue;
        const auto eventKey=QString::fromUtf8(QJsonDocument(observation).toJson(QJsonDocument::Compact));
        if(observedEvents.contains(eventKey))continue;observedEvents.insert(eventKey);
        const auto decision=evaluateMechanicTrigger(*entry,ctx,ctx.base,observation,triggerTimes);triggerDecisions.append(decision.json());
        // Only an explicit observation of the effect count may populate a
        // numeric event counter; a cast is never invented into eight hits.
        const auto facts=observation.value(QStringLiteral("facts")).toObject();
        const auto count=facts.value(QStringLiteral("numeric_effect_count"));
        const double ago=state.gameTime-observation.value(QStringLiteral("time")).toDouble();
        if(decision.matched&&count.isDouble()&&count.toDouble()>=0&&std::isfinite(count.toDouble())&&ago>=0&&ago<ctx.horizon){
            const auto prefix=(augment?QStringLiteral("augment:"):QStringLiteral("item:"))+QString::number(id)+QChar(u':');
            for(const auto &effect:entry->effects){const auto ef=effect.toObject();const auto op=ef.value(QStringLiteral("op")).toString();const auto counter=ef.value(QStringLiteral("counter")).toString();
                if((op==QStringLiteral("observed_damage")||op==QStringLiteral("observed_heal"))&&counter.startsWith(prefix)&&!state.effectCounters.contains(counter))ctx.counters[counter]+=count.toDouble();
            }
        }
    }
    MechanicLedger ledger;
    if(same){ctx.counters[QStringLiteral("observed_game_time")]=state.gameTime;
        if(std::isfinite(state.mechanicBaselineTime)&&state.mechanicBaselineTime>=0)ctx.counters[QStringLiteral("mechanic_baseline_time")]=state.mechanicBaselineTime;
        ledger=replayCombatMechanics(db,ctx,state.owned,selected,state.mechanicObservations);applyMechanicLedgerContext(ledger,state.gameTime,ctx,true);}
    auto preferenceState=state;preferenceState.championId=cid;const auto preference=resolvedCombatPreference(catalog,preferenceState,style);
    QJsonObject observedAnchor;
    if(same&&!state.liveStats.isEmpty()){
        observedAnchor=anchorObservedCombatStats(db,ctx,state.owned,selected,state.liveStats,preference);
        assumptions.append(QStringLiteral("按可见面板校准残差，保留模型内放大顺序；不能由面板识别未知增益的真实来源，未来变化保持未知"));
    }
    bindGrowthObjective(db,ctx,supplied?state.owned:QList<int>{},selected);
    Solver solver(db,ctx,selected,preference,state.owned);QList<int> pool,fixed;
    for(auto it=db.items.cbegin();it!=db.items.cend();++it)if(it.value().eligible&&it.value().price>0&&it.value().requiredLevel<=ctx.level&&!(cid==69&&it.value().boots)){
        pool.append(it.key());
    }
    if(supplied)for(int id:state.owned)if(db.items.contains(id)&&db.items[id].finished&&!fixed.contains(id)&&fixed.size()<6)fixed.append(id);
    struct Node {QList<int> ids;double score=0;int dependencies=0;QJsonObject metrics;};qint64 evaluated=0;
    const int width=std::clamp(beamWidth,1,64);
    const auto metricNames=ctx.growthObjectiveBasis.value(QStringLiteral("metric_weights")).toObject().keys();
    const auto valueFor=[](const Node &node,const QString &metric){return node.metrics.value(metric).toObject().value(QStringLiteral("ratio")).toDouble();};
    const auto compare=[](const Node &a,const Node &b){if(a.dependencies!=b.dependencies)return a.dependencies>b.dependencies;if(a.score!=b.score)return a.score>b.score;return a.ids<b.ids;};
    const auto nodeFor=[&](const QList<int> &ids,const CombatEvaluation &value){return Node{ids,value.score,growthRequiredItemCoverage(ctx,ids),value.growthObjective.value(QStringLiteral("metrics")).toObject()};};
    auto search=[&](const Solver &model){
        QVector<Node> candidates{nodeFor(fixed,model.eval(fixed))};++evaluated;
        for(int slot=fixed.size();slot<6;++slot){QVector<Node> next;QSet<QList<int>> seen;
            next.reserve(candidates.size()*pool.size());seen.reserve(candidates.size()*pool.size());
            for(const auto &n:candidates)for(int id:pool){if(!compatibleEquipment(db,n.ids,id,cid,ctx.level))continue;auto ids=n.ids;ids.append(id);std::sort(ids.begin(),ids.end());if(seen.contains(ids))continue;seen.insert(ids);
                auto value=model.eval(ids);++evaluated;if(value.valid)next.append(nodeFor(ids,value));}
            if(next.isEmpty())break;
            // A portfolio beam preserves representatives of every admitted
            // metric. One scalar tradeoff must not prune all durable or cyclic
            // alternatives and then masquerade as a unique six-slot optimum.
            QVector<Node> portfolio;QSet<QList<int>> kept;const int perMetric=std::max(1,width/std::max(1,int(metricNames.size())));
            for(const auto &metric:metricNames){const auto axis=[&](const Node &a,const Node &b){if(a.dependencies!=b.dependencies)return a.dependencies>b.dependencies;const double av=valueFor(a,metric),bv=valueFor(b,metric);if(av!=bv)return av>bv;return compare(a,b);};
                const int count=std::min(perMetric,int(next.size()));std::partial_sort(next.begin(),next.begin()+count,next.end(),axis);
                for(int i=0;i<count&&portfolio.size()<width;++i)if(!kept.contains(next[i].ids)){kept.insert(next[i].ids);portfolio.append(next[i]);}}
            const int fill=std::min(width,int(next.size()));std::partial_sort(next.begin(),next.begin()+fill,next.end(),compare);
            for(int i=0;i<fill&&portfolio.size()<width;++i)if(!kept.contains(next[i].ids)){kept.insert(next[i].ids);portfolio.append(next[i]);}
            std::sort(portfolio.begin(),portfolio.end(),compare);candidates=std::move(portfolio);
        }
        return candidates;
    };
    auto beam=search(solver);
    if(beam.isEmpty()||!solver.eval(beam.first().ids).valid)return {
        {QStringLiteral("route"),QStringLiteral("当前强化组合尚无法核实")},{QStringLiteral("items"),QJsonArray{}},
        {QStringLiteral("comparison"),solver.eval(fixed,true).json()},
        {QStringLiteral("advice"),QJsonArray{QStringLiteral("当前强化包含尚未支持的转换组合，暂不生成配装；请核对已选记录。")}},
        {QStringLiteral("method"),QStringLiteral("unsupported combination")}};
    auto best=beam.first().ids;const auto finalEval=solver.eval(best,true);QJsonArray result,alternative,offers,loadouts;
    // A presentation order only: chosen source-bound prerequisites precede
    // other conditional comparisons. This is not an observed shopping order.
    QList<int> order=fixed,left=best;for(int id:fixed)left.removeOne(id);
    while(!left.isEmpty()){int pick=left.first(),covered=-1;double gain=-1;const auto base=solver.eval(order).score;
        for(int id:left){auto next=order;next.append(id);const int after=remainingBuildCost(db,next,supplied?state.owned:QList<int>{}),before=remainingBuildCost(db,order,supplied?state.owned:QList<int>{});if(after<0||before<0)continue;const int cost=after-before,coverage=growthRequiredItemCoverage(ctx,next);const double g=(solver.eval(next).score-base)/std::max(1,cost);if(coverage>covered||(coverage==covered&&g>gain)){gain=g;covered=coverage;pick=id;}}
        order.append(pick);left.removeOne(pick);
    }
    for(int id:order)result.append(QJsonObject{{QStringLiteral("item_id"),id},{QStringLiteral("name"),catalog.itemName(id)},{QStringLiteral("price"),db.items[id].price>=0?QJsonValue(db.items[id].price):QJsonValue(QJsonValue::Null)},{QStringLiteral("owned"),supplied&&state.owned.contains(id)},{QStringLiteral("availability"),QStringLiteral("以本局商店为准")}});
    for(int n=0;n<std::min(3,int(beam.size()));++n){QJsonArray ids;for(int id:beam[n].ids)ids.append(id);alternative.append(QJsonObject{{QStringLiteral("item_ids"),ids},{QStringLiteral("comparison_score"),beam[n].score},{QStringLiteral("candidate_filter_not_recommendation_rank"),true}});}
    QMap<int,QStringList> representatives;representatives[0].append(QStringLiteral("normalized_tradeoff"));
    for(const auto &metric:metricNames){int pick=0;for(int n=1;n<beam.size();++n)if(beam[n].dependencies>beam[pick].dependencies||(beam[n].dependencies==beam[pick].dependencies&&valueFor(beam[n],metric)>valueFor(beam[pick],metric)))pick=n;representatives[pick].append(metric);}
    for(auto it=representatives.cbegin();it!=representatives.cend();++it){const auto &node=beam[it.key()];QJsonArray itemIds;for(int id:node.ids)itemIds.append(id);
        loadouts.append(QJsonObject{{QStringLiteral("item_ids"),itemIds},{QStringLiteral("retained_for_metrics"),QJsonArray::fromStringList(it.value())},{QStringLiteral("metrics"),node.metrics},
            {QStringLiteral("filter_score"),node.score},{QStringLiteral("required_items_satisfied"),node.dependencies},{QStringLiteral("reference_only"),true},{QStringLiteral("ranking_available"),false},
            {QStringLiteral("purchase_order_verified"),false},{QStringLiteral("complete_growth_value_quantified"),false}});}
    // The main item field is the actual inventory plus an evidenced growth
    // entry point. Full search combinations are alternatives for explanation,
    // never an inferred mandatory build or current purchase instruction.
    QJsonArray displayed;QSet<int> displayedIds,dependencyIds;
    for(const auto &v:ctx.growthObjectiveBasis.value(QStringLiteral("required_item_dependencies")).toArray())dependencyIds.insert(v.toObject().value(QStringLiteral("item_id")).toInt());
    if(supplied)for(int id:state.owned){displayedIds.insert(id);displayed.append(QJsonObject{{QStringLiteral("item_id"),id},{QStringLiteral("name"),catalog.itemName(id)},{QStringLiteral("owned"),true},{QStringLiteral("reference_only"),true},
        {QStringLiteral("inventory_is_offline_assumption"),explicitScenario},{QStringLiteral("availability"),QStringLiteral("当前背包或显式离线假设")}});}
    for(const auto &v:result){const auto item=v.toObject();const int id=item.value(QStringLiteral("item_id")).toInt();if(!dependencyIds.contains(id)||displayedIds.contains(id))continue;displayedIds.insert(id);auto dependency=item;
        dependency[QStringLiteral("reference_only")]=true;dependency[QStringLiteral("growth_prerequisite")]=true;dependency[QStringLiteral("purchase_legal_verified")]=false;displayed.append(dependency);}
    // Offers are counterfactuals, never silently applied to the chosen build.
    for(int id:state.offers){if(!catalog.ordinaryOfferEligible(id))continue;auto hypothetical=selected;if(!hypothetical.contains(id))hypothetical.append(id);Solver trial(db,ctx,hypothetical,preference,{},&solver);const auto v=trial.eval(best,true);const auto offerBeam=search(trial);QJsonArray ids;QJsonObject offerPlan;
        if(!offerBeam.isEmpty()&&offerBeam.first().ids.size()==6&&trial.eval(offerBeam.first().ids).valid){for(int itemId:offerBeam.first().ids)ids.append(itemId);offerPlan=trial.eval(offerBeam.first().ids,true).json();}
        offers.append(QJsonObject{{QStringLiteral("augment_id"),id},{QStringLiteral("same_build_comparison"),v.json()},{QStringLiteral("adapted_item_ids"),ids},{QStringLiteral("adapted_comparison"),offerPlan},{QStringLiteral("status"),QStringLiteral("counterfactual_not_selected")}});}
    double nextMechanicTime=-1;
    auto boundary=[&](double t){if(same&&t>state.gameTime&&(nextMechanicTime<0||t<nextMechanicTime))nextMechanicTime=t;};
    for(const auto &v:ledger.effects){const auto e=v.toObject();boundary(e.value(QStringLiteral("start")).toDouble(-1));boundary(e.value(QStringLiteral("expires")).toDouble(-1));}
    for(const auto &v:state.mechanicObservations)boundary(v.toObject().value(QStringLiteral("time")).toDouble(-1));
    QStringList advice{QStringLiteral("按已选海克斯、成长前置与本局商店和威胁比较；返回集合不代表购买顺序。"),QStringLiteral("部分英雄被动、任务层数和触发条件仍缺验证，当前组合用于比较取舍。")};
    if(!selected.isEmpty())advice.append(QStringLiteral("强化中已核实的效果参与比较；其余触发机制、层数和任务进度仍待确认。"));
    if(!same)advice.append(QStringLiteral("进入对局后会切换到当前等级和已学技能重新搭配。"));
    return {{QStringLiteral("route"),QStringLiteral("英雄与海克斯成长条件候选")},{QStringLiteral("resolved_preference"),preference},{QStringLiteral("route_policy_source"),QStringLiteral("hero_mechanisms_selected_augments_and_frozen_growth_baseline")},
        {QStringLiteral("role_matching_enabled"),false},{QStringLiteral("fixed_direction_matching_enabled"),false},{QStringLiteral("growth_conditioned_candidates"),true},{QStringLiteral("recommendation_kind"),QStringLiteral("growth_conditioned_candidates")},
        {QStringLiteral("reference_only"),true},{QStringLiteral("conditional_reference"),true},{QStringLiteral("ranking_available"),false},{QStringLiteral("globally_optimal_full_build"),false},{QStringLiteral("comparison_is_candidate_filter"),true},
        {QStringLiteral("items_are_inventory_and_evidenced_growth_dependencies"),true},{QStringLiteral("candidate_loadouts"),loadouts},{QStringLiteral("growth_objective"),finalEval.growthObjective},{QStringLiteral("target_context"),targetContext},{QStringLiteral("items"),displayed},{QStringLiteral("advice"),QJsonArray::fromStringList(advice)},
        {QStringLiteral("method"),QStringLiteral("deterministic beam search; partial combat model; not global optimum")},{QStringLiteral("comparison"),finalEval.json()},{QStringLiteral("alternatives"),alternative},{QStringLiteral("offer_comparisons"),offers},
        {QStringLiteral("assumptions"),QJsonArray::fromStringList(assumptions)},{QStringLiteral("observed_anchor"),observedAnchor},{QStringLiteral("trigger_decisions"),triggerDecisions},{QStringLiteral("remaining_build_gold"),remainingBuildCost(db,best,supplied?state.owned:QList<int>{})},
        {QStringLiteral("executed_mechanics"),ledger.json(same?state.gameTime:0)},{QStringLiteral("growth_projection"),predictCombatGrowth(db,ctx,best,selected)},
        {QStringLiteral("mechanic_snapshot_time"),state.gameTime},{QStringLiteral("mechanic_valid_until"),nextMechanicTime},
        {QStringLiteral("search"),QJsonObject{{QStringLiteral("eligible_pool"),pool.size()},{QStringLiteral("beam_width"),width},{QStringLiteral("metric_portfolio_beam"),true},{QStringLiteral("evaluated_combinations"),evaluated},{QStringLiteral("exhaustive"),false},{QStringLiteral("unique_full_build_winner_claimed"),false}}}};
}
