#pragma once
#include "analysis.h"
#include "combat.h"

// One disclosed comparison intent and one opponent estimate for equipment and
// card comparisons. Support intent does not assert modeled ally utility.
QString resolvedCombatPreference(const Catalog &, const GameState &, const QString &style);
QJsonObject applyOpponentLoadoutContext(const Catalog &, const GameState &, CombatContext &);

// Only explicit current row facts override the disclosed reference scenario.
// A malformed or contradictory provided group is unavailable, never silently
// replaced by a known-looking default. Historical rows are not inputs here.
struct PlayerCombatContext {
    bool available = false;
    CombatContext context;
    QJsonObject report;
};
PlayerCombatContext playerCombatContext(const CombatDatabase &, const QJsonObject &player);
// Self's live ability observations use the same rank legality and point-budget
// rules as roster rows. Missing slots retain a disclosed reference; malformed
// supplied slots never become rank zero or a reference value.
PlayerCombatContext selfSkillRankContext(const CombatDatabase &, const GameState &);

// Canonical anonymous loadouts of both teams. Slot/row rearrangement and KDA
// ticks are immaterial; missing fields and duplicate components are preserved.
QJsonArray liveLoadoutBasis(const GameState &);

struct TeammateCombatContext {
    bool available = false;
    QVector<CombatEvaluation> teammates;
    double offense = 0, survival = 0;
    double survivalWeight = -1;
    QJsonObject report;
};
// Four other allies, evaluated against the same explicit target/window as self.
// No hidden augments, health, casts, stacks or historical triggers are invented.
TeammateCombatContext evaluateTeammateContext(const Catalog &, const GameState &,
    const CombatContext &target, const QString &preference, bool useCache = true);
QJsonObject compareTeamObjective(const TeammateCombatContext &,
    const CombatEvaluation &before, const CombatEvaluation &after,
    const QString &preference, double beforeHealth = -1,
    double afterHealth = -1, double otherHealth = -1);

// A separate historical allocation sensitivity: only self's share of modeled
// team output is redistributed. It never changes individual damage or HP.
QJsonObject compareHistoricalTeamOutputObjective(const TeammateCombatContext &,
    const CombatEvaluation &before, const CombatEvaluation &after,
    const QString &preference, double selfOutputShare, double beforeHealth = -1,
    double afterHealth = -1, double otherHealth = -1);
// Complete 3/8/20s scenarios are averaged separately before selecting the
// lowest supported historical mean. Missing scenarios are never zero gains.
QJsonObject summarizeRosterHistoryScenarios(const QJsonObject &detailedSandbox);

// One strict 3/8/20s contract for card and purchase sensitivity. An advertised
// historical scenario with missing cells invalidates the robust aggregate;
// unavailable history is omitted, never replaced by a zero. Values are model
// score changes, with no probability or live-performance interpretation.
QJsonObject summarizeCombatScenarios(const QJsonObject &detailedSandbox,
                                    bool requireFiveTargets = true);
// At most twelve already-evaluated candidates. All must be eligible and have
// identical scenario/window axes. Dominance and minimax regret are diagnostic
// only; this function never changes the native recommendation objective.
QJsonObject compareCombatScenarioCandidates(const QJsonArray &candidates);
