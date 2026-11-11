#pragma once
#include <QCache>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

namespace DashboardFrame {
struct Corners { QImage topLeft, topRight, bottomLeft, bottomRight; };

// Supersample only the tiny corner tiles. A maximized desktop never needs a
// full-window high-resolution temporary image, even while it is being resized.
inline void paint(QPainter &p, const QSize &size, qreal dpr, qreal radius,
                  const QColor &fill, const QColor &border) {
    const qreal stroke = 1. / dpr;
    p.save();p.setRenderHint(QPainter::Antialiasing);
    if (radius <= 0) {
        p.fillRect(QRectF(QPointF(), size), fill);
        p.setPen(QPen(border, stroke));p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(QPointF(), size).adjusted(stroke/2,stroke/2,-stroke/2,-stroke/2));
        p.restore();return;
    }
    static QCache<QString, Corners> cache(16);
    const auto key=QStringLiteral("%1:%2:%3:%4").arg(dpr,0,'f',4).arg(radius,0,'f',4).arg(fill.rgba()).arg(border.rgba());
    auto *corners=cache.object(key);
    if (!corners) {
        constexpr int samples=4;
        const int tile=qCeil(radius*dpr)+2;
        QImage coverage(QSize(tile*samples,tile*samples),QImage::Format_ARGB32_Premultiplied);coverage.fill(Qt::transparent);
        {QPainter high(&coverage);high.setRenderHint(QPainter::Antialiasing);high.scale(samples,samples);
            QPainterPath shape;shape.addRoundedRect(QRectF(.5,.5,tile*2-1,tile*2-1),radius*dpr,radius*dpr);
            high.fillPath(shape,fill);high.setPen(QPen(border,1));high.setBrush(Qt::NoBrush);high.drawPath(shape);}
        const auto resolved=coverage.scaled(tile,tile,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
        corners=new Corners{resolved,resolved.mirrored(true,false),resolved.mirrored(false,true),resolved.mirrored(true,true)};
        for(auto *image:{&corners->topLeft,&corners->topRight,&corners->bottomLeft,&corners->bottomRight})image->setDevicePixelRatio(dpr);
        cache.insert(key,corners);
    }
    const qreal tile=corners->topLeft.width()/dpr,w=size.width(),h=size.height();
    QPainterPath interior;
    interior.addRect(QRectF(tile,stroke/2,w-2*tile,h-stroke));
    interior.addRect(QRectF(stroke/2,tile,tile-stroke/2,h-2*tile));
    interior.addRect(QRectF(w-tile,tile,tile-stroke/2,h-2*tile));
    p.fillPath(interior,fill);
    p.setPen(QPen(border,stroke,Qt::SolidLine,Qt::FlatCap));
    p.drawLine(QPointF(tile,stroke/2),QPointF(w-tile,stroke/2));
    p.drawLine(QPointF(tile,h-stroke/2),QPointF(w-tile,h-stroke/2));
    p.drawLine(QPointF(stroke/2,tile),QPointF(stroke/2,h-tile));
    p.drawLine(QPointF(w-stroke/2,tile),QPointF(w-stroke/2,h-tile));
    p.drawImage(QPointF(0,0),corners->topLeft);p.drawImage(QPointF(w-tile,0),corners->topRight);
    p.drawImage(QPointF(0,h-tile),corners->bottomLeft);p.drawImage(QPointF(w-tile,h-tile),corners->bottomRight);
    p.restore();
}
}
