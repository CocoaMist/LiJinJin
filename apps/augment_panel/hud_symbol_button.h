#pragma once
#include "hud_symbols.h"
#include "kda_theme.h"
#include "kda_surface.h"
#include "kda_motion.h"
#include <QAbstractButton>

class HudSymbolButton final : public QAbstractButton {
public:
    HudSymbolButton(HudSymbols::Symbol symbol,const QString &label,QWidget *parent=nullptr):QAbstractButton(parent),glyph(symbol),hoverMotion(this),pressMotion(this,140){
        setAccessibleName(label);setToolTip(label);setFocusPolicy(Qt::StrongFocus);setCursor(Qt::PointingHandCursor);setAttribute(Qt::WA_Hover);setFixedSize(24,24);
        connect(this,&QAbstractButton::pressed,this,[this]{pressMotion.retarget(1);});connect(this,&QAbstractButton::released,this,[this]{pressMotion.retarget(0);});
    }
    void setSymbol(HudSymbols::Symbol value){if(glyph!=value){glyph=value;update();}}
    QSize sizeHint()const override{return {24,24};}
protected:
    bool event(QEvent *event)override{const auto type=event->type();const bool result=QAbstractButton::event(event);if(type==QEvent::Enter||type==QEvent::FocusIn)hoverMotion.retarget(1);if(type==QEvent::Leave||type==QEvent::FocusOut)hoverMotion.retarget(underMouse()||hasFocus()?1:0);return result;}
    void paintEvent(QPaintEvent *)override{
        Kda::OverlayCanvas canvas(this);auto &painter=canvas.paint();if(!isEnabled())painter.setOpacity(.4);
        painter.setPen(hasFocus()?QPen(Kda::color("ice"),1):QPen(Qt::NoPen));
        painter.setBrush(Kda::mixColor(Kda::mixColor(QColor(21,20,28,0),Kda::color("surface"),hoverMotion.value()),Kda::color("raised"),pressMotion.value()));
        painter.drawRoundedRect(QRectF(rect()).adjusted(.5,.5,-.5,-.5),Kda::radius("tag"),Kda::radius("tag"));
        const qreal size=14+hoverMotion.value()*.8-pressMotion.value()*1.8;
        HudSymbols::draw(painter,glyph,QRectF((width()-size)/2.,(height()-size)/2.+pressMotion.value()*.7,size,size),Kda::mixColor(Kda::color("muted"),Kda::color("text"),hoverMotion.value()));
    }
private:
    HudSymbols::Symbol glyph;
    Kda::MotionValue hoverMotion,pressMotion;
};
