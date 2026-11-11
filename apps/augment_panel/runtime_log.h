#pragma once

#include <QString>
#include <QtGlobal>
#include <memory>

// A bounded local event log. The producer accepts only diagnostic enums and
// elapsed time. JSON conversion and all filesystem work run on the writer.
class RuntimeLog final {
public:
    enum class Event { PanelStarted, CatalogLoad, PanelReady, LiveClientState, OfferFeedState, ModelJob, CardJob, PanelStopped };
    enum class Outcome {
        None, Live, Demo, Replay, Fixture, Ready, Waiting, Unavailable, Missing, Stale, Stopped,
        Accepted, Discarded, Cancelled, TimedOut, Failed, Clean
    };
    struct Options {
        qint64 maxFileBytes = 512 * 1024;
        int retainedFiles = 3; // Includes the active file.
        int maxQueuedEvents = 256;
    };
    struct Stats {
        quint64 dropped = 0;
        quint64 written = 0;
        quint64 writeFailures = 0;
    };

    explicit RuntimeLog(QString directory);
    RuntimeLog(QString directory, Options options);
    ~RuntimeLog();
    RuntimeLog(const RuntimeLog &) = delete;
    RuntimeLog &operator=(const RuntimeLog &) = delete;

    // When the queue is full the oldest pending event yields to the newest.
    bool record(Event event, Outcome outcome = Outcome::None, qint64 elapsedMs = -1);
    Stats stats() const;

    // Close drains accepted events and never joins the disk writer. Callers
    // that own process shutdown can optionally wait for a bounded interval.
    void close();
    bool waitForFinished(int timeoutMs) const;

private:
    struct State;
    std::shared_ptr<State> state;
};
