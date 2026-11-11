#pragma once
#include "kda_theme.h"
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QResizeEvent>
#include <QRegion>
#include <algorithm>
#include <cmath>

namespace VersionAbout {
inline constexpr int EntranceDuration=760;
inline qreal cutProgress(qreal time,qreal delay,qreal duration=150.){
    const qreal p=std::clamp((time-delay)/duration,qreal(0),qreal(1));
    return 1.-std::pow(1.-p,4.);
}
inline qreal cutJolt(qreal time){
    if(time>=88&&time<116)return 1.;
    if(time>=186&&time<212)return -.75;
    if(time>=282&&time<304)return .5;
    return 0.;
}

// Native children remain inside a fixed layout slot. Only their own geometry
// and reveal mask move, so a late layout pass cannot pull an entrance apart.
class EntranceSlot final : public QWidget {
public:
    EntranceSlot(QWidget *page,qreal delay,int direction):content(page),delay(delay),direction(direction){
        content->setParent(this);setSizePolicy(content->sizePolicy());
    }
    QSize sizeHint()const override{return content->sizeHint();}
    QSize minimumSizeHint()const override{return content->minimumSizeHint();}
    void setEntrance(qreal milliseconds,bool running){
        constexpr qreal duration=210.;
        const auto next=running?std::clamp(milliseconds,delay,delay+duration):delay+duration;
        if(time==next)return;
        time=next;place();
    }
    bool restored()const{return content->geometry()==rect()&&content->mask().isEmpty();}
protected:
    void resizeEvent(QResizeEvent *event)override{QWidget::resizeEvent(event);place();}
private:
    void place(){
        const qreal progress=cutProgress(time,delay,210.);
        const int offset=progress>=1?0:qRound(direction*44*(1.-progress));
        const auto geometry=rect().translated(offset,0);
        if(content->geometry()!=geometry)content->setGeometry(geometry);
        if(progress>=1){if(!content->mask().isEmpty())content->clearMask();return;}
        QRegion visible(QRect(-3,-3,1,1));
        for(int band=0;band<3;++band){
            const qreal p=cutProgress(time,delay+band*22,166);if(p<=0)continue;
            const qreal top=height()*band/3.,bottom=height()*(band+1)/3.;
            const qreal front=(width()+22)*p;
            QPolygonF shape{{0,top},{front-22,top},{front,bottom},{0,bottom}};
            if(direction<0)for(auto &point:shape)point.setX(width()-point.x());
            visible|=QRegion(shape.toPolygon());
        }
        if(content->mask()!=visible)content->setMask(visible);
    }
    QWidget *content;qreal delay,time=EntranceDuration;int direction;
};

class PanelStrike final : public QWidget {
public:
    explicit PanelStrike(QWidget *parent):QWidget(parent){
        setAttribute(Qt::WA_TransparentForMouseEvents);setAttribute(Qt::WA_NoSystemBackground);hide();
    }
    void setEntrance(qreal milliseconds,bool running){
        const auto previous=time;time=milliseconds;
        if(!running||time>=520){if(!isHidden())hide();return;}
        if(isHidden()){show();raise();}
        else if(previous!=time&&(hasInk(previous)||hasInk(time)))update();
    }
protected:
    void paintEvent(QPaintEvent *)override{
        QPainter painter(this);Kda::configurePainter(painter);
        const auto ink=Kda::skin()==QStringLiteral("kda_red")?QColor("#ffabd6"):QColor("#d9ceff");
        for(int beat=0;beat<3;++beat){
            const qreal phase=(time-(35+beat*112))/125.;if(phase<0||phase>1)continue;
            const qreal x=-140+(width()+340)*phase,slant=height()*.30;
            auto flash=ink;flash.setAlpha(qRound(165*(1.-phase*.45)));painter.setPen(Qt::NoPen);painter.setBrush(flash);
            painter.drawPolygon(QPolygonF{{x-28,0},{x+6,0},{x+slant+6,qreal(height())},{x+slant-28,qreal(height())}});
            auto trail=ink;trail.setAlpha(28);painter.setBrush(trail);
            painter.drawPolygon(QPolygonF{{x-120,0},{x-35,0},{x+slant-35,qreal(height())},{x+slant-120,qreal(height())}});
            auto line=QColor("#f4efff");line.setAlpha(210);painter.setPen(QPen(line,1.3));
            painter.drawLine(QPointF(x+12,0),QPointF(x+slant+12,height()));
        }
        // Short, local offset strokes read as an edit cut rather than a fade.
        const auto jolt=cutJolt(time);if(jolt!=0){
            auto edge=ink;edge.setAlpha(175);painter.setPen(QPen(edge,2));
            const qreal y=height()*(time<150?.22:time<250?.49:.76);
            painter.drawLine(QPointF(width()*.06,y),QPointF(width()*.84,y));
        }
    }
private:
    // All three beats overlap within this window, including the cut strokes.
    static bool hasInk(qreal time){return time>=35.&&time<=384.;}
    qreal time=EntranceDuration;
};
inline void paintPortraitEntrance(QPainter &painter,const QPixmap &image,QRectF bounds,qreal time,int ordinal){
    const qreal local=time-ordinal*52.;
    const auto accent=Kda::skin()==QStringLiteral("kda_red")?QColor("#ffa0ce"):QColor("#d4c8ff");
    for(int band=0;band<6;++band){
        const qreal top=bounds.top()+bounds.height()*band/6.,bottom=bounds.top()+bounds.height()*(band+1)/6.;
        const qreal p=cutProgress(local,(band/2)*76.);if(p<=0)continue;
        const qreal front=bounds.left()+(bounds.width()+14)*p;
        QPainterPath mask;mask.addPolygon(QPolygonF{{bounds.left(),top},{front-14,top},{front,bottom},{bounds.left(),bottom}});
        painter.save();painter.setClipRect(QRectF(bounds.left(),top,bounds.width(),bottom-top),Qt::IntersectClip);painter.setClipPath(mask,Qt::IntersectClip);
        const qreal shift=(band%2?-1:1)*(25*(1.-p)+cutJolt(local)*7.);
        painter.drawPixmap(bounds.translated(shift,0),image,QRectF(image.rect()));
        if(p<1){auto edge=accent;edge.setAlpha(190);painter.setPen(QPen(edge,1.3));painter.drawLine(QPointF(front-14,top),QPointF(front,bottom));}
        if(cutJolt(local)!=0&&band==int(std::max(qreal(0),local)/90.)%6){auto hit=accent;hit.setAlpha(55);painter.fillRect(QRectF(bounds.left(),top,bounds.width(),bottom-top),hit);}
        painter.restore();
    }
    if(local>=350&&local<470){
        const qreal x=bounds.left()+bounds.width()*(local-350)/120.;
        auto glint=accent;glint.setAlpha(45);painter.setPen(QPen(glint,1));painter.drawLine(QPointF(x-10,bounds.top()),QPointF(x+14,bounds.bottom()));
    }
}

class Name final : public QLabel {
public:
    explicit Name(const QString &copy):QLabel(copy){setTextFormat(Qt::PlainText);}
    void setEntrance(qreal milliseconds,bool running){
        const bool animated=running&&milliseconds<470.;
        const auto next=std::max(milliseconds,qreal(65.));
        if(active==animated&&(!animated||time==next))return;
        time=next;active=animated;update();
    }
protected:
    void paintEvent(QPaintEvent *event)override{
        if(!active){QLabel::paintEvent(event);return;}
        QPainter painter(this);Kda::configurePainter(painter);
        const auto area=contentsRect();const QFontMetricsF metrics(font());
        const qreal baseline=area.top()+(area.height()-metrics.height())/2.+metrics.ascent();
        QPainterPath textPath;textPath.addText(QPointF(area.left(),baseline),font(),text());
        const qreal width=std::min(qreal(area.width()),metrics.horizontalAdvance(text())+3.);
        for(int band=0;band<3;++band){
            const qreal p=cutProgress(time,65+band*48,140);if(p<=0)continue;
            const qreal top=area.top()+area.height()*band/3.,height=area.height()/3.+1;
            painter.save();painter.setClipRect(QRectF(area.left(),top,width*p,height));
            painter.translate((band%2?-1:1)*(18*(1.-p)+cutJolt(time)*5),0);
            const auto ink=Kda::color("silver");
            if(time<155){painter.setBrush(Qt::NoBrush);painter.setPen(QPen(ink,.75));}
            else{painter.setPen(Qt::NoPen);painter.setBrush(ink);}
            painter.drawPath(textPath);painter.restore();
        }
    }
private:
    qreal time=EntranceDuration;bool active=false;
};
}
