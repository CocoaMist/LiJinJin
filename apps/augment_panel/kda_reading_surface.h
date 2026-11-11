#pragma once
#include "kda_glass_surface.h"
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>

namespace Kda {
inline void paintReadingSurface(QPainter &p, const QRectF &bounds, QWidget *owner=nullptr) {
    const QRectF plate = bounds.adjusted(.5, .5, -.5, -.5);
    const qreal rounding = radius("section");
    QPainterPath shape;
    shape.addRoundedRect(plate, rounding, rounding);
    if (owner) paintGlassButtonBackground(p, owner, shape, plate);

    auto upper = color("surface"), lower = color("bg");
    upper.setAlpha(glassEnabled() ? 222 : 255);
    lower.setAlpha(glassEnabled() ? 236 : 255);
    QLinearGradient surface(plate.topLeft(), plate.bottomLeft());
    surface.setColorAt(0, upper);
    surface.setColorAt(1, lower);
    p.fillPath(shape, surface);
    paintMetallicFinish(p,shape,1.,owner);

    paintPolishedEdge(p,shape,plate,rounding);
}

class ReadingSurface : public QWidget {
public:
    ReadingSurface() { markSharedGlassSurface(this); }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        configurePainter(painter);
        paintReadingSurface(painter, QRectF(rect()), this);
    }
};
}
