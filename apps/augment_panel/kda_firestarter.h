#pragma once
#include <QLinearGradient>
#include <QPainter>
#include <QPixmap>
#include <algorithm>
#include <cmath>

namespace Kda {
// Fine flame filaments and chrome reflections stay below the reading plane.
inline void paintFirestarterFlames(QPainter &p,const QRectF &bounds,qreal strength=1.,qreal drift=0.){
    if(bounds.isEmpty()||!std::isfinite(strength)||strength<=0)return;
    static const QPixmap atmosphere(QStringLiteral(":/lixin/kda/textures/firestarter-atmosphere-v1.png"));
    p.save();p.setClipRect(bounds,Qt::IntersectClip);
    p.setOpacity(p.opacity()*std::clamp(strength,0.,1.));
    p.setRenderHint(QPainter::SmoothPixmapTransform,true);
    if(!atmosphere.isNull()){
        const QRectF source(atmosphere.width()*.68,atmosphere.height()*.3,atmosphere.width()*.32,atmosphere.height()*.7);
        const qreal offset=std::isfinite(drift)?std::sin(drift)*.5:0.;
        p.drawPixmap(bounds.adjusted(-1,-1,1,1).translated(offset,0),atmosphere,source);
    }else{
        QLinearGradient heat(bounds.bottomLeft(),bounds.topRight());
        heat.setColorAt(0,QColor(99,16,33,0));heat.setColorAt(.7,QColor(176,30,48,40));
        heat.setColorAt(.94,QColor(255,177,124,75));heat.setColorAt(1,QColor(151,23,43,25));p.fillRect(bounds,heat);
    }
    p.restore();
}
}
