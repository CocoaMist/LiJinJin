#pragma once
#include "panel_state.h"
#include <QRect>

struct SelectionCardLayout {
    QList<QRect> cards, hints;
    QList<QRect> referenceCardContentAreas, referenceHintInteriors;
    QList<QRect> additionalControlAreas;
    QRect referenceFrame, referenceCardActions, referenceFooterControls, referenceGameHud;
    QString mode = QStringLiteral("hidden"), fallbackReason;
    int gap = 0, desiredHintHeight = 0;
    bool valid = false;
};
// A reference layout, not a measurement of the user's game UI. Coordinates
// are logical pixels in the game client; letterboxed around a 16:9 UI frame.
// Optional caller-supplied control rectangles reserve additional game UI.
// Labels cover only the lower interior of their own reference card, below its
// art/title/description area and clear of its border and reroll/footer controls.
// Constrained layouts shift/compact inside that band, then hide all 3 rather
// than move to another anchor or change the left/middle/right mapping.
SelectionCardLayout selectionCardLayout(const QRect &viewport,const QList<QRect> &additionalControlAreas = {});
bool selectionCardLayoutSafe(const QRect &viewport, const SelectionCardLayout &);
QJsonObject selectionCardLayoutJson(const SelectionCardLayout &);

// Explicit slots only. Metrics use the current local gain; AI is ignored.
// The AI/selected parameters remain for source compatibility with callers.
QJsonArray orderedCardHints(const PanelState &, const QJsonObject &decision,
                           const QJsonObject &validatedAiPlan, const QList<int> &selected,
                           bool currentModel);
bool selectionHintsVisible(const PanelState &, bool freshKiwi, bool gameForeground,
                           bool demo, bool hiddenByUser, QDateTime now = QDateTime::currentDateTimeUtc());
