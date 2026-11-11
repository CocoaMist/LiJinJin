#include "ds_usage_meter.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QString k(const char *s) { return QString::fromLatin1(s); }
constexpr qint64 MaxTokens = 1000000000;
constexpr int RetainedCalls = 4096, RetainedMatches = 512;
QString hashContext(const QString &s) {
    return s.isEmpty() ? QString() : QString::fromLatin1(QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha256).toHex());
}
QString modelField(const QString &s) {
    static const QRegularExpression syntax(QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9._:/-]{0,95}\\z"));
    return syntax.match(s).hasMatch() ? s : QString();
}
QString taskField(const QString &s) {
    static const QStringList allowed{QStringLiteral("forecast_full"),QStringLiteral("forecast_quick"),QStringLiteral("overlay"),QStringLiteral("roster"),QStringLiteral("explanation"),QStringLiteral("engine"),QStringLiteral("probe")};
    return allowed.contains(s) ? s : QStringLiteral("other");
}
QString reasonField(const QString &s) {
    static const QStringList allowed{QStringLiteral("cancelled"),QStringLiteral("network_error"),QStringLiteral("process_interrupted"),QStringLiteral("missing_usage"),QStringLiteral("invalid_usage"),QStringLiteral("response_unavailable")};
    return allowed.contains(s) ? s : QStringLiteral("response_unavailable");
}
bool count(const QJsonValue &v, qint64 &n) {
    if (!v.isDouble()) return false;
    const double x=v.toDouble();
    if (!std::isfinite(x)||x<0||x>MaxTokens||std::floor(x)!=x) return false;
    n=static_cast<qint64>(x); return true;
}
qint64 integer(const QJsonObject &o, const char *field) { return static_cast<qint64>(o.value(k(field)).toDouble()); }
QString stamp(const QDateTime &v) { return v.toUTC().toString(Qt::ISODateWithMs); }
QDateTime timeField(const QJsonObject &o,const char *field) { return QDateTime::fromString(o.value(k(field)).toString(),Qt::ISODateWithMs).toUTC(); }
struct Usage { bool valid=false, split=false; qint64 prompt=0, output=0, hit=0, miss=0; };
Usage usage(const QJsonObject &envelope) {
    Usage u; const auto o=envelope.value(k("usage")).toObject();
    if (!count(o.value(k("prompt_tokens")),u.prompt)||!count(o.value(k("completion_tokens")),u.output)) return u;
    qint64 total=0;
    if (o.contains(k("total_tokens"))&&(!count(o.value(k("total_tokens")),total)||total!=u.prompt+u.output)) return u;
    const auto details=o.value(k("prompt_tokens_details")).toObject();
    const bool explicitHit=o.contains(k("prompt_cache_hit_tokens")), explicitMiss=o.contains(k("prompt_cache_miss_tokens")), detailedHit=details.contains(k("cached_tokens"));
    qint64 detail=0;
    if ((explicitHit&&!count(o.value(k("prompt_cache_hit_tokens")),u.hit))||(explicitMiss&&!count(o.value(k("prompt_cache_miss_tokens")),u.miss))||(detailedHit&&!count(details.value(k("cached_tokens")),detail))) return u;
    if (explicitHit&&detailedHit&&u.hit!=detail) return u;
    if (!explicitHit&&detailedHit) u.hit=detail;
    const bool hasHit=explicitHit||detailedHit;
    if (hasHit&&u.hit>u.prompt) return u;
    if (explicitMiss&&u.miss>u.prompt) return u;
    if (hasHit&&explicitMiss&&u.hit+u.miss!=u.prompt) return u;
    if (hasHit&&!explicitMiss) u.miss=u.prompt-u.hit;
    if (!hasHit&&explicitMiss) u.hit=u.prompt-u.miss;
    u.split=hasHit||explicitMiss||u.prompt==0; u.valid=true; return u;
}
struct Rate { qint64 hit=0,miss=0,output=0; bool known=false; };
Rate rate(const QString &model) {
    // Official names/aliases, verified 2026-11-03. Historical chat/reasoner
    // names and unverified deepseek-pro must never inherit Flash prices.
    if (model==k("deepseek-flash")||model==k("deepseek-v4-flash")||model==k("deepseek-v4-flash-vision-exp")) return {20,1000,4000,true};
    if (model==k("deepseek-v4-pro")) return {150,4500,13500,true};
    return {};
}
bool possiblePeak(const QDateTime &utc) {
    const auto local=utc.toUTC().addSecs(8*3600);
    // Without a verified statutory-holiday/adjusted-workday calendar even a
    // weekend may be a working day. Only the time-of-day exclusion is certain.
    const int m=local.time().hour()*60+local.time().minute();
    return (m>=9*60&&m<12*60)||(m>=14*60&&m<18*60);
}
bool possiblyPeakDuring(const QDateTime &start,const QDateTime &end) {
    // Boundary settlement and the statutory holiday calendar are unverified.
    // Off-peak is exact only when the entire observed request interval is away
    // from every possible peak window (including boundary instants).
    if (!start.isValid()||!end.isValid()||end<start||start.secsTo(end)>86400) return true;
    if (possiblePeak(start)||possiblePeak(end)) return true;
    auto probe=QDateTime(start.date(),QTime(0,0),Qt::UTC);
    for (int day=0;day<3;++day) {
        for (int hour : {1,4,6,10}) { // UTC boundaries for 09,12,14,18 Beijing.
            const auto boundary=probe.addSecs(hour*3600);
            if (boundary>=start&&boundary<=end) return true;
        }
        probe=probe.addDays(1);
    }
    return false;
}
}

struct DsUsageMeter::State {
    struct Match {
        QString id; quint64 sequence=0; bool active=false,completed=false;
        qint64 calls=0,pending=0,knownUsage=0,unknownCost=0,prompt=0,output=0,minNano=0,maxNano=0,rangeCalls=0,calendarUnknown=0,cacheUnknown=0;
    };
    struct Call {
        QString id,match,requested,model,task,reason; quint64 sequence=0;
        QDateTime start,end; bool terminal=false,usageKnown=false,costKnown=false,split=false,timeRange=false;
        qint64 prompt=0,output=0,hit=0,miss=0,minNano=0,maxNano=0;
    };
    mutable QMutex mutex;
    QHash<QString,Match> matches; QHash<QString,Call> calls;
    QString file,root,current,persistenceError; bool persistenceOk=true,recovered=false; quint64 sequence=0; qint64 nonliveCalls=0;
    std::unique_ptr<QLockFile> fileLock;
    QJsonObject means;

    void updateMeans() {
        qint64 completed=0,priced=0,unknown=0,minNano=0,maxNano=0;
        for (const auto &m:matches) if(m.completed&&m.calls>0) {
            ++completed;
            if(m.unknownCost||m.pending) ++unknown;
            else { ++priced;minNano+=m.minNano;maxNano+=m.maxNano; }
        }
        means={{k("completed_match_count"),completed},{k("priced_match_count"),priced},{k("excluded_unknown_cost_match_count"),unknown},
            {k("mean_cost_min_cny"),priced?QJsonValue(double(minNano)/1e9/priced):QJsonValue(QJsonValue::Null)},
            {k("mean_cost_max_cny"),priced?QJsonValue(double(maxNano)/1e9/priced):QJsonValue(QJsonValue::Null)},
            {k("cohort"),k("reliably_completed_matches_with_at_least_one_actual_call_and_all_calls_priced")},{k("retained_match_limit"),RetainedMatches}};
    }
    QJsonObject matchSummary(const Match &m) const {
        return {{k("call_count"),m.calls},{k("pending_call_count"),m.pending},{k("usage_known_call_count"),m.knownUsage},
            {k("unknown_cost_call_count"),m.unknownCost},{k("prompt_tokens"),m.prompt},{k("completion_tokens"),m.output},
            {k("calendar_or_timing_unknown_call_count"),m.calendarUnknown},{k("cache_split_unknown_call_count"),m.cacheUnknown},
            {k("unknown_model_call_count"),m.knownUsage-(m.calls-m.unknownCost)},{k("missing_or_invalid_usage_call_count"),m.calls-m.pending-m.knownUsage},
            {k("total_tokens"),m.prompt+m.output},{k("cost_min_cny"),double(m.minNano)/1e9},{k("cost_max_cny"),double(m.maxNano)/1e9},
            {k("tokens_complete"),m.knownUsage==m.calls&&m.pending==0},{k("cost_complete"),m.unknownCost==0&&m.pending==0},{k("cost_is_range"),m.rangeCalls>0},
            {k("completed"),m.completed},{k("active"),m.active},{k("has_calls"),m.calls>0}};
    }
    void prune() {
        while(matches.size()>RetainedMatches) {
            auto victim=matches.end();
            for(auto it=matches.begin();it!=matches.end();++it) if(!it->active&&!it->pending&&it.key()!=current&&(victim==matches.end()||it->sequence<victim->sequence)) victim=it;
            if(victim==matches.end()) break;
            const auto key=victim.key();matches.erase(victim);
            for(auto it=calls.begin();it!=calls.end();) if(it->match==key) it=calls.erase(it);else ++it;
        }
        while(calls.size()>RetainedCalls) {
            auto victim=calls.end();
            for(auto it=calls.begin();it!=calls.end();++it) if(it->terminal&&(victim==calls.end()||it->sequence<victim->sequence)) victim=it;
            if(victim==calls.end()) break;
            if(victim->match.isEmpty())--nonliveCalls;
            calls.erase(victim);
        }
    }
    QJsonObject serialized() const {
        QJsonArray ms,cs;
        for(const auto &m:matches) ms.append(QJsonObject{{k("id"),m.id},{k("sequence"),double(m.sequence)},{k("active"),m.active},{k("completed"),m.completed},
            {k("calls"),m.calls},{k("pending"),m.pending},{k("known_usage"),m.knownUsage},{k("unknown_cost"),m.unknownCost},{k("prompt"),m.prompt},{k("output"),m.output},
            {k("min_nano"),QString::number(m.minNano)},{k("max_nano"),QString::number(m.maxNano)},{k("range_calls"),m.rangeCalls},{k("calendar_unknown"),m.calendarUnknown},{k("cache_unknown"),m.cacheUnknown}});
        for(const auto &c:calls) cs.append(QJsonObject{{k("call_id"),c.id},{k("match_hash"),c.match},{k("sequence"),double(c.sequence)},
            {k("requested_model"),c.requested},{k("provider_model"),c.model},{k("task"),c.task},{k("reason"),c.reason},{k("started_utc"),stamp(c.start)},{k("finished_utc"),stamp(c.end)},
            {k("terminal"),c.terminal},{k("usage_known"),c.usageKnown},{k("cost_known"),c.costKnown},{k("cache_split_known"),c.split},{k("calendar_or_timing_unknown"),c.timeRange},
            {k("prompt_tokens"),c.prompt},{k("completion_tokens"),c.output},{k("cache_hit_tokens"),c.hit},{k("cache_miss_tokens"),c.miss},
            {k("min_nano"),QString::number(c.minNano)},{k("max_nano"),QString::number(c.maxNano)}});
        return {{k("schema_version"),1},{k("estimated"),true},{k("not_invoice"),true},{k("sequence"),double(sequence)},{k("matches"),ms},{k("calls"),cs}};
    }
    void persist() {
        prune();updateMeans(); if(file.isEmpty()) return;
        auto snapshot=serialized();
        const auto payload=QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
        snapshot[k("payload_sha256")]=QString::fromLatin1(QCryptographicHash::hash(payload,QCryptographicHash::Sha256).toHex());
        const auto data=QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
        // QSaveFile preserves the last complete state across process interruption.
        // A second atomic copy permits recovery from a malformed primary file.
        QSaveFile next(file);
        if(!next.open(QIODevice::WriteOnly)||next.write(data)!=data.size()||!next.commit()) {persistenceOk=false;persistenceError=k("snapshot_write_failed");return;}
        QSaveFile backup(file+k(".previous"));
        if(!backup.open(QIODevice::WriteOnly)||backup.write(data)!=data.size()||!backup.commit()) {persistenceOk=false;persistenceError=k("backup_write_failed");return;}
        persistenceOk=true;persistenceError.clear();
    }
    bool restore(const QByteArray &data) {
        QJsonParseError error;const auto doc=QJsonDocument::fromJson(data,&error);auto o=doc.object();
        if(error.error!=QJsonParseError::NoError||o.value(k("schema_version")).toInt()!=1||!o.value(k("matches")).isArray()||!o.value(k("calls")).isArray()) return false;
        const auto digest=o.take(k("payload_sha256")).toString();
        if(digest.size()!=64||digest!=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(o).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex()))return false;
        const auto ms=o.value(k("matches")).toArray(),cs=o.value(k("calls")).toArray();
        if(ms.size()>RetainedMatches+8||cs.size()>RetainedCalls+16) return false;
        static const QRegularExpression hashSyntax(k("\\A[0-9a-f]{64}\\z")),idSyntax(k("\\A[0-9a-f-]{36}\\z"));
        QHash<QString,Match> newMatches;QHash<QString,Call> newCalls;
        for(const auto &v:ms) {
            const auto a=v.toObject();Match m;m.id=a.value(k("id")).toString();if(!hashSyntax.match(m.id).hasMatch()||newMatches.contains(m.id)) return false;
            m.sequence=integer(a,"sequence");m.completed=a.value(k("completed")).toBool();m.active=false;
            m.calls=integer(a,"calls");m.pending=integer(a,"pending");m.knownUsage=integer(a,"known_usage");m.unknownCost=integer(a,"unknown_cost");m.prompt=integer(a,"prompt");m.output=integer(a,"output");m.rangeCalls=integer(a,"range_calls");m.calendarUnknown=integer(a,"calendar_unknown");m.cacheUnknown=integer(a,"cache_unknown");
            bool okMin=false,okMax=false;m.minNano=a.value(k("min_nano")).toString().toLongLong(&okMin);m.maxNano=a.value(k("max_nano")).toString().toLongLong(&okMax);
            if(!okMin||!okMax||m.calls<0||m.pending<0||m.pending>m.calls||m.knownUsage<0||m.knownUsage>m.calls-m.pending||m.unknownCost<m.calls-m.knownUsage||m.unknownCost>m.calls||m.prompt<0||m.output<0||m.minNano<0||m.maxNano<m.minNano||m.rangeCalls<0||m.rangeCalls>m.calls||m.calendarUnknown<0||m.calendarUnknown>m.calls||m.cacheUnknown<0||m.cacheUnknown>m.knownUsage) return false;
            newMatches.insert(m.id,m);
        }
        for(const auto &v:cs) {
            const auto a=v.toObject();Call c;c.id=a.value(k("call_id")).toString();c.match=a.value(k("match_hash")).toString();
            if(!idSyntax.match(c.id).hasMatch()||newCalls.contains(c.id)||(!c.match.isEmpty()&&!newMatches.contains(c.match))) return false;
            c.sequence=integer(a,"sequence");c.requested=modelField(a.value(k("requested_model")).toString());c.model=modelField(a.value(k("provider_model")).toString());c.task=taskField(a.value(k("task")).toString());c.reason=reasonField(a.value(k("reason")).toString());
            c.start=timeField(a,"started_utc");c.end=timeField(a,"finished_utc");c.terminal=a.value(k("terminal")).toBool();c.usageKnown=a.value(k("usage_known")).toBool();c.costKnown=a.value(k("cost_known")).toBool();c.split=a.value(k("cache_split_known")).toBool();c.timeRange=a.value(k("calendar_or_timing_unknown")).toBool();
            c.prompt=integer(a,"prompt_tokens");c.output=integer(a,"completion_tokens");c.hit=integer(a,"cache_hit_tokens");c.miss=integer(a,"cache_miss_tokens");c.minNano=a.value(k("min_nano")).toString().toLongLong();c.maxNano=a.value(k("max_nano")).toString().toLongLong();
            if(!c.start.isValid()||c.prompt<0||c.output<0||c.hit<0||c.miss<0||c.minNano<0||c.maxNano<c.minNano) return false;
            if(!c.terminal) { c.terminal=true;c.reason=k("process_interrupted");c.end=QDateTime::currentDateTimeUtc();if(!c.match.isEmpty()) {auto &m=newMatches[c.match];if(m.pending<=0)return false;--m.pending;} }
            newCalls.insert(c.id,c);
        }
        for(const auto &m:newMatches) if(m.pending!=0) return false;
        matches=std::move(newMatches);calls=std::move(newCalls);sequence=integer(o,"sequence");current.clear();nonliveCalls=0;
        for(const auto &c:calls)if(c.match.isEmpty())++nonliveCalls;
        updateMeans();return true;
    }
    void terminate(Call &c,const QDateTime &finished) {
        c.terminal=true;c.end=finished.toUTC();
        if(!c.match.isEmpty()&&matches.contains(c.match)) {
            auto &m=matches[c.match];--m.pending;
            if(c.usageKnown) {++m.knownUsage;m.prompt+=c.prompt;m.output+=c.output;if(!c.split)++m.cacheUnknown;}
            if(c.timeRange)++m.calendarUnknown;
            if(c.costKnown) {--m.unknownCost;m.minNano+=c.minNano;m.maxNano+=c.maxNano;if(c.minNano!=c.maxNano)++m.rangeCalls;}
        }
        persist();
    }
};

DsUsageMeter::DsUsageMeter():d(new State) {}
DsUsageMeter::~DsUsageMeter()=default;
DsUsageMeter &dsUsageMeter() { static DsUsageMeter meter;return meter; }
bool DsUsageMeter::configure(const QString &root) {
    QMutexLocker lock(&d->mutex);
    if(root.isEmpty()) return false;
    const auto absolute=QDir(root).absolutePath();
    if(d->root==absolute) return d->persistenceOk;
    // Changing roots during live requests would silently separate one ledger.
    if(!d->root.isEmpty()||!d->calls.isEmpty()||!d->matches.isEmpty()) return false;
    d->root=absolute;const auto dir=absolute+k("/.local/ds-usage");d->file=dir+k("/ledger.v1.json");
    if(!QDir().mkpath(dir)) {d->persistenceOk=false;d->persistenceError=k("directory_unwritable");d->file.clear();return false;}
    d->fileLock.reset(new QLockFile(d->file+k(".lock")));d->fileLock->setStaleLockTime(0);
    if(!d->fileLock->tryLock(0)) {d->persistenceOk=false;d->persistenceError=k("ledger_in_use");d->file.clear();d->fileLock.reset();return false;}
    if(QFile::exists(d->file)||QFile::exists(d->file+k(".previous"))) {
        bool restored=false;
        for(const auto &path:{d->file,d->file+k(".previous")}) {
            QFile f(path);if(!f.open(QIODevice::ReadOnly)||f.size()>16*1024*1024) continue;
            if(d->restore(f.readAll())) {restored=true;d->recovered=path!=d->file;break;}
        }
        // Preserve the evidence rather than overwriting an unreadable ledger.
        if(!restored) {d->persistenceOk=false;d->persistenceError=k("ledger_unreadable");d->file.clear();return false;}
    }
    d->persist();return d->persistenceOk;
}
void DsUsageMeter::observeContext(const QString &contextId,bool active,bool finishedReliable) {
    const auto id=hashContext(contextId);if(id.isEmpty())return;
    QMutexLocker lock(&d->mutex);
    bool changed=false;
    if(active) {for(auto &m:d->matches)if(m.id!=id&&m.active){m.active=false;changed=true;}d->current=id;}
    if(!d->matches.contains(id)&&!active) return;
    auto &m=d->matches[id];if(m.id.isEmpty()){m.id=id;m.sequence=++d->sequence;changed=true;}
    const bool nextActive=active&&!m.completed&&!finishedReliable;
    if(!changed&&m.active==nextActive&&(!finishedReliable||m.completed))return;
    m.active=nextActive;if(finishedReliable){m.completed=true;m.active=false;}
    d->persist();
}
QString DsUsageMeter::beginCall(const QString &contextId,const QString &requestedModel,const QString &task,bool liveEligible,const QDateTime &startedUtc) {
    QMutexLocker lock(&d->mutex);State::Call c;c.id=QUuid::createUuid().toString(QUuid::WithoutBraces);c.sequence=++d->sequence;c.start=startedUtc.toUTC();c.requested=modelField(requestedModel);c.task=taskField(task);
    if(liveEligible&&!contextId.isEmpty()) {
        c.match=hashContext(contextId);auto &m=d->matches[c.match];if(m.id.isEmpty()){m.id=c.match;m.sequence=++d->sequence;}++m.calls;++m.pending;++m.unknownCost;
    } else ++d->nonliveCalls;
    const auto id=c.id;d->calls.insert(id,c);d->persist();return id;
}
void DsUsageMeter::finishCall(const QString &callId,const QJsonObject &envelope,const QDateTime &finishedUtc) {
    QMutexLocker lock(&d->mutex);auto it=d->calls.find(callId);if(it==d->calls.end()||it->terminal)return;
    auto &c=it.value();c.model=modelField(envelope.value(k("model")).toString());const auto u=usage(envelope);
    if(!u.valid)c.reason=envelope.value(k("usage")).isObject()?k("invalid_usage"):k("missing_usage");
    else {
        c.usageKnown=true;c.prompt=u.prompt;c.output=u.output;c.hit=u.hit;c.miss=u.miss;c.split=u.split;
        const auto r=rate(c.model);
        if(r.known) {
            c.costKnown=true;c.timeRange=possiblyPeakDuring(c.start,finishedUtc.toUTC());
            c.minNano=(u.split?u.hit*r.hit+u.miss*r.miss:u.prompt*r.hit)+u.output*r.output;
            c.maxNano=((u.split?u.hit*r.hit+u.miss*r.miss:u.prompt*r.miss)+u.output*r.output)*(c.timeRange?2:1);
        }
    }
    d->terminate(c,finishedUtc);
}
void DsUsageMeter::finishUnknown(const QString &callId,const QString &reason,const QDateTime &finishedUtc) {
    QMutexLocker lock(&d->mutex);auto it=d->calls.find(callId);if(it==d->calls.end()||it->terminal)return;
    it->reason=reasonField(reason);d->terminate(it.value(),finishedUtc);
}
QJsonObject DsUsageMeter::summary(const QString &contextId) const {
    QMutexLocker lock(&d->mutex);const auto id=contextId.isEmpty()?d->current:hashContext(contextId);
    const auto m=d->matches.value(id);const bool found=!m.id.isEmpty();
    return {{k("schema_version"),1},{k("estimated"),true},{k("not_invoice"),true},{k("currency"),k("CNY")},
        {k("price_evidence_date"),k("2026-11-03")},{k("pricing_policy"),k("provider_returned_model_only;calendar_and_boundary_uncertainty_use_range")},
        {k("current_match"),found?d->matchSummary(m):QJsonObject{{k("has_calls"),false},{k("call_count"),0},{k("cost_complete"),false}}},
        {k("completed_matches"),d->means.isEmpty()?QJsonObject{{k("completed_match_count"),0},{k("priced_match_count"),0},{k("mean_cost_min_cny"),QJsonValue(QJsonValue::Null)},{k("mean_cost_max_cny"),QJsonValue(QJsonValue::Null)}}:d->means},
        {k("persistence_ok"),d->persistenceOk},{k("persistence_enabled"),!d->file.isEmpty()},{k("persistence_error"),d->persistenceError},{k("recovered_previous_snapshot"),d->recovered},
        {k("retained_nonlive_call_count"),d->nonliveCalls},{k("separate_web_search_charge_cny"),0},{k("web_search_scope"),k("current_keyless_official_and_wiki_backend_only")}};
}
