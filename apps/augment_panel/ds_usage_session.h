#pragma once
#include <QString>

// Completeness of the observed session is independent of whether its calls
// have known prices. Costs still belong to every request's original context.
class DsUsageSessionTracker final {
public:
    QString observe(const QString &contextId, bool active,
                    const QString &stage, const QString &phase) {
        if (!active || contextId.isEmpty()) {
            const bool inactive = !active && contextId.isEmpty();
            const auto completed = inactive && eligible && progress == 3 && terminal(phase)
                ? observedContext : QString();
            observedContext.clear();
            eligible = false;
            progress = 0;
            // An empty/unknown/disconnected phase cannot arm a new full game.
            awaitingSelection = inactive && safeBetweenGames(phase);
            return completed;
        }

        displayedContext = contextId;
        const int next = stageProgress(stage, phase);
        if (contextId != observedContext) {
            // A UUID change in an active game is a discontinuity, even when
            // the new snapshot itself says champion selection.
            eligible = observedContext.isEmpty() && awaitingSelection && next == 1;
            observedContext = contextId;
            progress = next;
        } else {
            if (!next || next < progress) eligible = false;
            if (next > progress) progress = next;
        }
        awaitingSelection = false;
        return {};
    }

    QString displayContext() const { return displayedContext; }

private:
    QString observedContext, displayedContext;
    bool awaitingSelection = false, eligible = false;
    int progress = 0;

    static bool safeBetweenGames(const QString &phase) {
        return phase == QStringLiteral("None") || phase == QStringLiteral("Lobby")
            || phase == QStringLiteral("Matchmaking") || phase == QStringLiteral("ReadyCheck");
    }
    static bool terminal(const QString &phase) {
        return phase == QStringLiteral("EndOfGame") || phase == QStringLiteral("PreEndOfGame")
            || phase == QStringLiteral("WaitingForStats");
    }
    static int stageProgress(const QString &stage, const QString &phase) {
        if (stage == QStringLiteral("champ_select") && phase == QStringLiteral("ChampSelect")) return 1;
        if (stage == QStringLiteral("loading") &&
            (phase == QStringLiteral("GameStart") || phase == QStringLiteral("LoadingScreen")
             || phase == QStringLiteral("Loading") || phase == QStringLiteral("PreStart")
             || phase == QStringLiteral("Start"))) return 2;
        // Reconnect is deliberately not a proof of uninterrupted observation.
        if (stage == QStringLiteral("in_game") && phase == QStringLiteral("InProgress")) return 3;
        return 0;
    }
};
