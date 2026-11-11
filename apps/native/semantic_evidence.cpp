#include "semantic_evidence.h"
#include "catalog.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <algorithm>

namespace {
QString k(const char *s){return QString::fromLatin1(s);}
constexpr qint64 maximumFileBytes=64*1024*1024;
struct CachedFile {qint64 size=-1,modified=-1;QByteArray sha;QJsonValue json=QJsonValue(QJsonValue::Undefined);};
// Cache hashes and parsed indexes, not validation decisions. Every lookup
// checks the current file stamp and the expected source digest again.
CachedFile readFile(const QString &path,bool parse=false){
    static QMutex mutex;static QMap<QString,CachedFile> entries;
    QMutexLocker lock(&mutex);const QFileInfo info(path);
    if(!info.isFile()||info.size()>maximumFileBytes)return {};
    auto &entry=entries[path];
    if(entry.size!=info.size()||entry.modified!=info.lastModified().toMSecsSinceEpoch()
        ||(parse&&entry.json.isUndefined())){
        QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};
        const auto bytes=file.readAll();const QFileInfo after(path);
        if(bytes.size()!=info.size()||after.size()!=info.size()||after.lastModified()!=info.lastModified())return {};
        CachedFile next;next.size=info.size();next.modified=info.lastModified().toMSecsSinceEpoch();
        next.sha=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex();
        if(parse){QJsonParseError error;const auto doc=QJsonDocument::fromJson(bytes,&error);if(error.error!=QJsonParseError::NoError||doc.isNull())return {};next.json=doc.isObject()?QJsonValue(doc.object()):QJsonValue(doc.array());}
        entry=next;
    }
    return entry;
}
QString localPath(const QString &root,QString relative){
    relative.replace(QChar(u'\\'),QChar(u'/'));
    if(relative.isEmpty()||QDir::isAbsolutePath(relative)||relative.split(QChar(u'/')).contains(k("..")))return {};
    const auto base=QFileInfo(root).canonicalFilePath(),path=QFileInfo(QDir(root).filePath(relative)).canonicalFilePath();
    if(base.isEmpty()||path.isEmpty()||!path.startsWith(base+QChar(u'/'),Qt::CaseInsensitive))return {};
    return path;
}
bool verified(const QString &root,const QString &path,const QJsonValue &hash){
    static const QRegularExpression digest(k("\\A[0-9a-f]{64}\\z"));
    const auto expected=hash.toString();if(!digest.match(expected).hasMatch())return false;
    const auto local=localPath(root,path);return !local.isEmpty()&&readFile(local).sha==expected.toLatin1();
}
QJsonValue readValue(const QString &root,const QString &path){const auto local=localPath(root,path);return local.isEmpty()?QJsonValue{}:readFile(local,true).json;}
QJsonObject readObject(const QString &root,const QString &path){return readValue(root,path).toObject();}
bool scoped(const QJsonObject &index,const QString &patch,const QString &status){
    return index.value(k("schema_version")).toInt()==1&&index.value(k("patch")).toString()==patch
        &&index.value(k("semantic_status"))==status&&index.value(k("automatic_execution_ready")).isBool()
        &&!index.value(k("automatic_execution_ready")).toBool();
}
bool provenanceOkay(const QString &root,const QJsonObject &index){
    const auto p=index.value(k("provenance")).toObject();
    for(const auto &field:{k("active_pointer"),k("manifest_path"),k("combat_path")}){
        const auto hashKey=field==k("active_pointer")?k("active_pointer_sha256"):field==k("manifest_path")?k("manifest_sha256"):k("combat_sha256");
        if(!verified(root,p.value(field).toString(),p.value(hashKey)))return false;
    }
    if(p.contains(k("catalog_path")))return verified(root,p.value(k("catalog_path")).toString(),p.value(k("catalog_sha256")));
    const auto catalogs=p.value(k("catalogs")).toObject();if(catalogs.isEmpty())return false;
    for(const auto &value:catalogs){const auto row=value.toObject();if(!verified(root,row.value(k("path")).toString(),row.value(k("sha256"))))return false;}
    return true;
}
QJsonValue bounded(const QJsonValue &value,int depth=0){
    if(depth>5)return {};
    if(value.isString())return value.toString().left(600);
    if(value.isArray()){QJsonArray out;for(const auto &v:value.toArray()){if(out.size()>=8)break;out.append(bounded(v,depth+1));}return out;}
    if(value.isObject()){QJsonObject out;const auto object=value.toObject();for(auto i=object.begin();i!=object.end();++i){if(out.size()>=12)break;out[i.key().left(80)]=bounded(i.value(),depth+1);}return out;}
    if(value.isBool()||value.isNull())return value;
    return {};
}
QJsonObject oldCard(const QString &root,const QJsonObject &index,const QString &group,int id,const QString &kind){
    const auto card=index.value(group).toObject().value(QString::number(id)).toObject();
    const auto identity=kind==k("champion")?k("champion_id"):kind==k("item")?k("item_id"):k("augment_id");
    if(card.value(identity).toInt()!=id||card.value(k("patch"))!=index.value(k("patch"))
        ||card.value(k("automatic_execution_ready"))!=false||card.value(k("semantic_status"))!=k("language_reviewed_not_game_verified"))return {};
    const auto resources=index.value(k("provenance")).toObject().value(k("source_resource_hashes")).toObject();
    const auto expected=card.value(kind==k("champion")?k("source_property_sha256"):k("source_sha256"));bool found=false;
    for(auto i=resources.begin();i!=resources.end();++i)if(i.value()==expected&&verified(root,i.key(),expected)){found=true;break;}
    if(!found)return {};
    QJsonObject out{{k("kind"),kind},{k("id"),id},{k("name"),card.value(k("name"))},{k("source_review"),k("local_language_review")},
        {k("source_sha256"),expected},{k("automatic_execution_ready"),false}};
    for(const auto &field:{k("interpretation_zh"),k("champion_interpretation_zh"),k("level_growth_interpretation_zh"),k("base_stats_interpretation_zh"),
        k("trigger"),k("target"),k("temporal"),k("dependencies"),k("interaction_examples"),k("unknowns")})if(card.contains(field))out[field]=bounded(card.value(field));
    if(kind==k("champion")){
        QJsonArray skills;for(const auto &v:card.value(k("skills")).toArray()){
            const auto skill=v.toObject();QJsonObject row;for(const auto &field:{k("slot"),k("name"),k("interpretation_zh"),k("trigger"),k("target"),k("temporal"),k("dependencies"),k("interaction_examples"),k("unknowns")})if(skill.contains(field))row[field]=bounded(skill.value(field));
            if(skills.size()<5)skills.append(row);
        }out[k("skills")]=skills;
    }
    return out;
}
QJsonObject enrichedIndex(const QString &root,const QString &patch){
    const auto pointer=readObject(root,k("data/active-semantic-evidence.v1.json"));
    if(pointer.value(k("schema_version")).toInt()!=1||pointer.value(k("patch")).toString()!=patch||pointer.value(k("mode"))!=k("KIWI")
        ||!verified(root,pointer.value(k("index_path")).toString(),pointer.value(k("index_sha256"))))return {};
    const auto index=readObject(root,pointer.value(k("index_path")).toString());
    if(!scoped(index,patch,k("source_cited_model_interpretation_unverified"))||index.value(k("mode"))!=k("KIWI"))return {};
    const auto hashes=index.value(k("source_sha256")).toObject();
    if(!hashes.contains(k("data/active-runtime-catalog.v1.json"))||!hashes.contains(k("data/combat.v1.json"))||hashes.size()>2048)return {};
    for(auto i=hashes.begin();i!=hashes.end();++i)if(!verified(root,i.key(),i.value()))return {};
    return index;
}
QJsonObject equipmentIndex(const QString &root,const QString &patch){
    const auto pointer=readObject(root,k("data/active-equipment-evidence.v1.json"));
    if(pointer.value(k("schema_version")).toInt()!=1||pointer.value(k("patch")).toString()!=patch||pointer.value(k("mode"))!=k("KIWI")
        ||!verified(root,pointer.value(k("index_path")).toString(),pointer.value(k("index_sha256"))))return {};
    const auto index=readObject(root,pointer.value(k("index_path")).toString());
    if(!scoped(index,patch,k("independent_source_reasoning_reviewed_not_game_verified"))||index.value(k("mode"))!=k("KIWI")
        ||index.value(k("numeric_override"))!=false||index.value(k("ranking_override"))!=false||index.value(k("game_verified"))!=false)return {};
    const auto hashes=index.value(k("source_sha256")).toObject();
    if(!hashes.contains(k("data/active-runtime-catalog.v1.json"))||!hashes.contains(k("data/combat.v1.json"))||hashes.size()>256)return {};
    for(auto i=hashes.begin();i!=hashes.end();++i)if(!verified(root,i.key(),i.value()))return {};
    return index;
}
QJsonObject equipmentCard(const QJsonObject &index,int id){
    const auto card=index.value(k("cards")).toObject().value(k("items")).toObject().value(QString::number(id)).toObject();
    const auto review=card.value(k("source_review")).toString();const bool deep=review==k("independent_equipment_source_reasoning_review");
    if(card.value(k("kind"))!=k("item")||card.value(k("id")).toInt()!=id||card.value(k("patch"))!=index.value(k("patch"))||card.value(k("mode"))!=k("KIWI")
        ||card.value(k("automatic_execution_ready"))!=false||card.value(k("numeric_override"))!=false||card.value(k("ranking_override"))!=false||card.value(k("game_verified"))!=false
        ||(!deep&&review!=k("equipment_conditional_combination_reference"))||card.value(k("source_reasoning_reviewed"))!=QJsonValue(deep))return {};
    const auto hashes=index.value(k("source_sha256")).toObject(),provenance=card.value(k("provenance")).toObject();
    const auto bound=[&](const char *pathKey,const char *hashKey){const auto path=provenance.value(k(pathKey)).toString();
        return !path.isEmpty()&&hashes.contains(path)&&hashes.value(path)==provenance.value(k(hashKey));};
    if(!bound("independent_review_path","independent_review_sha256")
        ||(deep?!bound("dossier_path","dossier_sha256"):!bound("combination_rules_path","combination_rules_sha256")))return {};
    if(deep){
        const auto material=provenance.value(k("material_sha256")).toString(),binding=provenance.value(k("review_hash_binding")).toString();
        if(!QRegularExpression(k("\\A[0-9a-f]{64}\\z")).match(material).hasMatch())return {};
        if(binding==k("independent_material_hash")){if(provenance.value(k("reviewed_material_sha256")).toString()!=material)return {};}
        else if(binding==k("acceptance_manifest_file_hash")){if(!bound("acceptance_manifest_path","acceptance_manifest_sha256"))return {};}
        else return {};
    }
    // This is a bounded explanation of a pinned reviewed record, not an
    // executable effect. Keep conditions and counterconditions in whole rows;
    // never turn a clipped rule into an unconditional recommendation.
    QJsonObject out{{k("kind"),k("item")},{k("id"),id},{k("name"),card.value(k("name")).toString().left(80)},
        {k("patch"),index.value(k("patch"))},{k("mode"),k("KIWI")},{k("source_review"),review},
        {k("source_reasoning_reviewed"),deep},{k("reference_only"),true},{k("automatic_execution_ready"),false},
        {k("numeric_override"),false},{k("ranking_override"),false},{k("game_verified"),false},{k("provenance"),provenance},
        {k("presentation_compacted"),false},{k("omitted_fields"),QJsonArray{}}};
    constexpr int limit=8*1024;QJsonArray omitted;bool compacted=false;
    const auto add=[&](const QString &field,const QJsonValue &value){auto candidate=out;candidate[field]=value;
        if(QJsonDocument(candidate).toJson(QJsonDocument::Compact).size()>limit-512){compacted=true;if(!omitted.contains(field))omitted.append(field);return false;}
        out=candidate;return true;};
    // Reserve uncertainty, inputs and provenance before explanatory material.
    for(const auto &field:{k("unknowns"),k("required_context"),k("interpretation_zh"),k("formula_summary")})if(card.contains(field))add(field,card.value(field));
    for(const auto &field:{k("facts"),k("conditional_rules"),k("combinations")})if(card.contains(field)){
        const auto original=card.value(field);if(!original.isArray()){add(field,original);continue;}
        QJsonArray rows;int indexInDossier=0;for(auto row:original.toArray()){
            if(deep&&field==k("facts")&&row.isObject()){
                auto fact=row.toObject();fact[k("source_refs")]=QJsonArray{QJsonObject{{k("path"),provenance.value(k("dossier_path"))},
                    {k("sha256"),provenance.value(k("dossier_sha256"))},{k("json_pointer"),k("/facts/")+QString::number(indexInDossier)}}};row=fact;
            }
            auto next=rows;next.append(row);if(add(field,next))rows=next;else break;++indexInDossier;
        }
    }
    out[k("presentation_compacted")]=compacted;out[k("omitted_fields")]=omitted;out[k("maximum_bytes")]=limit;
    return QJsonDocument(out).toJson(QJsonDocument::Compact).size()<=limit?out:QJsonObject{};
}
QJsonValue jsonPointer(QJsonValue value,const QString &pointer){
    if(!pointer.startsWith(QChar(u'/')))return {};
    const auto parts=pointer.mid(1).split(QChar(u'/'));
    for(QString part:parts){part.replace(k("~1"),k("/"));part.replace(k("~0"),k("~"));
        if(value.isObject())value=value.toObject().value(part);
        else if(value.isArray()){bool ok=false;const int i=part.toInt(&ok);const auto a=value.toArray();if(!ok||i<0||i>=a.size())return {};value=a[i];}
        else return {};
    }return value;
}
QJsonArray citations(const QString &root,const QJsonObject &index,const QJsonArray &raw){
    QJsonArray out;const auto hashes=index.value(k("source_sha256")).toObject();
    for(const auto &v:raw){if(out.size()>=2)break;const auto row=v.toObject();const auto path=row.value(k("path")).toString(),quote=row.value(k("quote")).toString();
        if(quote.isEmpty()||quote.size()>1200||!hashes.contains(path)||row.value(k("sha256"))!=hashes.value(path))continue;
        const auto text=jsonPointer(readValue(root,path),row.value(k("json_pointer")).toString());
        if(!text.isString()||!text.toString().contains(quote))continue;
        QJsonObject clean;for(const auto &field:{k("doc_id"),k("path"),k("sha256"),k("json_pointer"),k("quote")})clean[field]=row.value(field);out.append(clean);
    }return out;
}
QJsonObject enrichedCard(const QString &root,const QJsonObject &index,const QString &group,int id,const QString &kind){
    const auto card=index.value(k("cards")).toObject().value(group).toObject().value(QString::number(id)).toObject();
    if(card.value(k("id")).toInt()!=id||card.value(k("kind"))!=kind||card.value(k("patch"))!=index.value(k("patch"))||card.value(k("mode"))!=k("KIWI")||card.value(k("automatic_execution_ready"))!=false)return {};
    QJsonArray sections,synergies;
    for(const auto &v:card.value(k("sections")).toArray()){if(sections.size()>=6)break;const auto row=v.toObject();const auto evidence=citations(root,index,row.value(k("evidence")).toArray());if(evidence.isEmpty())continue;
        QJsonObject clean{{k("evidence"),evidence},{k("semantic_entailment_verified"),false}};for(const auto &field:{k("doc_id"),k("interpretation_zh"),k("conditions"),k("observations_required"),k("tags"),k("unresolved_placeholders")})clean[field]=bounded(row.value(field));sections.append(clean);
    }
    for(const auto &v:card.value(k("synergies")).toArray()){if(synergies.size()>=3)break;const auto row=v.toObject();const auto evidence=citations(root,index,row.value(k("evidence")).toArray());if(evidence.isEmpty())continue;
        QJsonObject clean{{k("evidence"),evidence},{k("observed_active"),false},{k("requires_pair_verification"),true},{k("semantic_entailment_verified"),false}};for(const auto &field:{k("partner_tags"),k("condition_zh"),k("rationale_zh")})clean[field]=bounded(row.value(field));synergies.append(clean);
    }
    if(sections.isEmpty())return {};
    return {{k("kind"),kind},{k("id"),id},{k("name"),card.value(k("name"))},{k("source_review"),k("source_cited_ds_model_interpretation_unverified")},
        {k("sections"),sections},{k("synergies"),synergies},{k("unknowns"),bounded(card.value(k("unknowns")))},
        {k("unresolved_placeholders"),bounded(card.value(k("unresolved_placeholders")))},{k("availability_verified"),false},{k("automatic_execution_ready"),false}};
}
QJsonObject baseEvidence(const QString &patch){return {{k("schema_version"),1},{k("patch"),patch},{k("mode"),k("KIWI")},
    {k("reference_only"),true},{k("automatic_execution_ready"),false},{k("numeric_override"),false},{k("ranking_override"),false},
    {k("observed_state_inferred"),false},{k("semantic_entailment_verified"),false},{k("limits"),k("有引文的机器语言解释仍未验证语义蕴含；搭配须单独核实，不证明当前触发、数值执行、可购买性或实际胜率。")}};}
QJsonValue compactPresentation(const QJsonValue &value,int depth=0){
    if(depth>5)return {};
    if(value.isString())return value.toString().left(160);
    if(value.isArray()){QJsonArray out;for(const auto &v:value.toArray()){if(out.size()>=2)break;out.append(compactPresentation(v,depth+1));}return out;}
    if(value.isObject()){QJsonObject out;const auto object=value.toObject();for(auto i=object.begin();i!=object.end();++i)out[i.key()]=compactPresentation(i.value(),depth+1);return out;}
    return value;
}
QJsonObject compactCard(const QJsonObject &row){
    QJsonObject out;for(const auto &field:{k("kind"),k("id"),k("name"),k("source_review"),k("base_source_review"),k("source_sha256"),k("automatic_execution_ready")})if(row.contains(field))out[field]=row.value(field);
    out[k("name")]=row.value(k("name")).toString().left(80);
    for(const auto &field:{k("interpretation_zh"),k("champion_interpretation_zh"),k("level_growth_interpretation_zh"),k("unknowns")})if(row.contains(field))out[field]=compactPresentation(row.value(field));
    if(row.value(k("trigger")).isString())out[k("trigger")]=row.value(k("trigger")).toString().left(160);
    else if(row.value(k("trigger")).isObject())out[k("trigger_event")]=row.value(k("trigger")).toObject().value(k("event")).toString().left(160);
    QJsonArray skills;for(const auto &v:row.value(k("skills")).toArray()){
        const auto skill=v.toObject();QJsonObject small;for(const auto &field:{k("slot"),k("name"),k("interpretation_zh"),k("unknowns")})if(skill.contains(field))small[field]=compactPresentation(skill.value(field));
        if(skills.size()<5)skills.append(small);
    }if(!skills.isEmpty())out[k("skills")]=skills;
    const auto sections=row.value(k("sections")).toArray();if(!sections.isEmpty()){
        const auto original=sections.first().toObject();QJsonObject section{{k("interpretation_zh"),original.value(k("interpretation_zh")).toString().left(160)}};
        const auto evidence=original.value(k("evidence")).toArray();if(!evidence.isEmpty()){
            auto citation=evidence.first().toObject();citation[k("quote")]=citation.value(k("quote")).toString().left(160);section[k("evidence")]=QJsonArray{citation};
        }
        out[k("sections")]=QJsonArray{section};
    }
    out[k("presentation_compacted")]=true;return out;
}
QJsonArray fitBudget(QJsonArray cards){
    const auto oversized=[&]{return QJsonDocument(cards).toJson(QJsonDocument::Compact).size()>60000;};
    // Preserve the current hero and offered/selected augments before optional
    // equipment explanations. Never discard the three offers to keep items.
    while(oversized()){
        int item=-1;for(int i=cards.size()-1;i>=0;--i)if(cards[i].toObject().value(k("kind"))==k("item")){item=i;break;}
        if(item<0)break;cards.removeAt(item);
    }
    if(oversized())for(int i=0;i<cards.size();++i)cards[i]=compactCard(cards[i].toObject());
    return cards;
}
}

QJsonObject filterCandidateSemanticEvidence(const QJsonObject &evidence,int championId,const QSet<int> &itemIds,const QSet<int> &augmentIds){
    auto out=baseEvidence(evidence.value(k("patch")).toString());QJsonArray rows;
    if(evidence.value(k("schema_version")).toInt()!=1||evidence.value(k("mode"))!=k("KIWI")
        ||evidence.value(k("automatic_execution_ready"))!=false||evidence.value(k("numeric_override"))!=false
        ||evidence.value(k("ranking_override"))!=false){out[k("cards")]=rows;out[k("available")]=false;out[k("status")]=k("invalid_evidence_contract");return out;}
    for(const auto &v:evidence.value(k("cards")).toArray()){
        const auto row=v.toObject();const auto kind=row.value(k("kind")).toString();const int id=row.value(k("id")).toInt();
        if((kind==k("champion")&&id==championId)||(kind==k("item")&&itemIds.contains(id))||(kind==k("augment")&&augmentIds.contains(id)))rows.append(row);
    }
    const auto countBeforeBudget=rows.size();rows=fitBudget(rows);out[k("presentation_compacted")]=rows.size()!=countBeforeBudget;
    out[k("cards")]=rows;out[k("available")]=!rows.isEmpty();out[k("status")]=rows.isEmpty()?k("no_verified_candidate_cards"):k("source_cited_language_reference");return out;
}

QJsonObject candidateSemanticEvidence(const Catalog &catalog,int championId,const QSet<int> &itemIds,const QSet<int> &augmentIds){
    auto out=baseEvidence(catalog.patch);QJsonArray cards;
    if(catalog.patch!=k("16.21")||!catalog.champions.contains(championId)){out[k("available")]=false;out[k("cards")]=cards;out[k("status")]=k("scope_unknown");return out;}
    auto heroItems=readObject(catalog.root,k("research/2026-10-23/lifecycle-sandbox/semantic-v2/index.json"));
    if(!scoped(heroItems,catalog.patch,k("language_reviewed_not_game_verified"))||!provenanceOkay(catalog.root,heroItems))heroItems={};
    auto augments=readObject(catalog.root,k("research/2026-10-23/lifecycle-sandbox/augment-semantics/index.json"));
    if(!scoped(augments,catalog.patch,k("language_reviewed_not_game_verified"))||!provenanceOkay(catalog.root,augments))augments={};
    const auto ds=enrichedIndex(catalog.root,catalog.patch),equipment=equipmentIndex(catalog.root,catalog.patch);
    const auto add=[&](const QString &group,const QString &oldGroup,int id,const QString &kind,const QJsonObject &old){
        if(kind==k("item")){const auto reviewed=equipmentCard(equipment,id);if(!reviewed.isEmpty()){cards.append(reviewed);return;}}
        auto card=oldCard(catalog.root,old,oldGroup,id,kind);const auto enriched=enrichedCard(catalog.root,ds,group,id,kind);
        if(!enriched.isEmpty()){
            if(!card.isEmpty())card[k("base_source_review")]=card.value(k("source_review"));
            auto unknowns=card.value(k("unknowns")).toArray();for(const auto &unknown:enriched.value(k("unknowns")).toArray())if(!unknowns.contains(unknown))unknowns.append(unknown);
            for(auto i=enriched.begin();i!=enriched.end();++i)if(i.key()!=k("unknowns"))card[i.key()]=i.value();
            card[k("unknowns")]=unknowns;
        }
        if(!card.isEmpty())cards.append(card);
    };
    add(k("champions"),k("champions_by_id"),championId,k("champion"),heroItems);
    auto items=itemIds.values(),offered=augmentIds.values();std::sort(items.begin(),items.end());std::sort(offered.begin(),offered.end());
    for(int id:offered)if(catalog.augments.contains(id)&&cards.size()<29)add(k("augments"),k("cards_by_id"),id,k("augment"),augments);
    for(int id:items)if(catalog.items.contains(id)&&cards.size()<29)add(k("items"),k("items_by_id"),id,k("item"),heroItems);
    cards=fitBudget(cards);
    out[k("cards")]=cards;return filterCandidateSemanticEvidence(out,championId,itemIds,augmentIds);
}
