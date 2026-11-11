#include "runtime_log.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <cstdio>

namespace {
using Event = RuntimeLog::Event;
using Outcome = RuntimeLog::Outcome;

struct LogRows {
    QList<QJsonObject> rows;
    bool valid = true;
};

LogRows readRows(const QDir &directory, int retainedFiles) {
    LogRows result;
    for (int fileNumber = retainedFiles - 1; fileNumber >= 0; --fileNumber) {
        const auto name = fileNumber
            ? QStringLiteral("runtime.%1.jsonl").arg(fileNumber)
            : QStringLiteral("runtime.jsonl");
        QFile file(directory.filePath(name));
        if (!file.exists()) continue;
        if (!file.open(QIODevice::ReadOnly)) {
            result.valid = false;
            continue;
        }
        while (!file.atEnd()) {
            const auto line = file.readLine();
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(line, &error);
            if (!line.endsWith('\n') || error.error != QJsonParseError::NoError || !document.isObject()) {
                result.valid = false;
                break;
            }
            result.rows.append(document.object());
        }
        result.valid &= file.error() == QFileDevice::NoError;
    }
    return result;
}

bool safeSchema(const QJsonObject &row) {
    const QSet<QString> required{"schema_version", "ts_utc", "run_id", "seq", "level", "event"};
    const QSet<QString> optional{"outcome", "elapsed_ms", "dropped_before"};
    const auto rowKeys = row.keys();
    const QSet<QString> keys(rowKeys.cbegin(), rowKeys.cend());
    const auto timestamp = row.value("ts_utc").toString();
    return (required - keys).isEmpty() && (keys - required - optional).isEmpty()
        && row.value("schema_version").toInt() == 1
        && QDateTime::fromString(timestamp, Qt::ISODateWithMs).isValid() && timestamp.endsWith('Z')
        && !row.value("run_id").toString().isEmpty()
        && row.value("seq").toInteger() > 0
        && !row.value("level").toString().isEmpty()
        && !row.value("event").toString().isEmpty()
        && (!row.contains("outcome") || row.value("outcome").isString())
        && (!row.contains("elapsed_ms") || row.value("elapsed_ms").toInteger(-1) >= 0)
        && (!row.contains("dropped_before") || row.value("dropped_before").toInteger(-1) >= 0);
}
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    int checks = 0, failures = 0;
    const auto check = [&](bool ok, const char *name) {
        ++checks;
        if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", name); }
    };
    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        std::fputs("FAIL temporary_directory\n", stderr);
        return 1;
    }

    const QDir sessions(temporary.filePath(QStringLiteral("sessions")));
    RuntimeLog logger(sessions.absolutePath());
    const bool accepted = logger.record(Event::PanelStarted, Outcome::Demo)
        && logger.record(Event::CatalogLoad, Outcome::Ready, 25)
        && logger.record(Event::OfferFeedState)
        && logger.record(Event::CardJob, Outcome::TimedOut, 270)
        && logger.record(Event::PanelStopped, Outcome::Clean);
    check(accepted, "typed_events_accepted");
    logger.close();
    check(!logger.record(Event::ModelJob, Outcome::Accepted), "close_rejects_new_events");
    check(logger.waitForFinished(5000), "close_drains_accepted_events");
    const auto first = readRows(sessions, 3);
    check(first.valid && first.rows.size() == 5, "valid_json_line_for_each_event");
    if (first.rows.size() == 5) {
        const auto run = first.rows.first().value("run_id");
        bool ordered = true, schema = true;
        for (qsizetype i = 0; i < first.rows.size(); ++i) {
            const auto &row = first.rows.at(i);
            ordered &= row.value("run_id") == run && row.value("seq").toInteger() == i + 1;
            schema &= safeSchema(row);
        }
        check(ordered, "one_run_id_and_ordered_sequences");
        check(schema, "only_fixed_operational_fields_written");
        const auto &start = first.rows.at(0);
        const auto &catalog = first.rows.at(1);
        const auto &feed = first.rows.at(2);
        const auto &card = first.rows.at(3);
        const auto &stop = first.rows.at(4);
        check(start.value("event") == QStringLiteral("panel_started") && start.value("outcome") == QStringLiteral("demo")
            && catalog.value("event") == QStringLiteral("catalog_load") && catalog.value("elapsed_ms").toInteger() == 25
            && feed.value("event") == QStringLiteral("offer_feed_state") && !feed.contains("outcome") && !feed.contains("elapsed_ms")
            && card.value("event") == QStringLiteral("card_job") && card.value("outcome") == QStringLiteral("timed_out")
            && card.value("elapsed_ms").toInteger() == 270
            && stop.value("event") == QStringLiteral("panel_stopped") && stop.value("outcome") == QStringLiteral("clean"),
            "event_outcomes_and_optional_duration");
    }
    const auto stats = logger.stats();
    check(stats.written == 5 && stats.writeFailures == 0, "successful_write_count");

    RuntimeLog reopened(sessions.absolutePath());
    const bool reopenedAccepted = reopened.record(Event::PanelStarted, Outcome::Replay);
    reopened.close();
    const bool reopenedFinished = reopened.waitForFinished(5000);
    const auto second = readRows(sessions, 3);
    check(reopenedAccepted && reopenedFinished && second.valid && second.rows.size() == 6
        && second.rows.last().value("run_id") != second.rows.first().value("run_id")
        && second.rows.last().value("seq").toInteger() == 1
        && second.rows.last().value("outcome") == QStringLiteral("replay"), "reopen_preserves_history_and_starts_new_run");

    const QDir rotated(temporary.filePath(QStringLiteral("rotated")));
    RuntimeLog::Options options;
    options.maxFileBytes = 512;
    options.retainedFiles = 3;
    RuntimeLog rotating(rotated.absolutePath(), options);
    bool allAccepted = true;
    for (int i = 0; i < 40; ++i)
        allAccepted &= rotating.record(Event::ModelJob, Outcome::Accepted, i);
    allAccepted &= rotating.record(Event::PanelStopped, Outcome::Clean);
    rotating.close();
    check(allAccepted && rotating.waitForFinished(5000) && rotating.stats().written == 41,
        "rotation_drains_all_records");
    const auto files = rotated.entryInfoList({QStringLiteral("runtime*.jsonl")}, QDir::Files);
    bool sizesBounded = files.size() == options.retainedFiles;
    for (const auto &file : files) sizesBounded &= file.size() > 0 && file.size() <= options.maxFileBytes;
    check(sizesBounded, "rotation_respects_file_count_and_size_limits");
    const auto tail = readRows(rotated, options.retainedFiles);
    bool orderedTail = tail.valid && !tail.rows.isEmpty() && tail.rows.size() < 41;
    qint64 previous = 0;
    for (const auto &row : tail.rows) {
        const auto sequence = row.value("seq").toInteger();
        orderedTail &= safeSchema(row) && (previous == 0 || sequence == previous + 1);
        previous = sequence;
    }
    check(orderedTail && previous == 41 && tail.rows.last().value("event") == QStringLiteral("panel_stopped"),
        "rotated_segments_contain_complete_ordered_recent_lines");

    const auto blockedPath = temporary.filePath(QStringLiteral("file-instead-of-directory"));
    QFile blocker(blockedPath);
    const QByteArray sentinel("keep this file");
    const bool blockedFixture = blocker.open(QIODevice::WriteOnly) && blocker.write(sentinel) == sentinel.size();
    blocker.close();
    RuntimeLog blocked(blockedPath);
    QElapsedTimer producer;
    producer.start();
    for (int i = 0; i < 1000; ++i) blocked.record(Event::LiveClientState, Outcome::Unavailable);
    blocked.close();
    check(producer.elapsed() < 1000, "producer_and_close_do_not_wait_for_disk");
    const bool blockedFinished = blocked.waitForFinished(5000);
    check(blockedFixture && blockedFinished && blocked.stats().written == 0 && blocked.stats().writeFailures > 0
        && blocker.open(QIODevice::ReadOnly) && blocker.readAll() == sentinel, "unavailable_directory_does_not_damage_existing_file");

    std::printf("runtime_log_checks: %d/%d passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
