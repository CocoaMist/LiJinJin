#pragma once
#include "kda_theme.h"
#include <QApplication>
#include <QDynamicPropertyChangeEvent>
#include <QElapsedTimer>
#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QJsonArray>
#include <QTimer>
#include <QRegion>
#include <QWidget>
#include <algorithm>
#include <cmath>

namespace Kda {
// A local edge reflection; the interior remains transparent for native text.
inline void paintEdgeFlow(QPainter &p,const QRectF &surface,qreal seconds,qreal amount=.8,qreal rounding=-1){
    if(reducedMotion()||highContrastEnabled()||surface.width()<16||surface.height()<12||amount<=0)return;
    const qreal r=rounding<0?radius("hud"):rounding;
    QPainterPath edge;edge.addRoundedRect(surface.adjusted(.8,.8,-.8,-.8),std::max(qreal(0),r-.8),std::max(qreal(0),r-.8));
    const auto tint=[amount](const char *token,int alpha){auto c=color(token);c.setAlpha(qRound(alpha*std::clamp(amount,qreal(0),qreal(1))));return c;};
    QConicalGradient reflection(surface.center(),std::fmod(seconds*28.,360.));
    reflection.setColorAt(0,Qt::transparent);reflection.setColorAt(.012,tint("lilac",35));
    reflection.setColorAt(.035,tint("ice",155));reflection.setColorAt(.047,tint("silver",230));
    reflection.setColorAt(.060,tint("ice",95));reflection.setColorAt(.11,Qt::transparent);reflection.setColorAt(1,Qt::transparent);
    p.save();p.setBrush(Qt::NoBrush);p.setPen(QPen(QBrush(reflection),1.2,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));p.drawPath(edge);p.restore();
}
// A tiny pearl core with a soft halo and a tapered four-point glint.
inline void paintSparkle(QPainter &p, QPointF center, qreal size, qreal opacity, QColor tint) {
    if (size <= 0 || opacity <= 0) return;
    p.save();
    p.setOpacity(p.opacity() * std::clamp(opacity, qreal(0), qreal(1)));
    if(skin()==QStringLiteral("kda_red")){
        QRadialGradient heat(center,size*3.4);heat.setColorAt(0,QColor(255,155,67,130));heat.setColorAt(.4,QColor(238,58,38,42));heat.setColorAt(1,QColor(201,19,61,0));
        p.setPen(Qt::NoPen);p.setBrush(heat);p.drawEllipse(center,size*3.4,size*3.4);
        QPainterPath ember;ember.moveTo(center+QPointF(size*.28,-size*1.25));
        ember.cubicTo(center+QPointF(-size*.8,-size*.1),center+QPointF(-size*.55,size*.72),center+QPointF(0,size*.8));
        ember.cubicTo(center+QPointF(size*.65,size*.55),center+QPointF(size*.72,-size*.1),center+QPointF(size*.28,-size*1.25));
        QLinearGradient flame(center+QPointF(0,-size),center+QPointF(0,size));flame.setColorAt(0,QColor(255,78,39,200));flame.setColorAt(.5,QColor(255,178,81,230));flame.setColorAt(1,QColor(255,235,183,245));
        p.fillPath(ember,flame);p.restore();return;
    }
    const qreal halo = size * 2.6;
    QRadialGradient glow(center, halo);
    tint.setAlpha(85); glow.setColorAt(0, tint);
    tint.setAlpha(22); glow.setColorAt(.35, tint);
    tint.setAlpha(0); glow.setColorAt(1, tint);
    p.setPen(Qt::NoPen); p.setBrush(glow);
    p.drawEllipse(center, halo, halo);
    QPainterPath star;
    star.moveTo(center + QPointF(0, -size));
    star.lineTo(center + QPointF(size * .19, -size * .19));
    star.lineTo(center + QPointF(size * .72, 0));
    star.lineTo(center + QPointF(size * .19, size * .19));
    star.lineTo(center + QPointF(0, size));
    star.lineTo(center + QPointF(-size * .19, size * .19));
    star.lineTo(center + QPointF(-size * .72, 0));
    star.lineTo(center + QPointF(-size * .19, -size * .19));
    star.closeSubpath();
    tint.setAlpha(225); p.fillPath(star, tint);
    p.setBrush(QColor(250, 253, 255, 235));
    p.drawEllipse(center, .65, .65);
    p.restore();
}

// Decoration only: the owner paints this below its text, with no child surface,
// hit targets, geometry changes or dependency on the game refresh cadence.
class SparkleField final : public QObject {
public:
    enum class Activity { ActiveWindow, VisibleOverlay };
    explicit SparkleField(QWidget *widget,Activity mode=Activity::ActiveWindow) : QObject(widget), owner(widget),activity(mode) {
        timer.setInterval(mode==Activity::VisibleOverlay?50:33);
        timer.setTimerType(Qt::CoarseTimer);
        connect(&timer, &QTimer::timeout, this, [this] {
            if (!canAnimate()) { sync(); return; }
            seconds += std::min(clock.restart() / 1000., .08);
            ++frames;
            if(activity==Activity::VisibleOverlay)owner->update(repaintRegion.isEmpty()?QRegion(owner->rect()).subtracted(QRegion(owner->rect().adjusted(8,8,-8,-8))):repaintRegion);
            else owner->update();
        });
        if (qApp) qApp->installEventFilter(this);
    }
    void paint(QPainter &p, const QRectF &area, int count = 22, qreal strength = 1.,bool perimeter=false) const {
        if (area.isEmpty() || !hasVisibleDecoration() || reducedMotion()) return;
        p.save(); p.setClipRect(area, Qt::IntersectClip);
        const auto fraction = [](qreal value) { return value - std::floor(value); };
        for (int i = 0; i < std::clamp(count, 0, 32); ++i) {
            const qreal seed = i + 1;
            const qreal x = fraction(seed * .61803398875);
            const qreal y = fraction(seed * .75487766625 + .17);
            // Each mote has its own slow pulse. No broad flashing surface.
            const qreal pulse = std::pow(.5 + .5 * std::sin(seconds * (1.2 + fraction(seed * .27)) + seed * 2.4), 4);
            QPointF center(area.left() + 7 + x * std::max(qreal(0), area.width() - 14) + std::sin(seconds * .35 + seed) * 2,
                           area.top() + 7 + y * std::max(qreal(0), area.height() - 14) + std::cos(seconds * .3 + seed) * 1.5);
            if(perimeter){
                const qreal w=area.width(),h=area.height(),distance=fraction(x+seconds*.012)*2*(w+h);
                if(distance<w)center={area.left()+distance,area.top()+1};
                else if(distance<w+h)center={area.right()-1,area.top()+distance-w};
                else if(distance<2*w+h)center={area.right()-(distance-w-h),area.bottom()-1};
                else center={area.left()+1,area.bottom()-(distance-2*w-h)};
            }
            if(skin()==QStringLiteral("kda_red")){
                if(!perimeter)center.setY(area.top()+7+fraction(y-seconds*.024)*(std::max(qreal(0),area.height()-14)));
                paintSparkle(p,center,.75+pulse*1.6,(.12+pulse*.65)*strength,QColor(255,142,65));
                if(i%3==1){const auto tail=center+QPointF(1.5,4+pulse*6);QLinearGradient trail(tail,center);trail.setColorAt(0,QColor(255,60,30,0));trail.setColorAt(1,QColor(255,170,76,qRound((.12+pulse*.6)*strength*180)));p.setPen(QPen(trail,.7,Qt::SolidLine,Qt::RoundCap));p.drawLine(tail,center);}
                continue;
            }
            const QColor tint = i % 3 == 0 ? QColor(210, 186, 245) : i % 3 == 1 ? QColor(174, 229, 250) : QColor(247, 245, 255);
            if (i % 4 == 0) paintSparkle(p, center, 1.8 + pulse * 3.2, (.16 + pulse * .84) * strength, tint);
            else if(i%4==1){
                p.save();p.translate(center);p.rotate(seconds*15+seed*43);p.setPen(Qt::NoPen);
                auto shard=tint;shard.setAlpha(qRound((.18+pulse*.65)*strength*255));p.setBrush(shard);
                const qreal size=1.6+pulse*1.5;QPainterPath crystal;crystal.moveTo(0,-size);crystal.lineTo(size*.55,0);crystal.lineTo(0,size);crystal.lineTo(-size*.55,0);crystal.closeSubpath();p.drawPath(crystal);p.restore();
            }else if(i%4==3){
                QPointF direction(1,-.45);
                if(perimeter){if(center.y()<area.top()+3)direction={1,0};else if(center.x()>area.right()-3)direction={0,1};else if(center.y()>area.bottom()-3)direction={-1,0};else direction={0,-1};}
                const QPointF tail=center-direction*(3+pulse*6);QLinearGradient streak(tail,center);
                auto head=tint;head.setAlpha(qRound((.15+pulse*.7)*strength*255));auto clear=head;clear.setAlpha(0);
                streak.setColorAt(0,clear);streak.setColorAt(1,head);p.setPen(QPen(streak,.9,Qt::SolidLine,Qt::RoundCap));p.drawLine(tail,center);
            }else {
                p.setPen(Qt::NoPen); auto pearl = tint;
                pearl.setAlpha(qRound((.12 + pulse * .68) * std::clamp(strength, qreal(0), qreal(1)) * 255));
                p.setBrush(pearl); const qreal radius = .55 + pulse * .65;
                p.drawEllipse(center, radius, radius);
            }
        }
        p.restore();
    }
    void paintBorder(QPainter &p,const QRectF &surface,int count=18,qreal band=6)const{
        if(surface.width()<band*2||surface.height()<band*2)return;
        QPainterPath outer,inner;outer.addRoundedRect(surface,radius("hud"),radius("hud"));
        inner.addRoundedRect(surface.adjusted(band,band,-band,-band),radius("card_inner"),radius("card_inner"));
        p.save();p.setClipPath(outer.subtracted(inner),Qt::IntersectClip);
        paint(p,surface.adjusted(2,2,-2,-2),count,.9,true);p.restore();
    }
    // A narrow moving reflection, shared by HUD rims and dashboard headers.
    // The transparent middle keeps the reading plane and all hit targets still.
    void paintFlow(QPainter &p,const QRectF &surface,qreal amount=.8,qreal rounding=-1)const{
        if(hasVisibleDecoration())paintEdgeFlow(p,surface,seconds,amount,rounding);
    }
    QJsonObject report() const {
        return {{"running", timer.isActive()}, {"frames", frames}, {"frame_interval_ms", timer.interval()},
                {"max_particles", 32}, {"reduced_motion", reducedMotion()},
                {"flow_style","localized_kda_edge_reflection"},{"flow_band_px",2.2},{"flow_covers_text",false},
                {"particle_types",skin()==QStringLiteral("kda_red")?QJsonArray{"fire_ember","rising_cinder","heat_trail"}:QJsonArray{QStringLiteral("star_glint"),QStringLiteral("crystal_shard"),QStringLiteral("pearl_mote"),QStringLiteral("comet_trail")}},
                {"pauses_when_hidden_or_minimized", true},{"requires_window_focus",activity==Activity::ActiveWindow},
                {"pauses_when_hud_strength_zero",activity==Activity::VisibleOverlay},{"pauses_in_high_contrast",true},
                {"pauses_when_hidden_minimized_or_inactive", activity==Activity::ActiveWindow}, {"captures_input", false}};
    }
    void setEnabled(bool value){if(enabled==value)return;enabled=value;sync();if(owner)owner->update();}
    void setRepaintRegion(const QRegion &region){repaintRegion=region;}
    qreal elapsedSeconds()const{return seconds;}
protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        const auto type = event->type();
        const bool surface = object == owner || (owner && object == owner->window());
        if ((surface && (type == QEvent::Show || type == QEvent::Hide || type == QEvent::WindowStateChange ||
                         type == QEvent::WindowActivate || type == QEvent::WindowDeactivate)) ||
            (object == qApp && type == QEvent::ApplicationStateChange) ||
            (object == qApp && type == QEvent::DynamicPropertyChange &&
             (static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName() == "lixinReducedMotion" ||
              static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName() == "lixinHudSkinStrength"))) {
            sync();
            // Show/activation notifications can precede the final Qt state.
            if(!syncQueued){syncQueued=true;QTimer::singleShot(0,this,[this]{syncQueued=false;sync();});}
        }
        return QObject::eventFilter(object, event);
    }
private:
    bool hasVisibleDecoration() const {
        if(!enabled||highContrastEnabled())return false;
        if(activity==Activity::VisibleOverlay&&qApp){
            const auto setting=qApp->property("lixinHudSkinStrength");
            if(setting.isValid()&&std::isfinite(setting.toDouble())&&setting.toDouble()<=0)return false;
        }
        return true;
    }
    bool canAnimate() const {
        return hasVisibleDecoration() && owner && owner->isVisible() && !owner->window()->isMinimized() &&
               (activity==Activity::VisibleOverlay||owner->window()->isActiveWindow()) && !reducedMotion();
    }
    void sync() {
        const bool wasRunning=timer.isActive();
        if (canAnimate()) {
            if (!timer.isActive()) { clock.start(); timer.start(); }
        } else timer.stop();
        if (owner&&wasRunning!=timer.isActive()) owner->update();
    }
    QPointer<QWidget> owner;
    Activity activity;
    QTimer timer;
    QElapsedTimer clock;
    qreal seconds = .9;
    int frames = 0;
    bool enabled=true;
    bool syncQueued=false;
    QRegion repaintRegion;
};
}
