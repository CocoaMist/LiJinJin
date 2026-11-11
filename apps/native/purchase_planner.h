#pragma once
#include "recommendation_work.h"
#include <QJsonObject>
#include <QList>
#include <QString>
class Catalog;
class CombatDatabase;
struct GameState;
// Price/recipe facts for observed components, independent of combat ranking.
// Unknown augments never become an empty selected list or a recommended buy.
QJsonObject observedPurchaseFacts(const Catalog &,const GameState &);
// Separate from combat-model currency. A validated unchanged-item bridge
// permits factual recipe/reference continuity without enabling combat scores.
bool purchaseReferenceReady(const Catalog &,const GameState &);
bool purchaseReferenceItemCompatible(const Catalog &,const GameState &,int itemId);
// Offline, caller-declared facts bound to one actor, item and exact game clock.
// This is a conditional model input, never an authenticated live shop capture.
// Live adapters must not populate it from effectConditions or arbitrary JSON.
struct PurchaseAvailabilityScenario {
    QString actorKey, contextId, patch, combatRevision;
    int championId=0;
    double gameTime=-1;
    QJsonObject observation;
};
// Quote one new physical item. The returned consumption multiset and final
// inventory are shared by the shop planner, continuous executor and offline
// drivers. No gold is changed, and component eligibility is distinct from
// finished-build recommendation eligibility.
QJsonObject planInventoryPurchase(const CombatDatabase &,int championId,int level,
                                 const QList<int> &owned,int targetItemId);
QJsonObject planInventoryPurchase(const Catalog &,int championId,int level,
                                 const QList<int> &owned,int targetItemId);
// Catalog-only reference quotes accept physical copies of unfinished
// components. This never verifies that their combat effects can stack, and
// must not be fed into a combat executor or used to unlock a ranked purchase.
QJsonObject planInventoryRecipeReference(const Catalog &,int championId,int level,
                                       const QList<int> &owned,int targetItemId);
QJsonObject planInventoryPurchase(const CombatDatabase &,int championId,int level,
                                 const QList<int> &owned,int targetItemId,
                                 const PurchaseAvailabilityScenario &);
QJsonObject planInventoryPurchase(const Catalog &,int championId,int level,
                                 const QList<int> &owned,int targetItemId,
                                 const PurchaseAvailabilityScenario &);
// Validate materials of an already observed transaction. The result exposes
// recipe_valid, never legal: it grants no authority to plan a future purchase.
QJsonObject validateObservedPurchaseRecipe(const CombatDatabase &,int championId,int level,
                                         const QList<int> &materials,int targetItemId);
// Static candidates include finished equipment and plain components supplying
// a stat referenced by a selected augment's pinned formulas. Every candidate
// still needs the normal shop, recipe, slot and combat comparison checks.
QList<int> nextPurchaseCandidateIds(const Catalog &,const GameState &);
// Changes only when a legal candidate purchase crosses a budget boundary.
QString purchaseBudgetBasis(const Catalog &, const GameState &);

// Evaluates legal next purchases from the actual inventory and budget. Future
// trigger/stack scenarios are alternatives, never inserted as observed facts.
QJsonObject buildPurchaseSandbox(const Catalog &, const GameState &, const QString &style,
                                 const RecommendationWorkPtr &work = {});
QJsonObject reconnectPurchasePlan(const Catalog &, const GameState &, QJsonObject plan,
                                  const QJsonObject &sandbox);
