#pragma once
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <algorithm>
#include <cmath>

// App-owned monochrome vectors. Shared K/DA-inspired cut corners stay legible
// at HUD size; semantic colors are supplied by each caller.
namespace HudSymbols {
enum class Symbol { Sword, Shield, Coins, Star, Check, Reroll, Clock, Team, History,
    Chevron, Warning, Link, Info, Close, More, Collapse, Expand, Equal, Search, User, Back, Heart, Flame };
inline void draw(QPainter &p, Symbol symbol, const QRectF &bounds, const QColor &color, qreal stroke=1.5) {
    if(bounds.isEmpty())return;
    p.save();const qreal side=std::min(bounds.width(),bounds.height());
    p.translate(bounds.center()-QPointF(side/2,side/2));p.scale(side/16,side/16);
    p.setRenderHint(QPainter::Antialiasing);p.setPen(QPen(color,stroke,Qt::SolidLine,Qt::SquareCap,Qt::BevelJoin));p.setBrush(Qt::NoBrush);
    QPainterPath path;
    switch(symbol){
    case Symbol::Sword:
        path.moveTo(5,9);path.lineTo(11,3);path.lineTo(14,2);path.lineTo(13,5);path.lineTo(7,11);path.closeSubpath();
        path.moveTo(3,8);path.lineTo(8,13);path.moveTo(2,14);path.lineTo(5,11);break;
    case Symbol::Shield:
        path.moveTo(8,1.5);path.lineTo(13.5,3.5);path.lineTo(13,9);path.lineTo(11,12);path.lineTo(8,14.5);
        path.lineTo(5,12);path.lineTo(3,9);path.lineTo(2.5,3.5);path.closeSubpath();break;
    case Symbol::Coins:
        path.moveTo(2,3.5);path.lineTo(4,2);path.lineTo(9,2);path.lineTo(11,3.5);path.lineTo(9,5);path.lineTo(4,5);path.closeSubpath();
        path.moveTo(2,3.5);path.lineTo(2,8);path.lineTo(4,9.5);path.lineTo(9,9.5);path.lineTo(11,8);path.lineTo(11,3.5);
        path.moveTo(6,11);path.lineTo(6,12.5);path.lineTo(8,14);path.lineTo(12,14);path.lineTo(14,12.5);path.lineTo(14,8);path.lineTo(13,7);break;
    case Symbol::Star:
        path.moveTo(8,1.5);path.lineTo(10,6);path.lineTo(14.5,8);path.lineTo(10,10);path.lineTo(8,14.5);
        path.lineTo(6,10);path.lineTo(1.5,8);path.lineTo(6,6);path.closeSubpath();break;
    case Symbol::Check:path.moveTo(3,8);path.lineTo(6.3,11.3);path.lineTo(13,4.5);break;
    case Symbol::Reroll:
        path.moveTo(3,6);path.lineTo(5,2.5);path.lineTo(10.5,2.5);path.lineTo(13.5,6);
        path.moveTo(10,6);path.lineTo(13.5,6);path.lineTo(13.5,2.5);
        path.moveTo(13,10);path.lineTo(11,13.5);path.lineTo(5.5,13.5);path.lineTo(2.5,10);
        path.moveTo(6,10);path.lineTo(2.5,10);path.lineTo(2.5,13.5);break;
    case Symbol::Clock:case Symbol::History:
        path.moveTo(2,5);path.lineTo(5,2);
        path.lineTo(11,2);path.lineTo(14,5);path.lineTo(14,11);path.lineTo(11,14);path.lineTo(5,14);path.lineTo(2,11);
        if(symbol==Symbol::Clock)path.closeSubpath();
        path.moveTo(8,4.5);path.lineTo(8,8);path.lineTo(10.5,9.5);
        if(symbol==Symbol::History){path.moveTo(2,1.5);path.lineTo(2,5);path.lineTo(5.5,5);}break;
    case Symbol::Team:
        path.moveTo(6,2);path.lineTo(10,2);path.lineTo(11,4);path.lineTo(10,6);path.lineTo(6,6);path.lineTo(5,4);path.closeSubpath();
        path.moveTo(4,14);path.lineTo(4,10);path.lineTo(6,8);path.lineTo(10,8);path.lineTo(12,10);path.lineTo(12,14);
        path.moveTo(2,5);path.lineTo(2,7);path.lineTo(3,8);path.moveTo(14,5);path.lineTo(14,7);path.lineTo(13,8);
        path.moveTo(1,13);path.lineTo(1,10.5);path.lineTo(2,9.5);path.moveTo(15,13);path.lineTo(15,10.5);path.lineTo(14,9.5);break;
    case Symbol::Chevron:path.moveTo(6,3);path.lineTo(11,8);path.lineTo(6,13);break;
    case Symbol::Warning:path.moveTo(8,1.5);path.lineTo(15,14);path.lineTo(1,14);path.closeSubpath();path.moveTo(8,5.5);path.lineTo(8,9);p.drawPoint(QPointF(8,11.5));break;
    case Symbol::Link:
        path.moveTo(6,5);path.lineTo(9,2);path.lineTo(12,2);path.lineTo(14,4);path.lineTo(14,7);path.lineTo(11,10);
        path.moveTo(5,6);path.lineTo(2,9);path.lineTo(2,12);path.lineTo(4,14);path.lineTo(7,14);path.lineTo(10,11);
        path.moveTo(5.5,10.5);path.lineTo(10.5,5.5);break;
    case Symbol::Info:
        path.moveTo(5,1.5);path.lineTo(11,1.5);path.lineTo(14.5,5);path.lineTo(14.5,11);path.lineTo(11,14.5);
        path.lineTo(5,14.5);path.lineTo(1.5,11);path.lineTo(1.5,5);path.closeSubpath();
        p.drawPoint(QPointF(8,4.5));path.moveTo(8,7);path.lineTo(8,11.5);break;
    case Symbol::Close:path.moveTo(4,4);path.lineTo(12,12);path.moveTo(12,4);path.lineTo(4,12);break;
    case Symbol::More:p.drawPoint(QPointF(3,8));p.drawPoint(QPointF(8,8));p.drawPoint(QPointF(13,8));break;
    case Symbol::Collapse:path.moveTo(4,9);path.lineTo(8,5);path.lineTo(12,9);break;
    case Symbol::Expand:path.moveTo(4,6);path.lineTo(8,10);path.lineTo(12,6);break;
    case Symbol::Equal:path.moveTo(3,5.5);path.lineTo(13,5.5);path.moveTo(3,10.5);path.lineTo(13,10.5);break;
    case Symbol::Search:
        path.moveTo(5,2);path.lineTo(9,2);path.lineTo(12,5);path.lineTo(12,9);path.lineTo(9,12);
        path.lineTo(5,12);path.lineTo(2,9);path.lineTo(2,5);path.closeSubpath();path.moveTo(11,11);path.lineTo(14,14);break;
    case Symbol::User:
        path.moveTo(6,2);path.lineTo(10,2);path.lineTo(11,4);path.lineTo(10,6);path.lineTo(6,6);path.lineTo(5,4);path.closeSubpath();
        path.moveTo(3,14);path.lineTo(3,10);path.lineTo(6,8);path.lineTo(10,8);path.lineTo(13,10);path.lineTo(13,14);break;
    case Symbol::Back:path.moveTo(7,3);path.lineTo(2,8);path.lineTo(7,13);path.moveTo(2,8);path.lineTo(14,8);break;
    case Symbol::Flame:
        path.moveTo(9,1.5);path.cubicTo(10,6,5,5,6,9);path.cubicTo(3,8,4,6,4,6);
        path.cubicTo(-1,13,8,18,12,12);path.cubicTo(15,8,10,6,9,1.5);path.closeSubpath();break;
    case Symbol::Heart:
        path.moveTo(8,14);path.lineTo(2,8);path.lineTo(2,4);path.lineTo(4,2);path.lineTo(6,2);path.lineTo(8,4);
        path.lineTo(10,2);path.lineTo(12,2);path.lineTo(14,4);path.lineTo(14,8);path.closeSubpath();break;
    }
    p.drawPath(path);p.restore();
}
inline QPixmap pixmap(Symbol symbol,int size,qreal dpr,const QColor &color){
    QPixmap result(QSize(int(std::ceil(size*dpr)),int(std::ceil(size*dpr))));result.setDevicePixelRatio(dpr);result.fill(Qt::transparent);
    QPainter painter(&result);draw(painter,symbol,QRectF(0,0,size,size),color);return result;
}
}
