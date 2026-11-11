#include "hero_execution.h"
#include "combat_observation_scope.h"
#include "analysis.h"
#include "combat.h"
#include "continuous_action_state.h"
#include "continuous_sandbox.h"
#include "decision_context.h"
#include "headliner.h"
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {
QString k(const char *s) { return QString::fromLatin1(s); }
constexpr int candidateLimit = 12, eventLimit = 24, traceLimit = 48;
bool finite(const QJsonValue &v, double low = 0, double high = 1e9) {
    return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= low && v.toDouble() <= high;
}
QJsonArray codes(const QSet<QString> &values) {
    auto list = values.values(); std::sort(list.begin(), list.end()); return QJsonArray::fromStringList(list);
}
QJsonArray ids(const QList<int> &values) { QJsonArray out; for (int id : values) out.append(id); return out; }
QJsonObject baseResult(const Catalog &c, int champion) {
    return {{k("schema_version"), 1}, {k("champion_id"), champion}, {k("patch"), c.patch},
        {k("combat_revision"), c.combat ? c.combat->revision : QString()}, {k("available"), false},
        {k("complete"), false}, {k("damage_ranking_available"), false}, {k("best_candidate_id"), QString()},
        {k("candidates"), QJsonArray{}}, {k("global_optimum"), false}, {k("actual_win_probability"), QJsonValue::Null},
        {k("real_live_execution_verified"), false}, {k("server_mechanics_certified"), false},
        {k("search_scope"), k("finite_explicit_sequences_same_fixed_scenario_modeled_damage_only")},
        {k("budget_contract"), k("checked_between_bounded_native_calls_no_partial_winner_not_preemptive_wall_clock_deadline")}};
}
QString playerKey(const QJsonObject &p) {
    return p.value(k("player_key")).toString(p.value(k("team")).toString() + k(":") + QString::number(p.value(k("champion_id")).toInt()) + k(":1"));
}
QStringList abilitySlots() { return {k("Q"), k("W"), k("E"), k("R")}; }

// Optional prose is loaded once per immutable catalog revision. No network or
// provider request is performed. Input proofs must match the already verified
// numeric revision and champion file in Catalog's manifest.
QJsonObject playbookBundle(const Catalog &c) {
    if (c.root.isEmpty() || !c.combat) return {};
    static QMutex mutex; static QString previous; static QJsonObject saved;
    const auto digestPattern = QRegularExpression(k("\\A[0-9a-f]{64}\\z"));
    QString expectedHash = k("117379d71abb61a259bbc2843c763c53889254fbe19ddde4dc803e414856cc70"), pointerIdentity;
    QString path;
    const QString pointerPath = QDir(c.root).absoluteFilePath(k("data/active-hero-playbooks.v1.json"));
    if (QFileInfo::exists(pointerPath)) {
        QFile pointerFile(pointerPath); if (pointerFile.size() <= 0 || pointerFile.size() > 16 * 1024 || !pointerFile.open(QIODevice::ReadOnly)) return {};
        const auto bytes = pointerFile.readAll(); const auto pointer = QJsonDocument::fromJson(bytes).object();
        expectedHash = pointer.value(k("bundle_sha256")).toString();
        const auto relative = pointer.value(k("bundle_path")).toString();
        if (pointer.value(k("schema_version")) != QJsonValue(1) || pointer.value(k("patch")).toString() != c.patch || pointer.value(k("scope")).toString() != k("base_champion_rules_reference")
            || !digestPattern.match(expectedHash).hasMatch() || relative != k("data/hero-playbook-revisions/") + expectedHash + k(".json")) return {};
        const auto canonicalRoot = QFileInfo(c.root).canonicalFilePath(); path = QFileInfo(QDir(c.root).absoluteFilePath(relative)).canonicalFilePath();
        if (canonicalRoot.isEmpty() || path.isEmpty() || !path.startsWith(canonicalRoot + QChar(u'/'), Qt::CaseInsensitive)) return {};
        pointerIdentity = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    } else {
        path = QDir(c.root).absoluteFilePath(k("data/hero-playbooks.v1.16.21.json"));
        if (!QFileInfo::exists(path)) path = QDir(c.root).absoluteFilePath(k("research/2026-10-26/hero-playbooks/evidence.json"));
    }
    const QFileInfo info(path);
    const auto manifestIdentity = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(c.runtimeManifest).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    const QString cacheKey = path + k("|") + pointerIdentity + k("|") + manifestIdentity + k("|") + c.patch + k("|") + c.combat->revision + k("|")
        + QString::number(info.size()) + k("|") + QString::number(info.lastModified().toMSecsSinceEpoch());
    QMutexLocker lock(&mutex);
    if (previous == cacheKey) return saved;
    previous = cacheKey; saved = {};
    if (!info.exists() || info.size() <= 0 || info.size() > 8 * 1024 * 1024) return {};
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
    const auto bytes = file.readAll();
    if (QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) != expectedHash) return {};
    const auto bundle = QJsonDocument::fromJson(bytes).object();
    if (bundle.value(k("schema_version")) != QJsonValue(1) || bundle.value(k("patch")).toString() != c.patch
        || bundle.value(k("applicability")).toString() != k("base_champion_rules_reference")) return {};
    bool numericBound = false, catalogBound = false;
    for (const auto &raw : bundle.value(k("input_proofs")).toArray()) {
        const auto proof = raw.toObject(); const auto name = proof.value(k("local_path")).toString();
        if (name == k("data/combat.v1.json") && proof.value(k("sha256")).toString() == c.combat->revision) numericBound = true;
        if (name.endsWith(k("/catalog/champions.zh-CN.json"))) for (const auto &entry : c.runtimeManifest.value(k("files")).toArray()) {
            const auto row = entry.toObject();
            if (row.value(k("path")).toString() == k("catalog/champions.zh-CN.json") && row.value(k("sha256")) == proof.value(k("sha256"))) catalogBound = true;
        }
    }
    if (numericBound && catalogBound && bundle.value(k("heroes")).toArray().size() <= 200) saved = bundle;
    return saved;
}
// A separately published source/conditional reference. None of these fields
// enter the numeric ledger, action admission, or executable candidate search.
bool referenceFlags(const QJsonObject &o) {
    return o.value(k("reference_only")) == QJsonValue(true)
        && o.value(k("execution_eligible")) == QJsonValue(false)
        && o.value(k("game_verified")) == QJsonValue(false)
        && o.value(k("full_mechanism_understanding_verified")) == QJsonValue(false);
}
bool referenceText(const QJsonValue &v, int maximum = 24000) {
    return v.isString() && !v.toString().trimmed().isEmpty() && v.toString().size() <= maximum;
}
bool referenceStrings(const QJsonValue &v, int maximum = 100, bool allowEmpty = false) {
    if (!v.isArray() || v.toArray().size() > maximum || (!allowEmpty && v.toArray().isEmpty())) return false;
    for (const auto &text : v.toArray()) if (!referenceText(text)) return false;
    return true;
}
bool referenceHashes(const QJsonValue &v) {
    if (!v.isObject() || v.toObject().isEmpty() || v.toObject().size() > 100) return false;
    const auto o = v.toObject();
    const QRegularExpression digest(k("\\A[0-9a-f]{64}\\z"));
    for (auto it = o.begin(); it != o.end(); ++it)
        if (it.key().isEmpty() || it.key().size() > 1000 || !it.value().isString() || !digest.match(it.value().toString()).hasMatch()) return false;
    return true;
}
bool firstPassReference(const QJsonValue &v) {
    if (!v.isObject()) return false;
    const auto o = v.toObject();
    for (const auto &name : {k("identity"), k("resource_loop"), k("win_condition"), k("aram_implication")}) if (!referenceText(o.value(name))) return false;
    for (const auto &name : {k("combat_roles"), k("power_windows"), k("failure_modes"), k("kiwi_unknowns"), k("knowledge_gaps")}) if (!referenceStrings(o.value(name))) return false;
    if (!o.value(k("skills")).isArray() || o.value(k("skills")).toArray().isEmpty() || o.value(k("skills")).toArray().size() > 12) return false;
    QSet<QString> seenSlots;
    for (const auto &raw : o.value(k("skills")).toArray()) {
        const auto skill = raw.toObject(); const auto slot = skill.value(k("slot")).toString();
        if (!raw.isObject() || !QStringList{k("P"), k("Q"), k("W"), k("E"), k("R")}.contains(slot) || seenSlots.contains(slot)
            || !referenceText(skill.value(k("tactical_use"))) || !referenceText(skill.value(k("important_condition")))) return false;
        seenSlots.insert(slot);
    }
    const auto sequence = o.value(k("sequence")).toObject();
    return o.value(k("sequence")).isObject() && referenceStrings(sequence.value(k("steps"))) && referenceText(sequence.value(k("purpose")))
        && referenceStrings(sequence.value(k("preconditions"))) && referenceStrings(sequence.value(k("abort_if")))
        && sequence.value(k("execution_eligible")) == QJsonValue(false);
}
bool reviewedReference(const QJsonValue &v, int &facts, int &questions) {
    if (!v.isObject()) return false;
    const auto o = v.toObject(), provenance = o.value(k("provenance")).toObject();
    const QRegularExpression digest(k("\\A[0-9a-f]{64}\\z"));
    if (!referenceText(o.value(k("role_and_resource_text")), 128 * 1024) || !referenceText(o.value(k("guide_markdown")), 512 * 1024)
        || !digest.match(provenance.value(k("review_status_sha256")).toString()).hasMatch() || !referenceHashes(provenance.value(k("files_sha256")))
        || !o.value(k("current_facts")).isArray() || o.value(k("current_facts")).toArray().isEmpty() || o.value(k("current_facts")).toArray().size() > 200
        || !o.value(k("open_questions")).isArray() || o.value(k("open_questions")).toArray().isEmpty() || o.value(k("open_questions")).toArray().size() > 100) return false;
    QSet<QString> factIds, groupIds;
    for (const auto &raw : o.value(k("current_facts")).toArray()) {
        const auto fact = raw.toObject(); const auto id = fact.value(k("fact_id")).toString();
        if (!raw.isObject() || !referenceText(fact.value(k("fact_id")), 200) || factIds.contains(id)
            || !referenceText(fact.value(k("slot")), 100) || !referenceText(fact.value(k("claim")))
            || !referenceText(fact.value(k("caveat"))) || !referenceText(fact.value(k("support"))) || !referenceStrings(fact.value(k("source_refs")))) return false;
        factIds.insert(id); ++facts;
    }
    for (const auto &raw : o.value(k("open_questions")).toArray()) {
        const auto group = raw.toObject(); const auto id = group.value(k("group_id")).toString();
        if (!raw.isObject() || !referenceText(group.value(k("group_id")), 200) || groupIds.contains(id)
            || !referenceText(group.value(k("title"))) || !referenceStrings(group.value(k("questions")))) return false;
        groupIds.insert(id); ++questions;
    }
    return true;
}
bool individualReference(const QJsonValue &v, int champion, const QString &revision) {
    if (!v.isObject()) return false;
    const auto o=v.toObject(), analysis=o.value(k("analysis")).toObject(), provenance=o.value(k("provenance")).toObject();
    const QRegularExpression digest(k("\\A[0-9a-f]{64}\\z"));
    if (!referenceFlags(o) || o.value(k("champion_id"))!=QJsonValue(champion) || o.value(k("combat_revision"))!=revision
        || o.value(k("status"))!=k("codex_source_analysis")
        || provenance.value(k("review_independence"))!=k("single_agent_source_analysis")
        || provenance.value(k("ds_output_used"))!=QJsonValue(false)
        || !digest.match(provenance.value(k("source_pack_sha256")).toString()).hasMatch()
        || !referenceText(provenance.value(k("source_pack_path")),1000)) return false;
    for (const auto &flag:{k("independent_source_review_verified"),k("semantic_entailment_verified")})
        if (o.value(flag)!=QJsonValue(false)) return false;
    const auto skills=analysis.value(k("skills")).toArray(); QSet<QString> seenSlots;
    if (skills.size()!=5 || !referenceText(analysis.value(k("identity"))) || analysis.value(k("engine_gaps")).toArray().size()<3
        || !referenceStrings(analysis.value(k("observations_required")))) return false;
    for (const auto &value:skills) {
        const auto skill=value.toObject(); const auto slot=skill.value(k("slot")).toString();
        if (!QStringList{k("P"),k("Q"),k("W"),k("E"),k("R")}.contains(slot) || seenSlots.contains(slot)
            || !referenceText(skill.value(k("mechanism"))) || !referenceStrings(skill.value(k("conditions")))
            || !referenceStrings(skill.value(k("failure_cases"))) || skill.value(k("evidence")).toArray().isEmpty()) return false;
        seenSlots.insert(slot);
        for (const auto &raw:skill.value(k("evidence")).toArray()) {
            const auto evidence=raw.toObject();
            if (!referenceText(evidence.value(k("doc_id")),100) || !referenceText(evidence.value(k("quote")),700)
                || !referenceText(evidence.value(k("path")),1000) || !referenceText(evidence.value(k("json_pointer")),2000)
                || !digest.match(evidence.value(k("sha256")).toString()).hasMatch()) return false;
        }
    }
    for (const auto &field:{k("growth"),k("equipment")}) {
        const auto section=analysis.value(field).toObject();
        if (!referenceText(section.value(k("reasoning"))) || !referenceStrings(section.value(k("conditions")))
            || section.value(k("evidence")).toArray().isEmpty()) return false;
    }
    return true;
}
bool containedReferencePath(const QString &root, const QString &relative, QString &path) {
    const auto canonicalRoot = QFileInfo(root).canonicalFilePath();
    path = QFileInfo(QDir(root).absoluteFilePath(relative)).canonicalFilePath();
    return !canonicalRoot.isEmpty() && !path.isEmpty() && path.startsWith(canonicalRoot + QChar(u'/'), Qt::CaseInsensitive);
}
QByteArray boundedReferenceFile(const QString &path, qint64 maximum) {
    QFile file(path); if (file.size() <= 0 || file.size() > maximum || !file.open(QIODevice::ReadOnly)) return {};
    const auto bytes = file.read(maximum + 1); return bytes.size() <= maximum ? bytes : QByteArray{};
}
QJsonObject championUnderstandingBundle(const Catalog &c) {
    auto rejected=[](const char *stage)->QJsonObject {
        if(qEnvironmentVariableIsSet("LIXIN_REFERENCE_DIAGNOSTICS"))std::fprintf(stderr,"Champion reference rejected: %s\n",stage);
        return {};
    };
    if (c.root.isEmpty() || !c.combat) return rejected("catalog");
    QString pointerPath, basePointerPath;
    if (!containedReferencePath(c.root, k("data/active-champion-understanding.v1.json"), pointerPath)
        || !containedReferencePath(c.root, k("data/active-hero-playbooks.v1.json"), basePointerPath)) return rejected("pointer paths");
    const auto pointerBytes = boundedReferenceFile(pointerPath, 16 * 1024), basePointerBytes = boundedReferenceFile(basePointerPath, 16 * 1024);
    if (pointerBytes.isEmpty() || basePointerBytes.isEmpty()) return rejected("pointer bytes");
    const auto pointer = QJsonDocument::fromJson(pointerBytes).object(), basePointer = QJsonDocument::fromJson(basePointerBytes).object();
    const auto expected = pointer.value(k("bundle_sha256")).toString(), source = pointer.value(k("source_bundle_sha256")).toString();
    const QRegularExpression digest(k("\\A[0-9a-f]{64}\\z"));
    if (pointer.value(k("schema_version")) != QJsonValue(1) || pointer.value(k("scope")) != k("champion_understanding_reference")
        || pointer.value(k("patch")) != c.patch || !digest.match(expected).hasMatch() || !digest.match(source).hasMatch()
        || pointer.value(k("revision")) != expected || !referenceFlags(pointer)
        || pointer.value(k("combat_revision")) != c.combat->revision || !referenceText(pointer.value(k("content_version")), 200)
        || pointer.value(k("content_version")) != c.runtimeManifest.value(k("content_version"))
        || basePointer.value(k("schema_version")) != QJsonValue(1) || basePointer.value(k("patch")) != c.patch
        || basePointer.value(k("scope")) != k("base_champion_rules_reference") || basePointer.value(k("bundle_sha256")) != source
        || basePointer.value(k("bundle_path")) != k("data/hero-playbook-revisions/") + source + k(".json")
        || pointer.value(k("bundle_path")) != k("data/champion-understanding-revisions/") + expected + k(".json")) return rejected("pointer binding");
    QString path;
    if (!containedReferencePath(c.root, pointer.value(k("bundle_path")).toString(), path)) return rejected("bundle path");
    const auto bytes = boundedReferenceFile(path, 8 * 1024 * 1024);
    if (bytes.isEmpty() || QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) != expected) return rejected("bundle bytes");
    // Hash bytes on every lookup, including both pointers. A replacement with
    // identical size and timestamp must still invalidate the public reference.
    const auto key = c.root + k("|") + expected + k("|") + c.combat->revision + k("|")
        + QString::fromLatin1(QCryptographicHash::hash(pointerBytes + basePointerBytes + QJsonDocument(c.runtimeManifest).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    // The base pack is frozen prose ancestry, not a current numeric authority.
    // Its historical combat input may differ after a reviewed numeric update.
    // Verify the frozen bytes here; this new pack independently pins the active
    // combat revision above. playbookBundle still refuses stale numeric tips.
    QString ancestryPath;
    if (!containedReferencePath(c.root,basePointer.value(k("bundle_path")).toString(),ancestryPath)) return rejected("ancestry path");
    const auto ancestryBytes=boundedReferenceFile(ancestryPath,8*1024*1024);
    if (ancestryBytes.isEmpty() || QString::fromLatin1(QCryptographicHash::hash(ancestryBytes,QCryptographicHash::Sha256).toHex())!=source) return rejected("ancestry bytes");
    const auto ancestry=QJsonDocument::fromJson(ancestryBytes).object();
    if(ancestry.value(k("schema_version"))!=QJsonValue(1) || ancestry.value(k("applicability"))!=k("base_champion_rules_reference")
        || ancestry.value(k("patch"))!=c.patch || ancestry.value(k("heroes")).toArray().isEmpty()
        || ancestry.value(k("heroes")).toArray().size()>200) return rejected("ancestry contract");
    bool catalogBound=false;
    for (const auto &raw:ancestry.value(k("input_proofs")).toArray()) {
        const auto proof=raw.toObject();
        if (!proof.value(k("local_path")).toString().endsWith(k("/catalog/champions.zh-CN.json"))) continue;
        for (const auto &entry:c.runtimeManifest.value(k("files")).toArray()) {
            const auto row=entry.toObject();
            if (row.value(k("path"))==k("catalog/champions.zh-CN.json") && row.value(k("sha256"))==proof.value(k("sha256"))) catalogBound=true;
        }
    }
    if (!catalogBound) return rejected("ancestry catalog");
    static QMutex mutex; static QString previous; static QJsonObject saved;
    QMutexLocker lock(&mutex); if (previous == key) return saved;
    previous = key; saved = {};
    const auto bundle = QJsonDocument::fromJson(bytes).object();
    for (const auto &name : {k("schema_version"), k("patch"), k("scope"), k("source_bundle_sha256"), k("combat_revision"), k("content_version")})
        if (bundle.value(name) != pointer.value(name)) return rejected("header");
    if (!referenceFlags(bundle) || !bundle.value(k("heroes")).isArray() || bundle.value(k("heroes")).toArray().size() != 173) return rejected("roster");
    const auto provenance = bundle.value(k("source_provenance")).toObject();
    if (!referenceText(provenance.value(k("research_path")), 1000) || !referenceHashes(provenance.value(k("files_sha256")))
        || !provenance.value(k("official_sources")).isArray() || provenance.value(k("official_sources")).toArray().size() != 173) return rejected("provenance");
    QSet<QString> sourceIds;
    for (const auto &raw : provenance.value(k("official_sources")).toArray()) {
        const auto row = raw.toObject(); const auto id = row.value(k("source_id")).toString();
        if (!raw.isObject() || !referenceText(row.value(k("source_id")), 200) || sourceIds.contains(id)
            || !referenceText(row.value(k("url")), 2048) || !referenceText(row.value(k("local_path")), 1000)
            || !digest.match(row.value(k("sha256")).toString()).hasMatch()) return rejected("source entry");
        sourceIds.insert(id);
    }
    int reviewed = 0, facts = 0, questions = 0, individual = 0; QSet<int> ids;
    for (const auto &raw : bundle.value(k("heroes")).toArray()) {
        const auto hero = raw.toObject(); const int id = hero.value(k("champion_id")).toInt(-1); const auto status = hero.value(k("status")).toString();
        if (!raw.isObject() || hero.value(k("champion_id")) != QJsonValue(id) || !c.champions.contains(id) || ids.contains(id)
            || !referenceText(hero.value(k("key")), 100) || !referenceText(hero.value(k("name")), 100) || !referenceFlags(hero)
            || !firstPassReference(hero.value(k("first_pass")))) return rejected("hero entry");
        ids.insert(id);
        if (status == k("source_reasoning_reviewed")) { if (!reviewedReference(hero.value(k("reviewed")), facts, questions)) return rejected("reviewed entry"); ++reviewed; }
        else if (status != k("indexed") || hero.contains(k("reviewed"))) return rejected("hero status");
        if (hero.contains(k("individual_analysis"))) {
            if (!individualReference(hero.value(k("individual_analysis")),id,c.combat->revision)) return rejected("individual contract");
            ++individual;
        }
    }
    const auto counts = bundle.value(k("counts")).toObject();
    if (counts.value(k("heroes")) != QJsonValue(173) || counts.value(k("source_reasoning_reviewed")) != QJsonValue(reviewed)
        || counts.value(k("first_pass_only")) != QJsonValue(individual?0:173 - reviewed) || counts.value(k("current_source_facts")) != QJsonValue(facts)
        || counts.value(k("open_observation_groups")) != QJsonValue(questions)) return rejected("counts");
    if (individual && (individual!=173 || counts.value(k("individual_source_analyzed"))!=QJsonValue(individual))) return rejected("individual count");
    if (!individual && counts.contains(k("individual_source_analyzed"))) return rejected("missing individual count");
    saved = bundle; saved[k("revision")] = expected; return saved;
}
QJsonObject championUnderstanding(const Catalog &c, int champion) {
    QJsonObject out{{k("available"), false}, {k("reference_only"), true}, {k("execution_eligible"), false}, {k("game_verified"), false},
        {k("full_mechanism_understanding_verified"), false}, {k("status"), k("unavailable_or_incompatible_reference")}};
    const auto bundle = championUnderstandingBundle(c);
    for (const auto &raw : bundle.value(k("heroes")).toArray()) {
        const auto hero = raw.toObject(); if (hero.value(k("champion_id")).toInt() != champion) continue;
        out = hero; out[k("available")] = true; out[k("revision")] = bundle.value(k("revision")); out[k("patch")] = bundle.value(k("patch"));
        out[k("level")] = hero.value(k("status")) == k("source_reasoning_reviewed") ? k("source_reasoning_reviewed") : k("first_pass");
        if (hero.contains(k("individual_analysis")) && hero.value(k("status"))!=k("source_reasoning_reviewed")) out[k("level")]=k("individual_source_analyzed");
        out[k("scope")] = bundle.value(k("scope")); out[k("source_bundle_sha256")] = bundle.value(k("source_bundle_sha256"));
        out[k("combat_revision")] = bundle.value(k("combat_revision")); out[k("content_version")] = bundle.value(k("content_version"));
        out[k("limits")] = k("Source reasoning and first-pass indexing are conditional references, not complete mechanism certification, executable evidence, or game verification.");
        return out;
    }
    return out;
}
QJsonArray routeNotes(int champion) {
    if (champion == 117) return QJsonArray{
        QJsonObject{{k("branch"), k("protection")}, {k("text"), QStringLiteral("E给友军与给敌人互斥；W友军攻速/移速与敌方变形互斥。R提供临时最大生命。保护收益未闭合时保留保护路线。")}, {k("quantitative_utility_available"), false}},
        QJsonObject{{k("branch"), k("damage_augments")}, {k("text"), QStringLiteral("输出海克斯路线只比较明确命中的敌方E、Q和普攻；不把友方护盾、皮克斯跟随和控制遗漏解释成零价值。")}, {k("quantitative_utility_available"), false}}};
    if (champion == 113) return QJsonArray{
        QJsonObject{{k("branch"), k("engage_or_peel")}, {k("text"), QStringLiteral("开团和保护需要命中、距离、控制衔接与队友响应；E要求目标满足四层霜冻，不能默认已叠满。")}, {k("quantitative_utility_available"), false}},
        QJsonObject{{k("branch"), k("damage_ceiling")}, {k("text"), QStringLiteral("伤害比较只计算当前执行器支持的命中载荷，不等同于最优开团或最优出装。")}, {k("quantitative_utility_available"), false}}};
    if (champion == 22) return QJsonArray{
        QJsonObject{{k("branch"), k("damage")}, {k("text"), QStringLiteral("Q需要专注层数与激活状态；W对同一目标只计一次命中。实际普攻节奏受攻速、施法占用与控制约束。")}, {k("quantitative_utility_available"), false}},
        QJsonObject{{k("branch"), k("initiation")}, {k("text"), QStringLiteral("R开团和E视野收益不能用单目标伤害替代；命中率与射程在参考场景外。")}, {k("quantitative_utility_available"), false}}};
    return {};
}
bool hasReservedKey(const QJsonValue &value, int depth = 0) {
    if (depth > 16) return true;
    if (value.isObject()) { const auto o = value.toObject(); for (auto it = o.begin(); it != o.end(); ++it) if (it.key().startsWith(QChar(u'_')) || hasReservedKey(it.value(), depth + 1)) return true; }
    else if (value.isArray()) for (const auto &v : value.toArray()) if (hasReservedKey(v, depth + 1)) return true;
    return false;
}
bool validateEvents(const QJsonArray &events, const QString &actor, double start, double end,
                    bool candidate, QSet<QString> &seen, QString *error) {
    for (const auto &raw : events) {
        if (!raw.isObject() || hasReservedKey(raw)) { *error = k("invalid_or_reserved_action_payload"); return false; }
        const auto e = raw.toObject(); const auto id = e.value(k("event_id")).toString(), type = e.value(k("type")).toString();
        if (id.isEmpty() || id.size() > 128 || seen.contains(id)) { *error = k("branch_conflict_duplicate_event_id"); return false; }
        seen.insert(id);
        if (!finite(e.value(k("time")), start, end) || e.value(k("time")).toDouble() >= end) { *error = k("event_outside_half_open_window"); return false; }
        if (!QStringList{k("cast"), k("attack"), k("control"), k("skill_state"), k("idle")}.contains(type)) { *error = k("unsupported_execution_event_type"); return false; }
        if (candidate && (type == k("control") || e.value(k("player_key")).toString() != actor)) { *error = k("candidate_must_not_invent_external_control_or_other_actor"); return false; }
    }
    return true;
}
QJsonObject compactTrace(const QJsonObject &row) {
    QJsonObject out;
    for (const auto &name : {k("event_id"), k("type"), k("time"), k("player_key"), k("source_key"), k("target_key"), k("resolved_target_key"),
         k("slot"), k("accepted"), k("phase"), k("reason"), k("damage"), k("actual_damage"), k("shield_damage"), k("effective_amount"), k("resource_cost")})
        if (row.contains(name)) out[name] = row.value(name);
    const auto admission = row.value(k("action_admission")).toObject();
    if (!admission.isEmpty()) { QJsonObject receipt; for (const auto &name : {k("accepted"), k("reason"), k("resource_cost"), k("cast_complete_at"), k("ready_at")}) if (admission.contains(name)) receipt[name] = admission.value(name); out[k("action_admission")] = receipt; }
    return out;
}
}

QJsonObject heroExecutionOverview(const Catalog &c, int championId) {
    QJsonObject out{{k("available"), false}, {k("champion_id"), championId}, {k("patch"), c.patch},
        {k("verified_combo_sequences"), QJsonArray{}}, {k("server_mechanics_certified"), false}};
    if (!c.combat || !c.combat->champions.contains(championId) || !c.champions.contains(championId)) { out[k("status")] = k("unknown_hero"); return out; }
    const auto hero = c.combat->champions.value(championId); QJsonArray skills;
    for (const auto &raw : hero.value(k("ability_descriptions")).toArray()) {
        const auto row = raw.toObject(); const auto slot = row.value(k("spellKey")).toString();
        if (slot != k("P") && !abilitySlots().contains(slot)) continue;
        skills.append(QJsonObject{{k("slot"), slot}, {k("name"), row.value(k("name"))},
            {k("description"), plainText(row.value(k("description")).toString()).left(1800)}, {k("execution_eligible_from_prose"), false}});
    }
    if (skills.isEmpty()) for (const auto &raw : hero.value(k("spells")).toArray()) {
        const auto row = raw.toObject(); const auto slot = row.value(k("slot")).toString(); if (!abilitySlots().contains(slot) && slot != k("P")) continue;
        skills.append(QJsonObject{{k("slot"), slot}, {k("name"), row.value(k("name"))},
            {k("description"), plainText(row.value(k("description")).toString()).left(1800)}, {k("execution_eligible_from_prose"), false}});
        if (skills.size() >= 5) break;
    }
    QJsonArray coverage;
    for (const auto &slot : abilitySlots()) {
        const auto rule = hero.value(k("action_rules_v3")).toObject().value(k("slots")).toObject().value(slot).toObject();
        int damage = 0, support = 0, temporal = 0, stateful = 0, control = 0;
        for (const auto &row : hero.value(k("components")).toArray()) if (row.toObject().value(k("slot")).toString() == slot) ++damage;
        for (const auto &row : hero.value(k("support_components")).toArray()) if (row.toObject().value(k("slot")).toString() == slot) ++support;
        for (const auto &row : hero.value(k("temporal_bindings_v4")).toArray()) if (row.toObject().value(k("slot")).toString() == slot) ++temporal;
        for (const auto &row : hero.value(k("stateful_bindings_v7")).toArray()) if (row.toObject().value(k("slot")).toString() == slot) ++stateful;
        for (const auto &row : hero.value(k("control_bindings_v3")).toArray()) if (row.toObject().value(k("slot")).toString() == slot) ++control;
        coverage.append(QJsonObject{{k("slot"), slot}, {k("action_rule_present"), !rule.isEmpty()}, {k("cost_known"), rule.value(k("cost_known"))},
            {k("cooldown_known"), rule.value(k("cooldown_known"))}, {k("cast_time_seconds"), rule.value(k("cast_time_seconds"))},
            {k("damage_components"), damage}, {k("support_components"), support}, {k("temporal_bindings"), temporal}, {k("stateful_bindings"), stateful},
            {k("control_bindings"), control}, {k("source_pointer"), rule.value(k("source_pointer"))}, {k("unknowns"), rule.value(k("unknowns"))}, {k("all_mechanics_complete"), false}});
    }
    out[k("available")] = true; out[k("status")] = k("sourced_guide_with_explicit_executable_coverage");
    out[k("name")] = c.championName(championId); out[k("roles")] = hero.value(k("roles")); out[k("skill_descriptions")] = skills;
    out[k("numeric_source")] = QJsonObject{{k("path"), hero.value(k("source_path"))}, {k("sha256"), hero.value(k("source_sha256"))}, {k("combat_revision"), c.combat->revision}};
    out[k("execution_coverage")] = coverage; out[k("routes")] = routeNotes(championId);
    out[k("gaps")] = QJsonArray{k("full_combo_optimum_not_verified"), k("geometry_hit_probability_and_player_response_unobserved"), k("missing_support_or_control_not_zero_utility")};
    const auto bundle = playbookBundle(c);
    for (const auto &raw : bundle.value(k("heroes")).toArray()) {
        const auto book = raw.toObject(); if (book.value(k("champion_id")).toInt() != championId) continue;
        out[k("skill_descriptions")] = book.value(k("skill_descriptions")); out[k("verified_tips")] = book.value(k("verified_tips"));
        out[k("release_constraints")] = book.value(k("release_constraints")); out[k("gaps")] = book.value(k("gaps"));
        out[k("prose_reference_scope")] = bundle.value(k("applicability")); out[k("source_refs")] = book.value(k("source_refs"));
        QJsonArray sources;
        for (const auto &entry : bundle.value(k("sources")).toArray()) if (book.value(k("source_refs")).toArray().contains(entry.toObject().value(k("source_id")))) {
            const auto source = entry.toObject(); QJsonObject compact;
            for (const auto &name : {k("source_id"), k("url"), k("local_path"), k("sha256"), k("retrieved_at"), k("published_version"), k("scope")}) compact[name] = source.value(name);
            sources.append(compact);
        }
        out[k("sources")] = sources;
        if (book.contains(k("isolated_tips"))) out[k("isolated_tips")] = book.value(k("isolated_tips"));
        break;
    }
    out[k("champion_understanding")] = championUnderstanding(c, championId);
    return out;
}

QJsonObject evaluateHeroExecutionScenario(const Catalog &c, const QJsonObject &input, int championId, int budgetMs) {
    auto out = baseResult(c, championId); QElapsedTimer clock; clock.start();
    auto fail = [&](const QString &why) { out[k("status")] = why; return out; };
    if (!c.combat || !c.combat->champions.contains(championId)) return fail(k("unknown_actor_or_combat_database"));
    if (QJsonDocument(input).toJson(QJsonDocument::Compact).size() > 256 * 1024) return fail(k("execution_request_too_large"));
    const auto candidates = input.value(k("execution_candidates")).toArray(), players = input.value(k("players")).toArray();
    if (!input.value(k("execution_candidates")).isArray() || candidates.isEmpty() || candidates.size() > candidateLimit) return fail(k("execution_candidate_limit_or_shape"));
    if (players.size() != 10) return fail(k("exactly_ten_players_required"));
    QString actorKey, actorTeam; int matches = 0; const auto requestedKey = input.value(k("execution_actor_key")).toString();
    for (const auto &raw : players) { const auto row = raw.toObject(); if (row.value(k("champion_id")).toInt() == championId && (requestedKey.isEmpty() || requestedKey == playerKey(row))) { actorKey = playerKey(row); actorTeam = row.value(k("team")).toString(); ++matches; } }
    if (matches != 1) return fail(k("unique_execution_actor_required"));
    const double start = input.value(k("start_time")).toDouble(0), horizon = input.value(k("horizon")).toDouble(8), step = input.value(k("decision_step_seconds")).toDouble(.1);
    if (!std::isfinite(start) || start < 0 || !std::isfinite(horizon) || horizon < .001 || horizon > 8 || !std::isfinite(step) || step < .05 || step > 1) return fail(k("bounded_execution_time_required"));
    if (input.contains(k("end_time")) && (!finite(input.value(k("end_time")), start, start + 8) || std::abs(input.value(k("end_time")).toDouble() - start - horizon) > 1e-8)) return fail(k("execution_end_time_mismatch"));
    if (input.value(k("auto_actions")).toBool() || input.contains(k("checkpoint")) || input.contains(k("policies")) || input.contains(k("combat_units_v8")) || input.contains(k("action_states")) || input.contains(k("team_policies"))) return fail(k("explicit_single_policy_fresh_sequences_required"));
    const auto shared = input.value(k("action_events")).toArray(); if (shared.size() > eventLimit || (input.contains(k("action_events")) && !input.value(k("action_events")).isArray())) return fail(k("shared_event_limit_or_shape"));
    QSet<QString> candidateIds;
    for (const auto &raw : candidates) {
        const auto row = raw.toObject(); const auto id = row.value(k("candidate_id")).toString();
        if (!raw.isObject() || id.isEmpty() || id.size() > 80 || candidateIds.contains(id) || !row.value(k("action_events")).isArray() || row.value(k("action_events")).toArray().size() > eventLimit) return fail(k("candidate_identity_or_event_limit"));
        candidateIds.insert(id); QSet<QString> eventIds; QString error;
        if (!validateEvents(shared, actorKey, start, start + horizon, false, eventIds, &error) || !validateEvents(row.value(k("action_events")).toArray(), actorKey, start, start + horizon, true, eventIds, &error)) return fail(error);
        if (championId == 117) {
            QMap<double, QString> eTargets;
            QJsonArray combined = shared; for (const auto &v : row.value(k("action_events")).toArray()) combined.append(v);
            for (const auto &event : combined) { const auto e = event.toObject(); if (e.value(k("player_key")).toString() != actorKey || e.value(k("type")).toString() != k("cast") || e.value(k("slot")).toString() != k("E")) continue;
                const double at = e.value(k("time")).toDouble(); const auto target = e.value(k("target_key")).toString();
                if (eTargets.contains(at) && eTargets.value(at) != target) return fail(k("branch_conflict_lulu_e_target")); eTargets[at] = target;
                QString targetTeam; for (const auto &p : players) if (playerKey(p.toObject()) == target) targetTeam = p.toObject().value(k("team")).toString();
                const auto branch = row.value(k("branch")).toString();
                if ((branch == k("lulu_e_ally") && targetTeam != actorTeam) || (branch == k("lulu_e_enemy") && (targetTeam.isEmpty() || targetTeam == actorTeam))) return fail(k("branch_conflict_lulu_e_target"));
            }
        }
    }
    out[k("actor_key")] = actorKey; out[k("candidate_count")] = candidates.size(); out[k("horizon_seconds")] = horizon;
    if (budgetMs <= 0) return fail(k("execution_budget_exhausted"));
    QJsonArray rows; QSet<QString> unknowns; QString bestId; double bestDamage = -1; int completed = 0; bool anyAvailable = false;
    for (const auto &raw : candidates) {
        if (clock.elapsed() >= budgetMs) break;
        const auto candidate = raw.toObject(); auto request = input; request.remove(k("execution_candidates")); request.remove(k("execution_actor_key"));
        request[k("auto_actions")] = false; request[k("objective_team")] = actorTeam;
        QJsonArray actions = shared; for (const auto &e : candidate.value(k("action_events")).toArray()) actions.append(e); request[k("action_events")] = actions;
        const auto branch = candidate.value(k("branch")).toString();
        // This condition states only the chosen enemy E target in this explicit
        // scenario. It never admits friendly E damage or invents a hit.
        if (championId == 117 && branch == k("lulu_e_enemy")) {
            auto roster = players;
            for (int i = 0; i < roster.size(); ++i) { auto p = roster[i].toObject(); if (playerKey(p) != actorKey) continue; auto conditions = p.value(k("conditions")).toArray(); if (!conditions.contains(k("lulu_e_enemy_cast"))) conditions.append(k("lulu_e_enemy_cast")); p[k("conditions")] = conditions; roster[i] = p; }
            request[k("players")] = roster;
        }
        const auto callStart = clock.elapsed(); const auto result = runContinuousSandbox(c, request); const auto callMs = clock.elapsed() - callStart;
        ++completed; const bool available = result.value(k("available")).toBool(); anyAvailable |= available;
        QJsonObject final; QJsonArray initialHealthByActor;
        for (const auto &p : result.value(k("players_final")).toArray()) { const auto actor=p.toObject();
            if (actor.value(k("player_key")).toString() == actorKey) final = actor;
            initialHealthByActor.append(QJsonObject{{k("player_key"),actor.value(k("player_key"))},{k("initial_health"),actor.value(k("initial_health"))},
                {k("initially_dead"),actor.value(k("initially_dead"))}});
        }
        QSet<QString> accepted, rejected, reasons; QJsonArray trace; int payloads = 0;
        QSet<QString> inputActorEvents; for (const auto &e : actions) if (e.toObject().value(k("player_key")).toString() == actorKey) inputActorEvents.insert(e.toObject().value(k("event_id")).toString());
        for (const auto &entry : result.value(k("trace")).toArray()) {
            const auto row = entry.toObject(); const auto id = row.value(k("event_id")).toString();
            const bool actor = row.value(k("player_key")).toString() == actorKey || row.value(k("source_key")).toString() == actorKey;
            const bool affecting = row.value(k("target_key")).toString() == actorKey || row.value(k("control")).toObject().value(k("target")).toString() == actorKey;
            if (!actor && !affecting) continue;
            if (inputActorEvents.contains(id) && row.value(k("accepted")).isBool()) { if (row.value(k("accepted")).toBool()) accepted.insert(id); else rejected.insert(id); }
            const auto reason = row.value(k("reason")).toString(); if (!reason.isEmpty()) reasons.insert(reason);
            // Receipts, not a successful action-admission flag, establish that
            // the native executor actually delivered a supported payload.
            if (actor && row.value(k("type")).toString() == k("damage_receipt") && row.value(k("actual_damage")).toDouble() + row.value(k("shield_damage")).toDouble() > 0) ++payloads;
            if (actor && row.value(k("phase")).toString() == k("mechanic_delivery") && row.value(k("effective_amount")).toDouble() > 0) ++payloads;
            if (trace.size() < traceLimit) trace.append(compactTrace(row));
        }
        if (!available) reasons.insert(result.value(k("error")).toString(result.value(k("status")).toString(k("continuous_execution_unavailable"))));
        if (branch == k("lulu_e_ally")) reasons.insert(k("friendly_e_protection_payload_unmodeled"));
        if (available && payloads == 0) reasons.insert(k("no_supported_delivered_payload"));
        for (const auto &u : final.value(k("unknowns")).toArray()) if (u.isString()) unknowns.insert(u.toString());
        const double damage = final.value(k("effective_damage_dealt")).toDouble(0); const bool supportOnly = candidate.value(k("support_only")).toBool(false) || branch == k("lulu_e_ally");
        const bool fullyAdmitted = available && rejected.isEmpty() && accepted.size() == inputActorEvents.size();
        QJsonArray admittedSequence; QStringList admittedLabels;
        for (const auto &rawEvent : candidate.value(k("action_events")).toArray()) { const auto event = rawEvent.toObject();
            if (!accepted.contains(event.value(k("event_id")).toString())) continue;
            const auto slot = event.value(k("type")).toString() == k("attack") ? k("AA") : event.value(k("slot")).toString();
            admittedSequence.append(QJsonObject{{k("slot"), slot}, {k("time"), event.value(k("time"))}, {k("target_key"), event.value(k("target_key"))}}); admittedLabels.append(slot);
        }
        QJsonObject row{{k("candidate_id"), candidate.value(k("candidate_id"))}, {k("label"), candidate.value(k("label")).toString().left(120)},
            {k("branch"), branch}, {k("support_only"), supportOnly}, {k("available"), available}, {k("effective_damage"), damage},
            {k("shield_damage"), final.value(k("shield_damage_dealt"))}, {k("admitted_actions"), accepted.size()}, {k("rejected_actions"), rejected.size()},
            {k("supported_payloads"), payloads}, {k("reason_codes"), codes(reasons)}, {k("remaining_resource"), final.value(k("final_action_state")).toObject().value(k("resource_current"))},
            {k("remaining_health"), final.value(k("remaining_health"))}, {k("initial_health"), final.value(k("initial_health"))},
            {k("initially_dead"),final.value(k("initially_dead"))},{k("initial_health_by_actor"),initialHealthByActor},
            {k("final_maximum_health"), final.value(k("maximum_health"))}, {k("native_failure_reason"), result.value(k("reason"))},
            {k("trace"), trace}, {k("native_call_elapsed_ms"), double(callMs)},
            {k("execution_status"), !available || accepted.isEmpty() ? k("not_executed") : fullyAdmitted ? k("executed_with_payload_coverage_limits") : k("partially_executed")},
            {k("admitted_sequence"), admittedSequence}, {k("admitted_sequence_label"), admittedLabels.join(QStringLiteral(" → ")).left(160)},
            {k("all_actions_admitted"), fullyAdmitted}, {k("all_mechanics_complete"), false}};
        rows.append(row);
        if (available && !supportOnly && payloads > 0 && damage > bestDamage + 1e-9) { bestDamage = damage; bestId = candidate.value(k("candidate_id")).toString(); }
    }
    const bool complete = completed == candidates.size() && clock.elapsed() <= budgetMs;
    out[k("available")] = anyAvailable; out[k("complete")] = complete; out[k("evaluated_candidates")] = completed; out[k("elapsed_ms")] = double(clock.elapsed()); out[k("candidates")] = rows;
    out[k("status")] = complete ? k("conditional_execution_comparison") : k("execution_budget_exhausted");
    out[k("damage_ranking_available")] = complete && !bestId.isEmpty(); if (complete) out[k("best_candidate_id")] = bestId;
    out[k("unknowns")] = codes(unknowns); out[k("support_utility_ranking_available")] = false;
    out[k("winner_meaning")] = k("largest_actual_delivered_modeled_actor_damage_among_completed_non_support_candidates_fixed_scenario_only");
    return out;
}

QJsonObject buildHeroExecutionPlan(const Catalog &c, const GameState &state, const QString &preference, int budgetMs) {
    QElapsedTimer buildClock; buildClock.start();
    auto out = baseResult(c, state.championId); const auto overview = heroExecutionOverview(c, state.championId); out[k("guide")] = overview;
    auto health=combatHealthContract(c,state,true);
    const bool panelEligible=health.value(k("current_health_used")).toBool();
    bool panelBound=false,scenarioAttempted=false;QJsonArray healthInputs;
    // The action ledger derives maximum health from source stats and loadout.
    // The panel maximum validates a panel current-HP sample; it is not applied
    // as a replacement for that ledger maximum.
    health[k("panel_maximum_health_status")]=health.value(k("maximum_health_status"));
    health[k("maximum_health_source")]=k("catalog_stats_with_scenario_items_augments");
    health[k("panel_maximum_health_role")]=k("validation_bound_only_not_ledger_maximum_calibration");
    health[k("initial_health_above_modeled_maximum_rejected")]=true;
    auto result=[&](){
        bool numericAvailable=false;for(const auto &v:out.value(k("candidates")).toArray())numericAvailable|=v.toObject().value(k("available")).toBool();
        QJsonArray resolvedInputs;QJsonObject selfBinding;
        for(const auto &v:healthInputs){auto input=v.toObject();QJsonArray actual;
            for(const auto &candidateValue:out.value(k("candidates")).toArray()){const auto candidate=candidateValue.toObject();if(!candidate.value(k("available")).toBool())continue;
                for(const auto &actorValue:candidate.value(k("initial_health_by_actor")).toArray()){const auto actor=actorValue.toObject();
                    if(actor.value(k("player_key"))!=input.value(k("player_key")))continue;
                    actual.append(QJsonObject{{k("candidate_id"),candidate.value(k("candidate_id"))},{k("initial_health"),actor.value(k("initial_health"))},{k("initially_dead"),actor.value(k("initially_dead"))}});
                }
            }
            const bool resolved=!actual.isEmpty(),dead=input.value(k("is_dead_input")).isBool()&&input.value(k("is_dead_input")).toBool();
            QJsonValue initial=QJsonValue::Null;if(resolved){initial=actual.first().toObject().value(k("initial_health"));for(const auto &a:actual)if(a.toObject().value(k("initial_health"))!=initial){initial=QJsonValue::Null;break;}}
            input[k("source")]=resolved?(dead?k("explicit_roster_is_dead_zero_reference"):input.value(k("requested_current_health_source")).toString()):k("not_numerically_resolved");
            input[k("resolution_status")]=resolved?k("native_initial_health_reported"):!scenarioAttempted?k("scenario_not_executed"):out.value(k("evaluated_candidates")).toInt()>0?k("native_rejected"):k("no_native_candidate_evaluated");
            input[k("actual_initial_health")]=initial;input[k("actual_initial_health_by_candidate")]=actual;
            input[k("death_policy_applied")]=dead&&resolved;
            input[k("explicit_dead_flag_forwarded_to_scenario")]=scenarioAttempted&&input.value(k("is_dead_input")).isBool();
            input[k("bound_to_scenario")]=scenarioAttempted&&input.value(k("input_value")).isDouble();input[k("numeric_scenario_available")]=resolved;
            resolvedInputs.append(input);if(input.value(k("self")).toBool())selfBinding=input;
        }
        if(selfBinding.isEmpty())selfBinding=QJsonObject{{k("source"),k("not_numerically_resolved")},{k("requested_current_health_source"),k("not_bound")},
            {k("input_value"),QJsonValue(QJsonValue::Null)},{k("actual_initial_health"),QJsonValue(QJsonValue::Null)},{k("numeric_scenario_available"),false},{k("bound_to_scenario"),false}};
        selfBinding[k("live_panel_binding")]=panelBound&&health.value(k("snapshot_bound")).toBool()&&scenarioAttempted;
        health[k("panel_current_health_eligible")]=panelEligible;
        health[k("current_health_used")]=panelBound&&selfBinding.value(k("numeric_scenario_available")).toBool()&&!selfBinding.value(k("death_policy_applied")).toBool();
        health[k("current_health_used_semantics")]=k("panel_currentHealth_supplies_initial_health_in_available_native_scenario_without_explicit_dead_flag_override");
        health[k("current_health_bound_to_scenario")]=panelBound&&scenarioAttempted;
        health[k("scenario_execution_attempted")]=scenarioAttempted;health[k("numeric_scenario_available")]=numericAvailable;
        health[k("native_scenarios_evaluated")]=out.value(k("evaluated_candidates")).toInt();
        health[k("actor_health_inputs")]=resolvedInputs;
        health[k("self_current_health_binding")]=selfBinding;
        health[k("runtime_formula_health_counters")]=k("delivery_context_rebinds_current_max_missing_and_fractions_from_ledger_health_not_caller_initial_health_counters");
        if(!numericAvailable)health[k("summary")]=QStringLiteral("动作场景未产生可用的数值结果；保留输入依据，不声称已使用实时血量完成推演。");
        else if(selfBinding.value(k("death_policy_applied")).toBool())health[k("summary")]=QStringLiteral("名册显式标记为死亡，动作账本以零生命初始化；输入血量及其来源单独保留。");
        else if(selfBinding.value(k("source"))==k("explicit_roster_current_health_reference"))health[k("summary")]=QStringLiteral("动作场景使用名册显式提供的剩余生命参考；它不是已认证的实时面板观测。");
        else if(selfBinding.value(k("source"))==k("modeled_full_health_reference"))health[k("summary")]=QStringLiteral("剩余生命未提供，动作场景以模型满血为参考，未把它视为实时观测。");
        out[k("health_basis")]=health;return out;
    };
    auto fail = [&](const QString &why) { out[k("status")] = why; return result(); };
    if (!overview.value(k("available")).toBool()) return fail(k("hero_execution_guide_unavailable"));
    if (!state.fresh() || !modelRulesUsable(c, state) || state.gameMode != k("KIWI")) return fail(k("fresh_same_patch_kiwi_required"));
    if (state.players.size() != 10) return fail(k("complete_ten_player_roster_required"));
    QJsonArray roster, contexts; QString selfKey, selfTeam, targetKey, friendKey; QSet<QString> assumptions;
    int selfCount = 0; QMap<QString, int> duplicates; QJsonObject selfRow;
    for (const auto &raw : state.players) {
        if (!raw.isObject()) return fail(k("invalid_roster_row")); auto row = raw.toObject(); const bool self = row.value(k("self")).toBool();
        if (self) {
            ++selfCount; if (row.value(k("champion_id")).toInt() != state.championId) return fail(k("self_champion_mismatch"));
            row[k("level")] = state.level;
            if (!state.abilities.isEmpty()) row[k("abilities")] = state.abilities;
            if (!state.effectCounters.isEmpty()) { auto values = row.value(k("effect_counters")).toObject(); for (auto it = state.effectCounters.begin(); it != state.effectCounters.end(); ++it) values[it.key()] = it.value(); row[k("effect_counters")] = values; }
            if (!state.effectConditions.isEmpty()) { auto values = row.value(k("effect_conditions")).toArray(); for (const auto &v : state.effectConditions) if (!values.contains(v)) values.append(v); row[k("effect_conditions")] = values; }
        }
        const auto context = playerCombatContext(*c.combat, row); if (!context.available) return fail(context.report.value(k("reason")).toString(k("roster_context_unavailable")));
        if (!row.value(k("items")).isArray() || !row.value(k("items_complete")).toBool()) return fail(k("complete_visible_inventories_required"));
        const auto team = row.value(k("team")).toString(); if (team != k("ORDER") && team != k("CHAOS")) return fail(k("invalid_roster_team"));
        const QString stem = team + k(":") + QString::number(context.context.championId); const auto key = stem + k(":") + QString::number(++duplicates[stem]);
        QJsonObject p{{k("player_key"), key}, {k("champion_id"), context.context.championId}, {k("team"), team}, {k("level"), context.context.level},
            {k("items"), row.value(k("items"))}, {k("augments"), row.value(k("augment_ids")).toArray()}, {k("augments_complete"), row.value(k("augments_complete")).toBool()},
            {k("ranks"), context.report.value(k("resolved_ranks"))}, {k("conditions"), codes(context.context.conditions)}};
        QJsonObject counters; for (auto it = context.context.counters.begin(); it != context.context.counters.end(); ++it) counters[it.key()] = it.value(); p[k("counters")] = counters;
        if (context.report.value(k("rank_source")).toString() != k("observed")) assumptions.insert(k("unobserved_skill_ranks_use_disclosed_reference"));
        if (!p.value(k("augments_complete")).toBool()) assumptions.insert(k("unobserved_augments_not_invented"));
        auto action = row.value(k("action_state")).toObject();QString healthSource=k("modeled_full_health_reference");
        if (self) {
            p[k("items")] = ids(state.owned); p[k("augments")] = ids(effectiveSelectedAugments(c, state));
            if (!action.contains(k("resource_current")) && finite(state.liveStats.value(k("resourceValue")))) action[k("resource_current")] = state.liveStats.value(k("resourceValue"));
            if (!action.contains(k("cooldown_remaining"))) {
                QJsonObject remaining;
                for (const auto &slot : abilitySlots()) { const auto ability = state.abilities.value(slot).toObject(); if (finite(ability.value(k("cooldownRemaining")), 0, 1e7)) remaining[slot] = ability.value(k("cooldownRemaining")); }
                if (!remaining.isEmpty()) action[k("cooldown_remaining")] = remaining;
            }
            if (panelEligible) {p[k("current_health")] = state.liveStats.value(k("currentHealth"));panelBound=true;
                healthSource=state.source==GameState::Source::Live?k("live_panel_currentHealth"):k("caller_replay_panel_currentHealth_reference");}
        }
        // The public plan starts a new local clock at zero. Preserve an
        // absolute observed ready-at value only after rebasing from game time.
        if (action.contains(k("ready_at"))) { auto ready = action.value(k("ready_at")).toObject();
            for (auto it = ready.begin(); it != ready.end(); ++it) if (finite(it.value(), 0, 1e7)) it.value() = std::max(0., it.value().toDouble() - state.gameTime);
            action[k("ready_at")] = ready;
        }
        if (!p.contains(k("current_health")) && finite(row.value(k("current_health")))) {p[k("current_health")] = row.value(k("current_health"));healthSource=k("explicit_roster_current_health_reference");}
        QJsonObject formulaHealth;
        for(const auto &name:{k("self_current_hp"),k("self_max_hp"),k("self_missing_hp"),k("self_health_fraction"),k("self_missing_health_fraction")})
            if(counters.contains(name))formulaHealth[name]=QJsonObject{{k("value"),counters.value(name)},{k("source"),self&&state.effectCounters.contains(name)?k("explicit_state_effect_counter"):k("explicit_roster_effect_counter")}};
        const auto healthInput=p.contains(k("current_health"))?p.value(k("current_health")):QJsonValue(QJsonValue::Null);
        if (row.value(k("is_dead")).isBool()) p[k("is_dead")] = row.value(k("is_dead"));
        const bool explicitDead=p.value(k("is_dead")).toBool();
        healthInputs.append(QJsonObject{{k("player_key"),key},{k("self"),self},{k("source"),healthSource},{k("input_value"),healthInput},
            {k("requested_current_health_source"),healthSource},{k("is_dead_input"),row.contains(k("is_dead"))?row.value(k("is_dead")):QJsonValue(QJsonValue::Null)},
            {k("is_dead_input_status"),!row.contains(k("is_dead"))?k("missing"):row.value(k("is_dead")).isBool()?k("explicit_boolean"):k("invalid_type_not_applied")},
            {k("initial_health_policy"),explicitDead?k("explicit_dead_zero_positive_current_health_is_conflict"):p.contains(k("current_health"))?k("explicit_current_health"):k("modeled_full_health_reference")},
            {k("formula_health_counters"),formulaHealth},{k("formula_counters_are_initial_health_field"),false}});
        if(explicitDead)assumptions.insert(k("explicit_dead_flag_uses_zero_health_positive_health_conflict_rejected"));
        else if (!p.contains(k("current_health"))) assumptions.insert(k("unobserved_current_health_uses_modeled_full_health_reference"));
        if (!action.contains(k("resource_current"))) assumptions.insert(k("missing_current_resource_stays_unknown_not_full"));
        if (action.value(k("cooldown_remaining")).toObject().size() < 4 && !action.contains(k("ready_at"))) assumptions.insert(k("other_actors_unobserved_cooldowns_not_used_hold_policy"));
        if (!action.contains(k("initial_form"))) assumptions.insert(k("unobserved_form_uses_source_default_reference"));
        p[k("action_state")] = action; roster.append(p); contexts.append(QJsonObject{{k("player_key"), key}, {k("rank_source"), context.report.value(k("rank_source"))}, {k("unknowns"), context.report.value(k("unknowns"))}});
        if (self) { selfKey = key; selfTeam = team; selfRow = p; }
    }
    if (selfCount != 1) return fail(k("unique_self_required"));
    const auto selfAction = selfRow.value(k("action_state")).toObject();
    const auto selfRemaining = selfAction.value(k("cooldown_remaining")).toObject(), selfReady = selfAction.value(k("ready_at")).toObject();
    bool actorCooldownsKnown = true;
    for (const auto &slot : abilitySlots()) if (selfRow.value(k("ranks")).toObject().value(slot).toInt() > 0 && !finite(selfRemaining.value(slot), 0, 1e7) && !finite(selfReady.value(slot), 0, 1e7)) actorCooldownsKnown = false;
    if (!actorCooldownsKnown) {
        out[k("available")] = true; out[k("status")] = k("guide_only_current_cooldowns_unobserved"); out[k("execution_status")] = k("not_executed");
        out[k("summary")] = QStringLiteral("技能打法可查；当前技能冷却未读取，暂不生成实况连招优先级。");
        out[k("gaps")] = QJsonArray{k("actor_remaining_cooldowns_unobserved")}; out[k("assumptions")] = codes(assumptions); return result();
    }
    if (c.combat->context(state.championId, state.level).usesMana && !finite(selfAction.value(k("resource_current")))) {
        out[k("available")] = true; out[k("status")] = k("guide_only_current_resource_unobserved"); out[k("execution_status")] = k("not_executed");
        out[k("summary")] = QStringLiteral("技能打法可查；当前法力未读取，暂不生成实况连招优先级。");
        out[k("gaps")] = QJsonArray{k("actor_current_resource_unobserved")}; out[k("assumptions")] = codes(assumptions); return result();
    }
    for (const auto &raw : roster) { const auto p = raw.toObject(); if (p.value(k("is_dead")).toBool()) continue;
        if (p.value(k("team")).toString() != selfTeam && targetKey.isEmpty()) targetKey = p.value(k("player_key")).toString();
        if (p.value(k("team")).toString() == selfTeam && p.value(k("player_key")).toString() != selfKey && friendKey.isEmpty()) friendKey = p.value(k("player_key")).toString();
    }
    if (targetKey.isEmpty()) return fail(k("no_alive_reference_target"));
    const auto hero = c.combat->champions.value(state.championId), rules = hero.value(k("action_rules_v3")).toObject().value(k("slots")).toObject();
    QStringList damageSlots;
    for (const auto &slot : abilitySlots()) { bool hasDamage = false;
        for (const auto &raw : hero.value(k("components")).toArray()) if (raw.toObject().value(k("slot")).toString() == slot) hasDamage = true;
        for (const auto &raw : hero.value(k("timeline_damage_components_v3")).toArray()) if (raw.toObject().value(k("slot")).toString() == slot) hasDamage = true;
        if (hasDamage && selfRow.value(k("ranks")).toObject().value(slot).toInt() > 0) damageSlots.append(slot);
    }
    QList<int> gear, augments; for (const auto &v : selfRow.value(k("items")).toArray()) gear.append(v.toInt()); for (const auto &v : selfRow.value(k("augments")).toArray()) augments.append(v.toInt());
    auto context = c.combat->context(state.championId, state.level);
    const auto rankObject = selfRow.value(k("ranks")).toObject(); for (auto it = rankObject.begin(); it != rankObject.end(); ++it) context.ranks[it.key()] = it.value().toInt();
    const auto stats = evaluateCombat(*c.combat, context, gear, augments);
    const double attackInterval = stats.valid && stats.stats[Stat::As] > 0 ? std::max(.05, 1. / stats.stats[Stat::As]) : 2.;
    QJsonArray candidates;QSet<QByteArray> sequenceSignatures;
    auto sequence = [&](const QString &id, const QStringList &order, bool weave, const QString &branch = QString(), bool supportOnly = false, const QString &friendTarget = QString()) {
        QJsonArray events; QStringList eventLabels; double at = 0, nextAttack = 0;
        auto append = [&](const QString &slot, double when) { eventLabels.append(slot); const bool attack = slot == k("AA"); events.append(QJsonObject{{k("event_id"), id + k(":") + QString::number(events.size())}, {k("type"), attack ? k("attack") : k("cast")},
            {k("player_key"), selfKey}, {k("target_key"), friendTarget.isEmpty() ? targetKey : friendTarget}, {k("slot"), attack ? k("P") : slot}, {k("time"), when}, {k("hit_confirmed"), true}, {k("synthetic"), true}}); };
        for (const auto &slot : order) {
            at = std::max(at, selfRemaining.value(slot).toDouble(selfReady.value(slot).toDouble(0)));
            if (events.size() >= eventLimit || at >= 7.5) break; append(slot, at);
            at += std::max(.05, rules.value(slot).toObject().value(k("cast_time_seconds")).toDouble(.25) + .01);
            if (weave && events.size() < eventLimit) { at = std::max(at, nextAttack); if (at >= 7.5) break; append(k("AA"), at); nextAttack = at + attackInterval; at += .05; }
        }
        if (!supportOnly) while (events.size() < eventLimit) { at = std::max(at, nextAttack); if (at >= 7.5) break; append(k("AA"), at); nextAttack = at + attackInterval; at = nextAttack; }
        QJsonArray signatureEvents;for(const auto &value:events){auto event=value.toObject();event.remove(k("event_id"));signatureEvents.append(event);}
        const auto signature=QJsonDocument(QJsonObject{{k("branch"),branch},{k("support_only"),supportOnly},{k("events"),signatureEvents}}).toJson(QJsonDocument::Compact);
        if(sequenceSignatures.contains(signature))return;sequenceSignatures.insert(signature);
        candidates.append(QJsonObject{{k("candidate_id"), id}, {k("label"), eventLabels.join(QStringLiteral(" → ")).left(120)}, {k("branch"), branch}, {k("support_only"), supportOnly}, {k("action_events"), events}});
    };
    sequence(k("attacks_only"), {}, false);
    const auto enemyBranch = state.championId == 117 ? k("lulu_e_enemy") : k("damage_reference");
    if (!damageSlots.isEmpty()) {
        sequence(k("source_slot_order"), damageSlots, false, enemyBranch);
        sequence(k("weave_attacks"), damageSlots, true, enemyBranch);
        auto reversed = damageSlots; std::reverse(reversed.begin(), reversed.end()); if (reversed != damageSlots) sequence(k("reverse_slot_order"), reversed, true, enemyBranch);
        if (damageSlots.contains(k("R")) && damageSlots.first() != k("R")) { auto opener = damageSlots; opener.removeAll(k("R")); opener.prepend(k("R")); sequence(k("ultimate_opener"), opener, true, enemyBranch); }
    }
    if (state.championId == 117 && !friendKey.isEmpty() && rankObject.value(k("E")).toInt() > 0) sequence(k("protect_ally_e"), {k("E")}, false, k("lulu_e_ally"), true, friendKey);
    assumptions.insert(k("fixed_first_alive_opponent_stationary_hit_confirmed_is_hypothesis_not_live_hit"));
    assumptions.insert(k("opponents_and_other_allies_hold_no_geometry_or_response_prediction"));
    assumptions.insert(k("source_derived_stats_with_items_augments_level_growth_not_exact_enemy_panel_stats"));
    QJsonObject request{{k("players"), roster}, {k("execution_actor_key"), selfKey}, {k("execution_candidates"), candidates},
        {k("start_time"), 0}, {k("horizon"), 8}, {k("decision_step_seconds"), .1}, {k("auto_actions"), false}, {k("policy"), k("hold")}, {k("opponent_policy"), k("hold")}, {k("action_events"), QJsonArray{}}};
    scenarioAttempted=true;
    out = evaluateHeroExecutionScenario(c, request, state.championId, std::max(0, budgetMs - int(buildClock.elapsed()))); out[k("guide")] = overview;
    out[k("preference")] = preference.left(100); out[k("scenario_kind")] = k("anonymous_current_loadouts_explicit_stationary_reference_not_observed_combo");
    out[k("scenario_assumptions")] = codes(assumptions); out[k("player_contexts")] = contexts; out[k("reference_target_key")] = targetKey;
    out[k("reference_actor_key")] = selfKey; out[k("routes")] = routeNotes(state.championId);
    out[k("total_elapsed_ms")] = double(buildClock.elapsed());
    out[k("execution_status")] = out.value(k("complete")).toBool() ? k("executed_reference") : out.value(k("evaluated_candidates")).toInt() > 0 ? k("partially_executed") : k("not_executed");
    out[k("assumptions")] = codes(assumptions); out[k("gaps")] = QJsonArray{k("support_control_geometry_and_actual_hit_probability_not_fully_modeled")};
    out[k("summary")] = out.value(k("damage_ranking_available")).toBool() ? QStringLiteral("已比较固定命中场景下的技能顺序；保护、开团和实战命中仍需判断。") : QStringLiteral("连招推演尚未完成或缺少可执行伤害载荷，暂不排序。");
    for (const auto &row : out.value(k("candidates")).toArray()) if (row.toObject().value(k("candidate_id")) == out.value(k("best_candidate_id"))) {
        out[k("sequence")] = row.toObject().value(k("admitted_sequence_label")); out[k("sequence_events")] = row.toObject().value(k("admitted_sequence")); out[k("sequence_execution_status")] = row.toObject().value(k("execution_status"));
    }
    out[k("recommendation_use")] = k("explain_conditional_damage_sequence_never_replace_unmodeled_protection_engage_or_vision_with_zero_utility");
    return result();
}

QJsonObject buildHeroExecutionReference(const Catalog &c, const GameState &state, const QString &preference, int budgetMs) {
    QElapsedTimer clock; clock.start(); auto out = baseResult(c, state.championId);
    out[k("synthetic_reference")] = true; out[k("source_state_mutated")] = false; out[k("current_damage_ranking_available")] = false;
    out[k("current_best_candidate_id")] = QString(); out[k("reference_damage_ranking_available")] = false;
    out[k("guide")] = heroExecutionOverview(c, state.championId);
    auto fail = [&](const QString &why) { out[k("status")] = why; out[k("execution_status")] = k("not_executed"); return out; };
    if (!c.combat || !c.combat->champions.contains(state.championId) || state.players.size() != 10) return fail(k("reference_requires_known_hero_and_ten_loadouts"));
    if (budgetMs <= 0 || clock.elapsed() >= budgetMs) return fail(k("execution_budget_exhausted"));
    GameState reference = state; reference.source = GameState::Source::Replay; reference.observedPatch = c.patch;
    reference.gameMode = k("KIWI"); reference.phase = k("SyntheticReference"); reference.gameTime = 0; reference.liveStats = {};
    // Do not turn a window-wide active-state flag into an observed opener.
    // Observed permanent counters remain explicit current-loadout inputs.
    reference.effectConditions.clear(); reference.mechanicObservations = {}; reference.mechanicBaselineTime = -1;
    QJsonArray players; QJsonObject actor; int selves = 0;
    for (const auto &raw : state.players) {
        if (!raw.isObject()) return fail(k("reference_invalid_roster_row")); auto row = raw.toObject();
        row.remove(k("current_health")); row.remove(k("is_dead")); row.remove(k("action_state")); row.remove(k("conditions")); row.remove(k("effect_conditions"));
        if (row.value(k("self")).toBool()) {
            ++selves; if (row.value(k("champion_id")).toInt() != state.championId) return fail(k("reference_self_champion_mismatch"));
            row[k("level")] = state.level; if (!state.abilities.isEmpty()) row[k("abilities")] = state.abilities;
            if (!state.effectCounters.isEmpty()) { auto counters = row.value(k("effect_counters")).toObject(); for (auto it = state.effectCounters.begin(); it != state.effectCounters.end(); ++it) counters[it.key()] = it.value(); row[k("effect_counters")] = counters; }
            actor = row;
        }
        players.append(row);
    }
    if (selves != 1) return fail(k("reference_unique_self_required"));
    const auto resolved = playerCombatContext(*c.combat, actor); if (!resolved.available) return fail(k("reference_actor_context_unavailable"));
    const auto augments = effectiveSelectedAugments(c, state); const auto evaluated = evaluateCombat(*c.combat, resolved.context, state.owned, augments);
    if (!evaluated.valid) return fail(k("reference_actor_numeric_stats_unavailable"));
    const auto rules = c.combat->champions.value(state.championId).value(k("action_rules_v3")).toObject();
    QJsonObject cooldowns; for (const auto &slot : abilitySlots()) cooldowns[slot] = 0;
    QJsonObject initial{{k("cooldown_remaining"), cooldowns}};
    const auto sourceForm = rules.value(k("initial_form")).toString(); if (!sourceForm.isEmpty()) initial[k("initial_form")] = sourceForm;
    ContinuousActionState ledger; QString error;
    if (!ledger.load(rules, resolved.context, evaluated.stats, initial, 0, &error)) return fail(k("reference_resource_or_form_contract_unavailable:") + error);
    const auto baseline = ledger.snapshot();
    if (baseline.value(k("resource_maximum_source_known")) != QJsonValue(true) || !finite(baseline.value(k("resource_maximum")))) return fail(k("reference_source_resource_maximum_unknown"));
    initial[k("resource_current")] = baseline.value(k("resource_maximum"));
    for (int i = 0; i < players.size(); ++i) { auto row = players[i].toObject(); if (row.value(k("self")).toBool()) { row[k("action_state")] = initial; players[i] = row; } }
    reference.players = players;
    auto plan = buildHeroExecutionPlan(c, reference, preference, std::max(0, budgetMs - int(clock.elapsed())));
    plan[k("synthetic_reference")] = true; plan[k("source_state_mutated")] = false; plan[k("reference_kind")] = k("replay_ready_skills_source_full_resource_conditional_hits");
    plan[k("current_damage_ranking_available")] = false; plan[k("current_best_candidate_id")] = QString();
    plan[k("reference_damage_ranking_available")] = plan.value(k("damage_ranking_available")); plan[k("reference_best_candidate_id")] = plan.value(k("best_candidate_id"));
    plan[k("damage_ranking_available")] = false; plan[k("best_candidate_id")] = QString();
    plan[k("reference_sequence")] = plan.value(k("sequence")); plan[k("reference_sequence_events")] = plan.value(k("sequence_events"));
    plan[k("label")] = QStringLiteral("技能就绪时的连招参考");
    plan[k("summary")] = plan.value(k("reference_damage_ranking_available")).toBool()
        ? QStringLiteral("技能就绪、资源充足且命中时的连招参考；保护、控制和命中率仍有缺口。")
        : QStringLiteral("已建立独立参考场景；本次尚无完整可比较的伤害顺序。") ;
    auto assumptions = plan.value(k("assumptions")).toArray();
    for (const auto &v : {k("synthetic_replay_not_actual_current_game_state"), k("learned_skill_remaining_cooldowns_explicitly_zero"),
         k("actor_resource_explicitly_full_source_known_maximum_not_live_observation"), k("all_players_full_modeled_health_and_hold_reference"),
         k("source_default_form_is_reference_not_observed_transformation"), k("single_target_hits_conditional_scenario_parameter_not_observed_hits"),
         k("focus_frost_stacks_ammo_control_target_eligibility_not_granted"), k("window_wide_active_buff_conditions_not_carried_into_reference")}) if (!assumptions.contains(v)) assumptions.append(v);
    plan[k("assumptions")] = assumptions; plan[k("scenario_assumptions")] = assumptions;
    plan[k("reference_parameters")] = QJsonObject{{k("actor_remaining_cooldowns"), cooldowns}, {k("actor_resource_maximum"), baseline.value(k("resource_maximum"))},
        {k("resource_kind"), baseline.value(k("resource_kind"))}, {k("source_resource_maximum_known"), true}, {k("stationary_target_hits_assumed"), true},
        {k("extra_activation_facts_granted"), false}, {k("ammunition_assumed_full"), false}, {k("control_assumed_landed"), false}};
    plan[k("total_elapsed_ms")] = double(clock.elapsed()); plan[k("recommendation_use")] = k("reference_only_never_current_state_winner_or_live_optimum");
    return plan;
}
