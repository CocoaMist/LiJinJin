#pragma once
#include "kda_theme.h"
#include <QApplication>
#include <QDynamicPropertyChangeEvent>
#include <QEvent>
#include <QWidget>
#include <QSignalBlocker>
#include <algorithm>
#include <cmath>

namespace Kda {
inline QColor mixColor(const QColor &from,const QColor &to,qreal amount){
    const qreal t=std::clamp(amount,qreal(0),qreal(1));
    return QColor::fromRgbF(from.redF()+(to.redF()-from.redF())*t,
        from.greenF()+(to.greenF()-from.greenF())*t,from.blueF()+(to.blueF()-from.blueF())*t,
        from.alphaF()+(to.alphaF()-from.alphaF())*t);
}

// Retarget from the currently displayed value: reversing a hover or rapidly
// toggling a switch never jumps back to the start of a canned animation.
class MotionValue final : public QObject {
public:
    explicit MotionValue(QWidget *widget,int durationMs=240,qreal initial=0)
        :QObject(widget),owner(widget),current(initial),destination(initial),baseDuration(durationMs){
        animation.setDuration(durationMs);animation.setEasingCurve(QEasingCurve::OutCubic);
        connect(&animation,&QVariantAnimation::valueChanged,this,[this](const QVariant &value){
            current=value.toReal();++frames;if(owner)owner->update();
        });
        if(qApp)qApp->installEventFilter(this);
        if(owner)owner->installEventFilter(this);
    }
    void retarget(qreal value){
        if(qFuzzyCompare(destination+1,value+1))return;
        animation.stop();const qreal origin=current;destination=value;
        if(!owner||!owner->isVisible()||!owner->isEnabled()||reducedMotion()||qAbs(current-destination)<.0001){finish();return;}
        // Short remaining distances settle promptly instead of floating through
        // another full-length animation after a quick pointer reversal.
        {
            // Changing duration can recalculate an old keyframe immediately.
            // Configure the new segment silently before starting at the exact
            // value already on screen.
            const QSignalBlocker quiet(&animation);
            animation.setDuration(std::max(70,qRound(baseDuration*std::sqrt(std::min(qreal(1),qAbs(origin-destination))))));
            animation.setStartValue(origin);animation.setEndValue(destination);animation.setCurrentTime(0);
        }
        current=origin;animation.start();
    }
    void finish(){animation.stop();current=destination;if(owner)owner->update();}
    qreal value()const{return current;}
    QJsonObject report()const{return {{"value",current},{"target",destination},{"running",animation.state()==QAbstractAnimation::Running},
        {"easing","OutCubic"},{"duration_ms",animation.duration()},{"frames",frames},{"hit_geometry_animated",false}};}
protected:
    bool eventFilter(QObject *object,QEvent *event)override{
        if(object==owner&&(event->type()==QEvent::Hide||event->type()==QEvent::EnabledChange))finish();
        if(object==qApp&&event->type()==QEvent::DynamicPropertyChange&&
           static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName()=="lixinReducedMotion"&&reducedMotion())finish();
        return QObject::eventFilter(object,event);
    }
private:
    QPointer<QWidget> owner;
    QVariantAnimation animation;
    qreal current=0,destination=0;
    int frames=0,baseDuration=240;
};
}
