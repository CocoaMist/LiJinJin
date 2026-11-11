#pragma once
#include "recommendation_work.h"
#include <QJsonObject>
class Catalog;
struct GameState;
// An unkeyed content binding, not authentication of observations or outcomes.
QJsonObject explicitChainPurchaseBasis(const Catalog &, const GameState &);
QString explicitChainPurchaseCacheBasis(const GameState &);
QJsonObject buildExplicitChainPurchaseSandbox(const Catalog &, const GameState &, const RecommendationWorkPtr & = {});
// Revoke selection at every nesting level while retaining numerical diagnostics.
QJsonObject withdrawExplicitChainPurchaseSelection(const QJsonObject &);
