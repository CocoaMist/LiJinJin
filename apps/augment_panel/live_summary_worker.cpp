#include "live_summary_worker.h"
#include <QtConcurrent/QtConcurrentRun>
#include <QThread>
#include <algorithm>
#include <cmath>

namespace {
QString k(const char *s) { return QString::fromLatin1(s); }
constexpr qint64 minimumDispatchMs = 250;
constexpr qint64 periodicRefreshMs = 1000;
constexpr qint64 freshnessMs = 4000;
bool snapshotCurrent(const GameState &state) {
    if (state.source == GameState::Source::Replay) return true;
    const auto age = state.received.msecsTo(QDateTime::currentDateTimeUtc());
    return state.source == GameState::Source::Live && state.received.isValid() && age >= 0 && age < freshnessMs;
}
QJsonObject staticRole(const QJsonObject &role) {
    QJsonObject out;
    // Life, respawn and availability belong to the newer observation. Only
    // loadout-derived role fields may outlive their computation snapshot.
    for (const auto &name : {k("roles"), k("capabilities"), k("model_available"), k("damage_share"),
             k("durability_share"), k("output_8s"), k("reference_effective_hp"), k("augments_complete")})
        if (role.contains(name)) out[name] = role.value(name);
    return out;
}
}

AsyncLiveCombatSummary::AsyncLiveCombatSummary(ComputeFunction compute, QObject *parent, int budgetMs)
    : QObject(parent), computeFunction(std::move(compute)), workerEngine(std::make_shared<LiveCombatSummary>()),
      workBudgetMs(std::clamp(budgetMs, 1, 1000)) {
    clock.start();
    pool.setMaxThreadCount(1);
    pool.setExpiryTimeout(-1);
    pool.setThreadPriority(QThread::LowPriority);
    if (!computeFunction) {
        const auto engine = workerEngine;
        computeFunction = [engine](const Catalog &catalog, const GameState &state, const RecommendationWorkPtr &work) {
            auto out = engine->update(catalog, state, work);
            out[k("_async_native_report")] = engine->report();
            return out;
        };
    }
    connect(&watcher, &QFutureWatcher<QJsonObject>::finished, this, [this] { completed(); });
    dispatchTimer.setSingleShot(true);
    connect(&dispatchTimer, &QTimer::timeout, this, [this] { maybeDispatch(); });
}

AsyncLiveCombatSummary::~AsyncLiveCombatSummary() {
    finishing = true;
    dispatchTimer.stop();
    onUpdated = {};
    desired.reset();
    disconnect(&watcher, nullptr, this, nullptr);
    if (activeWork) activeWork->cancel();
    // Cooperative checkpoints bound the production job between finite combat
    // calls. Never terminate a thread or wait on any shared/global pool.
    watcher.waitForFinished();
    pool.waitForDone();
}

void AsyncLiveCombatSummary::setSynchronousInspection(bool enabled) {
    synchronousInspection = enabled;
    dispatchTimer.stop();
    if (activeWork) activeWork->cancel();
    desired.reset(); published.reset(); publishedSummary = {};
    failedBasis.clear();
}

bool AsyncLiveCombatSummary::eligible(const Request &request) const {
    const auto &state = request.state;
    return snapshotCurrent(state) && state.gameMode == k("KIWI") && modelRulesUsable(request.catalog, state)
        && (state.phase == k("InProgress") || state.phase == k("Reconnect"))
        && std::isfinite(state.gameTime) && state.gameTime >= 0
        && observedLiveCombatSummary(request.catalog, state).value(k("current_context")).toBool();
}

bool AsyncLiveCombatSummary::modelCurrent(const Request &request) const {
    if (!published || published->basis != request.basis || !snapshotCurrent(published->state)
        || !snapshotCurrent(request.state) || request.state.gameTime + 1e-6 < published->state.gameTime) return false;
    // Replays have no external freshness clock, but a HUD cache still has a
    // finite lifetime. One unchanged comparison is refreshed at most each sec.
    return clock.elapsed() - published->requestedAtMs < freshnessMs;
}

QJsonObject AsyncLiveCombatSummary::project(const Request &request) const {
    auto out = observedLiveCombatSummary(request.catalog, request.state);
    out[k("async_model")] = true;
    out[k("model_worker_running")] = watcher.isRunning();
    out[k("model_source_age_ms")] = QJsonValue::Null;
    if (!modelCurrent(request)) return out;
    const auto saved = publishedSummary.value(k("players")).toArray();
    auto current = out.value(k("players")).toArray();
    if (saved.size() != current.size()) return out;
    for (int index = 0; index < current.size(); ++index) {
        auto row = current[index].toObject(); const auto model = saved[index].toObject();
        for (const auto &identity : {k("row_index"), k("champion_id"), k("team"), k("self"), k("level")})
            if (row.value(identity) != model.value(identity)) return out;
        // Explicit allowlist: newer observed KDA, death state, equipment and
        // timestamps always come from the current source, never the worker.
        for (const auto &name : {k("output_8s_estimate"), k("effective_health_estimate"), k("model_available"),
                 k("model_partial"), k("model_unknowns"), k("output_components"), k("target_context"),
                 k("self_observed_anchor")})
            row[name] = model.value(name);
        row[k("tactical_role")] = staticRole(model.value(k("tactical_role")).toObject());
        current[index] = row;
    }
    out[k("players")] = current;
    for (const auto &name : {k("model_available_players"), k("model_kind"), k("assumptions"), k("model_generation")})
        out[name] = publishedSummary.value(name);
    auto tactics = out.value(k("tactical_context")).toObject();
    tactics[k("reason")] = k("dynamic_tactics_not_reprojected");
    tactics[k("dynamic_availability_known")] = false;
    const auto oldTactics = publishedSummary.value(k("tactical_context")).toObject();
    for (const auto &name : {k("roles_are_observed_behavior"), k("scope"), k("numeric_role_threshold"),
             k("unknowns"), k("purchase_horizon"), k("cooldowns_known"), k("movement_simulated")})
        if (oldTactics.contains(name)) tactics[name] = oldTactics.value(name);
    QJsonArray roleRows;
    for (const auto &value : current) {
        const auto row = value.toObject(); auto role = row.value(k("tactical_role")).toObject();
        role[k("row_index")] = row.value(k("row_index"));
        roleRows.append(role);
    }
    tactics[k("players")] = roleRows;
    out[k("tactical_context")] = tactics;
    out[k("status")] = k("ready"); out[k("cache_hit")] = true;
    out[k("model_snapshot_game_time")] = published->state.gameTime;
    out[k("model_source_age_ms")] = published->state.received.isValid()
        ? QJsonValue(published->state.received.msecsTo(QDateTime::currentDateTimeUtc())) : QJsonValue(clock.elapsed() - published->requestedAtMs);
    out[k("model_compute_ms")] = lastWorkerMs;
    return out;
}

QJsonObject AsyncLiveCombatSummary::update(const Catalog &catalog, const GameState &state) {
    QElapsedTimer gui; gui.start(); ++updates;
    if (synchronousInspection) {
        lastStatus = k("synchronous_inspection");
        auto out = inspectionEngine.update(catalog, state);
        out[k("async_model")] = false; out[k("synchronous_inspection")] = true;
        lastGuiMs = double(gui.nsecsElapsed()) / 1e6; maxGuiMs = std::max(maxGuiMs, lastGuiMs);
        return out;
    }
    Request request{catalog, state, liveCombatSummaryBasis(catalog, state), clock.elapsed()};
    const bool rewind = desired && state.matchContext.value(k("context_id")) == desired->state.matchContext.value(k("context_id"))
        && lastGameTime >= 0 && state.gameTime + 1e-6 < lastGameTime;
    lastGameTime = state.gameTime;
    if (rewind || !eligible(request)) {
        dispatchTimer.stop();
        if (activeWork) activeWork->cancel();
        desired.reset(); published.reset(); publishedSummary = {}; failedBasis.clear();
        lastStatus = rewind ? k("game_time_regressed") : k("observed_only_ineligible");
        auto out = observedLiveCombatSummary(catalog, state);
        out[k("async_model")] = true; out[k("model_worker_running")] = watcher.isRunning();
        if (rewind) out[k("status")] = lastStatus;
        lastGuiMs = double(gui.nsecsElapsed()) / 1e6; maxGuiMs = std::max(maxGuiMs, lastGuiMs);
        return out;
    }
    if (desired && desired->basis != request.basis && (active || dispatchTimer.isActive())) ++coalesced;
    desired = request;
    if (active && active->basis != request.basis && activeWork) activeWork->cancel();
    maybeDispatch();
    auto out = project(request);
    lastStatus = out.value(k("status")).toString();
    lastGuiMs = double(gui.nsecsElapsed()) / 1e6; maxGuiMs = std::max(maxGuiMs, lastGuiMs);
    out[k("update_ms")] = lastGuiMs;
    return out;
}

void AsyncLiveCombatSummary::maybeDispatch() {
    // A finished future may still have its completion event queued on the GUI.
    // Keep ownership until completed() consumes it; otherwise replacing the
    // watcher here can make the old signal wait on a newer running future.
    if (finishing || synchronousInspection || active || !desired || !eligible(*desired)) return;
    const auto now = clock.elapsed();
    const bool sameCurrent = modelCurrent(*desired);
    if (sameCurrent && now - published->requestedAtMs < periodicRefreshMs) return;
    if (now - lastDispatchAt < minimumDispatchMs) {
        dispatchTimer.start(int(minimumDispatchMs - (now - lastDispatchAt))); return;
    }
    if (failedBasis == desired->basis && now - failedAt < periodicRefreshMs) {
        dispatchTimer.start(int(periodicRefreshMs - (now - failedAt))); return;
    }
    active = desired; lastDispatchAt = now; ++dispatched;
    activeWork = std::make_shared<RecommendationWork>(workBudgetMs);
    const auto job = *active; const auto work = activeWork; const auto compute = computeFunction;
    watcher.setFuture(QtConcurrent::run(&pool, [job, work, compute] {
        QElapsedTimer timer; timer.start();
        auto out = compute(job.catalog, job.state, work);
        out[k("_async_worker_ms")] = double(timer.nsecsElapsed()) / 1e6;
        out[k("_async_stopped")] = work->stopped();
        out[k("_async_stop_reason")] = work->stopReason();
        return out;
    }));
}

void AsyncLiveCombatSummary::completed() {
    if (finishing) return;
    auto result = watcher.result();
    ++completions;
    lastWorkerMs = result.take(k("_async_worker_ms")).toDouble(); maxWorkerMs = std::max(maxWorkerMs, lastWorkerMs);
    nativeReport = result.take(k("_async_native_report")).toObject();
    const bool stoppedAtReturn = result.take(k("_async_stopped")).toBool();
    const bool stoppedNow = activeWork && activeWork->stopped();
    const bool stopped = stoppedAtReturn || stoppedNow;
    const auto returnedStopReason = result.take(k("_async_stop_reason")).toString();
    // Cancellation can happen after the future finished but before this GUI
    // event runs. Recheck the live token rather than trusting its return copy.
    const auto stopReason = stoppedNow ? activeWork->stopReason() : returnedStopReason;
    QString rejection;
    if (synchronousInspection || !active || !desired) rejection = k("request_invalidated");
    else if (stopped) rejection = stopReason.isEmpty() ? k("work_stopped") : stopReason;
    else if (active->basis != desired->basis) rejection = k("newer_basis_pending");
    else if (!eligible(*active) || !eligible(*desired)) rejection = k("snapshot_expired_or_scope_changed");
    else if (desired->state.gameTime + 1e-6 < active->state.gameTime) rejection = k("game_time_regressed");
    else if (result.value(k("status")) != k("ready")) rejection = result.value(k("stop_reason")).toString(k("model_unavailable"));
    if (!rejection.isEmpty()) {
        ++discarded; lastDiscardReason = rejection;
        if (active && desired && active->basis == desired->basis) { failedBasis = active->basis; failedAt = clock.elapsed(); }
    } else {
        published = active; publishedSummary = result;
        failedBasis.clear(); lastDiscardReason.clear();
    }
    active.reset(); activeWork.reset();
    maybeDispatch();
    if (onUpdated) onUpdated();
}

QJsonObject AsyncLiveCombatSummary::report() const {
    auto out = synchronousInspection ? inspectionEngine.report() : nativeReport;
    if (!synchronousInspection) {
        out[k("fresh")] = bool(desired && eligible(*desired));
        out[k("model_available_players")] = desired && modelCurrent(*desired)
            ? publishedSummary.value(k("model_available_players")) : QJsonValue(0);
    }
    const bool needsResult = desired && (!modelCurrent(*desired)
        || (published && clock.elapsed() - published->requestedAtMs >= periodicRefreshMs));
    const QJsonObject async{{k("schema_version"), 1}, {k("status"), lastStatus}, {k("async_live_path"), !synchronousInspection},
        {k("synchronous_inspection"), synchronousInspection}, {k("worker_running"), watcher.isRunning()},
        {k("latest_snapshot_pending"), bool(needsResult && (!active || desired->basis != active->basis))},
        {k("updates"), updates}, {k("dispatched"), dispatched}, {k("completed"), completions}, {k("discarded"), discarded},
        {k("coalesced"), coalesced}, {k("last_discard_reason"), lastDiscardReason},
        {k("last_worker_compute_ms"), lastWorkerMs}, {k("max_worker_compute_ms"), maxWorkerMs},
        {k("last_gui_dispatch_ms"), lastGuiMs}, {k("max_gui_dispatch_ms"), maxGuiMs},
        {k("work_budget_ms"), workBudgetMs}, {k("minimum_dispatch_ms"), minimumDispatchMs}, {k("periodic_refresh_ms"), periodicRefreshMs},
        {k("freshness_limit_ms"), freshnessMs}, {k("worker_thread_priority"), k("low")},
        {k("global_thread_pool_used"), false}, {k("identity_fields_retained"), false},
        {k("budget_scope"), k("cooperative_between_finite_combat_calls; no_forced_thread_termination")}};
    for (auto it = async.begin(); it != async.end(); ++it) out[it.key()] = it.value();
    return out;
}
