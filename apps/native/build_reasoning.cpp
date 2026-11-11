#include "build_reasoning.h"
#include "analysis.h"
#include "combat.h"
#include "role_guidance.h"
#include "external_strategy_reference.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>

namespace {
QString k(const char *s){return QString::fromLatin1(s);}
QString hash(const QByteArray &bytes){return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
QByteArray readBounded(const QString &path,qint64 maximum){QFile file(path);if(file.size()<=0||file.size()>maximum||!file.open(QIODevice::ReadOnly))return {};return file.read(maximum+1);}
bool flags(const QJsonObject &o){return o.value(k("numeric_authority"))==QJsonValue(false)&&o.value(k("source_condition_verified"))==QJsonValue(false)
    &&o.value(k("reference_only"))==QJsonValue(true)&&o.value(k("execution_eligible"))==QJsonValue(false)&&o.value(k("game_verified"))==QJsonValue(false);}
QJsonObject reasoningBundle(const Catalog &c){
    if(c.root.isEmpty()||!c.combat)return {};
    const auto pointerBytes=readBounded(QDir(c.root).filePath(k("data/active-hero-equipment-reasoning.v1.json")),16384);
    const auto pointer=QJsonDocument::fromJson(pointerBytes).object();const auto revision=pointer.value(k("bundle_sha256")).toString();
    if(pointer.value(k("schema_version"))!=QJsonValue(1)||pointer.value(k("reasoning_contract_version"))!=QJsonValue(1)
        ||pointer.value(k("scope"))!=k("hero_equipment_reasoning_reference")||pointer.value(k("patch"))!=c.patch
        ||pointer.value(k("combat_revision"))!=c.combat->revision||pointer.value(k("content_version"))!=c.runtimeManifest.value(k("content_version"))
        ||!flags(pointer)||!QRegularExpression(k("\\A[0-9a-f]{64}\\z")).match(revision).hasMatch()
        ||pointer.value(k("revision"))!=revision||pointer.value(k("bundle_path"))!=k("data/hero-equipment-reasoning-revisions/")+revision+k(".json"))return {};
    const auto root=QFileInfo(c.root).canonicalFilePath(),path=QFileInfo(QDir(c.root).filePath(pointer.value(k("bundle_path")).toString())).canonicalFilePath();
    if(root.isEmpty()||path.isEmpty()||!path.startsWith(root+QChar(u'/'),Qt::CaseInsensitive))return {};
    const auto bytes=readBounded(path,24*1024*1024);if(bytes.isEmpty()||hash(bytes)!=revision)return {};
    static QMutex mutex;static QString previous;static QJsonObject saved;
    const auto cacheKey=root+k("|")+hash(pointerBytes)+k("|")+c.combat->revision;
    QMutexLocker lock(&mutex);if(previous==cacheKey)return saved;previous=cacheKey;saved={};
    auto bundle=QJsonDocument::fromJson(bytes).object();
    for(const auto &name:{k("schema_version"),k("reasoning_contract_version"),k("scope"),k("patch"),k("combat_revision"),k("content_version")})if(bundle.value(name)!=pointer.value(name))return {};
    if(!flags(bundle))return {};
    const auto heroes=bundle.value(k("champions")).toObject(),items=bundle.value(k("items")).toObject();
    if(heroes.size()!=c.champions.size()||heroes.size()!=173||items.size()!=c.combat->items.size())return {};
    for(auto it=c.champions.cbegin();it!=c.champions.cend();++it)if(heroes.value(QString::number(it.key())).toObject().value(k("id"))!=QJsonValue(it.key()))return {};
    for(auto it=c.combat->items.cbegin();it!=c.combat->items.cend();++it)if(items.value(QString::number(it.key())).toObject().value(k("id"))!=QJsonValue(it.key()))return {};
    bundle[k("revision")]=revision;saved=bundle;return saved;
}
QJsonArray compactReferences(const QJsonValue &value){
    QJsonArray out;for(const auto &raw:value.toArray()){if(out.size()>=3)break;QJsonObject row;const auto ref=raw.toObject();
        for(const auto &field:{k("path"),k("sha256"),k("json_pointer")})if(ref.contains(field))row[field]=ref.value(field);if(!row.isEmpty())out.append(row);}
    return out;
}
QJsonArray firstUnits(const QJsonValue &value,int maximum){QJsonArray out;for(const auto &v:value.toArray()){if(out.size()>=maximum)break;out.append(v);}return out;}
QJsonObject compactItemReason(const QJsonObject &source,bool brief){
    QJsonObject out;for(const auto &field:{k("item_id"),k("name"),k("available"),k("profile_key"),k("availability"),k("requirements"),k("numeric_authority"),k("purchase_legal_from_reference")})out[field]=source.value(field);
    out[k("details_in_revision_bound_bundle")]=true;out[k("item_json_pointer")]=k("/items/")+QString::number(source.value(k("item_id")).toInt());
    out[k("unknowns")]=firstUnits(source.value(k("unknowns")),brief?1:2);out[k("unknowns_total")]=source.value(k("unknowns")).toArray().size();
    out[k("source_refs")]=firstUnits(source.value(k("source_refs")),1);
    const auto relation=source.value(k("hero_relationship")).toObject();
    if(!relation.isEmpty()){
        QJsonObject paired{{k("reason"),firstUnits(relation.value(k("reason")),1)},{k("conditions"),relation.value(k("conditions"))},
            {k("alternative_when"),firstUnits(relation.value(k("alternative_when")),1)},{k("certainty"),relation.value(k("certainty"))},
            {k("numeric_authority"),false},{k("source_refs"),firstUnits(relation.value(k("source_refs")),1)}};
        out[k("hero_relationship")]=paired;
    }
    const auto interpretation=source.value(k("public_build_interpretation")).toObject();
    if(!interpretation.isEmpty()){
        QJsonObject compact{{k("analysis_json_pointer"),interpretation.value(k("analysis_json_pointer"))},{k("numeric_score_added"),false},{k("provider_reason_claimed"),false}};
        if(relation.isEmpty()){compact[k("conditions")]=interpretation.value(k("conditions"));for(const auto &kind:{k("supported_links"),k("conditional_links"),k("unknown_or_mismatch")})compact[kind]=firstUnits(interpretation.value(kind),1);}
        else if(!brief)compact[k("unknown_or_mismatch")]=firstUnits(interpretation.value(k("unknown_or_mismatch")),1);
        out[k("public_build_interpretation")]=compact;
    }
    return out;
}
QJsonObject itemReason(const Catalog &c,const QJsonObject &bundle,const QJsonObject &hero,int id){
    const auto item=bundle.value(k("items")).toObject().value(QString::number(id)).toObject();QJsonObject out{{k("item_id"),id},{k("name"),c.itemName(id)}};
    if(item.isEmpty()){out[k("available")]=false;out[k("reason")]=k("source_bound_item_analysis_unavailable");return out;}
    out[k("available")]=true;for(const auto &field:{k("profile_key"),k("effect_tags"),k("requirements"),k("stat_relevance"),k("availability"),k("unknowns")})out[field]=item.value(field);
    if(out.value(k("requirements")).toArray().isEmpty())out[k("requirements")]=QJsonArray{QStringLiteral("装备触发与受益条件尚未完成绑定，不按无条件生效处理。")};
    out[k("source_refs")]=compactReferences(item.value(k("source_refs")));
    const auto relation=hero.value(k("item_relations")).toObject().value(QString::number(id)).toObject();
    if(!relation.isEmpty()){auto bounded=relation;bounded[k("source_refs")]=compactReferences(relation.value(k("source_refs")));out[k("hero_relationship")]=bounded;}
    const auto interpreted=hero.value(k("public_build_item_interpretations")).toObject().value(QString::number(id)).toObject();
    if(!interpreted.isEmpty()){
        QJsonObject summary{{k("analysis_json_pointer"),interpreted.value(k("analysis_json_pointer"))},{k("numeric_score_added"),false},
            {k("provider_reason_claimed"),false},{k("conditions"),interpreted.value(k("conditions"))},{k("details_in_source_analysis"),true}};
        for(const auto &kind:{k("supported_links"),k("conditional_links"),k("unknown_or_mismatch")}){
            QJsonArray links;for(const auto &raw:interpreted.value(kind).toArray()){if(links.size()>=2)break;const auto source=raw.toObject();QJsonObject link;
                for(const auto &field:{k("relation"),k("reason"),k("conditions")})link[field]=source.value(field);links.append(link);}
            summary[kind]=links;
        }
        out[k("public_build_interpretation")]=summary;
    }
    out[k("numeric_authority")]=false;out[k("purchase_legal_from_reference")]=false;return out;
}
QJsonObject buildReasoning(const Catalog &c,const GameState &state,int championId,const QList<int> &itemIds,const QJsonObject &bundle){
    const auto hero=bundle.value(k("champions")).toObject().value(QString::number(championId)).toObject();
    QJsonObject out{{k("schema_version"),1},{k("available"),!hero.isEmpty()},{k("champion_id"),championId},{k("patch"),c.patch},
        {k("combat_revision"),c.combat?c.combat->revision:QString()},{k("revision"),bundle.value(k("revision"))},{k("reference_only"),true},
        {k("numeric_authority"),false},{k("observed_conditions_inferred"),false},{k("causal_win_rate_known"),false},
        {k("rationale_origin"),k("local_mechanism_analysis_of_source_build_not_provider_explanation")},
        {k("missing_current_inputs"),QJsonArray{}},{k("items"),QJsonArray{}},{k("truncated"),false}};
    QJsonArray missing;if(!state.fresh()){missing.append(k("current_inventory"));missing.append(k("current_level_and_ranks"));missing.append(k("selected_augments"));}
    if(!state.goldKnown||!state.fresh())missing.append(k("current_budget"));if(state.players.isEmpty())missing.append(k("team_and_opponents"));out[k("missing_current_inputs")]=missing;
    if(hero.isEmpty()){out[k("status")]=k("source_analysis_unavailable_or_incompatible");return out;}
    QJsonObject requirements;for(const auto &field:{k("id"),k("key"),k("name"),k("profile_key"),k("needs"),k("effect_tags"),k("requirements")})requirements[field]=hero.value(field);
    out[k("hero_requirements")]=requirements;out[k("public_reference_provenance")]=bundle.value(k("public_build_provenance"));QJsonArray reasons;QSet<int> seen;
    for(int id:itemIds){if(seen.contains(id))continue;seen.insert(id);if(reasons.size()>=20){out[k("truncated")]=true;break;}reasons.append(itemReason(c,bundle,hero,id));}
    const auto complete=reasons;out[k("items")]=reasons;
    for(bool brief:{false,true}){
        if(QJsonDocument(out).toJson(QJsonDocument::Compact).size()<=32700)break;
        reasons={};for(const auto &row:complete)reasons.append(compactItemReason(row.toObject(),brief));out[k("items")]=reasons;out[k("truncated")]=true;
        if(brief){
            // Keep each need and its conditions. Extra citations remain in
            // the verified bundle instead of displacing a reference item.
            QJsonArray needs;
            for(const auto &value:requirements.value(k("needs")).toArray()){
                auto need=value.toObject();
                need[k("source_refs")]=firstUnits(compactReferences(need.value(k("source_refs"))),1);
                needs.append(need);
            }
            requirements[k("needs")]=needs;
            out[k("hero_requirements")]=requirements;
        }
    }
    while(QJsonDocument(out).toJson(QJsonDocument::Compact).size()>32700&&!reasons.isEmpty()){reasons.removeLast();out[k("items")]=reasons;out[k("truncated")]=true;}
    out[k("items")]=reasons;out[k("status")]=k("mechanism_requirements_and_conditional_item_relations");return out;
}
}

QString heroEquipmentReasoningRevision(const Catalog &c){return reasoningBundle(c).value(k("revision")).toString();}

QJsonObject heroEquipmentBuildReasoning(const Catalog &c,const GameState &state,int championId,const QList<int> &itemIds){
    return buildReasoning(c,state,championId,itemIds,reasoningBundle(c));
}

QJsonObject protectionEquipmentReference(const Catalog &c,const GameState &state,int championId,const QString &style){
    auto subject=state;subject.championId=championId;const auto role=roleAwareRecommendation(c,subject,style);
    const auto routes=role.value(k("routes")).toArray();QJsonObject selected;
    for(const auto &raw:routes){const auto row=raw.toObject();if(row.value(k("has_ally_support_reference_semantics")).toBool()){selected=row;break;}}
    const bool observed=state.fresh()&&state.championId==championId;
    const auto bundle=reasoningBundle(c),hero=bundle.value(k("champions")).toObject().value(QString::number(championId)).toObject();
    const auto itemProfiles=bundle.value(k("items")).toObject();
    QJsonArray shown,components,transformations,unavailable,pathways,footwear,referenceIds;QList<int> explained;QSet<int> seen;
    auto collect=[&](int id){if(id<=0||!c.combat||!c.combat->items.contains(id)||seen.contains(id))return;seen.insert(id);referenceIds.append(id);explained.append(id);};
    if(observed)for(int id:state.owned)shown.append(QJsonObject{{k("item_id"),id},{k("name"),c.itemName(id)},{k("owned"),true},{k("availability"),QStringLiteral("当前背包")}});
    if(c.combat)for(const auto &raw:routes)for(const auto &value:raw.toObject().value(k("data")).toObject().value(k("item_ids")).toArray()){
        collect(value.toInt());}
    const auto external=externalStrategyReference(c,subject);
    for(const auto &raw:external.value(k("records")).toArray()){
        const auto row=raw.toObject();if(row.value(k("kind"))!=k("item_build")||row.value(k("data")).toObject().value(k("category"))!=k("boots"))continue;
        footwear.append(row);for(const auto &value:row.value(k("data")).toObject().value(k("item_ids")).toArray())collect(value.toInt());
    }
    // Keep the acquisition path distinct from a directly purchasable finished
    // item. A public terminal build cannot establish current transformation.
    const auto initialIds=explained;
    for(int id:initialIds){const auto availability=itemProfiles.value(QString::number(id)).toObject().value(k("availability")).toObject();
        const auto parents=availability.value(k("transformation_from_ids")).toArray();if(parents.isEmpty())continue;
        pathways.append(QJsonObject{{k("target_item_id"),id},{k("requires_base_item_ids"),parents},{k("transformation_observed"),false},
            {k("required_trigger_not_assumed"),true},{k("direct_purchase_verified"),false}});
        for(const auto &parent:parents)collect(parent.toInt());
    }
    QJsonArray conditionalOptions;
    const auto relations=hero.value(k("item_relations")).toObject();
    for(auto it=relations.begin();it!=relations.end();++it){const int id=it.key().toInt();
        if(!c.combat||!c.combat->items.contains(id))continue;
        const auto availability=itemProfiles.value(it.key()).toObject().value(k("availability")).toObject();
        if(!c.combat->items.value(id).eligible&&availability.value(k("transformation_from_ids")).toArray().isEmpty())continue;
        collect(id);auto option=itemReason(c,bundle,hero,id);option[k("selection_basis")]=k("conditional_mechanism_match_not_provider_or_numeric_rank");conditionalOptions.append(option);
    }
    if(c.combat)for(const auto &value:selected.value(k("data")).toObject().value(k("item_ids")).toArray()){
        const int id=value.toInt();if(!c.combat->items.contains(id))continue;const auto &entry=c.combat->items.value(id);
        QJsonObject item{{k("item_id"),id},{k("name"),c.itemName(id)},{k("owned"),observed&&state.owned.contains(id)},
            {k("price"),entry.price},{k("reference_only"),true},{k("source_record_id"),selected.value(k("id"))},
            {k("source_url"),selected.value(k("source_url"))},{k("source_order_is_purchase_order"),false},
            {k("direct_purchase_verified"),false},{k("availability"),QStringLiteral("参考组合；购买与转化条件以本局为准")}};
        const auto availability=itemProfiles.value(QString::number(id)).toObject().value(k("availability")).toObject();
        item[k("acquisition")]=availability;
        if(!entry.finished)components.append(item);
        else if(!entry.eligible){if(!availability.value(k("transformation_from_ids")).toArray().isEmpty())transformations.append(item);else unavailable.append(item);}
        else if(!observed)shown.append(item);
    }
    for(int id:state.owned)if(!explained.contains(id))explained.append(id);
    QJsonObject sourceRoutes=role;sourceRoutes[k("numeric_winner_withheld_for_ally_objective")]=true;
    sourceRoutes[k("mechanism_reference_available")]=!hero.isEmpty()&&!conditionalOptions.isEmpty();
    sourceRoutes[k("prefer_mechanism_reference_for_display")]=!hero.isEmpty()&&!conditionalOptions.isEmpty();
    return {{k("route"),selected.isEmpty()?QStringLiteral("辅助机制与条件分析"):QStringLiteral("治疗与保护装备参考")},
        {k("resolved_preference"),QStringLiteral("辅助保护")},{k("objective"),k("ally_protection")},
        {k("recommendation_kind"),k("mechanism_and_public_reference")},{k("reference_only"),true},{k("conditional_reference"),true},
        {k("ranking_available"),false},{k("enumeration_complete"),true},{k("numeric_ally_utility_calculated"),false},
        {k("globally_optimal_full_build"),false},{k("version_match"),modelPatchMatches(c,state)},{k("rule_basis"),modelRuleBasis(c,state)},{k("items"),shown},
        {k("opening_components_reference"),components},{k("upgrade_targets_reference"),transformations},
        {k("unavailable_source_items"),unavailable},{k("acquisition_path_references"),pathways},{k("footwear_references"),footwear},
        {k("conditional_equipment_options"),conditionalOptions},
        {k("reference_item_ids"),referenceIds},{k("reference_routes"),routes},{k("role_aware_recommendation"),sourceRoutes},
        {k("build_reasoning"),buildReasoning(c,state,championId,explained,bundle)},
        {k("source_route_statistics_not_purchase_order"),true},{k("source_selected_by"),k("first_displayed_matching_ally_support_route_not_win_rate")},
        {k("method"),k("source_bound_mechanism_requirements_and_same_patch_public_build_reference_no_damage_beam_ranking")},
        {k("search"),QJsonObject{{k("six_slot_search_executed"),false},{k("evaluated_combinations"),0},{k("exhaustive"),false}}},
        {k("comparison"),QJsonObject{{k("valid"),false},{k("comparison_score"),QJsonValue::Null},{k("reason"),k("ally_support_objective_not_quantified")}}},
        {k("advice"),QJsonArray{QStringLiteral("按治疗与保护需求解释装备；公开组合是同版参考，页面顺序不代表胜率或购买顺序。"),
            QStringLiteral("先核对自身续航、法力、技能触发与队友受益，再按对面威胁替换；升级目标不等于可直接购买。"),
            QStringLiteral("未提供已选海克斯、阵容、背包和预算时，不生成本局唯一最优或立即购买指令。")}}};
}
