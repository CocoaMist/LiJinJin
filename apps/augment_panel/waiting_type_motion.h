#pragma once
#include "kda_theme.h"
#include <QApplication>
#include <QDynamicPropertyChangeEvent>
#include <QElapsedTimer>
#include <QEvent>
#include <QJsonObject>
#include <QTimer>
#include <QVariant>
#include <QWidget>
#include <algorithm>
#include <cmath>

namespace WaitingType {
inline constexpr qreal loopSeconds=6.4;
inline qreal progress(qreal t,qreal start,qreal duration){return std::clamp((t-start)/duration,qreal(0),qreal(1));}
inline qreal smooth(qreal t){return t*t*(3-2*t);}
inline qreal outBack(qreal t){const qreal u=t-1;return 1+2.25*u*u*u+1.25*u*u;}
inline qreal phase(qreal seconds){return std::fmod(std::max(qreal(0),seconds),loopSeconds);}
inline qreal impact(qreal t){
    if(t<2.08||t>=2.31)return 0;
    if(t<2.12)return .6;
    if(t<2.17)return -1;
    if(t<2.22)return .4;
    return .4*(1-progress(t,2.22,.09));
}
struct Pose {qreal x=0,y=0,sx=1,sy=1,opacity=1;};
inline Pose pose(qreal t,int index,int count,int row,qreal cap){
    const qreal enter=progress(t,.045*index+.13*row,.46);
    const qreal arrival=outBack(enter);
    const qreal leave=progress(t,5.40+(count-1-index)*.04+.06*row,.48);
    const qreal exit=leave*leave*leave;
    const qreal spread=(index-(count-1)*.5)*cap*.065;
    return {(1-arrival)*(row?-.50:.36)*cap+spread*(1-arrival)+exit*(row?.94:-.84)*cap,
        (1-arrival)*cap*.60-exit*cap*.30,
        .40+.60*arrival-exit*.55,
        .72+arrival*.28+exit*.12,
        smooth(enter)*(1-smooth(leave))};
}
inline const char *stage(qreal t){
    return t<.95?"stagger_in":t<2.08?"hold":t<2.31?"slice_impact":t<4.55?"outline_fill_wipe":t<5.40?"hold":t<6.30?"stagger_out":"loop_gap";
}

// A local activity clock, independent of the game refresh or inference thread.
// Keep the phase while unfocused or moving the window.
class Clock final:public QObject {
public:
    explicit Clock(QWidget *widget):QObject(widget),owner(widget){
        timer.setInterval(16);timer.setTimerType(Qt::PreciseTimer);
        connect(&timer,&QTimer::timeout,this,[this]{
            if(!canRun()){sync();return;}
            seconds+=std::min(elapsed.restart()/1000.,.066);++frames;owner->update();
        });
        if(qApp)qApp->installEventFilter(this);
    }
    void setEnabled(bool value){enabled=value;sync();}
    qreal elapsedSeconds()const{return seconds;}
    QJsonObject report()const{return {{"running",timer.isActive()},{"frames",frames},{"frame_interval_ms",timer.interval()},
        {"target_fps",60},{"loop_seconds",loopSeconds},{"reduced_motion",Kda::reducedMotion()},
        {"pauses_when_hidden_or_minimized",true},{"requires_window_focus",true},
        {"pauses_when_hidden_minimized_or_inactive",true},{"pauses_during_window_move_size",true},{"captures_input",false}};}
protected:
    bool eventFilter(QObject *object,QEvent *event)override{
        const auto type=event->type();const bool dashboard=owner&&object==owner->window();
        const bool surface=object==owner||dashboard;
        if(dashboard&&type==QEvent::DynamicPropertyChange&&
            static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName()=="dashboardInteractiveMoveSize")sync();
        if((surface&&(type==QEvent::Show||type==QEvent::Hide||type==QEvent::WindowStateChange||type==QEvent::WindowActivate||type==QEvent::WindowDeactivate))||
            (object==qApp&&type==QEvent::ApplicationStateChange)||
            (object==qApp&&type==QEvent::DynamicPropertyChange&&static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName()=="lixinReducedMotion")){
            sync();QTimer::singleShot(0,this,[this]{sync();});
        }
        return QObject::eventFilter(object,event);
    }
private:
    bool canRun()const{
        if(!enabled||!owner||!owner->isVisible()||Kda::reducedMotion())return false;
        const auto *window=owner->window();
        return !window->isMinimized()&&window->isActiveWindow()&&!window->property("dashboardInteractiveMoveSize").toBool();
    }
    void sync(){if(canRun()){if(!timer.isActive()){elapsed.start();timer.start();}}else timer.stop();if(owner)owner->update();}
    QPointer<QWidget> owner;QTimer timer;QElapsedTimer elapsed;qreal seconds=0;int frames=0;bool enabled=true;
};
}
