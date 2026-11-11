#pragma once
#include "analysis.h"

// Equipment decisions are tied to structural observations. Panel fluctuations
// request another calculation without revoking a matching structural result.
QString coachStructuralBasis(const GameState &state);
QString coachHardModelBasis(const Catalog &,const GameState &,const QString &style,double allyExecution);
QString coachSoftModelBasis(const GameState &state);

// Card decisions retain event evidence and observed player context. Timing is
// captured once per job from the committed ledger, rather than polling the
// ledger or putting every game-clock tick into the structural cache key.
QString coachCardModelBasis(const Catalog &,const GameState &,const QString &style);
QJsonObject coachCardMechanicValidity(const Catalog &,const GameState &);
bool coachCardClockCurrent(const QJsonObject &validity,double currentGameTime,qint64 currentUtcMs=-1);

struct CoachRefreshPolicy {
    static constexpr qint64 minimumSoftDispatchMs = 1200;
    static constexpr qint64 periodicRefreshMs = 5000;
    static constexpr qint64 maximumPublishAgeMs = 6000;
    static constexpr qint64 maximumRetainedAgeMs = 8000;
    static QJsonObject publication(const QString &workerHard,const QString &currentHard,
        const QString &workerSoft,const QString &currentSoft,qint64 snapshotMs,qint64 nowMs,
        bool fresh,bool patchMatches,double snapshotGameTime,double currentGameTime,double mechanicValidUntil);
    static bool refreshDue(bool softChanged,qint64 publishedSnapshotMs,qint64 lastDispatchMs,qint64 nowMs);
    static bool retain(qint64 publishedSnapshotMs,qint64 nowMs,bool hardMatches,bool fresh,bool patchMatches,
        double snapshotGameTime,double currentGameTime,double mechanicValidUntil);
};
