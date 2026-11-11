#pragma once

#include "recommendation_work.h"
#include <QJsonObject>

class Catalog;
struct GameState;

// Sidecar for an already computed coach result or current offer decision.
// The snapshot belongs to the same native work item. Nothing here ranks,
// completes, or changes a native candidate or grants execution authority.
QJsonObject buildLocalChoiceEvidence(const Catalog &, const GameState &,
    const QJsonObject &nativeResult, bool forAugments, const RecommendationWorkPtr &work = {});

// Checks the sidecar's freshness and actor/loadout/offer/budget binding at use.
bool localChoiceEvidenceCurrent(const Catalog &, const GameState &, const QJsonObject &,
    bool forAugments, qint64 nowMs = -1);
