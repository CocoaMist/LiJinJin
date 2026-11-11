#pragma once
#include <QRect>
#include <QList>
#include <QJsonObject>

namespace OverlayMetrics {
inline constexpr int titlebarHeight=35;
inline constexpr int foldedHeight=titlebarHeight+4;
inline constexpr int minimumWidth=150;
inline constexpr int narrowMinimumWidth=142;
}

struct OverlayPlacement {QRect right;QList<QRect> protectedAreas;bool collapsed=false;};
OverlayPlacement safeOverlayPlacement(const QRect &viewport);
// Prefer the free desktop beside the verified client. On small screens keep
// the center, bench strip and bottom-center confirmation controls uncovered.
OverlayPlacement championSelectPlacement(const QRect &client,const QRect &available);
bool overlayAvoidsProtectedAreas(const OverlayPlacement &);
QJsonObject overlayPlacementJson(const OverlayPlacement &);
// Conservative reference for the centered default shop, not shop-open detection.
QRect shopReferenceArea(const QRect &viewport);
QRect compactDeathTipPlacement(const QRect &viewport);
QRect combatCornerPlacement(const OverlayPlacement &,const QRect &coach,int height);
bool visibleOverlaysAvoidAreas(const QList<QRect> &windows,const QList<QRect> &areas);
