#include "hextech_conversion.h"
#include <QJsonDocument>
#include <QRegularExpression>
#include <cmath>
#include <algorithm>

namespace {
QString k(const char *s){return QString::fromLatin1(s);}
bool finite(const QJsonValue &v,double lo=0,double hi=1e9){return v.isDouble()&&std::isfinite(v.toDouble())&&v.toDouble()>=lo&&v.toDouble()<=hi;}
bool fail(QStringList *why,const char *s){if(why)why->append(k(s));return false;}
double scalar(const CombatEntry &e,const char *name){auto v=e.values.value(k(name));if(v.isArray())v=v.toArray().first();return v.toDouble();}
QJsonObject contract(const CombatEntry &e){return e.triggerContract.value(k("hextech40_conversion_v1")).toObject();}
bool integer(double v){return std::isfinite(v)&&v>=0&&v<=1e6&&std::floor(v)==v;}
}
bool hasHextech40Conversion(const CombatEntry &e){
    const auto o=contract(e);static const QRegularExpression sha(k("^[0-9a-f]{64}$"));
    return o.value(k("review_id"))==k("hextech40_conversion_v1")&&sha.match(o.value(k("review_sha256")).toString()).hasMatch()&&sha.match(o.value(k("source_projection_sha256")).toString()).hasMatch();
}
bool hextech40CrownReady(const CombatContext &c){
    const auto stack=k("augment:3132:stacks"),combat=k("augment:3132:in_combat");
    if(!c.counters.contains(stack)||!integer(c.counters.value(stack))||!c.counters.contains(combat)||
       (c.counters.value(combat)!=0&&c.counters.value(combat)!=1)||!c.conditions.contains(k("verified_hextech40_3132_combat_state")))return false;
    if(c.counters.value(combat)==0)return true;
    return c.conditions.contains(k("verified_hextech40_3132_ms_before_softcap"))!=c.conditions.contains(k("verified_hextech40_3132_ms_after_softcap"));
}
double hextech40CrownMovement(const CombatEntry &e,const CombatContext &c){
    if(!hextech40CrownReady(c)||c.counters.value(k("augment:3132:in_combat"))==0)return 1;
    return 1+std::max(scalar(e,"MaxMSDecrease"),scalar(e,"InitialMSDecrease")+c.counters.value(k("augment:3132:stacks"))*scalar(e,"MSDecreasePerTakedown"));
}
bool applyHextech40Conversions(const CombatDatabase &db,const CombatContext &c,const QList<int> &items,
    const QList<int> &augments,double ownedMana,CombatEvaluation &r){
    auto &s=r.stats;
    for(int id:augments){const auto e=db.augments.constFind(id);if(e==db.augments.cend()||!hasHextech40Conversion(*e))continue;
        if(id==3059){
            for(int item:items){const auto bonus=c.itemAttachedStats.value(item)[Stat::Mana];if(!std::isfinite(bonus)||bonus<0)return false;ownedMana+=bonus;}
            const auto key=k("augment:3059:item_mana");
            if(c.counters.contains(key)&&(!std::isfinite(c.counters.value(key))||std::abs(c.counters.value(key)-ownedMana)>1e-6)){
                r.unknowns.append(k("conversion_3059_item_mana_counter_does_not_match_owned_bucket"));return false;}
            if(!std::isfinite(ownedMana)||ownedMana<0||(c.usesMana&&s[Stat::Mana]+1e-6<ownedMana))return false;
            if(c.usesMana)s[Stat::Mana]-=ownedMana;
            const double credit=ownedMana*contract(*e).value(k("ratio")).toDouble();s[Stat::Ap]+=credit;
            r.conversionAudit.append(QJsonObject{{k("augment_id"),id},{k("stage"),k("after_flat_before_amplifiers")},
                {k("owned_item_mana_bucket"),ownedMana},{k("mana_debit"),c.usesMana?ownedMana:0},
                {k("ap_credit_before_amplifiers"),credit},{k("manaless"),!c.usesMana},{k("single_pass"),true}});
        }
        if(id==3058){const auto key=k("augment:3058:converted_ad");
            if(!c.counters.contains(key)){r.unknowns.append(k("conversion_3058_observed_debit_missing"));return false;}
            const double debit=c.counters.value(key);
            if(!std::isfinite(debit)||debit<0||debit>s[Stat::Ad]+1e-6){r.unknowns.append(k("conversion_3058_debit_exceeds_actual_preconversion_ad"));return false;}
            if(!c.conditions.contains(k("verified_hextech40_3058_preconversion_total_ad_single_pass"))||c.championId==202||!c.formStatTransforms.isEmpty()||augments.contains(3018)||augments.contains(3059)){
                r.unknowns.append(k("conversion_feedback_order_unreviewed"));return false;}
            const double available=s[Stat::Ad],credit=debit*contract(*e).value(k("ratio")).toDouble();
            s[Stat::Ad]-=debit;s[Stat::BonusAs]+=credit;
            r.conversionAudit.append(QJsonObject{{k("augment_id"),id},{k("available_preconversion_ad"),available},
                {k("ad_debit"),debit},{k("bonus_as_fraction_credit"),credit},{k("single_pass"),true}});
        }
        if(id==3132){
            const auto stack=k("augment:3132:stacks");if(c.counters.contains(stack)&&!integer(c.counters.value(stack))){r.unknowns.append(k("crown_observed_integer_stacks_required"));return false;}
            if(!hextech40CrownReady(c)){r.unknowns.append(k("crown_benefit_and_movement_cost_withheld_until_state_and_order_verified"));continue;}
            r.conversionAudit.append(QJsonObject{{k("augment_id"),id},{k("observed_stacks"),c.counters.value(stack)},
                {k("raw_crown_movement_factor"),hextech40CrownMovement(*e,c)},{k("combat_state_observed"),true},
                {k("movement_order"),c.conditions.contains(k("verified_hextech40_3132_ms_before_softcap"))?k("before_softcap"):k("after_softcap")}});
        }
    }
    return true;
}
bool executeHextech40Conversion(const CombatDatabase *db,const CombatEntry &e,const CombatContext &c,
    const QJsonObject &obs,MechanicLedger &ledger,QStringList *why){
    if(!hasHextech40Conversion(e))return fail(why,"conversion_review_identity_missing");
    if(!db||obs.value(k("source"))!=k("augment")||obs.value(k("id")).toInt()!=e.id||!ledger.augments.contains(e.id))return fail(why,"conversion_source_not_owned");
    const auto f=obs.value(k("facts")).toObject();const auto event=obs.value(k("event")).toString();
    if(event!=contract(e).value(k("event")).toString())return fail(why,"conversion_event_not_supported");
    const auto eventId=obs.value(k("event_id")).toString();
    if(eventId.isEmpty()||eventId.size()>512||!finite(obs.value(k("time")))||obs.value(k("time")).toDouble()<ledger.lastTime)return fail(why,"conversion_event_identity_or_time_invalid");
    const auto occurrence=k("hextech40_conversion:")+eventId;
    if(ledger.consumed.contains(occurrence))return fail(why,"conversion_duplicate_event");
    auto staged=ledger;const double at=obs.value(k("time")).toDouble();
    if(e.id==3018){
        if(f.value(k("verified_conversion_scope"))!=QJsonValue(true)||f.value(k("verified_source_eligibility"))!=QJsonValue(true))return fail(why,"conversion_source_scope_unverified");
        if(!finite(f.value(k("converted_bonus_as")))||!finite(f.value(k("converted_as_percentage_points")))||
           !finite(f.value(k("conversion_remaining")),1e-6,3600))return fail(why,"conversion_amount_or_lifetime_invalid");
        const double debit=f.value(k("converted_bonus_as")).toDouble(),points=f.value(k("converted_as_percentage_points")).toDouble();
        if(std::abs(points-100*debit)>1e-6)return fail(why,"conversion_fraction_percentage_points_mismatch");
        const auto ids=f.value(k("source_item_ids")).toArray();QSet<int> seen;double bucket=0;
        if(ids.isEmpty()||ids.size()>6)return fail(why,"conversion_owned_source_items_required");
        for(const auto &id:ids){const int item=id.toInt();if(!finite(id,1,1e7)||item!=id.toDouble()||seen.contains(item)||!ledger.inventory.contains(item)||!db->items.contains(item))return fail(why,"conversion_source_item_not_owned");
            seen.insert(item);const auto as=db->items[item].stats[Stat::BonusAs];if(as<=0)return fail(why,"conversion_source_item_has_no_fixed_as");bucket+=as;}
        if(ledger.augments.contains(3058)||c.championId==202||!c.formStatTransforms.isEmpty())return fail(why,"conversion_feedback_order_unreviewed");
        // Only this named owned-item subset is eligible. No aggregate visible
        // AS fallback: temporary 3078 and all other sources remain unreviewed.
        if(debit>bucket*contract(e).value(k("fraction")).toDouble()+1e-6)return fail(why,"conversion_as_debit_exceeds_owned_source_bucket");
        for(const auto &v:staged.effects){const auto o=v.toObject();if(o.value(k("conversion_pair"))==k("3018")&&at<o.value(k("expires")).toDouble())return fail(why,"conversion_active_pair_refresh_unreviewed");}
        const double end=at+f.value(k("conversion_remaining")).toDouble();
        for(const auto &pair:QList<QPair<QString,double>>{{k("bonus_as"),-debit},{k("crit_damage"),points*contract(e).value(k("ratio")).toDouble()}}){
            staged.effects.append(QJsonObject{{k("op"),k("buff")},{k("key"),pair.first},{k("value"),pair.second},{k("target"),k("self")},
                {k("source"),k("augment:3018:")},{k("start"),at},{k("expires"),end},{k("conversion_pair"),k("3018")},{k("source_item_ids"),ids}});
        }
    }else if(e.id==3132){
        if(!finite(f.value(k("observed_integer_stacks")))||!integer(f.value(k("observed_integer_stacks")).toDouble())||
           !f.value(k("in_combat")).isBool()||f.value(k("verified_combat_state"))!=QJsonValue(true))return fail(why,"crown_observed_integer_stacks_and_combat_state_required");
        const auto order=f.value(k("movement_softcap_order")).toString();
        if(f.value(k("in_combat")).toBool()&&order!=k("before_softcap")&&order!=k("after_softcap"))return fail(why,"crown_movement_softcap_order_unverified");
        staged.values.remove(k("augment:3132:state_invalidated"));
        staged.values[k("augment:3132:observed_state")]=f;
    }else return fail(why,"conversion_event_not_supported");
    staged.consumed.insert(occurrence);staged.lastTime=at;
    staged.decisions.append(QJsonObject{{k("id"),e.id},{k("event_id"),eventId},{k("time"),at},{k("applied"),true},{k("consumer"),k("hextech40_conversion_v1")}});
    ledger=std::move(staged);return true;
}
void projectHextech40ConversionState(const MechanicLedger &l,CombatContext &c){
    if(l.values.value(k("augment:3132:state_invalidated")).toBool()){
        c.counters.remove(k("augment:3132:stacks"));c.counters.remove(k("augment:3132:in_combat"));
        for(const auto *name:{"verified_hextech40_3132_combat_state","verified_hextech40_3132_ms_before_softcap","verified_hextech40_3132_ms_after_softcap"})c.conditions.remove(k(name));
        return;
    }
    const auto f=l.values.value(k("augment:3132:observed_state")).toObject();if(f.isEmpty()||!l.augments.contains(3132))return;
    c.counters[k("augment:3132:stacks")]=f.value(k("observed_integer_stacks")).toDouble();c.counters[k("augment:3132:in_combat")]=f.value(k("in_combat")).toBool()?1:0;
    c.conditions.insert(k("verified_hextech40_3132_combat_state"));c.conditions.remove(k("verified_hextech40_3132_ms_before_softcap"));c.conditions.remove(k("verified_hextech40_3132_ms_after_softcap"));
    const auto order=f.value(k("movement_softcap_order")).toString();if(order==k("before_softcap"))c.conditions.insert(k("verified_hextech40_3132_ms_before_softcap"));if(order==k("after_softcap"))c.conditions.insert(k("verified_hextech40_3132_ms_after_softcap"));
}
bool hextech40ConversionLocksItem(const MechanicLedger &l,int item,double at){
    for(const auto &v:l.effects){const auto o=v.toObject();if(o.value(k("conversion_pair"))==k("3018")&&at<o.value(k("expires")).toDouble()&&o.value(k("source_item_ids")).toArray().contains(item))return true;}return false;
}
