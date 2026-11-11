#pragma once
#include "analysis.h"
#include <functional>
#include <memory>

// Only disk artifacts are cached. Callers must evaluate every current input
// against the artifact again; cached predictions or match state are forbidden.
class LearnedStateArtifactCache final {
public:
    using Clock = std::function<qint64()>;
    static constexpr qint64 probeIntervalMs = 1000;
    static constexpr qint64 fullRecheckIntervalMs = 30000;
    static constexpr qint64 maximumArtifactBytes = 8 * 1024 * 1024;
    static constexpr int maximumEntries = 8;

    // The optional monotonic clock permits deterministic filesystem checks.
    explicit LearnedStateArtifactCache(Clock clock = {});
    ~LearnedStateArtifactCache();
    LearnedStateArtifactCache(const LearnedStateArtifactCache &) = delete;
    LearnedStateArtifactCache &operator=(const LearnedStateArtifactCache &) = delete;
    QJsonObject read(const QString &root, const QString &relativePath);
    QJsonObject report() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Qualified artifacts only. This is an observed-state association model; it
// never estimates the causal benefit of an item or augment choice.
QJsonObject evaluateLearnedState(const QJsonObject &artifact, const GameState &state);
QJsonObject learnedStatePrediction(const QString &root, const GameState &state);
QJsonObject historicalPurchaseReference(const QString &root, const GameState &state);
