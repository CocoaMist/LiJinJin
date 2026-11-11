#pragma once
#include <QJsonObject>
#include <QString>
#include <QtGlobal>
#include <memory>

// One in-flight write and one replaceable pending snapshot. Producers only
// copy implicitly shared JSON and briefly lock state; all serialization and
// filesystem operations belong to the low-priority writer thread.
class DiagnosticReportWriter final {
public:
    explicit DiagnosticReportWriter(QString path, int minimumIntervalMs = 500);
    ~DiagnosticReportWriter();
    DiagnosticReportWriter(const DiagnosticReportWriter &) = delete;
    DiagnosticReportWriter &operator=(const DiagnosticReportWriter &) = delete;

    // A sequence must be strictly newer than every previously accepted one.
    // Superseded pending snapshots never accumulate in a queue.
    bool submit(quint64 sequence, const QJsonObject &snapshot);
    QJsonObject report() const;

    // Both this method and the destructor are nonblocking, including while a
    // disk operation is in progress. The shared state outlives the owner.
    // flushLatest=false drops pending work; an atomic write already in progress
    // may complete. No new submissions are accepted after either shutdown mode.
    void shutdown(bool flushLatest = true);
    bool finished() const;

    // Explicit bounded wait for tests or a non-UI shutdown coordinator. Normal
    // UI code should poll finished() asynchronously if final drain is required.
    bool waitForFinished(int timeoutMs) const;

private:
    struct State;
    std::shared_ptr<State> state;
};
