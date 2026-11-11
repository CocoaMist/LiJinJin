#pragma once
#include "analysis.h"

// Bounded anonymous historical references for the current roster. This does
// not predict current DPS, infer hidden mechanics or produce a win probability.
// During loading, a complete visible roster may map history without inventing
// live loadouts. An unobserved patch remains an explicit non-live assumption.
QJsonObject buildRecommendationHistory(const Catalog &, const GameState &,
                                      const QJsonObject &anonymousHistory);

// Public projection extensions shared with the subprocess bridge. Unknown keys,
// identities and unsupported numeric values are discarded at both boundaries.
QJsonObject sanitizeHistoricalProjectionExtension(const QJsonObject &);
