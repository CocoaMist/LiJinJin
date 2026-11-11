#pragma once
#include "catalog.h"
struct GameState;

// Public rule evidence can inform an explanation, never execute code or replace
// pinned catalog values. These helpers are also exercised without a paid API.
QJsonObject coachRuleContract(const Catalog &);
// Bounded public source/conditional references shared by roster and card/item AI.
QJsonObject boundedHeroKnowledgeReference(const Catalog &, int championId);
// Plain-language conditions from the pinned card text, independent of a gain
// estimate. It never turns a missing trigger/stack into an observed quantity.
QJsonObject augmentMechanismReference(const Catalog &,const GameState &,int augmentId);
QJsonObject sanitizedMatchAiContext(const QJsonObject &);
bool validateMatchAiAnalysis(QJsonObject &, const QJsonObject &context, QString *error);
QString publicGameRuleQuery(const QString &patch);
bool validGameRuleQuery(const QString &);
QJsonObject makeMatchAiContext(const Catalog &, const GameState &, const QJsonObject &history = {});
