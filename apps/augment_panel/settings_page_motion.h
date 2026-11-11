#pragma once
#include "kda_theme.h"
#include "kda_reading_surface.h"
#include <QApplication>
#include <QDynamicPropertyChangeEvent>
#include <QEasingCurve>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLayout>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPointer>
#include <QRegion>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTabBar>
#include <QVariantAnimation>
#include <QWheelEvent>

namespace Kda {
class SettingsTabBar final : public QTabBar {
public:
    SettingsTabBar(){
        setShape(QTabBar::RoundedWest);
        setUsesScrollButtons(false);
        setFocusPolicy(Qt::StrongFocus);setMouseTracking(true);
    }
    bool userSelection()const{return selecting;}
    std::function<void()> onReselected;
    bool indicatorRunning()const{return indicatorProgress<1.;}
    qreal selectionProgress()const{return indicatorProgress;}
    QJsonArray selectionOpacities()const{
        QJsonArray opacities;
        for(int index=0;index<count();++index)opacities.append(selectionOpacity(index,currentIndex()));
        return opacities;
    }
    QJsonArray selectionDirtyTabs()const{
        QJsonArray indices;
        if(indicatorRunning())for(const int index:dirtyTabs)indices.append(index);
        return indices;
    }
    void beginSelection(int previous){
        // currentIndex() is already the new destination. Preserve the visible
        // blend toward the old destination before starting the new one.
        QList<qreal> opacities;
        opacities.reserve(count());
        dirtyTabs.clear();
        selectionDamage=QRegion();
        for(int index=0;index<count();++index){
            const auto opacity=selectionOpacity(index,previous);
            opacities.append(opacity);
            if(isTabVisible(index)&&opacity!=(index==currentIndex()?1.:0.)){
                dirtyTabs.append(index);
                selectionDamage|=tabRect(index);
            }
        }
        initialOpacities=std::move(opacities);
        indicatorProgress=0.;
        update(selectionDamage);
    }
    void setSelectionProgress(qreal progress){
        if(indicatorProgress==progress)return;
        indicatorProgress=progress;
        update(selectionDamage);
    }
    void finishIndicator(){
        if(!indicatorRunning())return;
        setSelectionProgress(1.);
        update(tabRect(currentIndex()));
    }
protected:
    void mousePressEvent(QMouseEvent *event)override{
        const int previous=currentIndex(),clicked=tabAt(event->position().toPoint());
        QScopedValueRollback<bool> guard(selecting,true);QTabBar::mousePressEvent(event);
        if(event->button()==Qt::LeftButton&&clicked==previous&&clicked==currentIndex()&&onReselected)onReselected();
    }
    void mouseReleaseEvent(QMouseEvent *event)override{
        QScopedValueRollback<bool> guard(selecting,true);QTabBar::mouseReleaseEvent(event);
    }
    void keyPressEvent(QKeyEvent *event)override{
        keyboardFocus=true;update();
        QScopedValueRollback<bool> guard(selecting,true);
        if(event->key()==Qt::Key_Up||event->key()==Qt::Key_Down){
            const int direction=event->key()==Qt::Key_Up?-1:1;
            for(int index=currentIndex()+direction;index>=0&&index<count();index+=direction){
                if(isTabEnabled(index)&&isTabVisible(index)){setCurrentIndex(index);break;}
            }
            event->accept();
            return;
        }
        QTabBar::keyPressEvent(event);
    }
    void wheelEvent(QWheelEvent *event)override{QScopedValueRollback<bool> guard(selecting,true);QTabBar::wheelEvent(event);}
    void mouseMoveEvent(QMouseEvent *event)override{
        const int next=tabAt(event->position().toPoint());
        if(next!=hovered){update(tabRect(hovered));hovered=next;update(tabRect(hovered));}
        QTabBar::mouseMoveEvent(event);
    }
    void leaveEvent(QEvent *event)override{update(tabRect(hovered));hovered=-1;QTabBar::leaveEvent(event);}
    void focusInEvent(QFocusEvent *event)override{keyboardFocus=event->reason()!=Qt::MouseFocusReason;QTabBar::focusInEvent(event);update();}
    QSize tabSizeHint(int)const override{return {76,38};}
    QSize minimumTabSizeHint(int index)const override{return tabSizeHint(index);}
    void paintEvent(QPaintEvent *event)override{
        QPainter painter(this);configurePainter(painter);
        const auto accent=color("ice");
        for(int index=0;index<count();++index){
            if(!isTabVisible(index)||!event->region().intersects(tabRect(index)))continue;
            const bool selected=index==currentIndex();
            const auto bounds=QRectF(tabRect(index));
            if(index==hovered&&!selected){auto fill=accent;fill.setAlpha(9);painter.setPen(Qt::NoPen);painter.setBrush(fill);painter.drawRoundedRect(bounds.adjusted(1,2,-1,-2),4,4);}
            const auto opacity=selectionOpacity(index,currentIndex());
            if(opacity>0.){
                const auto plate=bounds.adjusted(1,2,-1,-2);
                painter.save();painter.setOpacity(opacity);
                auto top=accent,bottom=accent;top.setAlpha(28);bottom.setAlpha(8);
                QLinearGradient fill(plate.topLeft(),plate.bottomRight());fill.setColorAt(0,top);fill.setColorAt(1,bottom);
                auto edge=accent;edge.setAlpha(38);painter.setPen(QPen(edge,.7));painter.setBrush(fill);painter.drawRoundedRect(plate,4,4);
                auto marker=accent;marker.setAlpha(210);painter.setPen(Qt::NoPen);painter.setBrush(marker);
                painter.drawRoundedRect(QRectF(plate.left(),plate.top()+9,1.5,plate.height()-18),.75,.75);
                painter.restore();
            }
            painter.setFont(Kda::font(FontRole::Body,13,selected?QFont::Medium:QFont::Normal));
            painter.setPen(color(selected?"text":"muted"));
            painter.drawText(bounds.adjusted(5,0,-5,0),Qt::AlignCenter,tabText(index));
            if(selected&&hasFocus()&&keyboardFocus){
                auto focus=color("silver");focus.setAlpha(140);
                painter.setPen(QPen(focus,.8));painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(bounds.adjusted(4,4,-4,-4),3,3);
            }
        }
    }
private:
    qreal selectionOpacity(int index,int target)const{
        const qreal selected=index==target?1.:0.;
        return initialOpacities.value(index)*(1.-indicatorProgress)+selected*indicatorProgress;
    }
    QList<qreal> initialOpacities;
    QList<int> dirtyTabs;
    QRegion selectionDamage;
    qreal indicatorProgress=1.;bool selecting=false,keyboardFocus=false;int hovered=-1;
};

// Fade a matching material veil instead of rasterising live text or attaching
// a graphics effect. The destination's native controls stay on their pixel grid.
class SettingsRevealVeil final : public QWidget {
public:
    explicit SettingsRevealVeil(QWidget *parent):QWidget(parent){setObjectName(QStringLiteral("settingsRevealVeil"));setAttribute(Qt::WA_TransparentForMouseEvents);setFocusPolicy(Qt::NoFocus);hide();}
    void place(const QRect &page,const QRect &visible){
        if(pageBounds==page&&geometry()==visible)return;
        pageBounds=page;
        plate={};
        setGeometry(visible);
        update();
    }
    void setProgress(qreal value){if(progress!=value){progress=value;update();}}
    int surfaceArea()const{return plate.isNull()?0:width()*height();}
protected:
    void paintEvent(QPaintEvent *)override{
        if(plate.isNull()||plate.devicePixelRatio()!=devicePixelRatioF()){
            plate=QImage(QSize(qCeil(width()*devicePixelRatioF()),qCeil(height()*devicePixelRatioF())),QImage::Format_ARGB32_Premultiplied);plate.setDevicePixelRatio(devicePixelRatioF());plate.fill(Qt::transparent);
            QPainter material(&plate);configurePainter(material);
            // Paint in page coordinates so a clipped veil keeps the same
            // gradient, material alignment and edge as the underlying page.
            material.translate(-geometry().topLeft());
            QPainterPath shape;shape.addRoundedRect(QRectF(pageBounds).adjusted(.5,.5,-.5,-.5),radius("section"),radius("section"));
            material.fillPath(shape,color("bg"));paintReadingSurface(material,QRectF(pageBounds),parentWidget());
        }
        QPainter painter(this);painter.setOpacity(1.-progress);
        painter.drawImage(QPointF(),plate);
    }
private:
    qreal progress=0.;QImage plate;
    QRect pageBounds;
};

// A finite material dissolve. Native text and hit targets remain stationary;
// only the cached blank material veil changes opacity.
class SettingsPageMotion final : public QWidget {
public:
    SettingsPageMotion(SettingsTabBar *bar,QStackedWidget *pages,QScrollArea *area)
        :QWidget(area->viewport()),tabs(bar),stack(pages),scroll(area),animation(this){
        setObjectName(QStringLiteral("settingsPageMotion"));setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_TransparentForMouseEvents);hide();
        animation.setDuration(280);animation.setStartValue(0.);animation.setEndValue(1.);animation.setEasingCurve(QEasingCurve::OutCubic);
        connect(&animation,&QVariantAnimation::valueChanged,this,[this](const QVariant &value){
            if(reducedMotion()){finish();return;}phase=value.toReal();++frames;
            if(tabs)tabs->setSelectionProgress(phase);
            if(veil)veil->setProgress(phase);
        });
        connect(&animation,&QVariantAnimation::finished,this,[this]{settlePage();});
        connect(tabs,&QTabBar::currentChanged,this,[this](int index){select(index);});
        tabs->onReselected=[this]{const int about=aboutIndex();if(stack&&about>=0&&stack->currentIndex()==about)select(about,true);};
        // Range clamping while changing page height is layout work, not a
        // user interruption. Wheel/key/scrollbar input still settles motion.
        connect(scroll->verticalScrollBar(),&QScrollBar::sliderPressed,this,[this]{finish();});
        connect(scroll->verticalScrollBar(),&QScrollBar::valueChanged,this,[this]{queuePlacement();});
        parentWidget()->installEventFilter(this);stack->installEventFilter(this);window()->installEventFilter(this);
        if(qApp)qApp->installEventFilter(this);
    }
    ~SettingsPageMotion()override{if(tabs)tabs->onReselected={};if(qApp)qApp->removeEventFilter(this);}
    std::function<void(bool)> aboutEntrance;
    std::function<QJsonObject()> aboutReport;
    void replayAboutEntrance(){
        const int about=aboutIndex();if(!stack||!tabs||!scroll||about<0||stack->currentIndex()!=about||tabs->currentIndex()!=about||!stack->currentWidget()
            ||!stack->currentWidget()->isVisibleTo(window())||!scroll->isVisible()||!window()->isActiveWindow()
            ||window()->isMinimized()||reducedMotion())return;
        finish();fromIndex=toIndex=about;++starts;if(aboutEntrance)aboutEntrance(true);
    }
    void finish(bool settleIndicator=true){
        settlePage(settleIndicator);
        if(aboutEntrance)aboutEntrance(false);
    }
    QJsonObject report()const{
        const auto about=aboutReport?aboutReport():QJsonObject{};const bool aboutRunning=about.value("running").toBool();
        return {{"running",animation.state()==QAbstractAnimation::Running||aboutRunning},{"page_running",animation.state()==QAbstractAnimation::Running},{"indicator_running",tabs&&tabs->indicatorRunning()},
            {"indicator_progress",tabs?tabs->selectionProgress():1.},{"indicator_opacities",tabs?tabs->selectionOpacities():QJsonArray{}},
            {"indicator_dirty_tabs",tabs?tabs->selectionDirtyTabs():QJsonArray{}},{"veil_surface_area",veil&&veil->isVisible()?veil->surfaceArea():0},
            {"duration_ms",toIndex==aboutIndex()?about.value("duration_ms").toInt():280},{"progress",aboutRunning?about.value("progress").toDouble():phase},{"starts",starts},{"frames",frames},{"from_index",fromIndex},{"to_index",toIndex},
            {"about_entrance",about},
            {"effect","stationary_material_dissolve"},{"content_reveal",phase},{"veil_visible",veil&&veil->isVisible()},{"veil_intercepts_input",false},
            {"offset_x",moving&&stack?moving->x()-stack->contentsRect().x():0},{"offset_y",moving&&stack?moving->y()-stack->contentsRect().y():0},{"axis","vertical"},{"direction",direction},{"distance_px",0},{"snapshot_count",0},{"capture_ms",0},
            {"reduced_motion",reducedMotion()},{"geometry_animated",false},{"text_pixel_grid_stable",true},{"blank_material_cached",true},{"input_geometry_follows_visuals",true},
            {"native_page_immediate",true},{"loops",1}};
    }
protected:
    bool eventFilter(QObject *object,QEvent *event)override{
        const auto type=event->type();
        const bool surface=object==parentWidget()||object==stack;
        if((surface||object==window())&&(type==QEvent::Hide||type==QEvent::Close))finish();
        // The selected page changes the stack height and scrollbar range.
        // Rebase after those internal layouts without cancelling its entrance.
        // Real window resize and loss of activation still settle immediately.
        if(object==window()&&(type==QEvent::Resize||type==QEvent::WindowDeactivate))finish();
        else if(surface&&(type==QEvent::Resize||type==QEvent::LayoutRequest))queuePlacement();
        if(object==qApp&&type==QEvent::ApplicationDeactivate)finish();
        if(object==qApp&&type==QEvent::DynamicPropertyChange){
            const auto name=static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName();
            if((name=="lixinReducedMotion"&&reducedMotion())||name=="lixinSkin"||name=="lixinGlassEnabled")finish();
        }
        if(type==QEvent::KeyPress||type==QEvent::Wheel){
            const auto widget=qobject_cast<QWidget*>(object);if(stack&&widget&&(widget==stack||stack->isAncestorOf(widget)||(scroll&&(widget==scroll||scroll->isAncestorOf(widget)))))finish();
        }
        if(type==QEvent::MouseButtonPress&&scroll){const auto widget=qobject_cast<QWidget*>(object);if(widget&&(widget==scroll->verticalScrollBar()||(stack&&(widget==stack||stack->isAncestorOf(widget)))))finish();}
        return QWidget::eventFilter(object,event);
    }
private:
    int aboutIndex()const{if(stack)for(int index=0;index<stack->count();++index)if(stack->widget(index)->objectName()==QStringLiteral("versionAboutPage"))return index;return -1;}
    void settlePage(bool settleIndicator=true){
        animation.stop();phase=1.;if(veil)veil->hide();moving=nullptr;
        if(settleIndicator&&tabs)tabs->finishIndicator();
    }
    void queuePlacement(){
        if(placementQueued||animation.state()!=QAbstractAnimation::Running)return;
        placementQueued=true;QMetaObject::invokeMethod(this,[this]{
            placementQueued=false;
            if(stack&&moving&&moving==stack->currentWidget()&&animation.state()==QAbstractAnimation::Running)placeVeil();
        },Qt::QueuedConnection);
    }
    void placeVeil(){
        if(!moving||!veil||!scroll)return;
        const auto viewport=scroll->viewport();
        const QRect visible(moving->mapFrom(viewport,QPoint()),viewport->size());
        veil->place(moving->rect(),visible.intersected(moving->rect()));
    }
    void select(int index,bool replay=false){
        if(!stack||!tabs||!scroll||index<0||index>=stack->count()||(!replay&&index==stack->currentIndex()))return;
        if(replay){replayAboutEntrance();return;}
        const int previous=stack->currentIndex();
        const bool animate=tabs->userSelection()&&scroll->isVisible()&&window()->isActiveWindow()&&!reducedMotion();
        finish(false);stack->setCurrentIndex(index);scroll->verticalScrollBar()->setValue(0);
        if(stack->layout())stack->layout()->activate();
        fromIndex=previous;toIndex=index;direction=index>previous?1:-1;
        if(!animate){tabs->finishIndicator();return;}
        if(index==aboutIndex()&&aboutEntrance)aboutEntrance(true);
        // About's own entrance can synchronously relayout its parent. Capture
        // the native resting geometry only after that layout has settled.
        if(stack->layout())stack->layout()->activate();
        moving=stack->currentWidget();phase=0.;++starts;
        // A new blank plate also picks up a changed skin or destination geometry.
        if(veil)delete veil;veil=new SettingsRevealVeil(moving);
        placeVeil();veil->show();
        // Showing/reparenting the veil polishes the destination and can invoke
        // its stacked layout. Apply the entrance after that synchronous work.
        if(stack->layout())stack->layout()->activate();
        placeVeil();tabs->beginSelection(previous);animation.start();
    }
    QPointer<SettingsTabBar> tabs;QPointer<QStackedWidget> stack;QPointer<QScrollArea> scroll;QPointer<QWidget> moving;
    QPointer<SettingsRevealVeil> veil;
    QVariantAnimation animation;qreal phase=1.;int starts=0,frames=0,fromIndex=0,toIndex=0,direction=1;bool placementQueued=false;
};
}
