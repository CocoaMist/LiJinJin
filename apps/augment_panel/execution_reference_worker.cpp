#include "execution_reference_worker.h"
#include "combat.h"
#include "future_completion.h"
#include "headliner.h"
#include <QCryptographicHash>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cmath>

namespace {
QString k(const char *s){return QString::fromLatin1(s);}
constexpr int explanationBudgetMs=350;
constexpr qint64 minimumDispatchMs=250,periodicRefreshMs=5000,referenceLifetimeMs=8000;
QByteArray hash(const QJsonObject &object){return QCryptographicHash::hash(QJsonDocument(object).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256);}
QJsonObject ranks(const QJsonObject &abilities){
    QJsonObject out;
    for(auto it=abilities.begin();it!=abilities.end();++it){auto ability=it.value().toObject();
        ability.remove(k("cooldownRemaining"));ability.remove(k("cooldown"));out[it.key()]=ability;}
    return out;
}
QJsonObject referenceInputs(const GameState &state){
    // Exactly the dynamic fields removed/overridden by the synthetic reference
    // builder. Everything else remains in the basis, including all counters,
    // ranks/forms, unknown markers and raw loadout completeness evidence.
    QJsonArray players;
    for(const auto &value:state.players){auto row=value.toObject();
        for(const auto &name:{k("current_health"),k("is_dead"),k("action_state"),k("conditions"),k("effect_conditions"),k("scores"),k("respawn_seconds")})row.remove(name);
        if(row.value(k("abilities")).isObject())row[k("abilities")]=ranks(row.value(k("abilities")).toObject());
        players.append(row);
    }
    QJsonObject counters;for(auto it=state.effectCounters.begin();it!=state.effectCounters.end();++it)counters[it.key()]=it.value();
    QJsonArray owned,selected;for(int id:state.owned)owned.append(id);for(int id:state.selected)selected.append(id);
    return {{k("players"),players},{k("champion"),state.championId},{k("level"),state.level},
        {k("owned"),owned},{k("selected"),selected},{k("abilities"),ranks(state.abilities)},
        {k("effect_counters"),counters},{k("headliner_context"),state.headlinerContext},
        {k("headliner_observation"),observedHeadliner(state)}};
}
void fieldsInto(const QJsonObject &patch,QJsonObject &out){
    for(const auto &name:{k("hero_execution_plan"),k("hero_execution_reference"),k("hero_execution_budget"),
             k("hero_execution_stage_timings_ms"),k("execution_reference_pending"),k("execution_reference_status"),
             k("execution_reference_binding")})if(patch.contains(name))out[name]=patch.value(name);
}
}

AsyncExecutionReference::AsyncExecutionReference(ComputeFunction compute,QObject *parent)
    :QObject(parent),computeFunction(std::move(compute)){
    clock.start();pool.setMaxThreadCount(1);pool.setExpiryTimeout(-1);pool.setThreadPriority(QThread::LowPriority);
    if(!computeFunction)computeFunction=[](const Catalog &catalog,const GameState &state,const QString &preference,
        int budget,const RecommendationWorkPtr &work){return buildCoachExecutionExplanation(catalog,state,preference,budget,work);};
    connect(&watcher,&QFutureWatcher<QJsonObject>::finished,this,[this]{completed();});
    dispatchTimer.setSingleShot(true);connect(&dispatchTimer,&QTimer::timeout,this,[this]{maybeDispatch();});
}

AsyncExecutionReference::~AsyncExecutionReference(){
    finishing=true;dispatchTimer.stop();onUpdated={};desired.reset();
    disconnect(&watcher,nullptr,this,nullptr);if(activeWork)activeWork->cancel();
    // Native explanation calls are finite and cooperative between scenarios.
    // No thread termination and no wait on the global pool.
    watcher.waitForFinished();pool.waitForDone();
}

AsyncExecutionReference::Request AsyncExecutionReference::request(const Catalog &catalog,const GameState &state,const QString &preference) const{
    Request out{catalog,state,preference,{},{},{},clock.elapsed()};
    out.scope=hash({{k("root"),catalog.root},{k("patch"),catalog.patch},
        {k("revision"),catalog.combat?catalog.combat->revision:QString()},
        {k("manifest"),catalog.runtimeManifest},{k("context"),state.matchContext.value(k("context_id"))},
        {k("context_active"),state.matchContext.value(k("active"))},{k("phase"),state.phase},
        {k("source"),int(state.source)},{k("mode"),state.gameMode},{k("queue"),state.queueId},
        {k("observed_patch"),state.observedPatch},{k("champion"),state.championId}});
    out.referenceBasis=hash({{k("scope"),QString::fromLatin1(out.scope.toHex())},
        {k("inputs"),referenceInputs(state)},{k("preference"),preference}});
    out.currentBasis=hash({{k("reference_basis"),QString::fromLatin1(out.referenceBasis.toHex())},
        {k("fingerprint"),state.fingerprint()},{k("live_stats"),state.liveStats},
        {k("hp"),state.hp},{k("game_time"),state.gameTime},{k("growth"),state.growthObservations},
        {k("received_at"),state.received.toString(Qt::ISODateWithMs)}});
    return out;
}

bool AsyncExecutionReference::eligible(const Request &r) const{
    const auto &s=r.state;
    return s.fresh()&&modelRulesUsable(r.catalog,s)&&r.catalog.combat
        &&r.catalog.combat->champions.contains(s.championId)&&s.players.size()==10
        &&s.gameMode==k("KIWI")&&(s.phase==k("InProgress")||s.phase==k("Reconnect"))
        &&s.matchContext.value(k("active")).toBool()&&!s.matchContext.value(k("context_id")).toString().isEmpty()
        &&std::isfinite(s.gameTime)&&s.gameTime>=0;
}

bool AsyncExecutionReference::referenceCurrent(const Request &r) const{
    return published&&published->referenceBasis==r.referenceBasis&&eligible(r)
        &&r.state.gameTime>=published->state.gameTime
        &&r.state.gameTime-published->state.gameTime<=8
        &&clock.elapsed()-published->requestedAtMs<referenceLifetimeMs;
}

QJsonObject AsyncExecutionReference::project(const Request &r) const{
    auto out=pendingCoachExecutionExplanation(r.state);
    if(!referenceCurrent(r))return out;
    fieldsInto(publishedResult,out);
    const bool sameCurrent=published->currentBasis==r.currentBasis&&published->state.fresh();
    if(!sameCurrent){auto unavailable=pendingCoachExecutionExplanation(r.state).value(k("hero_execution_plan")).toObject();
        unavailable[k("status")]=k("current_inputs_changed_reference_only");out[k("hero_execution_plan")]=unavailable;}
    out[k("execution_reference_binding")]=QJsonObject{{k("context_id"),r.state.matchContext.value(k("context_id"))},
        {k("source_game_time"),published->state.gameTime},{k("source_age_ms"),clock.elapsed()-published->requestedAtMs},
        {k("reference_inputs_match"),true},{k("current_execution_inputs_match"),sameCurrent},
        {k("synthetic_reference_only"),true},{k("current_state_execution"),false}};
    return out;
}

QJsonObject AsyncExecutionReference::update(const Catalog &catalog,const GameState &state,const QString &preference){
    ++updates;auto next=request(catalog,state,preference);
    const bool rewind=desired&&desired->scope==next.scope&&lastGameTime>=0&&state.gameTime<lastGameTime;
    lastGameTime=state.gameTime;
    if(!eligible(next)||rewind){invalidate();lastStatus=rewind?k("game_time_regressed"):k("ineligible_snapshot");
        auto out=pendingCoachExecutionExplanation(state);out[k("execution_reference_pending")]=false;out[k("execution_reference_status")]=lastStatus;return out;}
    if(desired&&desired->currentBasis!=next.currentBasis&&active)++coalesced;
    // A new match/rule scope is never allowed to finish as current work.
    // Within one scope, allow the bounded native call to finish and then take
    // only the newest request; cancelling every health tick would starve it.
    if(active&&active->scope!=next.scope&&activeWork&&activeWork->stopReason()!=k("cancelled")){activeWork->cancel();++cancelled;}
    desired=std::move(next);maybeDispatch();lastStatus=referenceCurrent(*desired)?k("ready_reference"):k("pending_async_explanation");
    return project(*desired);
}

bool AsyncExecutionReference::apply(const Catalog &catalog,const GameState &state,const QString &preference,QJsonObject &coach) const{
    if(coach.isEmpty()||!coach.value(k("computation_complete")).toBool())return false;
    const auto next=request(catalog,state,preference);if(!eligible(next)){
        auto patch=pendingCoachExecutionExplanation(state);patch[k("execution_reference_pending")]=false;
        patch[k("execution_reference_status")]=k("ineligible_snapshot");fieldsInto(patch,coach);return false;}
    const auto purchaseContext=coach.value(k("plan")).toObject().value(k("purchase_sandbox")).toObject().value(k("context_id")).toString();
    if(!purchaseContext.isEmpty()&&purchaseContext!=state.matchContext.value(k("context_id")).toString()){
        auto patch=pendingCoachExecutionExplanation(state);patch[k("execution_reference_pending")]=false;
        patch[k("execution_reference_status")]=k("purchase_context_mismatch");fieldsInto(patch,coach);return false;}
    const auto patch=project(next);fieldsInto(patch,coach);return referenceCurrent(next);
}

void AsyncExecutionReference::invalidate(){
    dispatchTimer.stop();if(activeWork&&activeWork->stopReason()!=k("cancelled")){activeWork->cancel();++cancelled;}
    desired.reset();published.reset();publishedResult={};failedReferenceBasis.clear();retryAtMs=0;lastGameTime=-1;lastStatus=k("invalidated");
}

void AsyncExecutionReference::maybeDispatch(){
    if(finishing||active||!desired||!eligible(*desired))return;
    const auto now=clock.elapsed();
    if(referenceCurrent(*desired)&&now-published->requestedAtMs<periodicRefreshMs)return;
    if(desired->referenceBasis==failedReferenceBasis&&now<retryAtMs){dispatchTimer.start(int(retryAtMs-now));return;}
    if(now-lastDispatchMs<minimumDispatchMs){dispatchTimer.start(int(minimumDispatchMs-(now-lastDispatchMs)));return;}
    active=desired;lastDispatchMs=now;++dispatched;
    // Time slicing stays with the shared 350ms helper. This token cancels
    // between bounded native calls; elapsed timeout is disclosed by the helper
    // instead of deleting an honest partial/unavailable explanation afterward.
    activeWork=std::make_shared<RecommendationWork>(explanationBudgetMs);
    const auto job=*active;const auto work=activeWork;const auto compute=computeFunction;
    watcher.setFuture(QtConcurrent::run(&pool,[job,work,compute]{QElapsedTimer elapsed;elapsed.start();
        auto out=compute(job.catalog,job.state,job.preference,explanationBudgetMs,work);
        out[k("_worker_elapsed_ms")]=double(elapsed.nsecsElapsed())/1e6;return out;}));
}

void AsyncExecutionReference::completed(){
    if(finishing)return;const auto completed=completedJsonFuture(watcher);auto result=completed.value;++completions;
    lastWorkerMs=result.take(k("_worker_elapsed_ms")).toDouble();maxWorkerMs=std::max(maxWorkerMs,lastWorkerMs);
    QString rejection;
    if(!active||!desired)rejection=k("request_invalidated");
    else if(activeWork&&activeWork->stopReason()==k("cancelled"))rejection=k("cancelled");
    else if(!completed.available())rejection=completed.error;
    else if(active->referenceBasis!=desired->referenceBasis)rejection=k("newer_reference_inputs_pending");
    else if(!eligible(*active)||!eligible(*desired))rejection=k("snapshot_expired_or_scope_changed");
    else if(desired->state.gameTime<active->state.gameTime)rejection=k("game_time_regressed");
    else if(!result.value(k("hero_execution_reference")).isObject()||!result.value(k("hero_execution_plan")).isObject())rejection=k("invalid_explanation_output");
    if(rejection.isEmpty())for(const auto &name:{k("hero_execution_plan"),k("hero_execution_reference")}){
        const auto value=result.value(name).toObject();
        if(value.value(k("context_id"))!=active->state.matchContext.value(k("context_id"))
            ||value.value(k("champion_id"))!=QJsonValue(active->state.championId)
            ||value.value(k("patch"))!=QJsonValue(active->catalog.patch)
            ||value.value(k("model_game_time"))!=QJsonValue(active->state.gameTime)
            ||value.value(k("current_state_execution"))!=QJsonValue(false))rejection=k("explanation_binding_mismatch");
    }
    if(rejection.isEmpty()&&result.value(k("hero_execution_reference")).toObject().value(k("synthetic_reference"))!=QJsonValue(true))rejection=k("reference_is_not_explicitly_synthetic");
    if(rejection.isEmpty()){published=active;publishedResult=result;lastDiscardReason.clear();failedReferenceBasis.clear();retryAtMs=0;lastStatus=k("ready_reference");}
    else{++discarded;lastDiscardReason=rejection;lastStatus=k("discarded_explanation");if(active&&desired&&active->referenceBasis==desired->referenceBasis){failedReferenceBasis=active->referenceBasis;retryAtMs=clock.elapsed()+1000;}}
    active.reset();activeWork.reset();maybeDispatch();if(onUpdated)onUpdated();
}

QJsonObject AsyncExecutionReference::report() const{
    return {{k("status"),lastStatus},{k("worker_running"),watcher.isRunning()},{k("completion_pending"),bool(active)},
        {k("latest_snapshot_pending"),bool(desired&&(!referenceCurrent(*desired)||clock.elapsed()-published->requestedAtMs>=periodicRefreshMs))},
        {k("dispatched"),dispatched},{k("completed"),completions},{k("discarded"),discarded},
        {k("coalesced"),coalesced},{k("cancelled"),cancelled},{k("updates"),updates},{k("last_discard_reason"),lastDiscardReason},
        {k("last_worker_compute_ms"),lastWorkerMs},{k("max_worker_compute_ms"),maxWorkerMs},
        {k("worker_thread_priority"),k("low")},{k("global_thread_pool_used"),false},
        {k("shared_explanation_budget_ms"),explanationBudgetMs},{k("preemptive_deadline"),false},
        {k("current_input_exact_match_required"),true},{k("reference_structural_binding_required"),true},
        {k("periodic_refresh_ms"),periodicRefreshMs},{k("maximum_reference_age_ms"),referenceLifetimeMs},
        {k("purchase_result_independent"),true},{k("role_guidance_overwritten"),false}};
}
