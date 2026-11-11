#pragma once
#include "catalog.h"

struct GameState;

// Read-only sourced descriptions and the exact limits of executable coverage.
// Prose never becomes a numerical rule or a claim of a verified optimal combo.
QJsonObject heroExecutionOverview(const Catalog &, int championId);

// Compare <= 12 explicit sequences through the existing continuous ledger.
// execution_candidates[].action_events is merged with request.action_events.
// Missing resources remain unknown. No partial search produces a winner.
QJsonObject evaluateHeroExecutionScenario(const Catalog &, const QJsonObject &request,
                                        int actorChampionId, int budgetMs = 150);

// Build a disclosed stationary, single-target reference scenario from the
// current anonymous ten-player loadouts; never sends gameplay input.
QJsonObject buildHeroExecutionPlan(const Catalog &, const GameState &,
                                 const QString &preference, int budgetMs = 150);

// Separate Replay-only counterfactual: learned spells ready and a source-known
// full resource pool. Never overwrites GameState or a current-state winner.
// The caller shares its job budget by passing the time remaining after Plan.
QJsonObject buildHeroExecutionReference(const Catalog &, const GameState &,
                                      const QString &preference, int budgetMs = 150);
