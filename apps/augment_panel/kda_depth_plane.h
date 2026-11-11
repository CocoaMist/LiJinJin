#pragma once
#include "kda_theme.h"
#include <QAbstractAnimation>
#include <QApplication>
#include <QDynamicPropertyChangeEvent>
#include <QEasingCurve>
#include <QEvent>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLineF>
#include <QMargins>
#include <QMouseEvent>
#include <QPointer>
#include <QResizeEvent>
#include <QSet>
#include <QVector>
#include <QVariantAnimation>
#include <QWidget>
#include <QWidgetItem>
#include <algorithm>

namespace Kda {
enum class DepthLayer { Readout, Hud };
class DepthScene;
inline QJsonArray depthPlaneRect(const QRect &rect){return {rect.x(),rect.y(),rect.width(),rect.height()};}

// The layout owns this stationary host. Its native child moves inside a gutter;
// drawing, input coordinates, tooltips and accessibility retain the same widget.
class DepthPlane final : public QWidget {
    static QWidget *hostParent(QWidget *content,QWidget *parent){
        if(!content)qFatal("Kda::DepthPlane requires a content widget");
        return parent?parent:content->parentWidget();
    }
public:
    explicit DepthPlane(QWidget *content,DepthLayer layer,QWidget *parent=nullptr)
        :QWidget(hostParent(content,parent)),body(content),kind(layer),
          gutter(layer==DepthLayer::Readout?QMargins(9,5,9,5):QMargins(2,2,2,2)){
        setObjectName(QStringLiteral("depthPlane_")+content->objectName());setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_NoSystemBackground);setAutoFillBackground(false);
        const bool explicitlyHidden=content->isHidden()&&content->testAttribute(Qt::WA_WState_ExplicitShowHide);
        syncing=true;content->setParent(this);content->setVisible(!explicitlyHidden);syncing=false;
        content->installEventFilter(this);syncMetrics();
        if(explicitlyHidden)hide();
    }
    QWidget *contentWidget()const{return body;}
    DepthLayer layer()const{return kind;}
    QSize sizeHint()const override{return measured(false);}
    QSize minimumSizeHint()const override{return measured(true);}
    bool hasHeightForWidth()const override{return body&&QWidgetItem(body).hasHeightForWidth();}
    int heightForWidth(int width)const override{
        if(!body)return -1;QWidgetItem item(body);if(!item.hasHeightForWidth())return -1;
        return item.heightForWidth(std::max(0,width-gutter.left()-gutter.right()))+gutter.top()+gutter.bottom();
    }
    QJsonObject report()const{
        const QRect hostBox(mapTo(window(),QPoint()),size());
        const QRect contentBox=body?QRect(body->mapTo(window(),QPoint()),body->size()):QRect();
        return {{"name",body?body->objectName():QString()},{"layer",kind==DepthLayer::Readout?"midground":"foreground"},
            {"visible",body&&isVisible()&&body->isVisibleTo(this)},{"host_geometry",depthPlaneRect(hostBox)},
            {"content_geometry",body?depthPlaneRect(body->geometry()):QJsonArray{}},{"visual_geometry",depthPlaneRect(contentBox)},
            {"input_geometry",depthPlaneRect(contentBox)},{"offset",QJsonArray{applied.x(),applied.y()}},
            {"content_inside_host",body&&rect().contains(body->geometry())},{"native_child",true},{"scaled",false}};
    }
protected:
    void resizeEvent(QResizeEvent *event)override{QWidget::resizeEvent(event);placeContent();}
    bool event(QEvent *event)override{
        const bool result=QWidget::event(event);
        if(event->type()==QEvent::LayoutRequest&&!syncing)syncMetrics();
        return result;
    }
    bool eventFilter(QObject *object,QEvent *event)override{
        if(object==body&&!syncing){
            if(event->type()==QEvent::HideToParent||event->type()==QEvent::ShowToParent){
                if(event->type()==QEvent::ShowToParent)syncMetrics();
                syncing=true;setVisible(event->type()==QEvent::ShowToParent);syncing=false;syncMetrics();
            }else if(event->type()==QEvent::LayoutRequest||event->type()==QEvent::FontChange||event->type()==QEvent::StyleChange||event->type()==QEvent::ContentsRectChange)syncMetrics();
        }
        return QWidget::eventFilter(object,event);
    }
private:
    friend class DepthScene;
    QSize padded(QSize value)const{
        return {std::clamp(value.width(),0,QWIDGETSIZE_MAX-gutter.left()-gutter.right())+gutter.left()+gutter.right(),
            std::clamp(value.height(),0,QWIDGETSIZE_MAX-gutter.top()-gutter.bottom())+gutter.top()+gutter.bottom()};
    }
    QSize measured(bool minimum)const{
        if(!body)return {};QWidgetItem item(body);return padded(minimum?item.minimumSize():item.sizeHint());
    }
    void syncMetrics(){
        if(syncing||!body)return;syncing=true;
        QWidgetItem item(body);setSizePolicy(body->sizePolicy());setMinimumSize(padded(body->minimumSize()));setMaximumSize(padded(item.maximumSize()));
        syncing=false;updateGeometry();placeContent();
    }
    void setOffset(const QPoint &offset){if(applied==offset)return;applied=offset;placeContent();}
    void placeContent(){
        if(!body||placing)return;placing=true;
        const QRect available=contentsRect().marginsRemoved(gutter);QWidgetItem item(body);
        const QSize extent=available.size().expandedTo(item.minimumSize()).boundedTo(item.maximumSize());
        const QPoint origin=available.topLeft()+QPoint((available.width()-extent.width())/2,(available.height()-extent.height())/2);
        const QRect geometry(origin+applied,extent);if(body->geometry()!=geometry)body->setGeometry(geometry);
        placing=false;
    }
    QPointer<QWidget> body;DepthLayer kind;QMargins gutter;QPoint applied;
    bool syncing=false,placing=false;
};

// One finite animation supplies a common phase to every registered plane.
// Pointer handling observes normal Qt events and never consumes or forwards them.
class DepthScene final : public QObject {
public:
    explicit DepthScene(QWidget *owner):QObject(owner),host(owner),animation(this){
        if(!owner)qFatal("Kda::DepthScene requires an owner widget");
        animation.setDuration(180);animation.setStartValue(0.);animation.setEndValue(1.);animation.setEasingCurve(QEasingCurve::OutCubic);
        connect(&animation,&QVariantAnimation::valueChanged,this,[this](const QVariant &value){current=from+(target-from)*value.toReal();apply();});
        connect(&animation,&QVariantAnimation::finished,this,[this]{current=target;apply();});
        host->installEventFilter(this);if(qApp)qApp->installEventFilter(this);
    }
    ~DepthScene()override{if(host)host->removeEventFilter(this);if(qApp)qApp->removeEventFilter(this);}
    void add(DepthPlane *plane){if(!plane)return;if(plane->property("kdaDepthStationary").toBool()){plane->setOffset({});return;}for(const auto &existing:planes)if(existing==plane)return;planes.append(plane);plane->setOffset(pixelOffset(plane->layer()));}
    QPointF normalizedOffset()const{return current;}
    QPointF settledValue()const{return target;}
    QJsonObject report()const{
        QJsonArray registered;for(const auto &plane:planes)if(plane)registered.append(plane->report());
        int mapped=0;if(host)for(auto widget:host->findChildren<QWidget*>())if(widget->property("kdaDepthMapped").toBool())++mapped;
        const auto middle=pixelOffset(DepthLayer::Readout),front=pixelOffset(DepthLayer::Hud);
        return {{"enabled",depthEnabled()},{"running",animation.state()==QAbstractAnimation::Running},{"layer_count",3},
            {"layer_order",QJsonArray{"backdrop","midground","foreground"}},{"backdrop_offset",QJsonArray{0,0}},
            {"midground_offset",QJsonArray{middle.x(),middle.y()}},{"background_offset",QJsonArray{middle.x(),middle.y()}},
            {"foreground_offset",QJsonArray{front.x(),front.y()}},{"offset_limit",QJsonArray{8,4}},{"foreground_offset_limit",QJsonArray{1,1}},
            {"content_moves",true},{"interactive_layer","foreground"},{"input_geometry_follows_visuals",true},{"no_graphics_effect",true},
            {"idle_timer",false},{"duration_ms",180},{"reduced_motion",reducedMotion()},{"sublayer_content",true},{"mapped_widget_count",mapped},
            {"normalized_offset",QJsonArray{current.x(),current.y()}},{"motion_direction","same_as_pointer"},{"registered_planes",registered}};
    }
protected:
    bool eventFilter(QObject *object,QEvent *event)override{
        const auto type=event->type();
        // Releases must retire tracked keys even while the scene is disabled
        // or hidden. Geometry-only resets preserve keys that are still held.
        if(type==QEvent::KeyRelease){const auto key=static_cast<QKeyEvent*>(event);if(!key->isAutoRepeat())keys.remove(key->key());return false;}
        if(object==host&&(type==QEvent::Hide||type==QEvent::Close)){reset();return false;}
        if(object==host&&type==QEvent::Resize){reset(false);return false;}
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
        if(object==host&&type==QEvent::DevicePixelRatioChange){reset(false);return false;}
#endif
        if(object==qApp&&type==QEvent::ApplicationDeactivate){reset();return false;}
        if(object==qApp&&type==QEvent::DynamicPropertyChange){
            const auto property=static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName();
            if((property=="lixinReducedMotion"||property=="lixinDepthEnabled")&&(!depthEnabled()||reducedMotion()))reset();
        }
        if(!host||!host->isVisible()||!depthEnabled()||reducedMotion()){if(animation.state()==QAbstractAnimation::Running||!current.isNull()||!keys.isEmpty())reset();return false;}
        if(type==QEvent::KeyPress){keys.insert(static_cast<QKeyEvent*>(event)->key());freeze();return false;}
        if(QApplication::activePopupWidget()||QApplication::activeModalWidget()){freeze();return false;}
        if(!host->window()->isActiveWindow()){if(animation.state()==QAbstractAnimation::Running||!current.isNull()||!keys.isEmpty())reset();return false;}
        auto widget=qobject_cast<QWidget*>(object);if(!widget||widget->window()!=host->window())return false;
        if(type==QEvent::MouseButtonPress||type==QEvent::MouseButtonDblClick){freeze();return false;}
        if(type==QEvent::MouseMove||type==QEvent::MouseButtonRelease){
            const auto mouse=static_cast<QMouseEvent*>(event);
            if(mouse->buttons()!=Qt::NoButton||mouse->modifiers()!=Qt::NoModifier||QApplication::keyboardModifiers()!=Qt::NoModifier||!keys.isEmpty()){freeze();return false;}
            const auto point=host->mapFromGlobal(mouse->globalPosition().toPoint());
            retarget({std::clamp(point.x()/qreal(std::max(1,host->width()))*2-1,qreal(-1),qreal(1)),
                std::clamp(point.y()/qreal(std::max(1,host->height()))*2-1,qreal(-1),qreal(1))});
        }else if(object==host&&type==QEvent::Leave&&QApplication::mouseButtons()==Qt::NoButton&&QApplication::keyboardModifiers()==Qt::NoModifier&&keys.isEmpty())retarget({});
        return false;
    }
private:
    bool available()const{return host&&host->isVisible()&&host->window()->isActiveWindow()&&depthEnabled()&&!reducedMotion();}
    QPoint pixelOffset(DepthLayer layer)const{return layer==DepthLayer::Readout?QPoint(qRound(current.x()*8),qRound(current.y()*4)):QPoint(qRound(current.x()),qRound(current.y()));}
    void apply(){
        for(int i=planes.size()-1;i>=0;--i){if(!planes[i])planes.removeAt(i);else planes[i]->setOffset(pixelOffset(planes[i]->layer()));}
        if(!host)return;
        host->setProperty("lixinDepthOffset",current);
        // A stable view can paint several depths without moving its frame or hit targets.
        for(auto widget:host->findChildren<QWidget*>())if(widget->property("kdaDepthMapped").toBool()){
            widget->setProperty("lixinDepthOffset",current);widget->update();
        }
    }
    void freeze(){animation.stop();from=target=current;}
    void reset(bool clearKeys=true){animation.stop();if(clearKeys)keys.clear();current=QPointF();from=target=current;apply();}
    void retarget(const QPointF &next){
        if(!available()){reset();return;}
        if(QLineF(next,target).length()<.025)return;
        animation.stop();from=current;target=next;animation.start();
    }
    QPointer<QWidget> host;QVariantAnimation animation;QVector<QPointer<DepthPlane>> planes;
    QSet<int> keys;QPointF current,from,target;
};
}
