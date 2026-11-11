#pragma once
#include "analysis.h"
#include "recommendation_work.h"
#include <QVector>

// Equal-weight examples over an explicitly supplied finite candidate pool.
// These values are never asserted to be the live game's roll distribution.
QJsonObject conditionalRerollExpectation(QVector<double> pool,double current);
// Compare the best of the original three with the best of the two retained
// slots on the same score basis.
QJsonObject singleSlotRerollTradeoff(const QVector<double> &scores,int slot);
// Classifies the observed match rule, not the upgrade state of any one slot.
QString observedRerollRule(const GameState &);
QJsonObject coachTeamNeeds(const Catalog &,const GameState &);
// Every offered ID retains its numeric fields and receives evaluation_status,
// evaluation_reason, evaluation_reason_code and a conditional evaluation_scope.
// A computed/zero status describes known numeric effects in that window only;
// evaluation_complete never certifies a complete live combat model.
// Independent ranking_quality gates conditional ordering on verified self input;
// support/tank tags only disclose objective gaps and never prohibit comparison.
// current_gain_percent and evaluation_* retain their original 8-second scope.
// decision_gain_percent is published only for complete five-enemy 3/8/20-second
// team comparisons with every advertised historical scenario complete, taking
// the lowest complete scenario mean. Unavailable history never becomes zero.
// scenario_comparison discloses dominance/duration tradeoffs without changing
// the recommendation objective or asserting any scenario's live probability.
// decision_trace is bounded; recommendation_evidence contains anonymous history.
// Numeric availability alone does not authorize ranking: decision_comparable /
// decision_marginal_comparable also require no attributable candidate input gap
// in the reference or any target/window. Explicit missing mechanic state or
// invalid numeric parameters block comparison even when shared with baseline;
// generic coverage disclosures alone do not. Blocking codes remain visible.
// A caller may bind one cooperative budget/cancellation token to this round.
// Interrupted rounds retain references only: no completed candidate prefix may
// be ranked. Preparation reuse is local to this call and its exact snapshot.
QJsonObject buildAugmentDecision(const Catalog &,const GameState &,const QJsonObject &buildPlan,
                                const QString &style,const QSet<int> &seen={},bool currentOffersOnly=false,
                                const RecommendationWorkPtr &work={});
