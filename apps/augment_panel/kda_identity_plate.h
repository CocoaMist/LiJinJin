#pragma once
#include "kda_theme.h"
#include <QPainter>
#include <QPainterPath>
#include <algorithm>

namespace Kda {
inline QColor identityInk(){return QColor(15,13,23);}
inline void paintIdentityPlate(QPainter &p,const QRectF &area){
    QPainterPath plate;plate.moveTo(area.topLeft());plate.lineTo(area.topRight());
    plate.lineTo(area.bottomRight()-QPointF(std::min(42.,area.width()*.12),0));plate.lineTo(area.bottomLeft());plate.closeSubpath();
    p.fillPath(plate,skin()==QStringLiteral("kda_red")?QColor(226,174,190):QColor(199,191,232));
}
}
