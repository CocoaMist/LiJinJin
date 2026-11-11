#include "overlay_window.h"
#include "overlay_layout.h"
#include "kda_theme.h"
#include "kda_controls.h"
#include "kda_surface.h"
#include "kda_hud_style.h"
#include "kda_notice.h"
#include "kda_glass_surface.h"
#include "hud_symbol_button.h"
#include <QCloseEvent>
#include <QFrame>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QMenu>
#include <QLinearGradient>
#include <QPainter>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QResizeEvent>
#include <QShowEvent>
#include <QHideEvent>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QDynamicPropertyChangeEvent>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
constexpr int titlebarHeight=OverlayMetrics::titlebarHeight;
constexpr int foldedHeight=OverlayMetrics::foldedHeight;
constexpr int preferredWidth=232;
constexpr int enterMs=180;
constexpr int exitMs=120;
class DragBar : public QWidget {
public:using QWidget::QWidget;
protected:
void paintEvent(QPaintEvent *)override{
    Kda::OverlayCanvas canvas(this);auto &painter=canvas.paint();
    painter.setPen(QPen(Kda::HudStyle::neutral(30),1));
    painter.drawLine(QPointF(8,height()-.5),QPointF(width()-8,height()-.5));
}
void mousePressEvent(QMouseEvent *e) override{
    if(e->button()==Qt::LeftButton&&window()->windowHandle()){window()->windowHandle()->startSystemMove();e->accept();}
    else QWidget::mousePressEvent(e);
}};
class StageShell : public QFrame {
public:
    StageShell():flow(this,Kda::SparkleField::Activity::VisibleOverlay){Kda::markSharedGlassSurface(this);}
    QJsonObject motionReport()const{
        return {{"effect","localized_kda_edge_reflection"},{"skin_finish",Kda::HudStyle::identity()},{"skin_strength",Kda::HudStyle::strength()},
                {"content_visible_immediately",true},{"window_geometry_animated",false},{"idle_animation",flow.report().value("running")},{"flow",flow.report()}};
    }
protected:
void paintEvent(QPaintEvent *)override{
    Kda::OverlayCanvas canvas(this);auto &p=canvas.paint();const auto bounds=QRectF(rect()).adjusted(.7,.7,-.7,-.7);
    Kda::HudStyle::paintSurface(p,bounds,true);
    flow.paintFlow(p,bounds,Kda::HudStyle::strength());
}
private:
    Kda::SparkleField flow;
};
}
OverlayWindow::OverlayWindow(const QString &title,const QString &key,QSettings &prefs,bool inspection)
    :settings(prefs),prefix(key),inspectionMode(inspection),visibilityMotion(this){
    connect(&visibilityMotion,&QVariantAnimation::valueChanged,this,[this](const QVariant &value){reveal=value.toReal();++visibilityFrames;applyVisibilityOpacity();});
    connect(&visibilityMotion,&QVariantAnimation::finished,this,[this]{finishVisibility();});
    if(qApp)qApp->installEventFilter(this);
    Kda::HudStyle::setStrength(settings.value("appearance/hudSkinStrength",.65).toDouble());
    Kda::installNoticeHost(this,0);
    setFont(Kda::font(Kda::FontRole::Body,11));Kda::bindThemeStyle(this,[]{return Kda::overlayStyleSheet();});
    Kda::setReducedMotion(settings.value("appearance/reducedMotion",false).toBool());
    setWindowTitle(QStringLiteral("李金金 · ")+title);
    setAttribute(Qt::WA_TranslucentBackground);setAttribute(Qt::WA_ShowWithoutActivating,!inspection);
    setWindowFlags((inspection?Qt::Window:Qt::Tool)|Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint
                   |(inspection?Qt::WindowFlags{}:Qt::WindowDoesNotAcceptFocus));
    Kda::attachOverlayBackdrop(this);
    setWindowOpacity(std::clamp(settings.value(prefix+"/opacity",.88).toDouble(),.45,1.));
    setMinimumSize(OverlayMetrics::minimumWidth,foldedHeight);resize(preferredWidth,expandedHeight);
    auto outer=new QVBoxLayout(this);outer->setContentsMargins(1,1,1,1);outer->setSpacing(0);
    // A page's minimum hint must never enlarge the game overlay past its lane.
    outer->setSizeConstraint(QLayout::SetNoConstraint);
    auto shell=new StageShell;shell->setObjectName("shell");outer->addWidget(shell);
    auto layout=new QVBoxLayout(shell);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);layout->setSizeConstraint(QLayout::SetNoConstraint);
    auto titlebar=new DragBar;titlebar->setObjectName("titlebar");titlebar->setFixedHeight(titlebarHeight);
    auto row=new QHBoxLayout(titlebar);row->setContentsMargins(10,0,6,0);row->setSpacing(0);
    heading=new QLabel;heading->setObjectName("brand");heading->setFont(Kda::hudFont(Kda::FontRole::Body,10));heading->setStyleSheet(QStringLiteral("color:#B7ABC8;"));heading->setMinimumWidth(0);heading->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);heading->setAttribute(Qt::WA_TransparentForMouseEvents);row->addWidget(heading,1);setHeading(title);
    auto fold=new HudSymbolButton(HudSymbols::Symbol::Collapse,QStringLiteral("折叠 / 展开"));fold->setFixedSize(26,28);fold->setObjectName("overlayFold");
    connect(fold,&QAbstractButton::clicked,this,[this]{setCollapsed(!folded);});row->addWidget(fold);
    auto customize=new HudSymbolButton(HudSymbols::Symbol::More,QStringLiteral("透明度、鼠标穿透与归位"));customize->setFixedSize(26,28);customize->setObjectName("overlayOptions");
    connect(customize,&QAbstractButton::clicked,this,[this,customize]{menu(customize);});row->addWidget(customize);
    auto close=new HudSymbolButton(HudSymbols::Symbol::Close,QStringLiteral("关闭助手浮窗"));close->setFixedSize(26,28);close->setObjectName("overlayClose");
    connect(close,&QAbstractButton::clicked,this,&QWidget::close);row->addWidget(close);layout->addWidget(titlebar);
    body=new QWidget;body->setObjectName("panelbody");body->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Ignored);layout->addWidget(body,1);
    folded=settings.value(prefix+"/folded",false).toBool();body->setVisible(!folded);
    fold->setSymbol(folded?HudSymbols::Symbol::Expand:HudSymbols::Symbol::Collapse);
}
void OverlayWindow::setCollapsed(bool value){
    if(value==folded)return;
    folded=value;body->setVisible(!value);
    if(auto control=dynamic_cast<HudSymbolButton*>(findChild<QAbstractButton*>(QStringLiteral("overlayFold"))))control->setSymbol(value?HudSymbols::Symbol::Expand:HudSymbols::Symbol::Collapse);
    const int targetHeight=value?foldedHeight:std::min(expandedHeight,lane.isEmpty()?expandedHeight:lane.height());
    setMaximumHeight(targetHeight);resize(width(),targetHeight);constrain();
    settings.setValue(prefix+"/folded",value);
}
void OverlayWindow::setContentHeight(int value){
    expandedHeight=std::max(foldedHeight,value);
    const int targetHeight=folded?foldedHeight:std::min(expandedHeight,lane.isEmpty()?expandedHeight:lane.height());
    // A hidden native window may later be resized from its layout's stale
    // expanded size hint. Keep the automatic HUD height as an actual bound.
    setMaximumHeight(targetHeight);
    if(!folded)resize(width(),targetHeight);
    constrain();
}
void OverlayWindow::setHeading(const QString &text){
    headingText=text;
    if(heading)heading->setText(heading->fontMetrics().elidedText(text,Qt::ElideRight,std::max(0,heading->width())));
}
void OverlayWindow::setSafeLane(const QRect &bounds,bool initial,bool bottomAligned){
    lane=bounds;bottomAnchored=bottomAligned;
    if(bounds.width()>=OverlayMetrics::narrowMinimumWidth)setMinimumWidth(std::min(OverlayMetrics::minimumWidth,bounds.width()));
    setMaximumWidth(bounds.width());setMaximumHeight(folded?foldedHeight:std::min(expandedHeight,std::max(foldedHeight,bounds.height())));
    resize(std::min(preferredWidth,bounds.width()),folded?foldedHeight:std::min(expandedHeight,bounds.height()));
    if(initial){const auto saved=settings.value(prefix+"/position").toPoint();move(bottomAnchored?QPoint(bounds.left(),bounds.bottom()-height()+1):saved.isNull()?QPoint(bounds.right()-width()+1,bounds.top()):saved);}
    constrain();
}
void OverlayWindow::constrain(){
    if(constraining||lane.isEmpty())return;constraining=true;
    const int x=std::clamp(pos().x(),lane.left(),std::max(lane.left(),lane.right()-width()+1));
    const int y=bottomAnchored?std::max(lane.top(),lane.bottom()-height()+1):std::clamp(pos().y(),lane.top(),std::max(lane.top(),lane.bottom()-height()+1));
    if(pos()!=QPoint(x,y))move(x,y);constraining=false;
}
void OverlayWindow::setCustomLayout(const QRect &bounds){
    setSafeLane(bounds,false,false);
    resize(bounds.width(),folded?foldedHeight:std::min(expandedHeight,bounds.height()));
    move(bounds.topLeft());constrain();
}
void OverlayWindow::moveEvent(QMoveEvent *event){QWidget::moveEvent(event);constrain();}
void OverlayWindow::resizeEvent(QResizeEvent *event){QWidget::resizeEvent(event);setHeading(headingText);constrain();}
void OverlayWindow::setThrough(bool value){
    if(through==value)return;
    through=value;const bool visible=isVisible();QScopedValueRollback<bool> guard(rebuildingFlags,true);
    auto flags=windowFlags();flags.setFlag(Qt::WindowTransparentForInput,value);flags.setFlag(Qt::WindowDoesNotAcceptFocus,value||(!interactive&&!inspectionMode));setWindowFlags(flags);if(visible)show();
}
void OverlayWindow::setInteractive(bool value){
    if(interactive==value)return;interactive=value;const bool visible=isVisible();
    QScopedValueRollback<bool> guard(rebuildingFlags,true);
    setWindowFlag(Qt::WindowDoesNotAcceptFocus,through||(!interactive&&!inspectionMode));if(visible)show();
}
void OverlayWindow::applyVisibilityOpacity(){QWidget::setWindowOpacity(baseOpacity*std::clamp(reveal,qreal(0),qreal(1)));}
void OverlayWindow::setWindowOpacity(qreal opacity){baseOpacity=std::clamp(opacity,qreal(.45),qreal(1));applyVisibilityOpacity();}
void OverlayWindow::finishVisibility(){
    visibilityMotion.stop();reveal=visibilityRequested?1.:0.;applyVisibilityOpacity();
    if(!visibilityRequested&&isVisible()){QScopedValueRollback<bool> guard(applyingVisibility,true);QWidget::hide();}
}
void OverlayWindow::setPresentedVisible(bool visible,bool animated){
    if(visibilityRequested==visible){if(!animated||Kda::reducedMotion())finishVisibility();return;}
    visibilityMotion.stop();visibilityRequested=visible;
    if(visible&&!isVisible()){
        reveal=0.;applyVisibilityOpacity();QScopedValueRollback<bool> guard(applyingVisibility,true);QWidget::show();
    }
    if(!animated||Kda::reducedMotion()||(!visible&&!isVisible())||qAbs(reveal-(visible?1.:0.))<.001){finishVisibility();return;}
    const qreal start=reveal,end=visible?1.:0.;
    const int duration=std::max(1,qRound((visible?enterMs:exitMs)*qAbs(start-end)));
    {
        // Resetting an old animation's endpoints can emit its previous end
        // value. Keep that bookkeeping out of the currently displayed frame.
        QSignalBlocker guard(&visibilityMotion);
        visibilityMotion.setStartValue(start);visibilityMotion.setEndValue(end);
        visibilityMotion.setDuration(duration);visibilityMotion.setCurrentTime(0);
        visibilityMotion.setEasingCurve(visible?QEasingCurve::OutCubic:QEasingCurve::InOutSine);
    }
    visibilityMotion.start();
}
void OverlayWindow::showEvent(QShowEvent *event){
    QWidget::showEvent(event);if(!applyingVisibility&&!rebuildingFlags){visibilityRequested=true;finishVisibility();}
}
void OverlayWindow::hideEvent(QHideEvent *event){
    QWidget::hideEvent(event);if(!applyingVisibility&&!rebuildingFlags){visibilityRequested=false;finishVisibility();}
}
bool OverlayWindow::eventFilter(QObject *object,QEvent *event){
    if(object==qApp&&event->type()==QEvent::DynamicPropertyChange&&static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName()=="lixinReducedMotion"&&Kda::reducedMotion())finishVisibility();
    return QWidget::eventFilter(object,event);
}
void OverlayWindow::closeEvent(QCloseEvent *event){
    settings.setValue(prefix+"/position",pos());settings.setValue(prefix+"/opacity",baseOpacity);settings.sync();
    if(onClose){event->ignore();onClose();}else QWidget::closeEvent(event);
}
bool OverlayWindow::nativeEvent(const QByteArray &type,void *message,qintptr *result){
#ifdef Q_OS_WIN
    const auto msg=static_cast<MSG*>(message);
    if(msg->message==WM_HOTKEY&&onHotkey){onHotkey(int(msg->wParam));*result=0;return true;}
#endif
    return QWidget::nativeEvent(type,message,result);
}
void OverlayWindow::menu(QWidget *anchor){
    QMenu menu;if(onOpenDashboard){auto open=menu.addAction(QStringLiteral("打开主界面 · Ctrl+Alt+F6"));connect(open,&QAction::triggered,this,[this]{if(onOpenDashboard)onOpenDashboard();});menu.addSeparator();}auto opacity=menu.addMenu(QStringLiteral("不透明度"));
    for(int percent:{45,60,75,88,100}){auto action=opacity->addAction(QString::number(percent)+"%");action->setCheckable(true);action->setChecked(std::abs(baseOpacity-percent/100.)<.02);
        connect(action,&QAction::triggered,this,[this,percent]{setWindowOpacity(percent/100.);settings.setValue(prefix+"/opacity",percent/100.);});}
    auto finish=menu.addMenu(QStringLiteral("皮肤浓度"));
    for(const auto &choice:QList<QPair<QString,qreal>>{{QStringLiteral("素净"),0.},{QStringLiteral("标准"),.65},{QStringLiteral("鲜明"),1.}}){
        auto action=finish->addAction(choice.first);action->setCheckable(true);action->setChecked(std::abs(Kda::HudStyle::strength()-choice.second)<.02);
        connect(action,&QAction::triggered,this,[this,choice]{Kda::HudStyle::setStrength(choice.second);settings.setValue("appearance/hudSkinStrength",choice.second);settings.sync();Kda::postNotice({QStringLiteral("hud-finish"),QStringLiteral("HUD 皮肤浓度：")+choice.first,{},Kda::NoticeKind::Success,2800});});
    }
    auto throughAction=menu.addAction(QStringLiteral("鼠标穿透 · Ctrl+Alt+F9"));throughAction->setEnabled(hotkeyAvailable);throughAction->setCheckable(true);throughAction->setChecked(through);
    connect(throughAction,&QAction::triggered,this,[this](bool value){if(onThrough)onThrough(value);});
    auto fold=menu.addAction(folded?QStringLiteral("展开面板"):QStringLiteral("折叠为标题栏"));connect(fold,&QAction::triggered,this,[this]{setCollapsed(!folded);});
    auto reset=menu.addAction(QStringLiteral("回到安全边缘 · Ctrl+Alt+F7"));connect(reset,&QAction::triggered,this,[this]{if(onReset)onReset();});
    menu.addSeparator();auto help=menu.addAction(QStringLiteral("Ctrl+Alt+F8 隐藏 / 显示"));help->setEnabled(false);
    menu.exec(anchor->mapToGlobal(QPoint(0,anchor->height())));
}
QJsonObject OverlayWindow::report()const{
    auto shell=dynamic_cast<StageShell*>(findChild<QFrame*>(QStringLiteral("shell")));
    const QJsonObject visibility{{"phase",visibilityMotion.state()==QAbstractAnimation::Running?(visibilityRequested?"enter":"exit"):visibilityRequested?"steady":"hidden"},{"requested_visible",visibilityRequested},{"progress",reveal},{"running",visibilityMotion.state()==QAbstractAnimation::Running},{"frames",visibilityFrames},{"enter_ms",enterMs},{"exit_ms",exitMs},{"entry_easing","OutCubic"},{"exit_easing","InOutSine"},{"geometry_animated",false},{"reduced_motion",Kda::reducedMotion()}};
    return {{"theme",Kda::themeReport()},{"shell_motion",shell?shell->motionReport():QJsonObject{}},{"visibility_motion",visibility},{"visible",isVisible()},{"geometry",QJsonArray{x(),y(),width(),height()}},{"opacity",baseOpacity},{"rendered_opacity",windowOpacity()},
        {"anchor",bottomAnchored?QStringLiteral("bottom-left"):QStringLiteral("custom")},{"collapsed",folded},{"click_through",through},{"hotkey_available",hotkeyAvailable},{"body_width",body->width()},
        {"preferred_content_height",expandedHeight},{"heading",heading?heading->text():QString()},{"heading_fits",!heading||heading->fontMetrics().horizontalAdvance(heading->text())<=heading->width()},{"chrome_uses_vector_icons",true},{"titlebar_height",titlebarHeight},
        {"accepts_keyboard_focus",!windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus)}};
}
