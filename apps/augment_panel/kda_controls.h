#pragma once
#include "kda_theme.h"
#include "kda_surface.h"
#include "kda_sparkles.h"
#include "kda_motion.h"
#include "kda_glass_surface.h"
#include "hud_symbols.h"
#include <QPushButton>
#include <QPainter>
#include <QPainterPath>
#include <QFontMetrics>
#include <QComboBox>
#include <QAbstractItemView>
#include <QLineEdit>
#include <QSlider>
#include <QStyleOptionComboBox>
#include <QStyleOptionSlider>
#include <QKeyEvent>
#include <QFocusEvent>
#include <optional>

// Keep Qt's keyboard, focus and accessibility behavior; own the visible surface.
namespace Kda {
inline QPainterPath facet(const QRectF &r,qreal cut=6){
    QPainterPath p;p.moveTo(r.left()+2,r.top());p.lineTo(r.right()-cut,r.top());p.lineTo(r.right(),r.top()+cut);
    p.lineTo(r.right(),r.bottom()-2);p.quadTo(r.right(),r.bottom(),r.right()-2,r.bottom());
    p.lineTo(r.left()+cut,r.bottom());p.lineTo(r.left(),r.bottom()-cut);p.lineTo(r.left(),r.top()+2);p.quadTo(r.left(),r.top(),r.left()+2,r.top());p.closeSubpath();return p;
}
class Button : public QPushButton {
public:
    explicit Button(const QString &text={},QWidget *parent=nullptr):QPushButton(text,parent),sparkleMotion(this,620),hoverMotion(this),pressMotion(this,140),selectionMotion(this,260){
        setCursor(Qt::PointingHandCursor);setFocusPolicy(Qt::StrongFocus);setMinimumHeight(Density::controlHeight);setFont(Kda::font(FontRole::Label,13,QFont::DemiBold));
        setProperty("kdaGlassButton",true);GlassButtonDetail::schedule();
        connect(this,&QAbstractButton::pressed,this,[this]{pressMotion.retarget(1);});
        connect(this,&QAbstractButton::released,this,[this]{pressMotion.retarget(0);});
        connect(this,&QAbstractButton::toggled,this,[this](bool on){selectionMotion.retarget(on?1:0);});
    }
    QJsonObject easingReport()const{return {{"hover",hoverMotion.report()},{"press",pressMotion.report()},{"selection",selectionMotion.report()}};}
    QJsonObject glassReport()const{return {{"enabled",glassEnabled()},{"eligible",GlassButtonDetail::eligible(this)},{"shared_surface",glassEnabled()&&(property("toolbarAction").toBool()||objectName()=="quiet")&&GlassButtonDetail::sharedSurface(this)!=nullptr},{"shared_surface_paint_count",qint64(sharedSurfacePaints)},{"last_paint_applied",glassPaintedLast},{"paint_count",qint64(glassPaints)},{"cache_ready",GlassButtonDetail::cache().ready()},{"native_text_last",true},{"geometry_unchanged",true},{"cache",glassButtonReport()}};}
    void setSymbol(HudSymbols::Symbol value){glyph=value;updateGeometry();update();}
    void setSymbolOnly(bool value=true){glyphOnly=value;setProperty("symbolOnly",value);updateGeometry();update();}
    bool isSymbolOnly()const{return glyphOnly;}
    bool hasSymbol()const{return glyph.has_value();}
    void setNavigationCoverage(qreal value){if(qAbs(navigationCoverage-value)>.001){navigationCoverage=value;update();}}
    QSize sizeHint() const override {
        // QSS may assign a smaller font than our custom painter uses.
        // Measure the actual painted face so ordinary actions never truncate.
        if(glyph&&glyphOnly)return QSize(28,minimumHeight());
        return QPushButton::sizeHint().expandedTo(QSize(QFontMetrics(labelFont()).horizontalAdvance(text())+(glyph?42:20),minimumHeight()));
    }
    QSize minimumSizeHint() const override {return sizeHint();}
private:
    Kda::Transition sparkleMotion;
    Kda::MotionValue hoverMotion,pressMotion,selectionMotion;
    qreal navigationCoverage=0;
    std::optional<HudSymbols::Symbol> glyph;
    bool glyphOnly=false;
    bool keyboardFocus=false;
    bool glassPaintedLast=false;quint64 glassPaints=0,sharedSurfacePaints=0;
    QFont labelFont() const {
        const int requested=property("labelPixels").toInt();
        auto face=Kda::font(FontRole::Label,requested>=11&&requested<=16?requested:13,QFont::Medium);
        if(property("labelRegular").toBool())face.setWeight(QFont::Normal);
        return face;
    }
protected:
    bool event(QEvent *event) override {
        const auto type=event->type();const bool accepted=QPushButton::event(event);
        // QObject members send child events while they are being destroyed.
        // Only input and state changes may inspect the animation members.
        if(type!=QEvent::Enter&&type!=QEvent::Leave&&type!=QEvent::FocusIn&&type!=QEvent::FocusOut
            &&type!=QEvent::MouseButtonPress&&type!=QEvent::KeyPress
            &&type!=QEvent::DynamicPropertyChange&&type!=QEvent::EnabledChange)return accepted;
        if(type==QEvent::FocusIn)keyboardFocus=static_cast<QFocusEvent*>(event)->reason()!=Qt::MouseFocusReason;
        if(type==QEvent::FocusOut)keyboardFocus=false;
        if(type==QEvent::Enter)hoverMotion.retarget(1);
        if(type==QEvent::Leave)hoverMotion.retarget(keyboardFocus?1:0);
        if(type==QEvent::FocusIn||type==QEvent::FocusOut)hoverMotion.retarget(underMouse()||keyboardFocus?1:0);
        const bool quiet=property("toolbarAction").toBool()||objectName()=="quiet";
        const bool ornament=property("primaryAction").toBool()||(!quiet&&!property("dashboardNavigation").toBool());
        if(!ornament&&sparkleMotion.running())sparkleMotion.finish();
        if(ornament&&isEnabled()&&(type==QEvent::Enter||type==QEvent::FocusIn||type==QEvent::MouseButtonPress||
           (type==QEvent::KeyPress&&static_cast<QKeyEvent*>(event)->key()==Qt::Key_Space))){sparkleMotion.finish();sparkleMotion.trigger();}
        return accepted;
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);configurePainter(p);if(!isEnabled())p.setOpacity(.55);
        const bool navigation=property("dashboardNavigation").toBool(),quiet=property("toolbarAction").toBool()||objectName()=="quiet";
        const bool sharedPlate=quiet&&glassEnabled()&&GlassButtonDetail::sharedSurface(this)!=nullptr;
        const bool primary=property("primaryAction").toBool();
        const bool selected=isChecked(),filter=isCheckable()&&!navigation;
        const bool active=selected||primary;
        const bool red=skin()==QStringLiteral("kda_red");
        const qreal hover=hoverMotion.value(),press=pressMotion.value(),selection=primary?1:selectionMotion.value();
        const qreal inset=.5+press*.8;
        const QRectF r=QRectF(rect()).adjusted(inset,inset,-inset,-inset);QPainterPath shape;shape.addRoundedRect(r,radius("control"),radius("control"));
        // Material can depress; glyph baselines must stay on their original
        // device grid throughout the transition.
        p.save();p.translate(0,press*.7);
        if(sharedPlate)++sharedSurfacePaints;
        glassPaintedLast=!sharedPlate&&paintGlassButtonBackground(p,this,shape,r);if(glassPaintedLast){
            ++glassPaints;const qreal chosen=quiet?0:selection;
            auto top=red?mixColor(QColor("#291923"),QColor("#a52b46"),chosen):mixColor(mixColor(QColor("#22232e"),QColor("#424759"),hover*.35),QColor("#e3e7ef"),chosen);
            auto bottom=red?mixColor(QColor("#160f16"),QColor("#65192d"),chosen):mixColor(QColor("#151720"),QColor("#c9d0dc"),chosen);
            const int tint=qRound((quiet?88:120)+chosen*112);top.setAlpha(tint);bottom.setAlpha(std::min(244,tint+12));
            QLinearGradient frost(r.topLeft(),r.bottomRight());frost.setColorAt(0,top);frost.setColorAt(1,bottom);p.fillPath(shape,frost);
        }
        if(!navigation&&!quiet){
            QLinearGradient foil(r.topLeft(),r.bottomRight());
            foil.setColorAt(0,mixColor(mixColor(QColor("#222229"),QColor("#3b3b4a"),hover),QColor("#e7e9ef"),selection));
            foil.setColorAt(.5,mixColor(mixColor(QColor("#1c1c24"),QColor("#292b37"),hover),QColor("#d9dce6"),selection));
            foil.setColorAt(1,mixColor(QColor("#18181f"),QColor("#c5cad7"),selection));
            if(red){
                foil.setColorAt(0,mixColor(mixColor(color("raised"),QColor("#48303a"),hover),QColor("#b72a45"),selection));
                foil.setColorAt(.5,mixColor(color("surface"),QColor("#8c1f37"),selection));
                foil.setColorAt(1,mixColor(QColor("#160e13"),QColor("#64182b"),selection));
            }
            if(!glassPaintedLast)p.fillPath(shape,foil);
            if(glassPaintedLast)paintMetallicFinish(p,shape,1.,this);
            p.setPen(QPen(mixColor(mixColor(color("border"),color("ice"),hover*.7),red?color("ice"):QColor("#b8bdcc"),selection),.8));p.setBrush(Qt::NoBrush);p.drawPath(shape);
            auto bevel=color("silver");bevel.setAlpha(qRound(18+hover*16+selection*12));p.setPen(QPen(bevel,.6));p.drawLine(QPointF(r.left()+5,r.top()+.8),QPointF(r.right()-5,r.top()+.8));
            if(red&&(active||hover>.01)){
                p.save();p.setClipPath(shape);const qreal heat=std::max(selection,hover*.6);
                QLinearGradient edge(r.bottomLeft(),r.topRight());edge.setColorAt(0,QColor(207,35,37,qRound(heat*80)));edge.setColorAt(.6,QColor(255,128,51,qRound(heat*150)));edge.setColorAt(.86,QColor(255,221,160,qRound(heat*210)));edge.setColorAt(1,QColor(255,73,43,qRound(heat*100)));
                p.setPen(QPen(edge,1.35));p.drawPath(shape);p.setPen(QPen(edge,.65));p.drawLine(QPointF(r.left()+5,r.top()+.8),QPointF(r.right()-5,r.top()+.8));p.restore();
            }
            if(filter&&selected){
                p.setPen(QPen(red?color("silver"):QColor("#8a839d"),1));p.drawLine(QPointF(9,height()-2.5),QPointF(width()-9,height()-2.5));
            }
        }
        else if(quiet&&selected){
            if(sharedPlate){
                auto top=color("silver"),bottom=color("silver");top.setAlpha(qRound(34+hover*10));bottom.setAlpha(8);
                QLinearGradient state(r.topLeft(),r.bottomLeft());state.setColorAt(0,top);state.setColorAt(1,bottom);p.fillPath(shape,state);
            }else p.fillPath(shape,QColor(218,218,235,qRound(24+hover*12)));
            p.setPen(QPen(color("silver"),1));p.drawLine(QPointF(8,height()-2.5),QPointF(width()-8,height()-2.5));
        }
        else if(!active){p.fillPath(shape,QColor(218,218,235,qRound(hover*28)));}
        if(quiet&&glassPaintedLast)paintMetallicFinish(p,shape,1.,this);
        if(press>0)p.fillPath(shape,QColor(0,0,0,qRound(press*45)));
        // Keep the label area clear; shimmer travels across the upper bevel.
        if(sparkleMotion.running()){
            p.save();p.setClipPath(shape);const qreal phase=sparkleMotion.progress(),fade=std::sin(phase*3.141592653589793);
            const qreal x=-20+phase*(width()+40);QLinearGradient sheen(x-18,0,x+18,0);
            auto highlight=red?color("silver"):QColor(211,235,255);highlight.setAlpha(qRound(fade*65));
            sheen.setColorAt(0,Qt::transparent);sheen.setColorAt(.5,highlight);sheen.setColorAt(1,Qt::transparent);
            p.fillRect(QRectF(3,1,width()-6,2.5),sheen);
            p.restore();
        }
        if(hasFocus()&&keyboardFocus){p.setPen(QPen(color("ice"),1));p.setBrush(Qt::NoBrush);p.drawRoundedRect(r.adjusted(1,1,-1,-1),radius("control"),radius("control"));}
        p.restore();
        auto f=labelFont();p.setFont(f);const auto ink=mixColor(mixColor(color("silver"),color("text"),hover),red&&!navigation?color("text"):QColor("#181820"),navigation?navigationCoverage:quiet?0:selection);p.setPen(ink);
        QRect labelArea=rect().adjusted(8,0,-8,0);
        if(glyph){
            constexpr int symbolSize=16;
            HudSymbols::draw(p,*glyph,QRectF(glyphOnly?(width()-symbolSize)*.5:8,(height()-symbolSize)*.5,symbolSize,symbolSize),ink);
            if(glyphOnly)return;
            labelArea.adjust(22,0,0,0);
        }
        p.drawText(labelArea,Qt::AlignCenter,QFontMetrics(f).elidedText(text(),Qt::ElideRight,labelArea.width()));
    }
};
class LineEdit : public QLineEdit {
public:
    explicit LineEdit(QWidget *parent=nullptr):QLineEdit(parent){setProperty("kdaGlassField",true);GlassButtonDetail::schedule();}
    explicit LineEdit(const QString &text,QWidget *parent=nullptr):LineEdit(parent){setText(text);}
    QJsonObject glassReport()const{return {{"enabled",glassEnabled()},{"eligible",GlassButtonDetail::eligible(this)},{"last_paint_applied",glassPaintedLast},{"native_foreground_last",true},{"geometry_unchanged",true},{"cache_ready",GlassButtonDetail::cache().ready()}};}
protected:
    void paintEvent(QPaintEvent *event)override{
        {QPainter painter(this);configurePainter(painter);if(!isEnabled())painter.setOpacity(.55);glassPaintedLast=paintGlassSurface(painter,this,QRectF(rect()),GlassSurfaceRole::Field);}
        QLineEdit::paintEvent(event);
    }
private:
    bool glassPaintedLast=false;
};
class ComboBox : public QComboBox {
public:
    explicit ComboBox(QWidget *parent=nullptr):QComboBox(parent),hoverMotion(this),openMotion(this,220){setAttribute(Qt::WA_Hover);setProperty("kdaGlassField",true);GlassButtonDetail::schedule();}
    bool currentLabelFits()const{QStyleOptionComboBox option;initStyleOption(&option);const auto field=style()->subControlRect(QStyle::CC_ComboBox,&option,QStyle::SC_ComboBoxEditField,this);return fontMetrics().horizontalAdvance(currentText())<=std::max(0,field.width()-4);}
    QJsonObject glassReport()const{return {{"enabled",glassEnabled()},{"eligible",GlassButtonDetail::eligible(this)},{"last_paint_applied",glassPaintedLast},{"native_foreground_last",true},{"native_complex_background_skipped",glassPaintedLast},{"geometry_unchanged",true},{"popup_visible",popupWindow&&popupWindow->isVisible()},{"popup_motion",openMotion.report()},{"cache_ready",GlassButtonDetail::cache().ready()}};}
    void showPopup()override{
        QComboBox::showPopup();
        auto popup=view()?view()->window():nullptr;
        if(popup==window())popup=nullptr;
        if(popupWindow!=popup){if(popupWindow)popupWindow->removeEventFilter(this);popupWindow=popup;if(popupWindow)popupWindow->installEventFilter(this);}
        openMotion.retarget(popupWindow&&popupWindow->isVisible()?1:0);
    }
    void hidePopup()override{QComboBox::hidePopup();openMotion.retarget(0);}
protected:
    bool eventFilter(QObject *object,QEvent *event)override{
        // Escape, outside clicks and native popup dismissal may hide Qt's
        // container without calling our hidePopup override.
        if(object==popupWindow.data()){
            if(event->type()==QEvent::Show)openMotion.retarget(1);
            else if(event->type()==QEvent::Hide||event->type()==QEvent::Destroy)openMotion.retarget(0);
        }
        return QComboBox::eventFilter(object,event);
    }
    bool event(QEvent *event)override{const auto type=event->type();const bool result=QComboBox::event(event);if(type==QEvent::Enter||type==QEvent::FocusIn)hoverMotion.retarget(1);if(type==QEvent::Leave||type==QEvent::FocusOut)hoverMotion.retarget(underMouse()||hasFocus()?1:0);return result;}
    void paintEvent(QPaintEvent *event)override{
        {QPainter painter(this);configurePainter(painter);if(!isEnabled())painter.setOpacity(.55);glassPaintedLast=paintGlassSurface(painter,this,QRectF(rect()),GlassSurfaceRole::Field);}
        // CC_ComboBox paints an editable control's native Base over the entire
        // frame, even though the owner's QSS requests a transparent background.
        // Keep Qt's label/icon/editor path; only replace that opaque backplate.
        if(!glassPaintedLast)QComboBox::paintEvent(event);
        QPainter p(this);configurePainter(p);
        if(glassPaintedLast){
            QStyleOptionComboBox option;initStyleOption(&option);
            option.palette.setBrush(QPalette::Base,Qt::NoBrush);
            if(currentIndex()<0&&!placeholderText().isEmpty()){
                option.palette.setBrush(QPalette::ButtonText,option.palette.placeholderText());option.currentText=placeholderText();
            }
            p.setPen(palette().color(QPalette::Text));style()->drawControl(QStyle::CE_ComboBoxLabel,&option,&p,this);
            const bool focused=hasFocus()||(lineEdit()&&lineEdit()->hasFocus());
            p.setPen(QPen(color(focused?"ice":"border"),1));p.setBrush(Qt::NoBrush);p.drawRoundedRect(QRectF(rect()).adjusted(.5,.5,-.5,-.5),radius("control"),radius("control"));
        }
        auto outline=color("ice");outline.setAlpha(qRound(hoverMotion.value()*150));p.setPen(QPen(outline,1));p.setBrush(Qt::NoBrush);p.drawRoundedRect(QRectF(rect()).adjusted(.7,.7,-.7,-.7),radius("control"),radius("control"));
        p.setPen(QPen(color(isEnabled()?"ice":"muted"),1.5));const qreal x=width()-15,y=height()/2.;p.translate(x,y);p.rotate(openMotion.value()*180);QPainterPath chevron;chevron.moveTo(-4,-2);chevron.lineTo(0,2);chevron.lineTo(4,-2);p.drawPath(chevron);
    }
private:MotionValue hoverMotion,openMotion;QPointer<QWidget> popupWindow;bool glassPaintedLast=false;
};
class Slider : public QSlider {
public:explicit Slider(Qt::Orientation o,QWidget *parent=nullptr):QSlider(o,parent),hoverMotion(this){setAttribute(Qt::WA_Hover);}
protected:
bool event(QEvent *event)override{const auto type=event->type();const bool result=QSlider::event(event);if(type==QEvent::Enter||type==QEvent::FocusIn)hoverMotion.retarget(1);if(type==QEvent::Leave||type==QEvent::FocusOut)hoverMotion.retarget(underMouse()||hasFocus()?1:0);return result;}
void paintEvent(QPaintEvent *)override{
    QStyleOptionSlider option;initStyleOption(&option);const auto handle=style()->subControlRect(QStyle::CC_Slider,&option,QStyle::SC_SliderHandle,this);const auto groove=style()->subControlRect(QStyle::CC_Slider,&option,QStyle::SC_SliderGroove,this);
    QPainter p(this);configurePainter(p);if(!isEnabled())p.setOpacity(.4);const qreal y=groove.center().y();p.setPen(QPen(color("border"),3));p.drawLine(QPointF(groove.left(),y),QPointF(groove.right(),y));QLinearGradient foil(groove.topLeft(),groove.topRight());foil.setColorAt(0,color("lilac"));foil.setColorAt(1,color("ice"));p.setPen(QPen(foil,3));p.drawLine(QPointF(groove.left(),y),QPointF(handle.center().x(),y));const auto center=handle.center();const qreal size=7+hoverMotion.value()*1.5;QPainterPath diamond;diamond.moveTo(center.x(),y-size);diamond.lineTo(center.x()+size,y);diamond.lineTo(center.x(),y+size);diamond.lineTo(center.x()-size,y);diamond.closeSubpath();p.setPen(QPen(color("lilac"),1));p.setBrush(mixColor(color("silver"),color("ice"),hoverMotion.value()*.5));p.drawPath(diamond);if(hasFocus()){p.setPen(QPen(color("ice"),1));p.setBrush(Qt::NoBrush);p.drawRoundedRect(QRectF(rect()).adjusted(1,1,-1,-1),2,2);}
}
private:MotionValue hoverMotion;
};
}
