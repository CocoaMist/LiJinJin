#pragma once
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QtGlobal>

// Resolves a passive, continuous HUD from the current match state. Time is
// supplied only to detect a reset; content never rotates or needs a page pin.
// This class owns no UI, timer, identities, services or game computation.
class CoachPresentation {
public:
    QJsonObject update(const QJsonObject &input, qint64 monotonicMs);
    QJsonObject report() const;
    void resumeAutomatic(qint64 monotonicMs); // Source compatibility; always automatic.
private:
    void reset();
    QString context, stage;
    QString lastReason = QStringLiteral("waiting_snapshot"), lastShownPage = QStringLiteral("waiting");
    QString lastLayout = QStringLiteral("waiting");
    QStringList seenCueOrder;
    QSet<QString> seenCues;
    qint64 lastMs = -1, updates = 0, resets = 0, urgencyEvents = 0;
    int lastItem = 0, lastDead = -1;
    bool lastAffordable = false, lastExpanded = false;
    bool lastPurchaseVisible = false, lastRosterVisible = false;
};
