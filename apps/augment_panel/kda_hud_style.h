#pragma once
#include "kda_theme.h"
#include "overlay_backdrop.h"
#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QWidget>
#include <algorithm>
#include <cmath>

// A static skin finish shared by every HUD. Strength changes decoration only;
// data contrast, geometry, team colours and input behaviour remain independent.
namespace Kda::HudStyle {
inline qreal strength(){
    const auto value=qApp?qApp->property("lixinHudSkinStrength"):QVariant();
    const qreal amount=value.isValid()?value.toDouble():.65;
    return std::isfinite(amount)?std::clamp(amount,qreal(0),qreal(1)):.65;
}
inline void setStrength(qreal value){
    value=std::isfinite(value)?std::clamp(value,qreal(0),qreal(1)):.65;
    if(qApp&&!qFuzzyCompare(strength()+1,value+1)){
        qApp->setProperty("lixinHudSkinStrength",value);
        for(auto widget:QApplication::allWidgets())widget->update();
    }
}
inline QColor palette(const char *token,int alpha=255){auto value=Kda::color(token);value.setAlpha(std::clamp(alpha,0,255));return value;}
inline QColor accent(int alpha=255){return palette("ice",qRound(alpha*(.25+.75*strength())));}
inline QColor neutral(int alpha=255){return palette("silver",alpha);}
inline QColor color(const char *token){
    const QByteArray key(token);
    if(key=="ice"||key=="lilac")return palette(token,qRound(255*(.25+.75*strength())));
    return Kda::color(token);
}
inline QString identity(){return Kda::skin()==QStringLiteral("kda_red")?QStringLiteral("kda_firestarter_shared_dark_hud"):QStringLiteral("kda_silver_shared_dark_hud");}
inline void paintSignature(QPainter &p,const QRectF &b,qreal scale=1.){
    const qreal amount=strength()*std::clamp(scale,qreal(0),qreal(1));if(b.width()<12||b.height()<10||amount<=0)return;
    p.save();p.setClipRect(b,Qt::IntersectClip);p.setPen(Qt::NoPen);
    const qreal x=b.right()-8,y=b.top()+1;
    p.setBrush(palette("ice",qRound(225*amount)));
    for(int i=0;i<3;++i){QPainterPath cut;const qreal left=x-7-i*7;cut.moveTo(left,y);cut.lineTo(left+3,y);cut.lineTo(left-4,y+10);cut.lineTo(left-7,y+10);cut.closeSubpath();p.drawPath(cut);}
    p.fillRect(QRectF(b.left()+1,b.top()+7,2,std::min(qreal(20),b.height()-12)),neutral(qRound(200*amount)));
    p.setPen(QPen(palette("ice",qRound(170*amount)),1));p.drawLine(b.bottomLeft()+QPointF(8,-1),b.bottomLeft()+QPointF(35,-1));p.restore();
}
inline void paintSurface(QPainter &p,const QRectF &b,bool prominent=false){
    if(b.isEmpty())return;
    p.save();
    QPainterPath shape;
    const qreal radius=Kda::radius("hud");shape.addRoundedRect(b,radius,radius);
    auto background=color("bg");background.setAlpha(overlaySurfaceAlpha());p.fillPath(shape,background);
    if(prominent){
        p.save();
        p.setClipPath(shape,Qt::IntersectClip);
        auto header=color("surface");header.setAlpha(glassEnabled()?48:255);p.fillRect(QRectF(b.left(),b.top(),b.width(),28),header);
        p.restore();
    }
    p.setPen(QPen(neutral(44),.8));
    p.setBrush(Qt::NoBrush);
    p.drawPath(shape);
    p.restore();
}
}
