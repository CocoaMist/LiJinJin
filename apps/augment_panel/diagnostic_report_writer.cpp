#include "diagnostic_report_writer.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

struct DiagnosticReportWriter::State {
    using Clock = std::chrono::steady_clock;
    explicit State(QString target, int interval)
        : path(std::move(target)), minimumIntervalMs(std::clamp(interval, 0, 60000)),
          ownerThread(std::this_thread::get_id()) {}
    const QString path;
    const int minimumIntervalMs;
    const std::thread::id ownerThread;
    mutable std::mutex mutex;
    std::condition_variable changed;
    QJsonObject pendingSnapshot;
    quint64 pendingSequence = 0, lastAcceptedSequence = 0, lastWrittenSequence = 0;
    quint64 submitted = 0, accepted = 0, coalesced = 0, rejectedOld = 0;
    quint64 rejectedAfterShutdown = 0, written = 0, writeFailures = 0, droppedOnShutdown = 0;
    bool hasAcceptedSequence = false, pending = false, writing = false;
    bool stopping = false, drainLatest = true, finished = false;
    bool distinctThread = false, priorityApplied = false;
    QString lastError;
    double lastWriteMs = 0;
    Clock::time_point nextWriteAt{};

    static void run(const std::shared_ptr<State> &s) {
        bool priority = false;
#ifdef Q_OS_WIN
        priority = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL) != 0;
#endif
        {
            std::lock_guard lock(s->mutex);
            s->priorityApplied = priority;
            s->distinctThread = std::this_thread::get_id() != s->ownerThread;
        }
        for (;;) {
            QJsonObject snapshot;
            quint64 sequence = 0;
            {
                std::unique_lock lock(s->mutex);
                for (;;) {
                    if (s->stopping && (!s->drainLatest || !s->pending)) {
                        s->finished = true;
                        s->changed.notify_all();
                        return;
                    }
                    if (!s->pending) {
                        s->changed.wait(lock);
                        continue;
                    }
                    const auto now = Clock::now();
                    if (!s->stopping && now < s->nextWriteAt) {
                        s->changed.wait_until(lock, s->nextWriteAt);
                        continue;
                    }
                    snapshot = std::move(s->pendingSnapshot);
                    sequence = s->pendingSequence;
                    s->pending = false;
                    s->writing = true;
                    break;
                }
            }

            const auto started = Clock::now();
            QString error;
            bool success = false;
            const auto bytes = QJsonDocument(snapshot).toJson();
            if (!QDir().mkpath(QFileInfo(s->path).absolutePath())) {
                error = QStringLiteral("diagnostic report directory unavailable");
            } else {
                QSaveFile file(s->path);
                // Never fall back to overwriting the previous valid report.
                file.setDirectWriteFallback(false);
                if (!file.open(QIODevice::WriteOnly)) error = file.errorString();
                else if (file.write(bytes) != bytes.size()) {
                    error = file.errorString();
                    file.cancelWriting();
                } else if (!file.commit()) error = file.errorString();
                else success = true;
            }
            {
                std::lock_guard lock(s->mutex);
                s->writing = false;
                s->lastWriteMs = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
                s->nextWriteAt = Clock::now() + std::chrono::milliseconds(s->minimumIntervalMs);
                if (success) {
                    ++s->written;
                    s->lastWrittenSequence = sequence;
                    s->lastError.clear();
                } else {
                    ++s->writeFailures;
                    s->lastError = error;
                }
                s->changed.notify_all();
            }
        }
    }
};

DiagnosticReportWriter::DiagnosticReportWriter(QString path, int minimumIntervalMs)
    : state(std::make_shared<State>(std::move(path), minimumIntervalMs)) {
    // Detached ownership is intentional: shutdown never joins a thread blocked
    // by the disk. The worker retains State until its finite drain completes.
    std::thread(&State::run, state).detach();
}
DiagnosticReportWriter::~DiagnosticReportWriter() { shutdown(true); }

bool DiagnosticReportWriter::submit(quint64 sequence, const QJsonObject &snapshot) {
    std::lock_guard lock(state->mutex);
    ++state->submitted;
    if (state->stopping) { ++state->rejectedAfterShutdown; return false; }
    if (state->path.isEmpty()) return false;
    if (state->hasAcceptedSequence && sequence <= state->lastAcceptedSequence) {
        ++state->rejectedOld;
        return false;
    }
    if (state->pending) ++state->coalesced;
    state->pendingSnapshot = snapshot;
    state->pendingSequence = sequence;
    state->lastAcceptedSequence = sequence;
    state->hasAcceptedSequence = true;
    state->pending = true;
    ++state->accepted;
    state->changed.notify_one();
    return true;
}

QJsonObject DiagnosticReportWriter::report() const {
    std::lock_guard lock(state->mutex);
    return {{"available", !state->path.isEmpty()}, {"submitted", qint64(state->submitted)},
        {"accepted", qint64(state->accepted)}, {"coalesced", qint64(state->coalesced)},
        {"written", qint64(state->written)}, {"write_failures", qint64(state->writeFailures)},
        {"failures", qint64(state->writeFailures)},
        {"last_error", state->lastError}, {"rejected_old", qint64(state->rejectedOld)},
        {"rejected_after_shutdown", qint64(state->rejectedAfterShutdown)},
        {"dropped_on_shutdown", qint64(state->droppedOnShutdown)},
        {"pending", state->pending}, {"writing", state->writing}, {"finished", state->finished},
        {"last_accepted_sequence", qint64(state->lastAcceptedSequence)},
        {"last_written_sequence", qint64(state->lastWrittenSequence)},
        {"minimum_interval_ms", state->minimumIntervalMs}, {"last_write_ms", state->lastWriteMs},
        {"maximum_pending_snapshots", 1}, {"writer_distinct_from_owner", state->distinctThread},
        {"low_priority_applied", state->priorityApplied}, {"filesystem_on_submit_thread", false},
        {"serialization_on_submit_thread", false}, {"atomic_replace", true}, {"atomic", true}};
}

void DiagnosticReportWriter::shutdown(bool flushLatest) {
    std::lock_guard lock(state->mutex);
    state->stopping = true;
    // A later destructor call must not undo an explicit discard request.
    state->drainLatest = state->drainLatest && flushLatest;
    if (!state->drainLatest && state->pending) {
        state->pendingSnapshot = {};
        state->pending = false;
        ++state->droppedOnShutdown;
    }
    state->changed.notify_all();
}
bool DiagnosticReportWriter::finished() const {
    std::lock_guard lock(state->mutex);
    return state->finished;
}
bool DiagnosticReportWriter::waitForFinished(int timeoutMs) const {
    std::unique_lock lock(state->mutex);
    return state->changed.wait_for(lock, std::chrono::milliseconds(std::max(0, timeoutMs)),
        [&] { return state->finished; });
}
