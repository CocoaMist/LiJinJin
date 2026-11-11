#include "augment_advisory.h"
#include "augment_context_advice.h"
#include "analysis.h"
#include "coach_knowledge.h"
#include "coach_recommendation_quality.h"
#include "combat.h"
#include "external_strategy_reference.h"
#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace {
constexpr int minimumPublicReferenceSamples=100;
QString k(const char *value){return QString::fromLatin1(value);}
QString label(const Catalog &catalog,int id){
    const auto card=catalog.augments.value(id);
    return card.value(k("name_zh_cn")).toString(card.value(k("name")).toString(QString::number(id)));
}
QJsonArray ids(const QList<int> &values){QJsonArray out;for(int id:values)out.append(id);return out;}
bool inventoryVerified(const Catalog &catalog,const GameState &state){
    if(state.owned.size()>6)return false;for(int id:state.owned)if(!catalog.items.contains(id))return false;
    QJsonObject self;int count=0;for(const auto &v:state.players)if(v.toObject().value(k("self"))==QJsonValue(true)){self=v.toObject();++count;}
    if(count!=1||self.value(k("champion_id")).toInt()!=state.championId||self.value(k("items_complete"))!=QJsonValue(true)||!self.value(k("items")).isArray())return false;
    QList<int> observed;for(const auto &v:self.value(k("items")).toArray()){
        if(!v.isDouble()||v.toDouble()!=v.toInt()||v.toInt()<=0)return false;observed.append(v.toInt());
    }
    auto owned=state.owned;std::sort(owned.begin(),owned.end());std::sort(observed.begin(),observed.end());return owned==observed;
}
QStringList inventoryScaling(const Catalog &catalog,const CombatStats &owned,int id){
    if(!catalog.combat||!catalog.combat->augments.contains(id))return {};
    QSet<int> dependencies;
    std::function<void(const QJsonValue &,int)> walk=[&](const QJsonValue &v,int depth){
        if(depth>24)return;if(v.isArray()){for(const auto &child:v.toArray())walk(child,depth+1);return;}
        if(!v.isObject())return;const auto object=v.toObject();
        if(object.value(k("op"))==k("stat")&&object.value(k("part")).toInt()!=1)
            if(const auto stat=combatStat(object.value(k("stat")).toString()))dependencies.insert(int(*stat));
        for(auto it=object.constBegin();it!=object.constEnd();++it)walk(it.value(),depth+1);
    };
    walk(catalog.combat->augments.value(id).calculations,0);
    if(dependencies.isEmpty())return {};
    static const QList<QPair<Stat,QString>> names{{Stat::Armor,QStringLiteral("护甲")},{Stat::Mr,QStringLiteral("魔抗")},{Stat::Hp,QStringLiteral("生命值")},
        {Stat::Ad,QStringLiteral("攻击力")},{Stat::Ap,QStringLiteral("法强")},{Stat::Mana,QStringLiteral("法力值")},{Stat::BonusAs,QStringLiteral("攻速")},
        {Stat::As,QStringLiteral("固定攻速")},{Stat::Crit,QStringLiteral("暴击几率")},{Stat::CritDamage,QStringLiteral("暴击伤害")},
        {Stat::Haste,QStringLiteral("技能急速")},{Stat::Ms,QStringLiteral("移动速度")},{Stat::MsPercent,QStringLiteral("百分比移速")},
        {Stat::MagicPen,QStringLiteral("固定法穿")},{Stat::MagicPenPercent,QStringLiteral("百分比法穿")},
        {Stat::ArmorPen,QStringLiteral("固定护甲穿透")},{Stat::ArmorPenPercent,QStringLiteral("百分比护甲穿透")},{Stat::Lethality,QStringLiteral("穿甲")},
        {Stat::Lifesteal,QStringLiteral("生命偷取")},{Stat::Omnivamp,QStringLiteral("全能吸血")},{Stat::HealShield,QStringLiteral("治疗护盾强度")},
        {Stat::Tenacity,QStringLiteral("韧性")},{Stat::SlowResist,QStringLiteral("减速抗性")},{Stat::Range,QStringLiteral("攻击距离")},
        {Stat::HpRegen,QStringLiteral("生命回复")},{Stat::ManaRegen,QStringLiteral("法力回复")},
        {Stat::HpRegenPercent,QStringLiteral("百分比生命回复")},{Stat::ManaRegenPercent,QStringLiteral("百分比法力回复")},
        {Stat::CooldownReduction,QStringLiteral("冷却缩减")},{Stat::AsMultiplier,QStringLiteral("攻速倍率")},{Stat::ItemHaste,QStringLiteral("装备急速")},
        {Stat::QHaste,QStringLiteral("Q技能急速")},{Stat::WHaste,QStringLiteral("W技能急速")},{Stat::EHaste,QStringLiteral("E技能急速")},{Stat::RHaste,QStringLiteral("终极技能急速")},
        {Stat::SpellAmp,QStringLiteral("技能增幅")},{Stat::DamageAmp,QStringLiteral("伤害增幅")},{Stat::Shield,QStringLiteral("护盾")},
        {Stat::Shred,QStringLiteral("双抗削减")},{Stat::ArmorShred,QStringLiteral("护甲削减")},{Stat::MagicShred,QStringLiteral("魔抗削减")},{Stat::MagicAmp,QStringLiteral("魔法伤害增幅")}};
    QStringList out;for(const auto &pair:names)if(dependencies.contains(int(pair.first))&&std::isfinite(owned[pair.first])&&owned[pair.first]>0)out.append(pair.second);return out;
}
struct Cohort {QMap<int,QJsonObject> rows;bool conflicting=false;QString scope,region,url;QDateTime retrieved;};
QString ordinaryRoundScope(int round){
    return round>=1&&round<=4?k("champion_augment_slot_")+QString::number(round):QString();
}
int scopePriority(const QString &scope,const QString &roundScope){
    if(!roundScope.isEmpty()&&scope==roundScope)return 2;
    return scope==k("champion_augment_reference")?1:0;
}
bool identicalStatistics(const QJsonObject &left,const QJsonObject &right){
    const auto a=left.value(k("data")).toObject(),b=right.value(k("data")).toObject();
    for(const auto *field:{"provider_augment_id","provider_augment_id_namespace","provider_rarity","performance","popular","sample_size","plays","wins","win_rate"})
        if(a.value(k(field))!=b.value(k(field)))return false;
    return true;
}
bool uniqueProviderIds(const Cohort &cohort){
    QSet<int> ids;
    QSet<QString> namespaces;
    for(const auto &row:cohort.rows){
        const auto data=row.value(k("data")).toObject();
        const auto providerNamespace=data.value(k("provider_augment_id_namespace"));
        if(!providerNamespace.isUndefined()&&!providerNamespace.isNull()&&providerNamespace.toString().trimmed().isEmpty())return false;
        namespaces.insert(providerNamespace.toString());
        ids.insert(data.value(k("provider_augment_id")).toInt());
    }
    return namespaces.size()==1&&ids.size()==cohort.rows.size();
}
bool positivePopularity(const QJsonObject &row){
    const auto value=row.value(k("data")).toObject().value(k("popular"));
    return value.isDouble()&&std::isfinite(value.toDouble())&&value.toDouble()>0.;
}
QJsonObject sampleEligibility(const QJsonObject &row){
    const auto data=row.value(k("data")).toObject();
    const auto n=data.value(k("sample_size")),plays=data.value(k("plays"));
    const auto count=[](QJsonValue v){return v.isDouble()&&std::isfinite(v.toDouble())&&v.toDouble()>=0
        &&v.toDouble()<=9007199254740991.&&v.toDouble()==std::floor(v.toDouble());};
    QString reason=k("eligible");
    if(!count(n)||!count(plays))reason=k("sample_counts_unpublished_or_invalid");
    else if(n!=plays)reason=k("sample_counts_conflict");
    else if(n.toDouble()<minimumPublicReferenceSamples)reason=k("below_minimum_sample_count");
    return {{k("eligible"),reason==k("eligible")},{k("reason"),reason},{k("minimum_samples"),minimumPublicReferenceSamples},
        {k("policy"),k("public_reference_samples_v1")},{k("accuracy_established"),false}};
}
bool comparableSamples(const QJsonObject &row){return sampleEligibility(row).value(k("eligible")).toBool();}
bool usableRecord(const QJsonObject &row,const Catalog &catalog,int id,int champion,const QString &roundScope){
    const auto data=row.value(k("data")).toObject();const auto performance=data.value(k("performance"));
    const auto scope=data.value(k("stat_scope")).toString();
    return row.value(k("kind"))==k("augment_stat")&&row.value(k("patch"))==catalog.patch&&row.value(k("mode"))==k("KIWI")
        &&row.value(k("source_kind"))==k("statistics")&&row.value(k("evidence_level"))==k("full_body")
        &&row.value(k("source_sha256")).toString().size()==64&&!row.value(k("source_url")).toString().isEmpty()&&!row.value(k("region")).toString().isEmpty()
        &&data.value(k("augment_id")).toInt()==id&&data.value(k("identity_mapping_basis"))==k("exact_versioned_catalog_key_with_published_provider_name")
        &&data.value(k("metric_semantics"))==k("provider_performance_and_popularity_not_win_rate")
        &&(scope==k("champion_augment_reference")||scope==k("global_augment")||(!roundScope.isEmpty()&&scope==roundScope))
        &&(scope!=k("global_augment")?row.value(k("champion_id")).toInt()==champion:row.value(k("champion_id")).isNull())
        &&performance.isDouble()&&std::isfinite(performance.toDouble());
}
QJsonObject sourceRecord(const QJsonObject &row){
    QJsonObject out;for(const auto &key:{"id","champion_id","patch","mode","region","source_url","source_sha256","retrieved_at"})if(row.contains(k(key)))out[k(key)]=row.value(k(key));
    const auto data=row.value(k("data")).toObject();
    for(const auto &key:{"augment_id","stat_scope","performance","popular","metric_semantics","identity_mapping_basis","wins","plays","sample_size","win_rate"})if(data.contains(k(key)))out[k(key)]=data.value(k(key));
    out[k("sample_eligibility")]=sampleEligibility(row);
    return out;
}
}

QJsonObject buildAugmentAdvisory(const Catalog &catalog,const GameState &state){
    return buildAugmentAdvisory(catalog,state,nullptr);
}

QJsonObject buildAugmentAdvisory(const Catalog &catalog,const GameState &state,QJsonObject *offeredReferences){
    if(offeredReferences)*offeredReferences={};
    QJsonObject out{{k("available"),false},{k("preferred_augment_id"),0},{k("numeric_ranking"),false},{k("numeric_gain_available"),false},
        {k("ranking_available"),false},{k("unknown_is_zero"),false},{k("reference_only"),true}};
    QJsonObject openingPrior{{k("available"),false},{k("opening_selection_confirmed"),false},{k("slot_specific"),false},
        {k("reference_only"),true},{k("preferred_augment_id"),0},{k("source_records"),QJsonArray{}},
        {k("route_anchor_candidate"),QJsonObject{}},{k("route_anchor_candidates"),QJsonArray{}},
        {k("reason_code"),k("opening_selection_not_confirmed")}};
    out[k("opening_reference_prior")]=openingPrior;
    auto reject=[&](const char *why){out[k("reason_code")]=k(why);return out;};
    if(!state.fresh()||!modelRulesUsable(catalog,state)||state.gameMode!=k("KIWI")||(state.queueId>0&&state.queueId!=2400)
        ||!catalog.champions.contains(state.championId))return reject("fresh_same_patch_kiwi_required");
    QSet<int> offers;QSet<QString> rarities;
    for(int id:state.offers){if(!catalog.ordinaryOfferEligible(id)||offers.contains(id))return reject("three_unique_current_offers_required");offers.insert(id);rarities.insert(catalog.augments.value(id).value(k("rarity")).toString());}
    if(state.offers.size()!=3)return reject("three_unique_current_offers_required");
    static const QSet<QString> knownRarities{k("kSilver"),k("kGold"),k("kPrismatic")};
    const bool comparableRarity=rarities.size()==1&&knownRarities.contains(*rarities.cbegin());
    const auto readiness=coachPurchaseInputReadiness(catalog,state);const bool complete=readiness.value(k("ready")).toBool();
    const auto observed=readiness.value(k("augment_observation")).toObject().value(k("observed_ids"));
    const bool opening=complete&&state.selected.isEmpty()&&observed.isArray()&&observed.toArray().isEmpty();
    // Only the existing verified opening is bound to an ordinary round.
    // Selected-card count does not identify any subsequent ordinary offer.
    const int ordinaryRound=opening?1:0;
    const auto roundScope=ordinaryRoundScope(ordinaryRound);
    openingPrior[k("opening_selection_confirmed")]=opening;
    if(opening){openingPrior[k("basis")]=k("complete_verified_empty_selected_augments");
        openingPrior[k("reason_code")]=k("champion_specific_all_offer_reference_unavailable");}
    const bool knownInventory=inventoryVerified(catalog,state);auto safeState=state;if(!knownInventory)safeState.owned.clear();
    CombatStats ownedStats;
    if(knownInventory&&catalog.combat)
        for(int item:state.owned){
            const auto it=catalog.combat->items.constFind(item);
            if(it!=catalog.combat->items.cend())ownedStats+=it->stats;
        }
    QJsonArray unknowns;if(!complete)unknowns.append(k("selected_augments_incomplete"));
    if(!knownInventory)unknowns.append(k("inventory_not_verified_for_synergy"));
    if(!comparableRarity)unknowns.append(k("offer_rarity_not_comparable"));
    unknowns.append(k("actual_trigger_counts_unobserved"));
    QMap<int,QJsonObject> options;QMap<QString,Cohort> cohorts;
    QSet<QString> incompleteBatches;
    const auto publicReferences=externalAugmentStrategyReferences(catalog,state,state.offers,&incompleteBatches);
    if(offeredReferences)*offeredReferences=publicReferences;
    for(int id:state.offers){
        const auto mechanism=augmentMechanismReference(catalog,safeState,id);auto condition=mechanism.value(k("decision_hint")).toString(mechanism.value(k("condition")).toString());
        if(id==3049&&mechanism.value(k("component_synergy")).toObject().value(k("raw_damage_delta")).isDouble())condition=QStringLiteral("继续走护甲路线；施法后接普攻");
        if(condition.isEmpty())condition=QStringLiteral("先核对原卡绑定技能与触发条件，再按对应打法选择");
        const auto scaling=knownInventory?inventoryScaling(catalog,ownedStats,id):QStringList{};
        QJsonObject option{{k("augment_id"),id},{k("name"),label(catalog,id)},{k("condition"),condition},{k("reason"),mechanism.value(k("condition"))},
            {k("inventory_scaling_attributes"),QJsonArray::fromStringList(scaling)},{k("numeric_ranking"),false},{k("recommended"),false}};
        if(!scaling.isEmpty())option[k("inventory_synergy_reason")]=QStringLiteral("现有装备提供%1，与这张卡的公式联动").arg(scaling.join(QStringLiteral("、")));
        if(mechanism.contains(k("component_synergy")))option[k("component_synergy")]=mechanism.value(k("component_synergy"));
        const auto contextual=buildAugmentContextAdvice(catalog,state,id);
        if(contextual.value(k("available")).toBool()){
            option[k("context_advice")]=contextual;
            const auto concise=contextual.value(k("concise_condition")).toString();
            if(!concise.isEmpty())option[k("condition")]=concise;
        }
        options[id]=option;
        const auto reference=publicReferences.value(QString::number(id)).toObject();
        for(const auto &value:reference.value(k("records")).toArray()){
            const auto row=value.toObject();if(!usableRecord(row,catalog,id,state.championId,roundScope))continue;
            const auto key=augmentStatisticsBatchKey(row);if(incompleteBatches.contains(key))continue;
            auto &cohort=cohorts[key];
            auto previous=cohort.rows.find(id);
            if(previous!=cohort.rows.end()){
                if(!identicalStatistics(*previous,row))cohort.conflicting=true;
                else if(row.value(k("id")).toString()<previous->value(k("id")).toString())*previous=row;
                continue;
            }
            cohort.rows[id]=row;cohort.scope=row.value(k("data")).toObject().value(k("stat_scope")).toString();cohort.region=row.value(k("region")).toString();
            cohort.url=row.value(k("source_url")).toString();
        }
    }
    QList<Cohort> references;
    for(auto &cohort:cohorts){
        if(cohort.rows.size()!=3||cohort.conflicting||!uniqueProviderIds(cohort))continue;
        for(const auto &row:cohort.rows)
            cohort.retrieved=std::max(cohort.retrieved,QDateTime::fromString(row.value(k("retrieved_at")).toString(),Qt::ISODate));
        references.append(cohort);
    }
    std::stable_sort(references.begin(),references.end(),[&](const Cohort &a,const Cohort &b){
        if(scopePriority(a.scope,roundScope)!=scopePriority(b.scope,roundScope))return scopePriority(a.scope,roundScope)>scopePriority(b.scope,roundScope);
        const bool ag=a.region==k("global"),bg=b.region==k("global");if(ag!=bg)return ag;
        if(a.retrieved!=b.retrieved)return a.retrieved>b.retrieved;return a.url<b.url;
    });
    out[k("available")]=true;out[k("offer_ids")]=ids(state.offers);out[k("selected_list_complete")]=complete;
    out[k("confirmed_selected_ids")]=complete?readiness.value(k("augment_observation")).toObject().value(k("observed_ids")):QJsonValue(QJsonValue::Null);
    out[k("known_model_selected_ids")]=ids(state.selected);out[k("inventory_verified")]=knownInventory;
    out[k("kind")]=k("conditional_options");out[k("confidence")]=k("conditional");out[k("source_records")]=QJsonArray{};
    const auto qualifying=std::find_if(references.cbegin(),references.cend(),[&](const Cohort &cohort){
        return comparableRarity&&cohort.scope!=k("global_augment")&&std::all_of(cohort.rows.cbegin(),cohort.rows.cend(),positivePopularity)
            &&std::all_of(cohort.rows.cbegin(),cohort.rows.cend(),comparableSamples);
    });
    const auto reference=qualifying!=references.cend()?qualifying:references.cbegin();
    const auto targetScope=roundScope.isEmpty()?k("champion_augment_reference"):roundScope;
    QJsonObject comparison{{k("eligible"),qualifying!=references.cend()},
        {k("policy"),k("public_reference_samples_v1")},{k("minimum_samples_per_offer"),minimumPublicReferenceSamples},
        {k("metric"),k("provider_performance_not_win_rate")},{k("accuracy_established"),false},
        {k("uncertainty_available"),false},{k("target_scope"),targetScope},
        {k("target_ordinary_round"),ordinaryRound?QJsonValue(ordinaryRound):QJsonValue(QJsonValue::Null)},
        {k("ordinary_round_basis"),opening?k("complete_verified_empty_selected_augments"):k("unobserved")},
        {k("scope"),reference!=references.cend()?QJsonValue(reference->scope):QJsonValue(QJsonValue::Null)},
        {k("reference_ordinary_round"),reference!=references.cend()&&!roundScope.isEmpty()&&reference->scope==roundScope?QJsonValue(ordinaryRound):QJsonValue(QJsonValue::Null)},
        {k("used_broader_scope"),reference!=references.cend()&&scopePriority(reference->scope,roundScope)<scopePriority(targetScope,roundScope)},
        {k("complete_cohort"),reference!=references.cend()},{k("leader_ids"),QJsonArray{}}};
    QJsonArray sources;
    if(reference!=references.cend()){
        bool samplesMissing=false;
        for(int id:state.offers){
            const auto row=reference->rows.value(id);
            const auto data=row.value(k("data")).toObject();
            sources.append(sourceRecord(row));
            samplesMissing|=!data.value(k("sample_size")).isDouble()||!data.value(k("plays")).isDouble();
        }
        out[k("source_records")]=sources;out[k("public_reference_scope")]=reference->scope;out[k("public_reference_region")]=reference->region;
        out[k("public_reference_metric_semantics")]=k("provider_performance_and_popularity_not_win_rate");
        out[k("public_reference_causal_effect_established")]=false;
        if(samplesMissing)unknowns.append(k("public_sample_sizes_unpublished"));
        if(!std::all_of(reference->rows.cbegin(),reference->rows.cend(),comparableSamples))
            unknowns.append(k("public_reference_sample_gate_not_met"));
        if(reference->scope==k("global_augment"))unknowns.append(k("public_reference_not_champion_specific"));
        if(!std::all_of(reference->rows.cbegin(),reference->rows.cend(),positivePopularity))
            unknowns.append(k("public_reference_popularity_zero_or_unpublished"));
    }
    if(qualifying!=references.cend()){
        const auto &cohort=*qualifying;
        const auto performance=[&](int id){return cohort.rows.value(id).value(k("data")).toObject().value(k("performance")).toDouble();};
        double best=-std::numeric_limits<double>::infinity();
        for(int id:state.offers)best=std::max(best,performance(id));
        QList<int> winners;
        for(int id:state.offers)if(std::abs(performance(id)-best)<=1e-9)winners.append(id);
        comparison[k("leader_ids")]=ids(winners);
        comparison[k("status")]=winners.size()==1?k("provider_preference"):k("close");
        comparison[k("detail")]=winners.size()==1?QStringLiteral("同一份公开资料给出评分倾向，现有数据无法估计评分误差")
            :QStringLiteral("公开评分的最高分并列，按本局触发条件选择");
        if(winners.size()>1)comparison[k("badge")]=QStringLiteral("资料接近");
        if(opening){
            const bool slotSpecific=cohort.scope==roundScope;
            openingPrior[k("available")]=true;openingPrior[k("reason_code")]=k("source_bound_opening_reference");
            openingPrior[k("stat_scope")]=cohort.scope;openingPrior[k("slot_specific")]=slotSpecific;
            openingPrior[k("metric_semantics")]=k("provider_performance_and_popularity_not_win_rate");
            openingPrior[k("causal_effect_established")]=false;
            openingPrior[k("offer_ids")]=ids(state.offers);openingPrior[k("preferred_augment_ids")]=ids(winners);
            openingPrior[k("source_records")]=sources;openingPrior[k("region")]=cohort.region;
            QJsonArray anchors;for(int id:winners)anchors.append(QJsonObject{{k("augment_id"),id},{k("active"),false},
                {k("activation"),k("only_after_matching_augment_is_observed_selected")},{k("locks_future_build"),false},
                {k("condition"),options.value(id).value(k("condition"))},{k("continuation_input"),k("effectiveSelectedAugments")}});
            openingPrior[k("route_anchor_candidates")]=anchors;
            if(winners.size()==1){openingPrior[k("preferred_augment_id")]=winners.first();openingPrior[k("route_anchor_candidate")]=anchors.first();}
            if(!slotSpecific)unknowns.append(k("opening_prior_uses_champion_aggregate_not_slot_1_statistics"));
        }
        if(winners.size()==1){
            const int preferred=winners.first();auto option=options.value(preferred);option[k("recommended")]=true;options[preferred]=option;
            QString reason=option.value(k("inventory_synergy_reason")).toString();
            if(reason.isEmpty())reason=option.value(k("reason")).toString();
            if(!reason.isEmpty())reason+=QStringLiteral("；");
            reason+=QStringLiteral("同英雄公开评分偏向这张牌，仅反映统计关联");
            out[k("kind")]=k("public_reference_preference");out[k("preferred_augment_id")]=preferred;out[k("confidence")]=k("low");
            out[k("reason")]=reason;out[k("condition")]=option.value(k("condition"));out[k("label")]=QStringLiteral("资料倾向");
            out[k("summary")]=QStringLiteral("同英雄资料倾向%1；%2").arg(label(catalog,preferred),option.value(k("condition")).toString());
        }else{
            out[k("preferred_augment_ids")]=ids(winners);out[k("reason")]=QStringLiteral("同版本公开参考没有区分这些牌，按下面的条件选择");unknowns.append(k("public_reference_tie"));
        }
    }else{
        QString reason=QStringLiteral("当前三张缺少同一口径的英雄统计，按对应打法选择");
        if(!comparableRarity)reason=QStringLiteral("当前三张的品质无法用同一口径比较，按对应打法选择");
        else if(reference!=references.cend()){
            if(reference->scope==k("global_augment"))reason=QStringLiteral("当前只有全英雄统计，按对应打法选择");
            else if(!std::all_of(reference->rows.cbegin(),reference->rows.cend(),comparableSamples))
                reason=QStringLiteral("公开资料的样本数不足、未公布或不一致，按对应打法选择");
            else if(!std::all_of(reference->rows.cbegin(),reference->rows.cend(),positivePopularity))
                reason=QStringLiteral("至少一张牌缺少可比较的热度数据，按对应打法选择");
        }
        comparison[k("status")]=k("unknown");comparison[k("badge")]=QStringLiteral("资料不足");comparison[k("detail")]=reason;
        out[k("reason")]=reason;unknowns.append(k("same_source_all_offer_comparison_unavailable"));
        // This fallback has one explicit goal: continue a verified existing
        // equipment direction. It is not a combat ordering of the three cards,
        // and never overrides a complete public cohort (including a tie).
        int equipmentLinkedOptions=0;for(int id:state.offers)if(!options.value(id).value(k("inventory_scaling_attributes")).toArray().isEmpty())++equipmentLinkedOptions;
        auto armor=options.value(3049);const auto contextual=armor.value(k("context_advice")).toObject();
        if(knownInventory&&comparableRarity&&equipmentLinkedOptions==1
            &&contextual.value(k("verified_inventory_formula_fit"))==QJsonValue(true)){
            armor[k("recommended")]=true;options[3049]=armor;
            out[k("kind")]=k("conditional_equipment_continuation");out[k("preferred_augment_id")]=3049;
            out[k("label")]=QStringLiteral("条件建议");out[k("confidence")]=k("conditional");
            out[k("preference_goal")]=k("continue_verified_existing_equipment_direction");
            out[k("reason")]=QStringLiteral("现有护甲与装甲冲击有已核对的公式联动；沿用现装可考虑，仍需施法后接普攻");
            out[k("condition")]=armor.value(k("condition"));out[k("context_sources")]=contextual.value(k("sources"));
            out[k("summary")]=QStringLiteral("条件建议选%1；%2").arg(label(catalog,3049),armor.value(k("condition")).toString());
            unknowns.append(k("conditional_equipment_preference_not_overall_optimum"));
            unknowns.append(k("cast_then_attack_delivery_unobserved"));
        }
    }
    QJsonArray alternatives;for(int id:state.offers)alternatives.append(options.value(id));out[k("alternatives")]=alternatives;out[k("unknowns")]=unknowns;
    out[k("public_reference_comparison")]=comparison;
    out[k("opening_reference_prior")]=openingPrior;
    return out;
}
