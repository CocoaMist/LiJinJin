#pragma once
#include "kda_theme.h"
#include "kda_hud_style.h"
#include <QPainter>
#include <QPainterPath>
#include <algorithm>


namespace HudReadingPlane {
inline QColor ink(const char *token,int alpha){auto value=Kda::HudStyle::color(token);value.setAlpha(alpha);return value;}

inline bool opaqueProtection(){return !Kda::glassEnabled()||Kda::highContrastEnabled();}

inline QString surfaceIdentity(){return opaqueProtection()?QStringLiteral("opaque_accessible_reading_surface"):Kda::HudStyle::identity()+QStringLiteral("_matte_reading_plane");}
inline void paint(QPainter &p,const QRectF &b,bool self=false){
    if(b.width()<8||b.height()<8)return;p.save();p.fillRect(b,ink(self?"surface":"bg",opaqueProtection()?255:self?32:12));
    if(self)p.fillRect(QRectF(b.left(),b.top()+3,2,b.height()-6),Kda::HudStyle::accent());p.restore();
}
inline void portraitFrame(QPainter &p,const QRectF &b,qreal radius,bool focus=false){
    if(b.width()<8||b.height()<8)return;p.save();p.setBrush(Qt::NoBrush);p.setPen(QPen(focus?Kda::HudStyle::accent():Kda::HudStyle::color("border"),focus?1.3:.8));p.drawRoundedRect(b.adjusted(.5,.5,-.5,-.5),radius,radius);
    if(focus){p.setPen(QPen(Kda::HudStyle::neutral(),1.4));p.drawLine(b.bottomLeft()+QPointF(1,-5),b.bottomLeft()+QPointF(6,-1));}p.restore();
}
inline void headingRule(QPainter &p,const QRectF &b,qreal textWidth,const char *token="silver"){
    const qreal start=b.left()+textWidth+10,end=b.right()-3;if(end-start<16)return;p.save();p.setPen(QPen(ink(token,70),.7));p.drawLine(QPointF(start,b.center().y()),QPointF(end,b.center().y()));p.restore();
}

inline void deadPortrait(QPainter &p,const QRectF &bounds,qreal radius,int alpha=170){
    p.save();p.setPen(Qt::NoPen);p.setBrush(ink("bg",alpha));p.drawRoundedRect(bounds,radius,radius);p.restore();
}
}
