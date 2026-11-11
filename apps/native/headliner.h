#pragma once
#include "headliner_context.h"
#include <QList>
class Catalog;
struct GameState;
QJsonObject observedHeadliner(const GameState &);
QJsonObject evaluateHeadliner(const Catalog &,const GameState &);
QList<int> effectiveSelectedAugments(const Catalog &,const GameState &);
QStringList headlinerComparisonUnknowns(const GameState &,int candidateId);
