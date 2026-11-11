#pragma once
#include "analysis.h"

// Only public, anonymous observations. No inferred casts, cooldowns or movement.
class TacticalTimeline {
public:
    QJsonObject observe(const GameState &);
    void clear();
private:
    QString context, roster;
    double lastTime = -1, continuousSince = -1;
    QMap<QString, QJsonObject> previous;
    QJsonArray events;
    QJsonObject last;
};

// Multi-label responsibilities estimated from current loadouts, plus explicitly
// reviewed ability capabilities. Death affects immediate availability only;
// durable purchase objectives always compare the next full-team fight.
QJsonObject analyzeTacticalContext(const Catalog &, const GameState &);
QJsonObject tacticalPlayer(const QJsonObject &context, int rowIndex);
double tacticalSurvivalWeight(const QJsonObject &player, double baseWeight);
