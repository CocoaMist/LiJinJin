#pragma once
#include <QJsonObject>
#include <QString>
#include <memory>

class Catalog;
struct GameState;

struct RecommendationTraceOptions {
    int maximumQueuedRecords = 64;
    int maximumRecordBytes = 16 * 1024;
    qint64 maximumSegmentBytes = 4 * 1024 * 1024;
    int shutdownWaitMs = 500;
};

// Bind each independently cached worker/render result to the context captured
// when it was created. These raw IDs are compared in memory and never persisted.
// Empty or mismatched bindings suppress that result, not live observations.
struct RecommendationTraceBinding {
    QString coachContextId;
    QString cardContextId;
    QString displayContextId;
};

// Single-producer interface: call from the owner/GUI thread after a model is
// produced and/or an actual render. record() does no filesystem IO; a bounded,
// low-priority writer owns every file. Never pass raw reports or settings.
class RecommendationTrace {
public:
    enum class Detail { Full, CardSurfaceOnly };
    explicit RecommendationTrace(const QString &workspace,
        const RecommendationTraceOptions &options = {});
    // Retain only the observed offer/widget evidence used by match review.
    RecommendationTrace(const QString &workspace, Detail detail,
        const RecommendationTraceOptions &options = {});
    ~RecommendationTrace();
    RecommendationTrace(const RecommendationTrace &) = delete;
    RecommendationTrace &operator=(const RecommendationTrace &) = delete;

    // Optional display evidence is a caller assertion of a completed render:
    // {rendered:true, visible:true, item_id:int, augment_ids:[actual offer IDs],
    //  source:"computed"|"role_guidance"|"reference"|"unknown",
    //  role_scope:"damage"|"survival"|"ally_protection"|"reference"}.
    // A model result alone can create "produced", never "shown" or "adopted".
    // Trace "patch" remains the legacy catalog label. "rules_patch" and
    // "rules_revision" describe the loaded numerical rules at capture;
    // each bound result's rule_basis describes its own worker snapshot.
    // "actual_offer_ids" uses the order received by the model. A three-card
    // "offer_order_key" identifies that content within the trace context.
    // displayEvidence.card_surface optionally records a widget observation
    // with a positive offer_revision and an optional provider round, opaque
    // offer_source_ref, and offer_observed_at_ms. offer_slots and display_slots
    // have exactly three left-to-right entries. Use null for unknown offer IDs
    // and hidden widgets. A visible widget has an augment_id and a mode
    // (mechanism, conditional, reference, comparison, or unknown). Include
    // disposition (shown, hidden, or waiting) and a coded hide_reason.
    // The caller must supply only a verified source reference. Card widget
    // evidence does not assert independent screen or selection verification.
    void record(const Catalog &, const GameState &,
        const QJsonObject &coachResult = {}, const QJsonObject &cardDecision = {},
        const QJsonObject &displayEvidence = {}, const RecommendationTraceBinding &binding = {});
    QJsonObject report() const;
    // One bounded diagnostic per completed worker, including rejected jobs.
    // Only timings, reason codes and a salted context key are persisted.
    void recordAttempt(const QString &contextId,const QJsonObject &attempt);

    // Not for a render/timer hot path. Bounded waiting is useful for tests or
    // orderly shutdown; timeout never kills a worker or another process.
    bool flush(int timeoutMs = 1000);
    void close(int timeoutMs = -1);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
