#pragma once
#include <QJsonObject>
#include <QString>

// Bounded public score contract. These functions do no I/O or calculation.
QJsonObject sanitizeRosterScoreContext(const QJsonObject &, const QJsonValue &cutoff);
QJsonObject sanitizeRosterRecentScore(const QJsonObject &, const QJsonObject &scoreContext);
QJsonObject buildRosterRecentResultsView(const QJsonObject &snapshot);
QString rosterRecentScoreModel();
