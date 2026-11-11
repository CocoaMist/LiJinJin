#include "external_strategy_reference.h"
#include "analysis.h"
#include "catalog.h"
#include "purchase_planner.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QStringList>
#include <QThreadPool>
#include <QUrl>
#include <chrono>
#include <cmath>
#include <memory>

namespace {
QString k(const char *s){return QString::fromLatin1(s);}
constexpr qint64 maximumBundleBytes=32*1024*1024;
constexpr qint64 refreshIntervalMs=2000,rejectedRetryIntervalMs=30000;
qint64 steadyMilliseconds(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
struct StrategyRefreshEntry {
    QMutex mutex;
    QJsonObject pack;
    QByteArray pointerDigest;
    QString lastError,lastCheckedUtc;
    qint64 nextProbeMs=0,lastRejectedMs=0;
    bool refreshing=false;
};
std::shared_ptr<StrategyRefreshEntry> refreshEntry(const QString &root,const QString &patch,const QJsonObject &seed){
    static QMutex mutex;
    static QMap<QString,std::shared_ptr<StrategyRefreshEntry>> entries;
    const auto key=root+QChar(u'\n')+patch;
    QMutexLocker lock(&mutex);auto &entry=entries[key];
    if(!entry){
        entry=std::make_shared<StrategyRefreshEntry>();
        if(seed.value(k("available")).toBool()&&seed.value(k("patch"))==patch&&seed.value(k("mode"))==k("KIWI")){
            entry->pack=seed;entry->pointerDigest=QByteArray::fromHex(seed.value(k("bundle_pointer_sha256")).toString().toLatin1());
        }
    }
    return entry;
}
bool integer(const QJsonValue &v,double minimum=0,double maximum=1e12){return v.isDouble()&&std::isfinite(v.toDouble())&&v.toDouble()>=minimum&&v.toDouble()<=maximum&&std::floor(v.toDouble())==v.toDouble();}
bool digest(const QString &s){static const QRegularExpression pattern(k("\\A[0-9a-f]{64}\\z"));return pattern.match(s).hasMatch();}
bool clock(const QJsonValue &v){
    if(!v.isString())return false;
    auto at=QDateTime::fromString(v.toString(),Qt::ISODateWithMs);if(!at.isValid())at=QDateTime::fromString(v.toString(),Qt::ISODate);
    return at.isValid()&&at.toMSecsSinceEpoch()>0&&at<=QDateTime::currentDateTimeUtc().addDays(1);
}
bool publicUrl(const QString &s){
    if(s.size()>2048||s.contains(QRegularExpression(k("[\\s\\x00-\\x1f\\\\]"))))return false;
    const QUrl url(s,QUrl::StrictMode);
    static const QSet<QString> hosts{k("op.gg"),k("www.op.gg"),k("lol.qq.com"),k("apps.game.qq.com"),k("www.leagueoflegends.com"),
        k("support-leagueoflegends.riotgames.com"),k("www.xiaohongshu.com"),k("www.douyin.com"),k("www.bilibili.com"),k("www.arambro.com"),k("arambro.com")};
    if(!url.isValid()||url.scheme()!=k("https")||!hosts.contains(url.host())||!url.userInfo().isEmpty()||(url.port()!=-1&&url.port()!=443))return false;
    static const QRegularExpression privateQuery(k("(?:^|[?&])(?:token|access_token|xsec_token|authorization|api_key|puuid|summoner|account)(?:=|&|$)"),QRegularExpression::CaseInsensitiveOption);
    return !privateQuery.match(url.query()).hasMatch();
}
bool safeObject(const QJsonValue &v,int depth=0){
    if(depth>7)return false;
    static const QRegularExpression identity(k("puuid|summoner|riot.?id|account|authorization|api.?key|token|player.?key|game.?id|match.?ref|raw.?json|aliases|co.?play|display.?name|full.?name|game.?name|tag.?line|nick.?name|identity"),QRegularExpression::CaseInsensitiveOption);
    if(v.isObject()){
        const auto o=v.toObject();if(o.size()>80)return false;
        for(auto i=o.begin();i!=o.end();++i)if(identity.match(i.key()).hasMatch()||!safeObject(i.value(),depth+1))return false;
    }else if(v.isArray()){
        if(v.toArray().size()>512)return false;for(const auto &x:v.toArray())if(!safeObject(x,depth+1))return false;
    }else if(v.isString()){
        if(v.toString().size()>12000)return false;
    }else if(v.isDouble()){
        if(!std::isfinite(v.toDouble())||std::abs(v.toDouble())>1e15)return false;
    }
    return !v.isUndefined();
}
QJsonValue cleanDataValue(const QJsonValue &v,int depth=0){
    if(depth>5)return {};
    if(v.isString())return v.toString().left(1800);
    if(v.isNull()||v.isBool()||v.isDouble())return v;
    if(v.isArray()){QJsonArray a;for(const auto &x:v.toArray()){if(a.size()>=32)break;a.append(cleanDataValue(x,depth+1));}return a;}
    if(!v.isObject())return {};
    static const QSet<QString> fields{
        k("champion_key"),k("champion_name"),k("tier"),k("rank"),k("wins"),k("plays"),k("games"),k("win_rate"),k("sample_size"),
        k("category"),k("item_ids"),k("item_names"),k("row_index"),k("ordered"),k("raw_table_heading"),k("augment_id"),k("name"),k("key"),k("rarity"),
        k("performance"),k("popular"),k("metric_semantics"),k("stat_scope"),k("recommended_champions"),k("id"),
        k("augment_id_namespace"),k("name_and_key_published"),
        k("title"),k("excerpt"),k("claim"),k("description"),k("text"),k("published_at"),k("reported_patch"),k("patch_mapping_basis"),
        k("source_heading"),k("section"),k("section_title"),k("content_kind"),k("reference_only"),k("numeric_override"),k("url"),k("source_url")};
    QJsonObject out;const auto o=v.toObject();for(auto i=o.begin();i!=o.end();++i)if(fields.contains(i.key()))out[i.key()]=cleanDataValue(i.value(),depth+1);return out;
}
QJsonObject unavailable(const QString &reason){return {{k("available"),false},{k("status"),k("unavailable")},{k("reason"),reason},{k("reference_only"),true},{k("numeric_override"),false},{k("ranking_override"),false}};}
QString sourceKey(const QJsonObject &o,bool record=false){
    return o.value(record?k("source_url"):k("url")).toString()+QChar(u'\n')+o.value(record?k("source_sha256"):k("sha256")).toString()
        +QChar(u'\n')+o.value(k("patch")).toString()+QChar(u'\n')+o.value(k("mode")).toString()+QChar(u'\n')+o.value(k("region")).toString()
        +QChar(u'\n')+o.value(k("source_kind")).toString();
}
bool sourceOkay(const QJsonObject &o,const QString &patch){
    const auto status=o.value(k("status"));const bool okay=(status.isDouble()&&integer(status,200,299))
        ||QStringList{k("ok"),k("fetched"),k("success"),k("cached"),k("complete")}.contains(status.toString());
    const auto access=o.value(k("access")).toString();
    return okay&&QStringList{k("full_body"),k("public_body"),k("readable")}.contains(access)
        &&publicUrl(o.value(k("url")).toString())&&digest(o.value(k("sha256")).toString())&&clock(o.value(k("retrieved_at")))
        &&o.value(k("patch")).toString()==patch&&o.value(k("mode"))==k("KIWI")
        &&QRegularExpression(k("\\A[A-Za-z][A-Za-z0-9_-]{0,23}\\z")).match(o.value(k("region")).toString()).hasMatch()
        &&QStringList{k("statistics"),k("official_rule"),k("community_guide")}.contains(o.value(k("source_kind")).toString());
}
bool recordDataOkay(const QJsonObject &data){
    const auto wins=data.value(k("wins")),plays=data.value(k("plays")),rate=data.value(k("win_rate")),n=data.value(k("sample_size"));
    if(!wins.isNull()&&!wins.isUndefined()&&!integer(wins))return false;
    if(!plays.isNull()&&!plays.isUndefined()&&!integer(plays))return false;
    if(!n.isNull()&&!n.isUndefined()&&!integer(n))return false;
    if(wins.isDouble()&&(!plays.isDouble()||wins.toDouble()>plays.toDouble()))return false;
    if(!rate.isNull()&&!rate.isUndefined()){
        if(!rate.isDouble()||!std::isfinite(rate.toDouble())||rate.toDouble()<0||rate.toDouble()>1||!plays.isDouble()||plays.toDouble()<=0||!wins.isDouble())return false;
        if(std::abs(rate.toDouble()-wins.toDouble()/plays.toDouble())>0.005)return false;
    }
    return true;
}
QJsonObject cleanSource(const QJsonObject &raw){QJsonObject out;for(const auto &name:{k("id"),k("url"),k("sha256"),k("retrieved_at"),k("status"),k("source_kind"),k("region"),k("mode"),k("patch"),k("access")})if(raw.contains(name))out[name]=raw[name];return out;}
QString normalizedAugmentKey(const QJsonObject &row,const char *field){
    return row.value(k(field)).toString().trimmed().toCaseFolded();
}
QHash<QString,int> augmentKeyIndex(const Catalog &catalog,const QSet<QString> *requestedKeys=nullptr){
    QHash<QString,int> keys;
    if(requestedKeys)keys.reserve(requestedKeys->size());
    for(auto it=catalog.augments.cbegin();it!=catalog.augments.cend();++it){
        for(const auto *field:{"key","identity"}){
            const auto key=normalizedAugmentKey(it.value(),field);
            if(key.isEmpty()||(requestedKeys&&!requestedKeys->contains(key)))continue;
            auto existing=keys.find(key);
            if(existing==keys.end())keys.insert(key,it.key());
            else if(existing.value()!=it.key())existing.value()=0;
        }
    }
    return keys;
}
int resolveAugment(const Catalog &catalog,const QHash<QString,int> &keys,QJsonObject &data){
    // OP.GG's numeric namespace is not the game's catalog namespace. Exact,
    // versioned internal keys are the identity proof; a coincident ID is not.
    const int id=keys.value(normalizedAugmentKey(data,"key"));
    const auto providerId=data.value(k("augment_id"));
    if(id<=0||!integer(providerId,1,10000000)||data.value(k("name")).toString().trimmed().isEmpty())return 0;
    const auto local=catalog.augments.value(id);
    // The provider's rarity enum is another namespace. Keep its original value
    // and show the catalog label separately rather than guessing enum meanings.
    data[k("provider_rarity")]=data.value(k("rarity"));
    data[k("catalog_rarity")]=local.value(k("rarity"));
    data[k("provider_rarity_translation_verified")]=false;
    data[k("provider_augment_id")]=providerId;
    data[k("provider_augment_id_namespace")]=data.value(k("augment_id_namespace"));
    data[k("augment_id")]=id;data[k("augment_id_namespace")]=k("local_versioned_catalog");
    data[k("identity_mapping_basis")]=k("exact_versioned_catalog_key_with_published_provider_name");
    data[k("catalog_mapping_basis")]=k("exact_versioned_catalog_key_with_published_provider_name");
    data[k("name_translation_verified")]=false;
    return id;
}
}

QString augmentStatisticsBatchKey(const QJsonObject &row){
    const auto data=row.value(k("data")).toObject();
    return sourceKey(row,true)+QChar(u'\n')+QString::number(row.value(k("champion_id")).toInt())
        +QChar(u'\n')+data.value(k("stat_scope")).toString()+QChar(u'\n')+data.value(k("metric_semantics")).toString();
}

QJsonObject loadExternalStrategyEvidence(const QString &root,const QString &expectedPatch){
    const auto loaded=[&](QJsonObject result){
        QJsonObject snapshot;
        for(const auto &field:{k("available"),k("status"),k("reason"),k("revision"),k("patch"),k("mode")})
            if(result.contains(field))snapshot[field]=result.value(field);
        result[k("runtime_reload_origin")]=QJsonObject{{k("schema_version"),1},
            {k("workspace"),QDir::cleanPath(QFileInfo(root).absoluteFilePath())},{k("expected_patch"),expectedPatch},{k("snapshot"),snapshot}};
        return result;
    };
    const QString base=QFileInfo(root).canonicalFilePath();if(base.isEmpty())return loaded(unavailable(k("workspace_unavailable")));
    QFile pointerFile(QDir(base).filePath(k("data/active-strategy-evidence.v1.json")));
    if(!pointerFile.exists())return loaded(unavailable(k("not_installed")));
    if(!pointerFile.open(QIODevice::ReadOnly)||pointerFile.size()>65536)return loaded(unavailable(k("pointer_unreadable")));
    const auto pointerBytes=pointerFile.readAll();
    QJsonParseError pe;const auto pointerDoc=QJsonDocument::fromJson(pointerBytes,&pe);const auto pointer=pointerDoc.object();
    if(pe.error!=QJsonParseError::NoError||pointer.value(k("schema_version")).toInt()!=1||pointer.value(k("patch")).toString()!=expectedPatch||pointer.value(k("mode"))!=k("KIWI"))return loaded(unavailable(k("pointer_scope_mismatch")));
    const auto relative=pointer.value(k("bundle_path")).toString(),sha=pointer.value(k("bundle_sha256")).toString();
    if(!digest(sha)||pointer.value(k("revision"))!=sha||QDir::isAbsolutePath(relative)||relative.contains(QChar(u'\\'))||!relative.startsWith(k("data/strategy-evidence-revisions/"))||relative.contains(k("..")))return loaded(unavailable(k("pointer_path_invalid")));
    const QString path=QFileInfo(QDir(base).filePath(relative)).canonicalFilePath();
    if(path.isEmpty()||!path.startsWith(base+QChar(u'/'),Qt::CaseInsensitive))return loaded(unavailable(k("bundle_outside_workspace")));
    QFile file(path);if(!file.open(QIODevice::ReadOnly)||file.size()>maximumBundleBytes)return loaded(unavailable(k("bundle_unreadable_or_too_large")));
    const auto bytes=file.readAll();if(QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())!=sha)return loaded(unavailable(k("bundle_hash_mismatch")));
    const auto doc=QJsonDocument::fromJson(bytes,&pe);const auto bundle=doc.object();
    if(pe.error!=QJsonParseError::NoError||bundle.value(k("schema_version")).toInt()!=1||bundle.value(k("patch")).toString()!=expectedPatch||bundle.value(k("mode"))!=k("KIWI")||!clock(bundle.value(k("generated_at")))||!bundle.value(k("sources")).isArray()||!bundle.value(k("records")).isArray())return loaded(unavailable(k("bundle_scope_or_schema_invalid")));
    const auto records=bundle.value(k("records")).toArray();if(records.isEmpty()||records.size()>200000||bundle.value(k("sources")).toArray().size()>2048)return loaded(unavailable(k("bundle_record_count_invalid")));
    QMap<QString,QJsonObject> sources;
    for(const auto &v:bundle.value(k("sources")).toArray()){
        const auto source=v.toObject();if(sourceOkay(source,expectedPatch))sources.insert(sourceKey(source),cleanSource(source));
    }
    QJsonObject byChampion;QJsonArray generic,conflictingStatistics;
    QHash<QString,QJsonObject> rowsByScopedId;QSet<QString> conflictingIds;int count=0,rejected=0;
    for(const auto &v:records){
        const auto raw=v.toObject();const auto id=raw.value(k("id")).toString(),kind=raw.value(k("kind")).toString();
        const auto scopedId=id+QChar(u'\n')+sourceKey(raw,true);
        const auto source=sources.value(sourceKey(raw,true));
        const auto data=raw.value(k("data")).toObject();const auto champion=raw.value(k("champion_id"));
        const bool hasChampion=integer(champion,1,100000),optionalChampion=champion.isNull()||hasChampion;
        if(id.isEmpty()||id.size()>256||!QStringList{k("champion_tier"),k("item_build"),k("augment_stat"),k("guide_claim")}.contains(kind)
            ||!optionalChampion||((kind==k("champion_tier")||kind==k("item_build"))&&!hasChampion)
            ||raw.value(k("patch")).toString()!=expectedPatch||raw.value(k("mode"))!=k("KIWI")||raw.value(k("evidence_level"))!=k("full_body")
            ||source.isEmpty()||raw.value(k("region"))!=source.value(k("region"))||raw.value(k("source_kind"))!=source.value(k("source_kind"))
            ||!clock(raw.value(k("retrieved_at")))||!safeObject(data)||!recordDataOkay(data)) {++rejected;continue;}
        QJsonObject clean{{k("id"),id},{k("kind"),kind},{k("champion_id"),champion},{k("patch"),expectedPatch},{k("mode"),k("KIWI")},
            {k("region"),raw.value(k("region"))},{k("source_url"),raw.value(k("source_url"))},{k("source_sha256"),raw.value(k("source_sha256"))},
            {k("retrieved_at"),raw.value(k("retrieved_at"))},{k("source_kind"),raw.value(k("source_kind"))},{k("evidence_level"),k("full_body")},{k("data"),cleanDataValue(data)}};
        const auto previous=rowsByScopedId.constFind(scopedId);
        if(previous!=rowsByScopedId.cend()){
            const bool wasStat=previous->value(k("kind"))==k("augment_stat"),isStat=kind==k("augment_stat");
            if((wasStat||isStat)&&(wasStat!=isStat||previous->value(k("champion_id"))!=champion||previous->value(k("data"))!=clean.value(k("data")))){
                if(wasStat&&!conflictingIds.contains(scopedId))conflictingStatistics.append(*previous);
                if(isStat)conflictingStatistics.append(clean);
                conflictingIds.insert(scopedId);
            }
            ++rejected;
            continue;
        }
        if(hasChampion){auto a=byChampion.value(QString::number(champion.toInt())).toArray();a.append(clean);byChampion[QString::number(champion.toInt())]=a;}else generic.append(clean);
        rowsByScopedId.insert(scopedId,clean);++count;
    }
    if(!count)return loaded(unavailable(k("no_verified_full_body_records")));
    QJsonObject sourceIndex;for(auto it=sources.cbegin();it!=sources.cend();++it)sourceIndex[it.key()]=it.value();
    return loaded(QJsonObject{{k("available"),true},{k("status"),k("verified")},{k("patch"),expectedPatch},{k("mode"),k("KIWI")},{k("revision"),sha},
        {k("generated_at"),bundle.value(k("generated_at"))},{k("record_count"),count},{k("rejected_records"),rejected},
        {k("bundle_workspace"),QDir::cleanPath(QFileInfo(root).absoluteFilePath())},
        {k("bundle_pointer_sha256"),QString::fromLatin1(QCryptographicHash::hash(pointerBytes,QCryptographicHash::Sha256).toHex())},
        {k("records_by_champion"),byChampion},{k("generic_records"),generic},{k("source_index"),sourceIndex},
        {k("conflicting_augment_statistics"),conflictingStatistics},
        {k("reference_only"),true},{k("numeric_override"),false},{k("ranking_override"),false}});
}

QJsonObject currentExternalStrategyEvidence(const Catalog &catalog){
    if(catalog.root.trimmed().isEmpty()||catalog.patch.isEmpty())return catalog.strategyEvidence;
    const auto origin=catalog.strategyEvidence.value(k("runtime_reload_origin")).toObject();
    if(origin.value(k("schema_version")).toInt()!=1||origin.value(k("expected_patch"))!=catalog.patch)return catalog.strategyEvidence;
    const auto snapshot=origin.value(k("snapshot")).toObject();
    // A caller clearing/rejecting an optional pack is an explicit pinned
    // override. Only an untouched loader result (including not_installed)
    // authorizes background discovery from the workspace.
    for(const auto &field:{k("available"),k("status"),k("reason"),k("revision"),k("patch"),k("mode")})
        if(snapshot.value(field)!=catalog.strategyEvidence.value(field))return catalog.strategyEvidence;
    const auto root=QDir::cleanPath(QFileInfo(catalog.root).absoluteFilePath());
    const auto seedRoot=origin.value(k("workspace")).toString();
    // An explicitly injected fixture/reference pack from a different workspace
    // is pinned to that source, rather than silently replaced from this root.
    if(seedRoot.isEmpty()||QDir::cleanPath(seedRoot).compare(root,Qt::CaseInsensitive)!=0)return catalog.strategyEvidence;
    const auto patch=catalog.patch;
    const auto entry=refreshEntry(root,patch,catalog.strategyEvidence);
    bool probe=false;QJsonObject pack;
    {
        QMutexLocker lock(&entry->mutex);
        const auto now=steadyMilliseconds();
        if(!entry->refreshing&&now>=entry->nextProbeMs){entry->refreshing=true;entry->nextProbeMs=now+refreshIntervalMs;probe=true;}
        pack=entry->pack;
        const auto status=entry->lastError.isEmpty()?(pack.value(k("available")).toBool()?k("ready"):k("awaiting_verified_bundle"))
            :(pack.value(k("available")).toBool()?k("update_rejected_previous_preserved"):k("update_unavailable"));
        pack[k("runtime_refresh")]=QJsonObject{{k("enabled"),true},{k("interval_ms"),refreshIntervalMs},
            {k("pending"),entry->refreshing},{k("status"),status},
            {k("last_checked_at"),entry->lastCheckedUtc},{k("last_rejection_reason"),entry->lastError},
            {k("previous_revision_retained"),!entry->lastError.isEmpty()&&pack.value(k("available")).toBool()}};
    }
    if(probe)QThreadPool::globalInstance()->start([entry,root,patch]{
        QFile pointer(QDir(root).filePath(k("data/active-strategy-evidence.v1.json")));
        QByteArray pointerDigest;QString error;
        if(!pointer.exists())error=k("not_installed");
        else if(!pointer.open(QIODevice::ReadOnly)||pointer.size()>65536)error=k("pointer_unreadable");
        else pointerDigest=QCryptographicHash::hash(pointer.readAll(),QCryptographicHash::Sha256);
        bool validate=false;
        {
            QMutexLocker lock(&entry->mutex);
            validate=error.isEmpty()&&(pointerDigest!=entry->pointerDigest||entry->pack.isEmpty()
                ||(!entry->lastError.isEmpty()&&steadyMilliseconds()-entry->lastRejectedMs>=rejectedRetryIntervalMs));
        }
        QJsonObject updated;
        if(validate){
            updated=loadExternalStrategyEvidence(root,patch);
            if(!updated.value(k("available")).toBool())error=updated.value(k("reason")).toString(k("unverified_update"));
        }
        QMutexLocker lock(&entry->mutex);
        entry->lastCheckedUtc=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        if(validate||!error.isEmpty()){
            // The loader reopens the pointer. A publisher may replace it between
            // our cheap probe and validation; bind the cache to what was loaded.
            entry->pointerDigest=error.isEmpty()?QByteArray::fromHex(updated.value(k("bundle_pointer_sha256")).toString().toLatin1()):pointerDigest;
            entry->lastError=error;
            if(error.isEmpty())entry->pack=updated;
            else entry->lastRejectedMs=steadyMilliseconds();
        }
        entry->nextProbeMs=steadyMilliseconds()+refreshIntervalMs;entry->refreshing=false;
    },-1);
    return pack;
}

static QJsonObject strategyReferenceProjection(const Catalog &catalog,const GameState &state,const QSet<int> &allowedItems,const QSet<int> &allowedAugments,int offeredAugmentId,bool candidateOnly,bool equipmentOnly){
    const auto pack=currentExternalStrategyEvidence(catalog);
    if(!pack.value(k("available")).toBool())return unavailable(pack.value(k("reason")).toString(k("not_installed")));
    if(state.gameMode!=k("KIWI")||(state.queueId>0&&state.queueId!=2400)
        ||(state.observedPatch!=catalog.patch&&(!equipmentOnly||!purchaseReferenceReady(catalog,state)))
        ||pack.value(k("patch")).toString()!=catalog.patch||pack.value(k("mode"))!=k("KIWI"))return unavailable(k("current_patch_or_mode_unconfirmed"));
    if(!catalog.champions.contains(state.championId))return unavailable(k("current_champion_unknown"));
    if(offeredAugmentId&&(!state.offers.contains(offeredAugmentId)||!catalog.augments.contains(offeredAugmentId)))return unavailable(k("augment_not_actually_offered"));
    QSet<int> actualAugments;for(int id:state.offers)if(catalog.augments.contains(id))actualAugments.insert(id);for(int id:state.selected)if(catalog.augments.contains(id))actualAugments.insert(id);
    const auto augmentKeys=equipmentOnly?QHash<QString,int>{}:augmentKeyIndex(catalog);
    const auto forChampion=pack.value(k("records_by_champion")).toObject().value(QString::number(state.championId)).toArray();
    QJsonArray candidates=forChampion;for(const auto &v:pack.value(k("generic_records")).toArray())candidates.append(v);
    QJsonArray rows,sources;QSet<QString> sourceKeys,regions;int buildCount=0,cardCount=0,claimCount=0,tierCount=0;
    for(const auto &v:candidates){
        if(rows.size()>=24)break;auto row=v.toObject();const auto kind=row.value(k("kind")).toString();auto data=row.value(k("data")).toObject();
        if(equipmentOnly&&kind!=k("item_build"))continue;
        if(row.value(k("champion_id")).isDouble()&&row.value(k("champion_id")).toInt()!=state.championId)continue;
        const auto augment=data.value(k("augment_id"));
        const bool hasAugment=!augment.isUndefined()&&!augment.isNull();
        const int aid=(kind==k("augment_stat")||(kind==k("guide_claim")&&hasAugment))?resolveAugment(catalog,augmentKeys,data):0;
        if(offeredAugmentId&&((kind!=k("augment_stat")&&kind!=k("guide_claim"))||aid!=offeredAugmentId))continue;
        if(kind==k("augment_stat")){
            if(aid<=0||!actualAugments.contains(aid)||(candidateOnly&&!allowedAugments.contains(aid))||cardCount>=8)continue;
            ++cardCount;
        }else if(kind==k("item_build")){
            const auto ids=data.value(k("item_ids")).toArray();bool okay=!ids.isEmpty()&&ids.size()<=6;
            for(const auto &x:ids)okay&=integer(x,1,10000000)&&catalog.items.contains(x.toInt())&&(!candidateOnly||allowedItems.contains(x.toInt()));
            if(equipmentOnly)for(const auto &x:ids)okay&=purchaseReferenceItemCompatible(catalog,state,x.toInt());
            if(!okay||buildCount>=10)continue;++buildCount;
        }else if(kind==k("guide_claim")){
            if((hasAugment&&(aid<=0||!actualAugments.contains(aid)||(candidateOnly&&!allowedAugments.contains(aid))))||claimCount>=4)continue;
            ++claimCount;
        }else if(kind==k("champion_tier")){
            if(offeredAugmentId||tierCount>=1)continue;++tierCount;
        }
        row[k("data")]=data;rows.append(row);regions.insert(row.value(k("region")).toString());
        const auto key=sourceKey(row,true);
        if(!sourceKeys.contains(key)){const auto source=pack.value(k("source_index")).toObject().value(key).toObject();if(!source.isEmpty()){sources.append(source);sourceKeys.insert(key);}}
    }
    QStringList regionList=regions.values();regionList.sort();
    return {{k("available"),!rows.isEmpty()},{k("status"),rows.isEmpty()?k("no_matching_reference"):k("matching_public_reference")},
        {k("patch"),catalog.patch},{k("mode"),k("KIWI")},{k("champion_id"),state.championId},{k("revision"),pack.value(k("revision"))},
        {k("generated_at"),pack.value(k("generated_at"))},{k("records"),rows},{k("sources"),sources},{k("statistics_region_scope"),QJsonArray::fromStringList(regionList)},
        {k("reference_only"),true},{k("numeric_override"),false},{k("ranking_override"),false},{k("real_win_rate"),false},
        {k("provider_scores_are_win_rates"),false},{k("currently_offered_augment_id"),offeredAugmentId?QJsonValue(offeredAugmentId):QJsonValue(QJsonValue::Null)},
        {k("observed_patch"),state.observedPatch},{k("source_patch_matches_current"),state.observedPatch==catalog.patch},
        {k("scope"),equipmentOnly?k("equipment_reference_with_original_source_patch"):k("same_patch_KIWI_public_descriptive_reference")},
        {k("limits"),k("Provider tier/performance/popularity and terminal builds are descriptive references; missing wins/plays stay unknown. Regions are separate. These records do not establish causal benefit, alter legal purchases or offers, replace pinned mechanics, or prove a real win rate.")}};
}

QJsonObject externalStrategyReference(const Catalog &catalog,const GameState &state,const QSet<int> &items,const QSet<int> &augments,int offered,bool candidateOnly){
    return strategyReferenceProjection(catalog,state,items,augments,offered,candidateOnly,false);
}
QJsonObject externalEquipmentReference(const Catalog &catalog,const GameState &state){
    return strategyReferenceProjection(catalog,state,{},{},0,false,true);
}

QJsonObject externalAugmentStrategyReferences(const Catalog &catalog,const GameState &state,const QList<int> &offeredAugmentIds,
    QSet<QString> *incompleteStatisticsBatches){
    // Reject an oversized request instead of silently returning a partial set.
    // The display contract has at most three actual cards, including rerolls.
    if(incompleteStatisticsBatches)incompleteStatisticsBatches->clear();
    if(offeredAugmentIds.isEmpty()||offeredAugmentIds.size()>3)return {};
    const auto pack=currentExternalStrategyEvidence(catalog);
    QJsonObject out,error;
    if(!pack.value(k("available")).toBool())error=unavailable(pack.value(k("reason")).toString(k("not_installed")));
    else if(state.gameMode!=k("KIWI")||(state.queueId>0&&state.queueId!=2400)||state.observedPatch!=catalog.patch||pack.value(k("patch")).toString()!=catalog.patch||pack.value(k("mode"))!=k("KIWI"))error=unavailable(k("current_patch_or_mode_unconfirmed"));
    else if(!catalog.champions.contains(state.championId))error=unavailable(k("current_champion_unknown"));
    if(!error.isEmpty()){
        for(int id:offeredAugmentIds)out[QString::number(id)]=error;
        return out;
    }
    struct Projection {
        QJsonArray rows,sources;
        QSet<QString> sourceKeys,regions;
        int cardCount=0,claimCount=0;
    };
    QMap<int,Projection> projections;
    for(int id:offeredAugmentIds){
        if(id<=0||!state.offers.contains(id)||!catalog.augments.contains(id))out[QString::number(id)]=unavailable(k("augment_not_actually_offered"));
        else projections[id]=Projection{};
    }
    if(projections.isEmpty())return out;
    // All identity resolution and row/source reads below belong to this one
    // snapshot. Keep collisions, original candidate order and per-card caps.
    QSet<QString> requestedKeys;
    for(int id:projections.keys()){
        const auto augment=catalog.augments.value(id);
        requestedKeys.insert(normalizedAugmentKey(augment,"key"));
        requestedKeys.insert(normalizedAugmentKey(augment,"identity"));
    }
    requestedKeys.remove(QString());
    const auto augmentKeys=augmentKeyIndex(catalog,&requestedKeys);
    if(incompleteStatisticsBatches){
        for(const auto &value:pack.value(k("conflicting_augment_statistics")).toArray()){
            const auto row=value.toObject();const auto champion=row.value(k("champion_id"));
            if(champion.isDouble()&&champion.toInt()!=state.championId)continue;
            auto data=row.value(k("data")).toObject();
            if(projections.contains(resolveAugment(catalog,augmentKeys,data)))incompleteStatisticsBatches->insert(augmentStatisticsBatchKey(row));
        }
    }
    QJsonArray candidates=pack.value(k("records_by_champion")).toObject().value(QString::number(state.championId)).toArray();
    for(const auto &v:pack.value(k("generic_records")).toArray())candidates.append(v);
    const auto sourceIndex=pack.value(k("source_index")).toObject();
    for(const auto &v:candidates){
        auto row=v.toObject();const auto kind=row.value(k("kind")).toString();
        if(kind!=k("augment_stat")&&kind!=k("guide_claim"))continue;
        if(row.value(k("champion_id")).isDouble()&&row.value(k("champion_id")).toInt()!=state.championId)continue;
        auto data=row.value(k("data")).toObject();const auto augment=data.value(k("augment_id"));
        const bool hasAugment=!augment.isUndefined()&&!augment.isNull();
        if(kind==k("guide_claim")&&!hasAugment)continue;
        const int aid=resolveAugment(catalog,augmentKeys,data);
        auto it=projections.find(aid);if(it==projections.end())continue;
        auto &projection=it.value();if(projection.rows.size()>=24)continue;
        if(kind==k("augment_stat")){
            if(projection.cardCount>=8){
                if(incompleteStatisticsBatches)incompleteStatisticsBatches->insert(augmentStatisticsBatchKey(row));
                continue;
            }
            ++projection.cardCount;
        }else{
            if(projection.claimCount>=4)continue;++projection.claimCount;
        }
        row[k("data")]=data;projection.rows.append(row);projection.regions.insert(row.value(k("region")).toString());
        const auto key=sourceKey(row,true);
        if(!projection.sourceKeys.contains(key)){
            const auto source=sourceIndex.value(key).toObject();
            if(!source.isEmpty()){projection.sources.append(source);projection.sourceKeys.insert(key);}
        }
    }
    for(auto it=projections.cbegin();it!=projections.cend();++it){
        const auto &projection=it.value();QStringList regions=projection.regions.values();regions.sort();
        out[QString::number(it.key())]=QJsonObject{
            {k("available"),!projection.rows.isEmpty()},{k("status"),projection.rows.isEmpty()?k("no_matching_reference"):k("matching_public_reference")},
            {k("patch"),catalog.patch},{k("mode"),k("KIWI")},{k("champion_id"),state.championId},{k("revision"),pack.value(k("revision"))},
            {k("generated_at"),pack.value(k("generated_at"))},{k("records"),projection.rows},{k("sources"),projection.sources},{k("statistics_region_scope"),QJsonArray::fromStringList(regions)},
            {k("reference_only"),true},{k("numeric_override"),false},{k("ranking_override"),false},{k("real_win_rate"),false},
            {k("provider_scores_are_win_rates"),false},{k("currently_offered_augment_id"),it.key()},
            {k("observed_patch"),state.observedPatch},{k("source_patch_matches_current"),state.observedPatch==catalog.patch},
            {k("scope"),k("same_patch_KIWI_public_descriptive_reference")},
            {k("limits"),k("Provider tier/performance/popularity and terminal builds are descriptive references; missing wins/plays stay unknown. Regions are separate. These records do not establish causal benefit, alter legal purchases or offers, replace pinned mechanics, or prove a real win rate.")}};
    }
    return out;
}
