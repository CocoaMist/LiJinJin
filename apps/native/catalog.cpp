#include "catalog.h"
#include "combat.h"
#include "external_strategy_reference.h"
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QSet>

QJsonDocument loadJson(const QString &file, QString *error) {
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法读取：%1").arg(file);
        return {};
    }
    QJsonParseError pe;
    auto doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError && error) *error = pe.errorString();
    return doc;
}
QString plainText(QString s) {
    s.replace(QRegularExpression(QStringLiteral("<br\\s*/?>")), QStringLiteral("\n"));
    s.remove(QRegularExpression(QStringLiteral("<[^>]+>")));
    s.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    s.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    return s;
}
bool Catalog::load(const QString &workspace, QString *error) {
    root = workspace;champions.clear();augments.clear();items.clear();mechanics={};combat.reset();runtimeManifest={};strategyEvidence={};patch.clear();catalogPath.clear();
    if(error)error->clear();
    auto fail=[&](const QString &message){if(error)*error=message;champions.clear();augments.clear();items.clear();mechanics={};combat.reset();return false;};
    const QString pointerPath=root+QStringLiteral("/data/active-runtime-catalog.v1.json");
    const bool versioned=QFileInfo::exists(pointerPath);
    QString base=root+QStringLiteral("/research/2026-10-20");
    QString expectedPatch=QStringLiteral("16.20");
    QString expectedCombatSha256;
    if(versioned){
        const auto pointer=loadJson(pointerPath,error).object();
        if(pointer.value(QStringLiteral("schema_version")).toInt()!=1||pointer.value(QStringLiteral("catalog_path")).toString().isEmpty())return fail(QStringLiteral("目录版本指针无效"));
        expectedCombatSha256=pointer.value(QStringLiteral("combat_sha256")).toString();
        if(pointer.value(QStringLiteral("combat_path")).toString()!=QStringLiteral("data/combat.v1.json")
            ||!QRegularExpression(QStringLiteral("\\A[0-9a-f]{64}\\z")).match(expectedCombatSha256).hasMatch())return fail(QStringLiteral("数值规则版本指针无效"));
        expectedPatch=pointer.value(QStringLiteral("patch")).toString();
        if(!QRegularExpression(QStringLiteral("\\A[0-9]{1,3}\\.[0-9]{1,3}\\z")).match(expectedPatch).hasMatch())return fail(QStringLiteral("目录版本格式无效"));
        const QString canonicalRoot=QFileInfo(root).canonicalFilePath();
        base=QFileInfo(QDir(root).absoluteFilePath(pointer.value(QStringLiteral("catalog_path")).toString())).canonicalFilePath();
        if(canonicalRoot.isEmpty()||base.isEmpty()||!base.startsWith(canonicalRoot+QChar(u'/'),Qt::CaseInsensitive))return fail(QStringLiteral("目录路径超出工作区"));
        QFile manifestFile(base+QStringLiteral("/manifest.json"));if(!manifestFile.open(QIODevice::ReadOnly))return fail(QStringLiteral("缺少目录清单"));
        const auto bytes=manifestFile.readAll();
        if(QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())!=pointer.value(QStringLiteral("manifest_sha256")).toString())return fail(QStringLiteral("目录清单校验不一致"));
        runtimeManifest=QJsonDocument::fromJson(bytes).object();
        if(runtimeManifest.value(QStringLiteral("patch")).toString()!=expectedPatch||!runtimeManifest.value(QStringLiteral("files")).isArray())return fail(QStringLiteral("目录清单版本不一致"));
        QSet<QString> verified;
        for(const auto &v:runtimeManifest.value(QStringLiteral("files")).toArray()){
            const auto row=v.toObject();const auto relative=row.value(QStringLiteral("path")).toString();
            const auto path=QFileInfo(QDir(base).absoluteFilePath(relative)).canonicalFilePath();
            if(relative.isEmpty()||path.isEmpty()||!path.startsWith(base+QChar(u'/'),Qt::CaseInsensitive)||verified.contains(relative))return fail(QStringLiteral("目录清单路径无效"));
            QFile file(path);if(!file.open(QIODevice::ReadOnly)||QString::fromLatin1(QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256).toHex())!=row.value(QStringLiteral("sha256")).toString())return fail(QStringLiteral("目录文件校验不一致"));
            verified.insert(relative);
        }
        for(const auto &required:{QStringLiteral("summary.json"),QStringLiteral("catalog/champions.zh-CN.json"),QStringLiteral("catalog/augments.KIWI.zh-CN.json"),QStringLiteral("catalog/items.extended.zh-CN.json")})if(!verified.contains(required))return fail(QStringLiteral("目录清单缺少必需文件"));
    }
    catalogPath=base;base+=QChar(u'/');
    auto summary = loadJson(base + QStringLiteral("summary.json"), error).object();
    patch = summary.value(QStringLiteral("patch")).toString();
    if(patch!=expectedPatch)return fail(QStringLiteral("目录摘要版本不一致"));
    auto index = [&](const QString &name, const QString &key, QMap<int,QJsonObject> &out) {
        auto doc = loadJson(base + name, error);
        if (!doc.isArray()) return false;
        out.clear();
        for (const auto &v : doc.array()) {
            const auto o = v.toObject();
            const int id = o.value(key).toInt();
            if (id <= 0 || out.contains(id)) return false;
            out.insert(id, o);
        }
        return true;
    };
    if (!index(QStringLiteral("catalog/champions.zh-CN.json"), QStringLiteral("champion_id"), champions)
        || !index(QStringLiteral("catalog/augments.KIWI.zh-CN.json"), QStringLiteral("augment_id"), augments)
        || !index(QStringLiteral("catalog/items.extended.zh-CN.json"), QStringLiteral("item_id"), items)) return fail(QStringLiteral("目录身份重复或不完整"));
    const auto legacyMechanics=loadJson(root+QStringLiteral("/data/mechanics.v1.json")).object();
    if(legacyMechanics.value(QStringLiteral("patch")).toString()==patch)mechanics=legacyMechanics.value(QStringLiteral("rules")).toArray();
    combat=std::make_shared<CombatDatabase>();
    combat->audit.insert(QStringLiteral("source_status"),loadJson(root+QStringLiteral("/data/source-status.v1.json")).object());
    if(!combat->load(root,error)||combat->patch!=patch)return fail(QStringLiteral("目录与数值规则版本不一致"));
    // revision hashes the exact bytes parsed by CombatDatabase::load.
    if(versioned&&combat->revision!=expectedCombatSha256)return fail(QStringLiteral("数值规则校验不一致"));
    auto sameIds=[](const auto &a,const auto &b){return a.keys()==b.keys();};
    if(!sameIds(champions,combat->champions)||!sameIds(augments,combat->augments)||!sameIds(items,combat->items))return fail(QStringLiteral("目录与数值身份集合不一致"));
    const auto counts=runtimeManifest.value(QStringLiteral("counts")).toObject();
    const bool ok=versioned?champions.size()==counts.value(QStringLiteral("champions")).toInt(-1)&&augments.size()==counts.value(QStringLiteral("augments")).toInt(-1)&&items.size()==counts.value(QStringLiteral("items")).toInt(-1)
        :champions.size()==173&&augments.size()==223&&items.size()==870&&!mechanics.isEmpty();
    if(ok)for(const auto &v:combat->audit.value(QStringLiteral("verified_description_overrides")).toArray()){const auto row=v.toObject();const int id=row.value(QStringLiteral("augment_id")).toInt();if(augments.contains(id)){augments[id][QStringLiteral("description_original_unversioned")]=augments[id].value(QStringLiteral("description"));augments[id][QStringLiteral("description")]=row.value(QStringLiteral("description"));augments[id][QStringLiteral("source_conflict")]=row;}}
    if(!ok)return fail(QStringLiteral("目录数量与版本清单不一致"));
    strategyEvidence=loadExternalStrategyEvidence(root,patch);
    // Optional continuity data is nonfatal and never changes modelPatchMatches.
    const auto continuityPointer=loadJson(root+QStringLiteral("/data/purchase-reference-continuity.v1.json")).object();
    const auto relative=continuityPointer.value(QStringLiteral("path")).toString();
    const auto canonicalRoot=QFileInfo(root).canonicalFilePath();
    const auto continuityPath=QFileInfo(QDir(root).absoluteFilePath(relative)).canonicalFilePath();
    if(continuityPointer.value(QStringLiteral("schema_version")).toInt()==1&&!relative.isEmpty()
        &&!canonicalRoot.isEmpty()&&continuityPath.startsWith(canonicalRoot+QChar(u'/'),Qt::CaseInsensitive)){
        QFile file(continuityPath);
        if(file.open(QIODevice::ReadOnly)&&file.size()<=262144){
            const auto bytes=file.readAll();const auto bridge=QJsonDocument::fromJson(bytes).object();
            if(QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())==continuityPointer.value(QStringLiteral("sha256"))
                &&bridge.value(QStringLiteral("schema_version")).toInt()==1
                &&bridge.value(QStringLiteral("scope"))==QStringLiteral("unchanged_client_itemdata_recipe_reference_only")
                &&bridge.value(QStringLiteral("source_patch"))==patch&&bridge.value(QStringLiteral("mode"))==QStringLiteral("KIWI")
                &&bridge.value(QStringLiteral("combat_revision"))==combat->revision
                &&bridge.value(QStringLiteral("combat_model_upgraded"))==QJsonValue(false)
                &&QRegularExpression(QStringLiteral("\\A[0-9]+\\.[0-9]+\\z")).match(bridge.value(QStringLiteral("observed_patch")).toString()).hasMatch()){
                QSet<int> ids;bool valid=true;
                for(const auto &v:bridge.value(QStringLiteral("reference_item_ids")).toArray()){
                    const int id=v.toInt();if(!v.isDouble()||v.toDouble()!=id||!items.contains(id)||ids.contains(id)){valid=false;break;}ids.insert(id);
                }
                for(int id:ids)for(int part:combat->items.value(id).from)if(!ids.contains(part))valid=false;
                if(valid&&!ids.isEmpty())runtimeManifest[QStringLiteral("purchase_reference_continuity")]=bridge;
            }
        }
    }
    return true;
}
bool Catalog::rewardOnlyAugment(int id)const{
    const auto entry=augments.value(id);if(entry.isEmpty())return false;
    return entry.value(QStringLiteral("reward_only")).toBool()||(patch==QStringLiteral("16.21")&&QSet<int>{3170,3171,3172,3173,3176}.contains(id));
}
QString Catalog::offerEligibilityReason(int id)const{
    const auto entry=augments.value(id);if(entry.isEmpty()||entry.value(QStringLiteral("removed")).toBool())return QStringLiteral("removed_or_unknown_augment");
    if(entry.value(QStringLiteral("enabled")).isBool()&&!entry.value(QStringLiteral("enabled")).toBool())return QStringLiteral("disabled_in_local_resource");
    if(rewardOnlyAugment(id))return QStringLiteral("reward_only_not_ordinary_offer");
    if(patch!=QStringLiteral("16.20")&&!entry.value(QStringLiteral("ordinary_offer_eligible")).toBool())return QStringLiteral("ordinary_offer_eligibility_unknown");
    return {};
}
bool Catalog::ordinaryOfferEligible(int id)const{return offerEligibilityReason(id).isEmpty();}
int Catalog::championId(const QString &name) const {
    QString value = name;
    value.remove(QStringLiteral("game_character_displayname_"));
    for (auto it = champions.cbegin(); it != champions.cend(); ++it) {
        if (it.value().value(QStringLiteral("key")).toString().compare(value, Qt::CaseInsensitive) == 0
            || it.value().value(QStringLiteral("name_zh_cn")).toString() == value) return it.key();
    }
    return 0;
}
QString Catalog::championName(int id) const {
    return champions.value(id).value(QStringLiteral("name_zh_cn")).toString(QStringLiteral("未知英雄"));
}
QString Catalog::itemName(int id) const {
    return items.value(id).value(QStringLiteral("name_zh_cn")).toString(QStringLiteral("未知装备 %1").arg(id));
}
