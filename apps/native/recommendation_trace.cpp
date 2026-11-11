#include "recommendation_trace.h"
#include "analysis.h"
#include "combat.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QUuid>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
QString k(const char *s) { return QString::fromLatin1(s); }
QJsonValue nil() { return QJsonValue(QJsonValue::Null); }
bool finite(double n, double lo = 0, double hi = 1e9) { return std::isfinite(n) && n >= lo && n <= hi; }
bool integer(const QJsonValue &v, int lo, int hi) {
    return v.isDouble() && finite(v.toDouble(),lo,hi) && std::floor(v.toDouble()) == v.toDouble();
}
QJsonValue number(const QJsonValue &v, double lo = 0, double hi = 1e9) {
    return v.isDouble() && finite(v.toDouble(),lo,hi) ? v : nil();
}
QString enumValue(const QJsonValue &v, const QStringList &allowed) {
    return v.isString() && allowed.contains(v.toString()) ? v.toString() : k("unknown");
}
QString status(const QJsonValue &v) {
    return enumValue(v,{k("ready"),k("available"),k("partial"),k("unavailable"),k("interrupted"),
        k("calculating"),k("complete"),k("completed"),k("failed"),k("time_budget_exceeded"),k("waiting"),k("input_required")});
}
QJsonValue revision(const QJsonValue &v) {
    const auto s=v.toString(); if(s.size()!=64)return nil();
    for(const auto c:s)if(!((c>=QLatin1Char('0')&&c<=QLatin1Char('9'))||(c>=QLatin1Char('a')&&c<=QLatin1Char('f'))))return nil();
    return s;
}
QJsonValue patchVersion(const QJsonValue &value) {
    static const QRegularExpression pattern(QStringLiteral("\\A[1-9][0-9]{0,2}\\.[0-9]{1,3}(?:\\.[0-9]{1,6})?\\z"));
    const auto patch=value.toString();
    return patch.size()<=14&&pattern.match(patch).hasMatch()?value:nil();
}
QString patchRelation(const QJsonValue &observed,const QJsonValue &rules) {
    if(observed.isNull()||rules.isNull())return k("unknown");
    return observed==rules?k("match"):k("mismatch");
}
QJsonValue resultRuleBasis(const QJsonObject &result) {
    const auto basis=result.value(k("rule_basis")).toObject();
    const auto rules=patchVersion(basis.value(k("source_patch")));
    if(basis.value(k("rules_available"))!=QJsonValue(true)||rules.isNull())return nil();
    const auto observed=patchVersion(basis.value(k("observed_patch")));
    return QJsonObject{{k("source_patch"),rules},{k("observed_patch"),observed},
        {k("patch_relation"),patchRelation(observed,rules)}};
}
QByteArray bytes(const QJsonObject &o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }
QByteArray digest(const QJsonObject &o) { return QCryptographicHash::hash(bytes(o),QCryptographicHash::Sha256).toHex(); }
QJsonArray list(const QList<int> &v) { QJsonArray a; for(int id:v)a.append(id); return a; }
bool ids(const Catalog &c,const QList<int> &input,bool augment,int maximum,bool unique,QJsonArray &out) {
    if(input.size()>maximum)return false;QSet<int> seen;
    for(int id:input){if(id<=0||id>1000000||(augment?!c.augments.contains(id):!c.items.contains(id))||(unique&&seen.contains(id)))return false;seen.insert(id);out.append(id);}
    return true;
}
bool jsonIds(const Catalog &c,const QJsonValue &raw,bool augment,int maximum,bool unique,QList<int> &out) {
    if(!raw.isArray()||raw.toArray().size()>maximum)return false;
    for(const auto &v:raw.toArray()){if(!integer(v,1,1000000))return false;out.append(v.toInt());}
    QJsonArray checked;return ids(c,out,augment,maximum,unique,checked);
}
bool sameIds(QList<int> a,QList<int> b) { std::sort(a.begin(),a.end());std::sort(b.begin(),b.end());return a==b; }
QJsonValue itemId(const Catalog &c,const QJsonValue &v) { return integer(v,1,1000000)&&c.items.contains(v.toInt())?v:nil(); }
std::optional<QJsonObject> cardSurfaceObservation(const QJsonObject &source,const QString &contextKey) {
    const auto offerRevision=source.value(k("offer_revision")).toInteger(0);
    const auto disposition=enumValue(source.value(k("disposition")),{k("shown"),k("hidden"),k("waiting")});
    const auto offer=source.value(k("offer_slots")).toArray(),display=source.value(k("display_slots")).toArray();
    if(offerRevision<=0||disposition==k("unknown")||offer.size()!=3||display.size()!=3)return std::nullopt;

    QJsonArray offerSlots,displaySlots;
    QSet<int> offeredIds;
    for(int i=0;i<3;++i){
        const auto id=offer.at(i),visible=display.at(i);
        if(!id.isNull()&&(!integer(id,1,1000000)||offeredIds.contains(id.toInt())))return std::nullopt;
        if(!id.isNull())offeredIds.insert(id.toInt());
        offerSlots.append(id);
        if(visible.isNull()){displaySlots.append(nil());continue;}
        const auto row=visible.toObject();
        const auto visibleId=row.value(k("augment_id"));
        if(!integer(visibleId,1,1000000))return std::nullopt;
        // Keep conflicting visible IDs as observations so a replay can detect stale UI.
        const auto mode=enumValue(row.value(k("mode")),{k("mechanism"),k("conditional"),k("reference"),k("comparison")});
        QJsonObject shown{{k("augment_id"),visibleId},{k("mode"),mode}};
        const auto window=row.value(k("leading_window_seconds"));
        if(mode==k("comparison")&&(window==QJsonValue(3)||window==QJsonValue(20)))shown[k("leading_window_seconds")]=window;
        displaySlots.append(shown);
    }

    const auto round=source.value(k("round")),offerTime=source.value(k("offer_observed_at_ms"));
    const auto hideReason=enumValue(source.value(k("hide_reason")),{k("reroll_pending"),k("offer_closed"),k("offer_stale"),
        k("unbound_offer"),k("incomplete_offer"),k("foreground_lost"),k("overlay_disabled"),k("user_hidden"),k("scoreboard"),
        k("invalid_layout"),k("hud_editor"),k("waiting_offer"),k("rules_unavailable")});
    const auto generation=QString::fromLatin1(digest({{k("context_key"),contextKey},{k("offer_revision"),offerRevision}}));
    return QJsonObject{{k("schema_version"),1},{k("offer_revision"),offerRevision},{k("offer_generation_key"),generation},
        {k("round"),integer(round,1,100)?round:nil()},
        {k("offer_source_ref"),revision(source.value(k("offer_source_ref")))},
        {k("offer_observed_at_ms"),offerTime.toInteger(0)>0?offerTime:nil()},
        {k("offer_slots"),offerSlots},{k("display_slots"),displaySlots},{k("disposition"),disposition},
        {k("hide_reason"),disposition==k("shown")?nil():QJsonValue(hideReason)},
        {k("render_evidence"),k("caller_asserted_widget_visibility_not_screen_capture")}};
}
bool within(const QString &root,const QString &path) {
    const auto a=QDir::cleanPath(root),b=QDir::cleanPath(path);
#ifdef Q_OS_WIN
    return b.startsWith(a+QLatin1Char('/'),Qt::CaseInsensitive);
#else
    return b.startsWith(a+QLatin1Char('/'));
#endif
}
}

struct RecommendationTrace::Impl {
    explicit Impl(Detail level):detail(level) {}
    const Detail detail;
    struct State {
        QString workspace,runId,directory,lastSegment,lastError;
        RecommendationTraceOptions options;
        std::mutex mutex;
        std::condition_variable changed;
        std::deque<QByteArray> queue;
        bool stopping=false,writing=false,finished=false;
        std::atomic<bool> ready{false},failed{false};
        std::atomic<bool> priorityApplied{false},distinctThread{false};
        std::thread::id ownerThread;
        std::atomic<quint64> enqueued{0},written{0},dropped{0},errors{0},segments{0};
        std::atomic<int> queued{0};
    };
    QString workspace,runId,lastCaptureReason=k("not_recorded");
    QString contextId,contextKey;
    QMap<QString,QByteArray> last;
    QMap<QString,QJsonObject> lastSignatures;
    qint64 captureCalls=0,capturePayloads=0;
    QByteArray salt;
    std::shared_ptr<State> state;
    std::thread writer;
    bool closed=false;

    static void fail(const std::shared_ptr<State> &s,const char *reason) {
        s->errors.fetch_add(1);s->failed.store(true);
        {std::lock_guard lock(s->mutex);s->lastError=k(reason);}
    }
    static bool prepareDirectory(const std::shared_ptr<State> &s) {
        const auto root=QFileInfo(s->workspace);
        if(!QDir::isAbsolutePath(s->workspace)||s->workspace.startsWith(k("//"))||s->workspace.startsWith(k("\\\\"))||!root.isDir())return false;
        const auto canonical=root.canonicalFilePath();if(canonical.isEmpty())return false;
        const auto local=QDir(canonical).filePath(k(".local"));
        if(QFileInfo::exists(local)&&(!QFileInfo(local).isDir()||!within(canonical,QFileInfo(local).canonicalFilePath())))return false;
        if(!QDir().mkpath(local))return false;
        const auto directory=QDir(local).filePath(k("recommendation-trace"));
        if(QFileInfo::exists(directory)&&(!QFileInfo(directory).isDir()||!within(canonical,QFileInfo(directory).canonicalFilePath())))return false;
        if(!QDir().mkpath(directory)||!within(canonical,QFileInfo(directory).canonicalFilePath()))return false;
        s->directory=QFileInfo(directory).canonicalFilePath();return true;
    }
    static void run(const std::shared_ptr<State> &s) {
#ifdef Q_OS_WIN
        s->priorityApplied.store(SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL)!=0);
#endif
        s->distinctThread.store(std::this_thread::get_id()!=s->ownerThread);
        if(!prepareDirectory(s)){
            fail(s,"trace_directory_unavailable");
            std::lock_guard lock(s->mutex);s->dropped.fetch_add(s->queue.size());s->queue.clear();s->queued.store(0);s->finished=true;s->changed.notify_all();return;
        }
        s->ready.store(true);QFile file;qint64 size=0;quint64 segment=0;
        for(;;){
            QByteArray record;
            {
                std::unique_lock lock(s->mutex);s->changed.wait(lock,[&]{return s->stopping||!s->queue.empty();});
                if(s->queue.empty()&&s->stopping)break;
                record=std::move(s->queue.front());s->queue.pop_front();s->queued.store(int(s->queue.size()));s->writing=true;
            }
            bool ok=true;
            if(!file.isOpen()||size+record.size()>s->options.maximumSegmentBytes){
                if(file.isOpen()){file.flush();file.close();}
                const auto name=k("recommendations-")+s->runId+k("-")+QString::number(++segment).rightJustified(6,QLatin1Char('0'))+k(".jsonl");
                const auto path=QDir(s->directory).filePath(name);file.setFileName(path);
                // A fresh unpredictable segment never opens or truncates an
                // existing file, including an attacker-created symlink.
                ok=file.open(QIODevice::WriteOnly|QIODevice::NewOnly);size=0;
                if(ok){s->segments.fetch_add(1);std::lock_guard lock(s->mutex);s->lastSegment=name;}
            }
            if(ok){ok=file.write(record)==record.size()&&file.flush();if(ok)size+=record.size();}
            if(ok)s->written.fetch_add(1);
            else {fail(s,"trace_append_failed");s->dropped.fetch_add(1);file.close();size=0;}
            {
                std::lock_guard lock(s->mutex);s->writing=false;s->changed.notify_all();
            }
        }
        if(file.isOpen()){file.flush();file.close();}
        {std::lock_guard lock(s->mutex);s->finished=true;s->changed.notify_all();}
    }
    template<class BuildRecord>
    bool enqueue(const QString &event,const QJsonObject &signature,BuildRecord buildRecord) {
        const auto previous=lastSignatures.constFind(event);
        if(previous!=lastSignatures.cend()&&previous.value()==signature)return false;
        const auto hash=digest(signature);if(last.value(event)==hash)return false;
        auto record=buildRecord();
        record[k("event")]=event;record[k("recorded_at_utc")]=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        record[k("evidence_digest")]=QString::fromLatin1(hash);
        record[k("queue_drops_before_record")]=qint64(state->dropped.load());
        auto line=bytes(record);line.append('\n');
        if(line.size()>state->options.maximumRecordBytes){state->dropped.fetch_add(1);lastCaptureReason=k("record_size_limit");return false;}
        std::unique_lock lock(state->mutex,std::try_to_lock);
        if(!lock.owns_lock()){state->dropped.fetch_add(1);lastCaptureReason=k("writer_queue_busy");return false;}
        if(state->stopping||state->finished||state->failed.load()||int(state->queue.size())>=state->options.maximumQueuedRecords){state->dropped.fetch_add(1);lastCaptureReason=k("writer_unavailable_or_queue_full");return false;}
        state->queue.push_back(std::move(line));state->queued.store(int(state->queue.size()));state->enqueued.fetch_add(1);
        last[event]=hash;lastSignatures[event]=signature;state->changed.notify_one();return true;
    }
};

RecommendationTrace::RecommendationTrace(const QString &workspace,const RecommendationTraceOptions &options)
    :RecommendationTrace(workspace,Detail::Full,options) {}

RecommendationTrace::RecommendationTrace(const QString &workspace,Detail detail,const RecommendationTraceOptions &options)
    :impl(std::make_unique<Impl>(detail)) {
    impl->workspace=QDir::cleanPath(workspace);impl->salt=QUuid::createUuid().toRfc4122();
    impl->runId=QDateTime::currentDateTimeUtc().toString(k("yyyyMMddTHHmmsszzzZ"))+k("-")+QUuid::createUuid().toString(QUuid::Id128);
    impl->state=std::make_shared<Impl::State>();auto &s=*impl->state;s.workspace=impl->workspace;s.runId=impl->runId;s.options=options;s.ownerThread=std::this_thread::get_id();
    s.options.maximumQueuedRecords=std::clamp(s.options.maximumQueuedRecords,1,64);
    s.options.maximumRecordBytes=std::clamp(s.options.maximumRecordBytes,1024,16*1024);
    s.options.maximumSegmentBytes=std::clamp(s.options.maximumSegmentBytes,qint64(s.options.maximumRecordBytes),qint64(16*1024*1024));
    s.options.shutdownWaitMs=std::clamp(s.options.shutdownWaitMs,0,2000);
    try {impl->writer=std::thread([state=impl->state]{
        try {Impl::run(state);}
        catch(...){Impl::fail(state,"trace_writer_failed");std::lock_guard lock(state->mutex);state->dropped.fetch_add(state->queue.size());state->queue.clear();state->queued.store(0);state->finished=true;state->changed.notify_all();}
    });}
    catch(...){Impl::fail(impl->state,"trace_writer_start_failed");std::lock_guard lock(s.mutex);s.finished=true;}
}
RecommendationTrace::~RecommendationTrace(){close();}

void RecommendationTrace::record(const Catalog &catalog,const GameState &state,const QJsonObject &coachResult,const QJsonObject &cardDecision,const QJsonObject &displayEvidence,const RecommendationTraceBinding &binding) {
    ++impl->captureCalls;
    auto reject=[&](const char *reason){impl->lastCaptureReason=k(reason);};
    if(impl->closed)return reject("trace_closed");
    const auto context=state.matchContext.value(k("context_id")).toString();
    const auto display=binding.displayContextId==context?displayEvidence:QJsonObject{};
    const auto cardSurface=display.value(k("card_surface")).toObject();
    const bool cardSurfaceOnly=impl->detail==Detail::CardSurfaceOnly;
    if(cardSurfaceOnly&&cardSurface.isEmpty())return reject("no_card_surface");
    const bool fresh=state.fresh(),rulesUsable=modelRulesUsable(catalog,state);
    const bool mayObserveSurface=!cardSurface.isEmpty()&&(fresh||cardSurface.value(k("disposition"))!=k("shown"));
    if(state.source!=GameState::Source::Live||(!mayObserveSurface&&(!fresh||!rulesUsable))||state.gameMode!=k("KIWI")
        ||!state.matchContext.value(k("active")).toBool()||context.isEmpty()||context.size()>512||!finite(state.gameTime,0,86400)
        ||state.level<1||state.level>18||!catalog.champions.contains(state.championId)||state.players.size()>10)return reject("real_fresh_kiwi_context_and_local_rules_required");
    for(const auto c:context)if(c.unicode()<32||c.unicode()==127)return reject("context_invalid");
    if(!catalog.root.isEmpty()&&QDir::cleanPath(catalog.root).compare(impl->workspace,Qt::CaseInsensitive)!=0)return reject("catalog_workspace_mismatch");
    if(catalog.patch.size()>24)return reject("catalog_patch_invalid");
    for(const auto c:catalog.patch)if(!(c.isDigit()||c==QLatin1Char('.')))return reject("catalog_patch_invalid");
    const auto observedPatch=patchVersion(state.observedPatch),rulesPatch=rulesUsable?patchVersion(catalog.combat->patch):nil();
    const auto rulesRevision=rulesUsable?revision(catalog.combat->revision):nil();
    if(impl->contextId!=context){
        impl->contextId=context;
        impl->contextKey=QString::fromLatin1(QCryptographicHash::hash(impl->salt+context.toUtf8(),QCryptographicHash::Sha256).toHex());
    }
    const auto &contextKey=impl->contextKey;
    int accepted=0;impl->lastCaptureReason=k("unchanged_or_not_shown");
    if(mayObserveSurface)if(const auto observed=cardSurfaceObservation(cardSurface,contextKey)){
        const QJsonObject evidence{{k("schema_version"),1},{k("trace_run_id"),impl->runId},{k("context_key"),contextKey},
            {k("mode"),k("KIWI")},{k("champion_id"),state.championId},{k("patch"),catalog.patch},
            {k("observed_patch"),observedPatch},{k("rules_patch"),rulesPatch},{k("rules_revision"),rulesRevision},
            {k("patch_relation"),patchRelation(observedPatch,rulesPatch)},{k("card_surface"),*observed}};
        accepted+=impl->enqueue(k("card_surface_observed"),evidence,[&]{
            ++impl->capturePayloads;
            auto record=evidence;record[k("game_time")]=state.gameTime;return record;
        });
    }
    if(cardSurfaceOnly||!fresh||!rulesUsable){
        if(accepted)impl->lastCaptureReason=k("enqueued");
        return;
    }
    const auto coach=binding.coachContextId==context?coachResult:QJsonObject{};
    const auto cards=binding.cardContextId==context?cardDecision:QJsonObject{};
    const auto coachRuleBasis=resultRuleBasis(coach),cardRuleBasis=resultRuleBasis(cards);
    const auto selfRows=[&]{QList<QJsonObject> out;for(const auto &v:state.players)if(v.toObject().value(k("self")).toBool())out.append(v.toObject());return out;}();
    const bool selfIdentity=selfRows.size()==1&&selfRows.first().value(k("champion_id")).toInt()==state.championId;
    const auto self=selfIdentity?selfRows.first():QJsonObject{};
    QJsonArray owned,offers,selected;const bool ownedValid=ids(catalog,state.owned,false,6,false,owned),offersValid=ids(catalog,state.offers,true,12,true,offers),selectedValid=ids(catalog,state.selected,true,12,true,selected);
    QList<int> observedOwned,observedSelected;
    const bool inventoryComplete=selfIdentity&&self.value(k("items_complete")).toBool()
        &&ownedValid&&jsonIds(catalog,self.value(k("items")),false,6,false,observedOwned)&&sameIds(observedOwned,state.owned);
    const bool manualConfirmed=selfIdentity&&self.value(k("augment_source")).toString()==k("manual_confirmation")
        &&self.value(k("augments_complete")).toBool()&&selectedValid
        &&jsonIds(catalog,self.value(k("augment_ids")),true,12,true,observedSelected)&&sameIds(observedSelected,state.selected);
    const auto plan=coach.value(k("plan")).toObject(),purchase=plan.value(k("purchase_sandbox")).toObject(),quality=coach.value(k("recommendation_quality")).toObject();
    const auto modelStatus=coach.value(k("status")).isString()?status(coach.value(k("status")))
        :coach.value(k("computation_complete")).isBool()&&coach.value(k("computation_complete")).toBool()?k("completed"):status(purchase.value(k("status")));
    const QJsonObject model{{k("status"),modelStatus},{k("computation_complete"),coach.value(k("computation_complete")).isBool()?coach.value(k("computation_complete")):nil()},
        {k("enumeration_complete"),purchase.value(k("enumeration_complete")).isBool()?purchase.value(k("enumeration_complete")):nil()},
        {k("ranking_available"),plan.value(k("ranking_available")).isBool()?plan.value(k("ranking_available")):nil()},
        {k("computed_winner_item_id"),itemId(catalog,purchase.value(k("selected_item_id")))},
        {k("model_game_time"),number(coach.value(k("model_game_time")),0,86400)},
        {k("rule_basis"),coachRuleBasis},
        {k("confidence"),enumValue(quality.value(k("confidence")),{k("conditional"),k("candidate_only"),k("unavailable")})},
        {k("comparison_scope"),enumValue(quality.value(k("objective_scope")),{k("source_route_and_separate_damage_durability_comparison_without_ally_utility"),k("modeled_self_survival_without_full_engage_or_protection_value"),k("modeled_damage_and_self_durability_only")})},
        {k("external_strategy_revision"),revision(coach.value(k("external_strategy_reference")).toObject().value(k("revision")))},
        {k("catalog_strategy_revision"),revision(catalog.strategyEvidence.value(k("revision")))}};
    QJsonArray cardRows;QSet<int> comparedCards;
    if(offersValid&&cards.value(k("offers")).isArray()&&cards.value(k("offers")).toArray().size()<=12)for(const auto &v:cards.value(k("offers")).toArray()){
        const auto row=v.toObject();const auto id=row.value(k("augment_id"));if(!integer(id,1,1000000)||!state.offers.contains(id.toInt())||comparedCards.contains(id.toInt()))continue;
        comparedCards.insert(id.toInt());
        cardRows.append(QJsonObject{{k("augment_id"),id},{k("available"),row.value(k("available")).isBool()?row.value(k("available")):nil()},
            {k("evaluation_status"),enumValue(row.value(k("evaluation_status")),{k("computed"),k("current_window_zero"),k("trigger_unobserved"),k("effect_unmodeled"),k("input_missing"),k("unavailable"),k("not_eligible")})},
            {k("decision_numeric_available"),row.value(k("decision_numeric_available")).isBool()?row.value(k("decision_numeric_available")):nil()},
            {k("decision_gain_percent"),number(row.value(k("decision_gain_percent")),-1e6,1e6)},
            {k("current_gain_percent"),number(row.value(k("current_gain_percent")),-1e6,1e6)}});
    }
    const QJsonObject selection{{k("model_input_ids"),selectedValid?QJsonValue(selected):nil()},
        {k("confirmed_selected"),manualConfirmed?QJsonValue(selected):nil()},
        {k("confirmation_source"),manualConfirmed?k("manual_assertion"):k("unconfirmed")},{k("server_selection_verified"),false}};
    const auto ownedItems=inventoryComplete?QJsonValue(owned):nil();
    const auto actualOffers=offersValid?QJsonValue(offers):nil();
    std::optional<QJsonObject> payload;
    const auto baseRecord=[&]{
        if(!payload){
            ++impl->capturePayloads;
            // The same ordered offers can recur after a reroll. This key only
            // identifies their content within the salted trace context.
            const auto offerOrderKey=offersValid&&offers.size()==3
                ?QJsonValue(QString::fromLatin1(digest({{k("context_key"),contextKey},{k("offer_ids"),offers}}))):nil();
            auto recordedModel=model;
            const auto modelTime=model.value(k("model_game_time"));
            const auto timings=coach.value(k("stage_timings_ms")).toObject();
            recordedModel[k("model_game_age_ms")]=modelTime.isDouble()&&state.gameTime>=modelTime.toDouble()
                ?QJsonValue((state.gameTime-modelTime.toDouble())*1000):nil();
            recordedModel[k("calculation_ms")]=number(timings.value(k("total")));
            recordedModel[k("purchase_calculation_ms")]=number(purchase.value(k("elapsed_ms")));
            payload=QJsonObject{{k("schema_version"),1},{k("trace_run_id"),impl->runId},{k("context_key"),contextKey},{k("patch"),catalog.patch},{k("mode"),k("KIWI")},
                {k("observed_patch"),observedPatch},{k("rules_patch"),rulesPatch},{k("rules_revision"),rulesRevision},
                {k("patch_relation"),patchRelation(observedPatch,rulesPatch)},
                {k("champion_id"),state.championId},{k("level"),state.level},{k("game_time"),state.gameTime},
                {k("owned_items"),ownedItems},{k("inventory_complete"),inventoryComplete},
                {k("actual_offer_ids"),actualOffers},{k("offer_order_key"),offerOrderKey},{k("selection"),selection},
                {k("model"),recordedModel},{k("card_comparisons"),cardRows},{k("card_rule_basis"),cardRuleBasis},{k("adopted"),nil()},{k("match_outcome"),nil()},
                {k("inventory_change_proves_adoption"),false},{k("user_correction_is_unprompted_success"),false}};
        }
        return *payload;
    };
    const QJsonObject common{{k("context_key"),contextKey},{k("champion_id"),state.championId}};
    auto resultSignature=common;
    resultSignature[k("observed_patch")]=observedPatch;
    resultSignature[k("rules_patch")]=rulesPatch;
    resultSignature[k("rules_revision")]=rulesRevision;
    if(!coach.isEmpty()||!cards.isEmpty()){
        auto signature=resultSignature;signature[k("model")]=model;signature[k("cards")]=cardRows;signature[k("card_rule_basis")]=cardRuleBasis;
        signature[k("offers")]=actualOffers;signature[k("selection")]=selection;signature[k("level")]=state.level;
        accepted+=impl->enqueue(k("produced"),signature,baseRecord);
    }
    auto inventorySignature=common;inventorySignature[k("complete")]=inventoryComplete;inventorySignature[k("items")]=ownedItems;
    accepted+=impl->enqueue(k("inventory_observed"),inventorySignature,baseRecord);
    auto selectionSignature=common;selectionSignature[k("selection")]=selection;
    accepted+=impl->enqueue(k("selection_observed"),selectionSignature,baseRecord);
    if(display.value(k("rendered")).isBool()&&display.value(k("rendered")).toBool()&&display.value(k("visible")).isBool()&&display.value(k("visible")).toBool()){
        const auto shownItem=itemId(catalog,display.value(k("item_id")));QList<int> shownAugments;
        bool shownCards=cardSurface.isEmpty()&&offersValid&&jsonIds(catalog,display.value(k("augment_ids")),true,12,true,shownAugments);
        if(shownCards)for(int id:shownAugments)if(!state.offers.contains(id))shownCards=false;
        if(shownItem.isDouble()||(shownCards&&!shownAugments.isEmpty())){
            const QJsonObject shown{{k("item_id"),shownItem},{k("augment_ids"),shownCards?QJsonValue(list(shownAugments)):nil()},
                {k("source"),enumValue(display.value(k("source")),{k("computed"),k("role_guidance"),k("reference"),k("explicit_chain_condition_comparison")})},
                {k("role_scope"),enumValue(display.value(k("role_scope")),{k("damage"),k("survival"),k("ally_protection"),k("reference")})},
                {k("render_evidence"),k("caller_asserted_rendered_and_visible_not_screen_capture")}};
            auto signature=resultSignature;signature[k("shown")]=shown;signature[k("model")]=model;signature[k("offers")]=actualOffers;signature[k("selection")]=selection;signature[k("level")]=state.level;
            if(shownCards&&!shownAugments.isEmpty())signature[k("card_rule_basis")]=cardRuleBasis;
            accepted+=impl->enqueue(k("shown"),signature,[&]{auto record=baseRecord();record[k("display")]=shown;return record;});
        }
    }
    if(accepted)impl->lastCaptureReason=k("enqueued");
}

void RecommendationTrace::recordAttempt(const QString &context,const QJsonObject &attempt){
    if(impl->detail==Detail::CardSurfaceOnly||context.isEmpty()||context.size()>512||impl->closed)return;
    QJsonObject row{{k("schema_version"),1},{k("trace_run_id"),impl->runId},
        {k("context_key"),QString::fromLatin1(QCryptographicHash::hash(impl->salt+context.toUtf8(),QCryptographicHash::Sha256).toHex())}};
    for(const auto &name:{k("request_id"),k("game_time"),k("input_age_ms"),k("elapsed_ms")})row[name]=number(attempt.value(name),0,1e12);
    for(const auto &name:{k("stop_reason"),k("rejection_reason"),k("status")}){
        const auto value=attempt.value(name).toString();bool valid=value.size()<=80;
        for(const auto ch:value)valid&=(ch>=QChar(u'a')&&ch<=QChar(u'z'))||ch==QChar(u'_');
        row[name]=valid?QJsonValue(value):nil();
    }
    row[k("publication_accepted")]=attempt.value(k("publication_accepted")).toBool();
    QJsonObject timings;const auto source=attempt.value(k("stage_timings_ms")).toObject();
    for(const auto &name:{k("total"),k("purchase_plan"),k("input_validation"),k("teamfight_simulation"),k("augment_decision"),k("team_needs"),k("visible_snapshot"),k("hero_execution_plan"),k("hero_execution_reference")})if(source.contains(name))timings[name]=number(source.value(name),0,1e9);
    row[k("stage_timings_ms")]=timings;impl->enqueue(k("worker_attempt"),row,[&]{return row;});
}

QJsonObject RecommendationTrace::report() const {
    auto &s=*impl->state;
    QJsonObject out{{k("available"),s.ready.load()&&!s.failed.load()&&!impl->closed},{k("status"),impl->closed?k("closed"):s.failed.load()?k("unavailable"):s.ready.load()?k("ready"):k("starting")},
        {k("last_capture"),impl->lastCaptureReason},{k("queued"),s.queued.load()},{k("enqueued"),qint64(s.enqueued.load())},{k("written"),qint64(s.written.load())},
        {k("capture_calls"),impl->captureCalls},{k("capture_payloads"),impl->capturePayloads},
        {k("dropped"),qint64(s.dropped.load())},{k("write_errors"),qint64(s.errors.load())},{k("segments"),qint64(s.segments.load())},
        {k("maximum_queued_records"),s.options.maximumQueuedRecords},{k("maximum_record_bytes"),s.options.maximumRecordBytes},{k("maximum_segment_bytes"),s.options.maximumSegmentBytes},
        {k("filesystem_on_capture_thread"),false},{k("single_writer"),true},{k("low_priority_requested"),true},
        {k("low_priority_applied"),s.priorityApplied.load()},{k("writer_distinct_from_owner"),s.distinctThread.load()},
        {k("adoption_measured"),false},{k("private_player_identifiers_recorded"),false}};
    std::unique_lock lock(s.mutex,std::try_to_lock);
    if(lock.owns_lock()){out[k("last_error")]=s.lastError;out[k("last_segment")]=s.lastSegment;}
    return out;
}
bool RecommendationTrace::flush(int timeoutMs) {
    auto &s=*impl->state;std::unique_lock lock(s.mutex);
    const bool done=s.changed.wait_for(lock,std::chrono::milliseconds(std::clamp(timeoutMs,0,5000)),[&]{return (s.queue.empty()&&!s.writing)||s.finished;});
    return done&&s.queue.empty()&&!s.writing&&!s.failed.load();
}
void RecommendationTrace::close(int timeoutMs) {
    if(!impl||impl->closed)return;impl->closed=true;auto state=impl->state;
    {std::lock_guard lock(state->mutex);state->stopping=true;state->changed.notify_all();}
    if(!impl->writer.joinable())return;
    bool finished=false;
    {std::unique_lock lock(state->mutex);finished=state->changed.wait_for(lock,std::chrono::milliseconds(std::clamp(timeoutMs<0?state->options.shutdownWaitMs:timeoutMs,0,2000)),[&]{return state->finished;});}
    if(finished)impl->writer.join();
    else impl->writer.detach(); // Shared state owns the finite remaining IO.
}
