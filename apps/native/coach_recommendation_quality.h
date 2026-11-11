#pragma once
#include <QJsonObject>

class Catalog;
struct GameState;

// Annotates existing calculations. Does not search, reorder, or authorize a
// purchase, and never infers missing augments from visible stat residuals.
// Callers may supply coach.requested_combat_style from the actual UI choice;
// resolved_preference alone cannot establish an explicit output-mode request.
void annotateCoachRecommendationQuality(const Catalog &, const GameState &, QJsonObject &coach);

// Anonymous coverage counts from the validated roster adapter. A completed
// lookup is distinct from ten usable, fresh per-player analyses.
QJsonObject recommendationHistoryCoverage(const GameState &);

// Cheap prerequisite check. Unknown selection never starts a doomed live
// purchase search. Explicit offline/chain scenarios keep their own contracts.
QJsonObject coachPurchaseInputReadiness(const Catalog &, const GameState &);
