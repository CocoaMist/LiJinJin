#pragma once
#include "analysis.h"
#include "recommendation_work.h"
#include <QJsonObject>
#include <memory>
class CombatDatabase;
struct CombatSandboxOptions;

// One read-only recommendation job owns this bounded cache. Do not share it
// between threads or mutate the catalog while that job is evaluating. Exact
// snapshot/context bytes are compared; GameState::fingerprint is not a key.
// Only observed-snapshot preparation and baselines are reusable. Proposed
// equipment/cards are always evaluated separately and cannot become another
// candidate's baseline. Incomplete/cancelled preparation is never published.
class CombatSandboxReuse {
public:
    CombatSandboxReuse();
    // The two inputs must stay alive and immutable for this synchronous batch.
    // Other inputs still use exact snapshot matching.
    CombatSandboxReuse(const Catalog &, const GameState &);
    ~CombatSandboxReuse();
    CombatSandboxReuse(const CombatSandboxReuse &) = delete;
    CombatSandboxReuse &operator=(const CombatSandboxReuse &) = delete;
    void clear();
    QJsonObject stats() const;
private:
    struct Data;
    std::unique_ptr<Data> data;
    friend QJsonObject evaluateCombatSandbox(const Catalog &, const GameState &, const QList<int> &, const CombatSandboxOptions &);
};

// Explicit counterfactual inputs. They are never promoted to observations or
// written back to GameState. A missing counter stays missing rather than zero.
struct CombatSandboxOptions {
    QString style;
    bool teamObjective = false;
    // One explicit next-card counterfactual. This is the complete candidate
    // selection (including every effective baseline augment), never a new
    // observation. Inventory must remain unchanged in this comparison mode.
    bool compareCandidateAugments = false;
    QList<int> candidateAugments;
    QString scenarioLabel;
    QMap<QString, double> scenarioCounters;
    QSet<QString> scenarioConditions;
    std::shared_ptr<CombatSandboxReuse> reuse;
    RecommendationWorkPtr work;
    // Decision callers may skip the presentation-only level projection.
    bool includeGrowthProjection = true;
};

// Explicit Q/W/E-priority reference; never evidence of a player's real ranks.
QMap<QString, int> referenceCombatRanks(const CombatDatabase &, int champion, int level);

// Bounded deterministic model comparison, not hit prediction or win probability.
// Uses at most five visible opponents and the fixed 3/8/20-second windows.
QJsonObject evaluateCombatSandbox(const Catalog &, const GameState &,
    const QList<int> &candidateInventory, const CombatSandboxOptions &options = {});
