#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include "recommendation_work.h"

class Catalog;
struct GameState;

// Cheap, observation-only projection and the exact model inputs used by the
// async HUD adapter. Neither entry point evaluates combat or tactical models.
QJsonObject observedLiveCombatSummary(const Catalog &, const GameState &);
QByteArray liveCombatSummaryBasis(const Catalog &, const GameState &);

// One instance belongs to one native snapshot stream. It never queries a
// service, retains player identities, or treats modeled output as a statistic.
class LiveCombatSummary {
public:
    QJsonObject update(const Catalog &, const GameState &, const RecommendationWorkPtr &work = {});
    QJsonObject report() const;

private:
    void clearCache();
    QByteArray modelBasis;
    QJsonArray modelRows;
    const void *databaseIdentity = nullptr;
    QString activeContext, lastStatus = QStringLiteral("waiting_snapshot");
    double lastGameTime = -1, lastComputeMs = 0, lastUpdateMs = 0, maxComputeMs = 0;
    qint64 updates = 0, hits = 0, misses = 0, evaluations = 0, clears = 0, generation = 0;
    int modeledPlayers = 0;
    bool lastFresh = false;
};
