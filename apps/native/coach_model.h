#pragma once
#include "analysis.h"
#include <QtGlobal>

// Scenario parameters, not fitted skill ratings or calibrated match probabilities.
struct CoachSimulationOptions {
    int samples=4096;
    quint32 seed=44038;
    double allyExecution=1,enemyExecution=1,executionSpread=.18;
    double horizon=8;
    // The live purchase worker shares one budget across every pipeline stage.
    // Offline callers keep the full-build/offer comparison path by default.
    bool currentPurchaseOnly=false;
    // The live HUD publishes completed purchase work before optional action
    // explanations. Offline callers retain the synchronous explanation path.
    bool deferExecutionReference=false;
    RecommendationWorkPtr work;
};
QJsonObject simulateCoachFight(const Catalog &,const GameState &,const CoachSimulationOptions & = {});
QJsonObject buildCoachSnapshot(const Catalog &,const GameState &,const QString &style,
                               const CoachSimulationOptions & = {},const QSet<int> &seen={});
// Update inexpensive visible economy/KDA without rerunning equipment search.
void refreshCoachSnapshot(const Catalog &,const GameState &,QJsonObject &);
// Uniform, without-replacement conditional example. Unknown live pool/weights => not a live probability.
QJsonObject conditionalAugmentChance(int poolSize,int desiredCount,int drawSlots,int rerolls=0);
QJsonArray championScenarioVariants(const CombatDatabase &,int championId);
GameState makeCoachDemo(const Catalog &);
// Only the optional action explanation fields; never computes or changes a
// purchase winner. The two calls share a cooperative, non-preemptive budget.
QJsonObject buildCoachExecutionExplanation(const Catalog &,const GameState &,
    const QString &style,int budgetMs=350,const RecommendationWorkPtr &work={});
QJsonObject pendingCoachExecutionExplanation(const GameState &);
