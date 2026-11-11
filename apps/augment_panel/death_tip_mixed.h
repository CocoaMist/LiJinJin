#pragma once
#include "kda_theme.h"
#include "kda_hud_style.h"
#include <QFontMetrics>
#include <QJsonArray>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QRegion>
#include <QVector>
#include <functional>
#include <algorithm>
#include <cmath>

namespace DeathTipMixed {
using IconResolver=std::function<QPixmap(const QString &,int)>;
struct Context {IconResolver resolve; qreal seconds=0,entrance=1; bool animated=false;};
struct Geometry {
    QString kind;
    QRectF caption,centerMetric,centerLabel,extra,connector;
    QVector<QRectF> tiles,icons,labels,metrics,notes;
    bool large=false;
};
inline bool identity(const QJsonValue &value){return value.isDouble()&&std::isfinite(value.toDouble())&&value.toDouble()>0&&value.toDouble()==value.toInt();}
inline QString layout(const QJsonObject &visual){
    const auto kind=visual.value("layout").toString();const auto bars=visual.value("bars").toArray();
    if(visual.value("kind")==QStringLiteral("bars")&&(kind==QStringLiteral("loadout")||kind==QStringLiteral("duel")||kind==QStringLiteral("rings"))){
        if(bars.isEmpty()||bars.size()>3||(kind==QStringLiteral("duel")&&bars.size()<2))return {};
        for(const auto &v:bars)if(!identity(v.toObject().value(kind==QStringLiteral("loadout")?"item_id":"champion_id")))return {};
        return kind;
    }
    if(kind==QStringLiteral("change")&&visual.value("kind")==QStringLiteral("facts")&&visual.value("rows").toArray().size()>=2
        &&identity(visual.value("champion_id"))&&identity(visual.value("item_id")))return kind;
    return {};
}
inline QString category(const QString &kind){
    if(kind==QStringLiteral("loadout"))return QStringLiteral("装备收益");
    if(kind==QStringLiteral("duel"))return QStringLiteral("输出对比");
    if(kind==QStringLiteral("rings"))return QStringLiteral("承伤结构");
    if(kind==QStringLiteral("change"))return QStringLiteral("装备动态");
    return QStringLiteral("对局分析");
}
inline Geometry geometry(const QRect &r,const QJsonObject &visual){
    Geometry g;g.kind=layout(visual);g.large=r.width()>=360;if(g.kind.isEmpty())return g;
    const qreal caption=g.large?20:14,top=r.top()+caption+7,w=r.width(),gap=g.large?12:6;
    g.caption={qreal(r.left()),qreal(r.top()),w,caption};
    const auto bars=visual.value("bars").toArray();
    if(g.kind==QStringLiteral("loadout")){
        const qreal width=(w-gap*(bars.size()-1))/bars.size(),size=g.large?34:20;
        for(int i=0;i<bars.size();++i){const qreal x=r.left()+i*(width+gap);
            g.tiles.append({x,top,width,r.bottom()-top+1});
            g.icons.append({x+(g.large?10:6),top+(g.large?8:6),size,size});
            g.metrics.append({x+size+(g.large?22:11),top+(g.large?7:5),width-size-(g.large?30:15),g.large?30.:20.});
            g.labels.append({x+8,top+(g.large?54:32),width-16,g.large?20.:13.});
            g.notes.append({x+8,top+(g.large?78:45),width-16,g.large?14.:12.});
        }
    }else if(g.kind==QStringLiteral("duel")){
        const qreal size=g.large?38:22;
        for(int i=0;i<2;++i){const qreal center=r.left()+w*(i==0?.16:.84),column=w*.31;
            g.icons.append({center-size/2,top+(g.large?4:2),size,size});
            g.labels.append({center-column/2,top+(g.large?48:30),column,g.large?16.:12.});
            g.metrics.append({center-column/2,top+(g.large?65:42),column,g.large?22.:16.});
        }
        g.centerMetric={r.left()+w*.34,top+(g.large?17:11),w*.32,g.large?36.:24.};
        g.centerLabel={r.left()+w*.34,g.centerMetric.bottom()+2,w*.32,g.large?16.:12.};
        if(g.large&&bars.size()>2)g.extra={qreal(r.left()),qreal(r.bottom()-10),w,11};
    }else if(g.kind==QStringLiteral("rings")){
        const qreal column=w/bars.size(),size=g.large?34:18;
        for(int i=0;i<bars.size();++i){const qreal center=r.left()+column*(i+.5);
            g.icons.append({center-size/2,top+4,size,size});
            g.labels.append({r.left()+column*i+2,top+(g.large?50:31),column-4,g.large?16.:12.});
            g.metrics.append({r.left()+column*i+2,top+(g.large?68:44),column-4,g.large?24.:14.});
        }
    }else{
        const qreal size=g.large?42:26;
        for(int i=0;i<2;++i){const qreal center=r.left()+w*(i==0?.15:.82),column=w*(i==0?.30:.36);
            g.icons.append({center-size/2,top+(g.large?6:4),size,size});
            g.labels.append({center-column/2,top+(g.large?56:36),column,g.large?20.:14.});
        }
        g.connector={r.left()+w*.29,top+(g.large?27:17),w*.35,1};
        g.centerLabel={r.left()+w*.36,top+(g.large?55:33),w*.20,g.large?20.:16.};
    }
    return g;
}
inline QRegion animatedRegion(const Geometry &g){
    QRegion region;for(const auto &r:g.icons)region+=r.adjusted(-9,-9,9,9).toAlignedRect();
    if(!g.connector.isEmpty())region+=g.connector.adjusted(-3,-3,3,3).toAlignedRect();return region;
}
inline QRegion stableRegion(const Geometry &g){
    QRegion region(g.caption.toAlignedRect());
    for(const auto &rows:{g.labels,g.metrics,g.notes})for(const auto &r:rows)region+=r.toAlignedRect();
    for(const auto &r:{g.centerMetric,g.centerLabel,g.extra})if(!r.isEmpty())region+=r.toAlignedRect();return region;
}
inline bool fits(const Geometry &g,const QRect &bounds){
    const QRectF area(bounds);
    for(const auto &rows:{g.tiles,g.icons,g.labels,g.metrics,g.notes})for(const auto &r:rows)if(!area.contains(r))return false;
    for(const auto &r:{g.caption,g.centerMetric,g.centerLabel,g.extra,g.connector})if(!r.isEmpty()&&!area.contains(r))return false;
    return true;
}
inline void text(QPainter &p,const QRectF &r,const QString &value,int pixels,QColor tint,bool center=false,bool bold=false){
    if(r.isEmpty())return;const auto font=Kda::font(Kda::FontRole::Body,pixels,bold?QFont::DemiBold:QFont::Medium);
    p.setFont(font);p.setPen(tint);p.drawText(r,(center?Qt::AlignHCenter:Qt::AlignLeft)|Qt::AlignVCenter,QFontMetrics(font).elidedText(value,Qt::ElideRight,qRound(r.width())));
}
inline void icon(QPainter &p,const QRectF &r,const QString &kind,int id,const QString &label,const Context &ctx,int index,QColor accent){
    const qreal phase=ctx.animated?ctx.seconds*1.6+index*1.7:0;
    p.save();p.setPen(Qt::NoPen);
    auto glow=accent;glow.setAlpha(qRound((ctx.animated?24+10*std::sin(phase):22)*Kda::HudStyle::strength()));p.setBrush(glow);p.drawEllipse(r.adjusted(-4,-4,4,4));
    QPainterPath clip;if(kind==QStringLiteral("champion"))clip.addEllipse(r);else clip.addRoundedRect(r,3,3);
    p.setBrush(QColor(24,28,38));p.drawPath(clip);
    const auto pixmap=ctx.resolve?ctx.resolve(kind,id):QPixmap();
    p.save();p.setClipPath(clip,Qt::IntersectClip);
    if(!pixmap.isNull()){
        const qreal scale=(.94+.06*ctx.entrance)*(ctx.animated?1.018+.018*std::sin(phase):1.);
        QRectF target(r.center().x()-r.width()*scale/2,r.center().y()-r.height()*scale/2,r.width()*scale,r.height()*scale);
        target.translate(0,ctx.animated?.5*std::sin(phase):0);
        const qreal side=std::min(pixmap.width(),pixmap.height());p.drawPixmap(target,pixmap,QRectF((pixmap.width()-side)/2,(pixmap.height()-side)/2,side,side));
    }else text(p,r,label.left(1),qRound(r.height()*.45),Kda::color("muted"),true);
    p.restore();
    auto rim=accent;rim.setAlpha(100+qRound(50*Kda::HudStyle::strength()));p.setPen(QPen(rim,.9));p.setBrush(Qt::NoBrush);p.drawPath(clip);
    const qreal angle=ctx.animated?ctx.seconds*38+index*71:0;
    accent.setAlpha(qRound(200*Kda::HudStyle::strength()));p.setPen(QPen(accent,1.2,Qt::SolidLine,Qt::RoundCap));p.drawArc(r.adjusted(-3,-3,3,3),qRound((-90-angle)*16),60*16);
    p.restore();
}
inline void paint(QPainter &p,const QRect &r,const QJsonObject &visual,const Context &ctx){
    const auto g=geometry(r,visual);if(g.kind.isEmpty())return;
    p.save();p.setClipRect(r,Qt::IntersectClip);
    text(p,g.caption,visual.value("caption").toString(),g.large?12:10,Kda::color("muted"));
    const auto bars=visual.value("bars").toArray();
    if(g.kind==QStringLiteral("loadout")){
        for(int i=0;i<bars.size();++i){const auto row=bars[i].toObject();const auto accent=Kda::color(i==0?"ice":"silver");
            auto tile=Kda::color("raised");tile.setAlpha(i==0?115:55);p.setPen(Qt::NoPen);p.setBrush(tile);p.drawRoundedRect(g.tiles[i].adjusted(.5,.5,-.5,-.5),4,4);
            if(i==0){p.setPen(QPen(Kda::HudStyle::accent(135),1.1,Qt::SolidLine,Qt::RoundCap));p.drawLine(g.tiles[i].topLeft()+QPointF(8,.7),g.tiles[i].topLeft()+QPointF(std::min(qreal(38),g.tiles[i].width()-8),.7));}
            icon(p,g.icons[i],QStringLiteral("item"),row.value("item_id").toInt(),row.value("label").toString(),ctx,i,accent);
            text(p,g.metrics[i],QStringLiteral("+%1%").arg(row.value("value").toDouble(),0,'f',1),g.large?22:12,accent,false,true);
            text(p,g.labels[i],row.value("label").toString(),g.large?14:10,Kda::color("silver"));
            const auto cost=row.value("cost");if(cost.isDouble()&&std::isfinite(cost.toDouble())&&cost.toDouble()>0)
                text(p,g.notes[i],QStringLiteral("合成 %1 G").arg(cost.toDouble(),0,'f',0),g.large?10:9,Kda::color("muted"));
        }
    }else if(g.kind==QStringLiteral("duel")){
        for(int i=0;i<2;++i){const auto row=bars[i].toObject();icon(p,g.icons[i],QStringLiteral("champion"),row.value("champion_id").toInt(),row.value("label").toString(),ctx,i,Kda::color(i==0?"ice":"silver"));
            text(p,g.labels[i],row.value("label").toString(),g.large?13:10,Kda::color("silver"),true);
            text(p,g.metrics[i],QString::number(row.value("value").toDouble(),'f',0),g.large?20:12,Kda::color("text"),true,true);
        }
        const double base=bars[1].toObject().value("value").toDouble(),gap=base>0?(bars[0].toObject().value("value").toDouble()/base-1)*100:0;
        text(p,g.centerMetric,base<=0||!std::isfinite(gap)?QStringLiteral("—"):gap<5?QStringLiteral("接近"):QStringLiteral("+%1%").arg(gap,0,'f',1),g.large?26:16,Kda::color("ice"),true,true);
        text(p,g.centerLabel,QStringLiteral("条件输出差距"),g.large?11:9,Kda::color("muted"),true);
        if(!g.extra.isEmpty()){const auto third=bars[2].toObject();text(p,g.extra,QStringLiteral("第三位  %1 · %2").arg(third.value("label").toString()).arg(third.value("value").toDouble(),0,'f',0),9,Kda::color("muted"),true);}
    }else if(g.kind==QStringLiteral("rings")){
        qreal maximum=0;for(const auto &v:bars)maximum=std::max(maximum,v.toObject().value("value").toDouble());
        for(int i=0;i<bars.size();++i){const auto row=bars[i].toObject();const auto accent=Kda::color(i==0?"ice":"silver");const auto ring=g.icons[i].adjusted(g.large?-7:-5,g.large?-7:-5,g.large?7:5,g.large?7:5);
            p.setBrush(Qt::NoBrush);p.setPen(QPen(Kda::HudStyle::neutral(42),g.large?3:2,Qt::SolidLine,Qt::RoundCap));p.drawArc(ring,225*16,-270*16);
            p.setPen(QPen(accent,g.large?3:2,Qt::SolidLine,Qt::RoundCap));p.drawArc(ring,225*16,-qRound(270*16*row.value("value").toDouble()/maximum));
            icon(p,g.icons[i],QStringLiteral("champion"),row.value("champion_id").toInt(),row.value("label").toString(),ctx,i,accent);
            text(p,g.labels[i],row.value("label").toString(),g.large?13:10,Kda::color("silver"),true);
            text(p,g.metrics[i],QString::number(row.value("value").toDouble(),'f',0),g.large?21:11,Kda::color("text"),true,true);
        }
    }else{
        const auto rows=visual.value("rows").toArray();
        for(int i=0;i<2;++i){const auto label=rows[i].toObject().value("value").toString();icon(p,g.icons[i],i==0?QStringLiteral("champion"):QStringLiteral("item"),visual.value(i==0?"champion_id":"item_id").toInt(),label,ctx,i,Kda::color(i==0?"silver":"ice"));
            text(p,g.labels[i],label,g.large?15:10,Kda::color("silver"),true);
        }
        p.setPen(QPen(Kda::HudStyle::neutral(80),1));p.drawLine(g.connector.topLeft(),g.connector.topRight());
        const qreal x=g.connector.left()+g.connector.width()*(ctx.animated?std::fmod(ctx.seconds*.35,1.):.5);p.setPen(Qt::NoPen);p.setBrush(Kda::HudStyle::accent(220));p.drawEllipse(QPointF(x,g.connector.y()),2,2);
        text(p,g.centerLabel,QStringLiteral("新增"),g.large?12:10,Kda::color("ice"),true,true);
    }
    p.restore();
}
}
