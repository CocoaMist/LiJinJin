#include "runtime_log.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QStringList>
#include <QTimeZone>
#include <QUuid>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
using Event = RuntimeLog::Event;
using Outcome = RuntimeLog::Outcome;

const char *eventName(Event event) {
    switch (event) {
    case Event::PanelStarted: return "panel_started";
    case Event::CatalogLoad: return "catalog_load";
    case Event::PanelReady: return "panel_ready";
    case Event::LiveClientState: return "live_client_state";
    case Event::OfferFeedState: return "offer_feed_state";
    case Event::ModelJob: return "model_job";
    case Event::CardJob: return "card_job";
    case Event::PanelStopped: return "panel_stopped";
    }
    return "unknown";
}

const char *outcomeName(Outcome outcome) {
    switch (outcome) {
    case Outcome::None: return "none";
    case Outcome::Live: return "live";
    case Outcome::Demo: return "demo";
    case Outcome::Replay: return "replay";
    case Outcome::Fixture: return "fixture";
    case Outcome::Ready: return "ready";
    case Outcome::Waiting: return "waiting";
    case Outcome::Unavailable: return "unavailable";
    case Outcome::Missing: return "missing";
    case Outcome::Stale: return "stale";
    case Outcome::Stopped: return "stopped";
    case Outcome::Accepted: return "accepted";
    case Outcome::Discarded: return "discarded";
    case Outcome::Cancelled: return "cancelled";
    case Outcome::TimedOut: return "timed_out";
    case Outcome::Failed: return "failed";
    case Outcome::Clean: return "clean";
    }
    return "unknown";
}

const char *levelName(Outcome outcome, quint64 drops) {
    if (outcome == Outcome::Failed) return "error";
    if (drops || outcome == Outcome::TimedOut || outcome == Outcome::Unavailable || outcome == Outcome::Missing
        || outcome == Outcome::Stale || outcome == Outcome::Stopped) return "warning";
    return "info";
}

struct Entry {
    Event event{};
    Outcome outcome{};
    qint64 elapsedMs = -1;
    qint64 timestampMs = 0;
    qint64 sequence = 0;
};

QByteArray row(const Entry &entry, const QString &runId, quint64 drops) {
    QJsonObject fields{{"schema_version", 1}, {"run_id", runId}, {"seq", entry.sequence},
        {"ts_utc", QDateTime::fromMSecsSinceEpoch(entry.timestampMs, QTimeZone::UTC).toString(Qt::ISODateWithMs)},
        {"event", QString::fromLatin1(eventName(entry.event))}, {"level", QString::fromLatin1(levelName(entry.outcome, drops))}};
    if (entry.outcome != Outcome::None) fields["outcome"] = QString::fromLatin1(outcomeName(entry.outcome));
    if (entry.elapsedMs >= 0) fields["elapsed_ms"] = entry.elapsedMs;
    if (drops) fields["dropped_before"] = qint64(drops);
    return QJsonDocument(fields).toJson(QJsonDocument::Compact) + '\n';
}

bool plainFile(const QString &path) {
    const QFileInfo info(path);
    return !info.isSymLink() && !info.isJunction() && (!info.exists() || info.isFile());
}

bool plainDirectory(QString path) {
    // Walk the lexical parents so an existing link above a missing leaf is
    // caught before mkpath can create anything through it.
    for (;;) {
        const QFileInfo info(path);
        if (info.isSymLink() || info.isJunction() || (info.exists() && !info.isDir())) return false;
        const auto parent = info.absolutePath();
        if (parent == path) return true;
        path = parent;
    }
}

class RotatingLog {
public:
    RotatingLog(const QString &path, const RuntimeLog::Options &options)
        : directory(QDir(path).absolutePath()), lockPath(QDir(directory).filePath(QStringLiteral("runtime.lock"))),
          lock(lockPath), maxBytes(options.maxFileBytes) {
        lock.setStaleLockTime(0);
        paths.append(QDir(directory).filePath(QStringLiteral("runtime.jsonl")));
        for (int i = 1; i < options.retainedFiles; ++i)
            paths.append(QDir(directory).filePath(QStringLiteral("runtime.%1.jsonl").arg(i)));
    }

    bool append(const QByteArray &bytes) {
        if (!file.isOpen() && !open()) return false;
        if (file.pos() > maxBytes - bytes.size() && (!rotate() || !open())) return false;
        const auto start = file.pos();
        if (file.write(bytes) == bytes.size()) return true;
        // A partial append should never invalidate the prior complete rows.
        file.resize(start);
        file.close();
        return false;
    }

    bool flush() {
        if (!file.isOpen() || file.flush()) return true;
        file.close();
        return false;
    }

private:
    bool open() {
        if (!plainDirectory(directory) || !plainFile(lockPath)
            || !std::all_of(paths.cbegin(), paths.cend(), plainFile)) return false;
        if (!lock.isLocked() && (!QDir().mkpath(directory) || !lock.tryLock(0))) return false;
        file.setFileName(paths.first());
        if (!file.open(QIODevice::ReadWrite | QIODevice::Unbuffered)) return false;
        if (seekEndOfCompleteRows()) return true;
        file.close();
        return false;
    }

    bool seekEndOfCompleteRows() {
        const auto size = file.size();
        if (!size) return true;
        if (size > maxBytes) return file.resize(0) && file.seek(0);
        // Recover from a torn final write after abrupt process exit. Existing
        // complete lines stay byte-for-byte intact.
        qint64 end = size;
        while (end > 0) {
            const auto start = std::max(qint64(0), end - 512);
            if (!file.seek(start)) return false;
            const auto tail = file.read(end - start);
            if (tail.size() != end - start) return false;
            const auto newline = tail.lastIndexOf('\n');
            if (newline >= 0) {
                const auto validEnd = start + newline + 1;
                return (validEnd == size || file.resize(validEnd)) && file.seek(validEnd);
            }
            end = start;
        }
        return file.resize(0) && file.seek(0);
    }

    bool rotate() {
        file.close();
        if (!plainDirectory(directory) || !std::all_of(paths.cbegin(), paths.cend(), plainFile)) return false;
        if (QFileInfo::exists(paths.last()) && !QFile::remove(paths.last())) return false;
        for (qsizetype i = paths.size() - 1; i > 0; --i)
            if (QFileInfo::exists(paths[i - 1]) && !QFile::rename(paths[i - 1], paths[i])) return false;
        return true;
    }

    const QString directory;
    const QString lockPath;
    QLockFile lock;
    const qint64 maxBytes;
    QStringList paths;
    QFile file;
};
}

struct RuntimeLog::State {
    State(QString path, Options limits) : directory(std::move(path)), options(limits) {}

    static void run(const std::shared_ptr<State> &state) {
#ifdef Q_OS_WIN
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
        {
            RotatingLog disk(state->directory, state->options);
            const auto runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            std::array<Entry, 32> batch;
            for (;;) {
                size_t count = 0;
                quint64 pendingDrops = 0;
                {
                    std::unique_lock lock(state->mutex);
                    state->changed.wait(lock, [&] { return state->stopping || !state->queue.empty(); });
                    if (state->queue.empty()) break;
                    while (count < batch.size() && !state->queue.empty()) {
                        batch[count++] = state->queue.front();
                        state->queue.pop_front();
                    }
                    pendingDrops = std::exchange(state->unreportedDrops, 0);
                }

                quint64 written = 0, failed = 0;
                for (size_t i = 0; i < count; ++i) {
                    if (disk.append(row(batch[i], runId, pendingDrops))) {
                        ++written;
                        pendingDrops = 0;
                    } else {
                        ++failed;
                        ++pendingDrops;
                    }
                }
                const bool flushed = disk.flush();
                {
                    std::lock_guard lock(state->mutex);
                    state->unreportedDrops += pendingDrops;
                    state->stats.dropped += failed;
                    state->stats.writeFailures += failed + !flushed;
                    state->stats.written += written;
                }
            }
        } // Close the file and release the directory lock before signaling.
        std::lock_guard lock(state->mutex);
        state->finished = true;
        state->changed.notify_all();
    }

    const QString directory;
    const Options options;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Entry> queue;
    Stats stats;
    qint64 nextSequence = 1;
    quint64 unreportedDrops = 0;
    bool stopping = false;
    bool finished = false;
};

RuntimeLog::RuntimeLog(QString directory) : RuntimeLog(std::move(directory), Options{}) {}

RuntimeLog::RuntimeLog(QString directory, Options options) {
    options.maxFileBytes = std::clamp(options.maxFileBytes, qint64(512), qint64(64 * 1024 * 1024));
    options.retainedFiles = std::clamp(options.retainedFiles, 1, 8);
    options.maxQueuedEvents = std::clamp(options.maxQueuedEvents, 1, 4096);
    state = std::make_shared<State>(std::move(directory), options);
    if (state->directory.isEmpty()) {
        state->stopping = state->finished = true;
        return;
    }
    try {
        std::thread(&State::run, state).detach();
    } catch (const std::system_error &) {
        state->stopping = state->finished = true;
        ++state->stats.writeFailures;
    }
}

RuntimeLog::~RuntimeLog() { close(); }

bool RuntimeLog::record(Event event, Outcome outcome, qint64 elapsedMs) {
    std::lock_guard lock(state->mutex);
    if (state->stopping) return false;
    if (state->queue.size() == size_t(state->options.maxQueuedEvents)) {
        state->queue.pop_front();
        ++state->stats.dropped;
        ++state->unreportedDrops;
    }
    state->queue.push_back({event, outcome, elapsedMs, QDateTime::currentMSecsSinceEpoch(), state->nextSequence++});
    state->changed.notify_one();
    return true;
}

RuntimeLog::Stats RuntimeLog::stats() const {
    std::lock_guard lock(state->mutex);
    return state->stats;
}

void RuntimeLog::close() {
    std::lock_guard lock(state->mutex);
    state->stopping = true;
    state->changed.notify_all();
}

bool RuntimeLog::waitForFinished(int timeoutMs) const {
    std::unique_lock lock(state->mutex);
    return state->changed.wait_for(lock, std::chrono::milliseconds(std::max(0, timeoutMs)), [&] { return state->finished; });
}
