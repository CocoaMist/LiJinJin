#pragma once
#include "kda_theme.h"
#include "kda_hud_style.h"
#include "kda_firestarter.h"
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QWidget>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace Kda {
// Small overlays are painted at twice the monitor's physical resolution.
// Resolve coverage once, keeping grayscale text and translucent edges intact.
class OverlayCanvas final {
public:
    explicit OverlayCanvas(QWidget *widget):owner(widget),ratio(widget->devicePixelRatioF()),
        physicalSize(std::max(1,qRound(widget->width()*ratio)),std::max(1,qRound(widget->height()*ratio))),
        buffer(physicalSize*2,QImage::Format_ARGB32_Premultiplied){
        buffer.fill(Qt::transparent);buffer.setDevicePixelRatio(ratio*2);painter.begin(&buffer);configurePainter(painter);
    }
    ~OverlayCanvas(){
        // Derive both surfaces from the same physical size. Independently
        // rounding the source and target changes coverage at fractional DPR.
        painter.end();auto resolved=buffer.scaled(physicalSize,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
        resolved.setDevicePixelRatio(ratio);QPainter target(owner);
        if(owner->isWindow()&&owner->testAttribute(Qt::WA_TranslucentBackground))target.setCompositionMode(QPainter::CompositionMode_Source);
        target.drawImage(QPointF(0,0),resolved);
    }
    QPainter &paint(){return painter;}
private:
    QWidget *owner; qreal ratio; QSize physicalSize; QImage buffer; QPainter painter;
};

inline QFont hudFont(FontRole role,int pixels,QFont::Weight weight=QFont::DemiBold){
    auto result=font(role,pixels,weight);
    // Inherit the shared grayscale and vertical hinting policy. Retain this
    // explicit request for callers supplying their own application font.
    result.setStyleStrategy(QFont::StyleStrategy(QFont::PreferAntialias|QFont::NoSubpixelAntialias));
    return result;
}

// A small, static material accent. Text and hit targets remain native and stable.
inline void paintFoil(QPainter &p,const QRectF &r,qreal opacity=1.){
    if(r.isEmpty())return;
    static const QPixmap material(QStringLiteral(":/lixin/kda/textures/more-foil.png"));
    p.save();p.setClipRect(r,Qt::IntersectClip);p.setOpacity(p.opacity()*std::clamp(opacity,qreal(0),qreal(1)));
    if(skin()==QStringLiteral("kda_red")){
        QLinearGradient lacquer(r.topLeft(),r.bottomRight());
        lacquer.setColorAt(0,QColor("#35151f"));lacquer.setColorAt(.24,QColor("#d93832"));lacquer.setColorAt(.43,QColor("#ffe1b4"));
        lacquer.setColorAt(.47,QColor("#8d8c9f"));lacquer.setColorAt(.59,QColor("#47141f"));lacquer.setColorAt(.83,QColor("#e44b65"));lacquer.setColorAt(1,QColor("#241016"));
        p.fillRect(r,lacquer);p.restore();return;
    }
    QLinearGradient fallback(r.topLeft(),r.bottomRight());
    fallback.setColorAt(0,QColor("#eff5fc"));fallback.setColorAt(.3,QColor("#b6e4ee"));fallback.setColorAt(.56,QColor("#f0ecfa"));fallback.setColorAt(.8,QColor("#a9b6d3"));fallback.setColorAt(1,QColor("#dbc8eb"));p.fillRect(r,fallback);
    if(!material.isNull()){
        const qreal sourceHeight=std::min(qreal(material.height()),material.width()*r.height()/r.width());
        p.drawPixmap(r,material,QRectF(0,(material.height()-sourceHeight)*.46,material.width(),sourceHeight));
    }
    p.restore();
}

// Opaque charcoal stone with static, low-contrast cut faces. The caller keeps
// its own outer shape clip; strength affects the finish, not base coverage.
// This material paints no outline and does not need a texture or animation.
inline void paintFacetedMarble(QPainter &p,const QRectF &r,qreal strength=1.0){
    if(r.isEmpty())return;
    p.save();p.setClipRect(r,Qt::IntersectClip);p.setRenderHint(QPainter::Antialiasing,true);
    p.fillRect(r,color("surface"));
    p.setOpacity(p.opacity()*std::clamp(strength,qreal(0),qreal(1)));
    const auto point=[&](qreal x,qreal y){return QPointF(r.left()+r.width()*x,r.top()+r.height()*y);};
    const auto face=[&](const QPolygonF &vertices,const QLinearGradient &reflection){
        QPainterPath plane;plane.addPolygon(vertices);plane.closeSubpath();p.fillPath(plane,reflection);
    };
    if(skin()==QStringLiteral("kda_red")){
        // Broad black-chrome planes borrow the MV's lacquer reflections. The
        // reading plane stays opaque; only the rear material catches light.
        QLinearGradient lacquer(point(0,0),point(1,1));
        lacquer.setColorAt(0,QColor(62,35,44,66));lacquer.setColorAt(.38,QColor(7,8,12,105));lacquer.setColorAt(1,QColor(18,8,13,95));p.fillRect(r,lacquer);
        QLinearGradient smoke(point(.35,0),point(.7,1));
        smoke.setColorAt(0,QColor(135,145,173,22));smoke.setColorAt(.45,QColor(46,53,70,9));smoke.setColorAt(1,Qt::transparent);
        face({point(.52,0),point(1,0),point(1,.24),point(.29,1),point(.12,1)},smoke);
        QLinearGradient red(point(1,.05),point(.58,.94));
        red.setColorAt(0,QColor(229,52,83,46));red.setColorAt(.35,QColor(108,27,48,17));red.setColorAt(1,Qt::transparent);
        face({point(.91,0),point(1,0),point(1,1),point(.55,1)},red);
        if(r.width()>=64&&r.height()>=28){
            QPainterPath rim;rim.setFillRule(Qt::OddEvenFill);rim.addRect(r);rim.addRect(r.adjusted(5,5,-5,-5));
            p.save();p.setClipPath(rim,Qt::IntersectClip);paintFirestarterFlames(p,QRectF(r.left()+r.width()*.68,r.top()+r.height()*.25,r.width()*.32,r.height()*.75),.55);p.restore();
        }
        p.setPen(QPen(QColor(230,207,205,19),.65));p.drawLine(point(.52,0),point(.12,1));
        p.restore();return;
    }
    QLinearGradient polish(point(0,0),point(.85,1));
    polish.setColorAt(0,QColor(62,62,67,56));polish.setColorAt(.38,QColor(35,35,39,24));polish.setColorAt(1,QColor(4,4,6,58));p.fillRect(r,polish);

    // Few broad planes keep the surface calm behind compact HUD typography.
    QLinearGradient upper(point(.12,0),point(.42,.7));
    upper.setColorAt(0,QColor(111,111,117,26));upper.setColorAt(.48,QColor(67,67,72,19));upper.setColorAt(1,QColor(8,8,10,34));
    face({point(0,0),point(.69,0),point(.35,.43),point(0,.74)},upper);
    QLinearGradient center(point(.32,.16),point(.72,.94));
    center.setColorAt(0,QColor(126,126,131,29));center.setColorAt(.4,QColor(65,65,70,15));center.setColorAt(1,QColor(5,5,7,49));
    face({point(.69,0),point(1,.2),point(.77,.68),point(.43,.91),point(.1,.65),point(.35,.43)},center);
    QLinearGradient right(point(.76,.04),point(1,.88));
    right.setColorAt(0,QColor(8,8,10,36));right.setColorAt(.56,QColor(94,94,101,27));right.setColorAt(1,QColor(26,26,30,15));
    face({point(.69,0),point(1,0),point(1,1),point(.77,.68),point(1,.2)},right);
    QLinearGradient lower(point(.1,.62),point(.67,1));
    lower.setColorAt(0,QColor(5,5,7,42));lower.setColorAt(.62,QColor(47,47,52,18));lower.setColorAt(1,QColor(105,105,112,20));
    face({point(0,.74),point(.1,.65),point(.43,.91),point(.77,.68),point(1,1),point(0,1)},lower);

    // Sparse, softened veins sit below text contrast; no fine triangle mesh.
    if(r.width()>=64&&r.height()>=32){
        QPainterPath veins;veins.moveTo(point(-.03,.48));
        veins.cubicTo(point(.17,.34),point(.24,.57),point(.43,.43));
        veins.cubicTo(point(.51,.37),point(.55,.41),point(.62,.32));
        if(r.height()>=80){
            veins.moveTo(point(.57,.79));veins.cubicTo(point(.7,.64),point(.8,.84),point(1.03,.59));
            veins.moveTo(point(.76,.73));veins.cubicTo(point(.79,.7),point(.82,.62),point(.88,.6));
        }
        p.setBrush(Qt::NoBrush);p.setPen(QPen(QColor(112,112,119,7),1.7,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));p.drawPath(veins);
        p.setPen(QPen(QColor(147,147,154,16),.45,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));p.drawPath(veins);
    }
    p.restore();
}

inline void stagePanel(QPainter &p,const QRectF &r,bool prominent=false){
    HudStyle::paintSurface(p,r,prominent);
}
}
