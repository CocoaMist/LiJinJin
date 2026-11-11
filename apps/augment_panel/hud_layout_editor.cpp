#include "hud_layout_editor.h"
#include "kda_theme.h"
#include "kda_controls.h"
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSlider>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <functional>
#include <algorithm>
#include <optional>

namespace {
struct Snap {
    int position;
    std::optional<int> guide;
};

QList<int> anchors(int start,int length){
    return {start,start+length/2,start+length};
}

Snap nearestAlignment(int start,int length,int limitStart,int limitLength,const QList<int> &targets,qreal threshold){
    Snap result{start,{}};
    qreal distance=threshold;
    for(int edge:anchors(start,length))for(int target:targets){
        const int shift=target-edge;
        const int position=start+shift;
        if(position<limitStart||position+length>limitStart+limitLength)continue;
        if(qAbs(shift)>distance||(result.guide&&qAbs(shift)==distance))continue;
        distance=qAbs(shift);
        result={position,target};
    }
    return result;
}
}

class HudEditorBar final : public QWidget {
public:
    using QWidget::QWidget;
protected:
    void paintEvent(QPaintEvent *)override{
        QPainter p(this);Kda::configurePainter(p);
        auto fill=Kda::color("surface");fill.setAlpha(226);
        auto edge=Kda::color("silver");edge.setAlpha(55);
        p.setBrush(fill);p.setPen(QPen(edge,1));p.drawRoundedRect(QRectF(rect()).adjusted(.5,.5,-.5,-.5),4,4);
    }
};

class HudLayoutCanvas final : public QWidget {
public:
    QRect viewport;
    HudLayout draft;
    QMap<QString,QRect> defaults;
    QMap<QString,QImage> previews;
    QString selected="coach";
    qreal previewOpacity=.75;
    std::function<void()> changed;
    HudLayoutCanvas(const QRect &v,const HudLayout &layout,const QMap<QString,QRect> &base,const QMap<QString,QImage> &images)
        :viewport(v),draft(layout),defaults(base),previews(images){setObjectName("hudLayoutCanvas");setAttribute(Qt::WA_NoSystemBackground);setAutoFillBackground(false);setAccessibleName(QStringLiteral("HUD 布局画布，拖动面板或边框调整"));setFocusPolicy(Qt::StrongFocus);setMouseTracking(true);setMinimumSize(320,240);}
    QRect bounds(const QString &key)const{return draft.resolve(key,viewport,defaults.value(key));}
    QRectF frame()const{
        // No header/footer inset: when the window covers the client area this
        // mapping is exactly one logical screen pixel per game pixel.
        QRectF area=QRectF(rect());
        const qreal scale=std::min(area.width()/viewport.width(),area.height()/viewport.height());
        const QSizeF size(viewport.width()*scale,viewport.height()*scale);
        return {area.center()-QPointF(size.width()/2,size.height()/2),size};
    }
    QRectF display(const QRect &r)const{const auto f=frame();const qreal s=f.width()/viewport.width();return {f.left()+(r.x()-viewport.x())*s,f.top()+(r.y()-viewport.y())*s,r.width()*s,r.height()*s};}
    void select(const QString &key){selected=key;update();if(changed)changed();}
    void changeSize(QSize size){auto r=bounds(selected);r.setSize(size);draft.set(selected,r,viewport);update();if(changed)changed();}
protected:
    void paintEvent(QPaintEvent *)override{
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);const auto f=frame();
        // Alpha 1 retains the editor's hit surface without painting over the
        // live desktop/game. There is no desktop capture or fake backdrop.
        p.fillRect(rect(),QColor(0,0,0,1));
        p.save();p.setClipRect(f);
        auto order=HudLayout::keys();order.removeOne(selected);order.append(selected);
        for(const auto &key:order){
            const auto r=display(bounds(key));const bool active=key==selected;
            auto fill=Kda::color("surface");fill.setAlphaF(previewOpacity);
            auto edge=Kda::color(active?"ice":"silver");if(!active)edge.setAlpha(100);
            p.setBrush(fill);p.setPen(QPen(edge,active?1.4:1));p.drawRoundedRect(r,4,4);
            const auto image=previews.value(key);
            p.save();QPainterPath clip;clip.addRoundedRect(r.adjusted(1,1,-1,-1),4,4);p.setClipPath(clip);
            if(!image.isNull()){p.setRenderHint(QPainter::SmoothPixmapTransform);p.setOpacity(previewOpacity);p.drawImage(r,image);p.setOpacity(1);}
            else if(r.height()>52){
                p.setFont(Kda::font(Kda::FontRole::Body,12));p.setPen(Kda::color("muted"));
                p.drawText(r.adjusted(12,30,-12,-8),Qt::AlignCenter|Qt::TextWordWrap,key=="coach"?QStringLiteral("装备建议\n阵容信息"):key=="combat"?QStringLiteral("敌我战况\n击杀与伤害对照"):QStringLiteral("推荐与模型提示"));
            }p.restore();
            p.setFont(Kda::font(Kda::FontRole::Body,11,QFont::DemiBold));
            const QString caption=QStringLiteral("%1  ·  %2 × %3").arg(HudLayout::title(key)).arg(bounds(key).width()).arg(bounds(key).height());
            const qreal labelY=r.top()>24?r.top()-23:r.top()+4;
            const QRectF label(r.left()+6,labelY,std::min(r.width()-12,qreal(p.fontMetrics().horizontalAdvance(caption)+12)),20);
            auto labelFill=Kda::color("surface");labelFill.setAlpha(220);p.setPen(Qt::NoPen);p.setBrush(labelFill);p.drawRoundedRect(label,3,3);
            p.setPen(Kda::color("silver"));p.drawText(label.adjusted(6,0,-3,0),Qt::AlignVCenter|Qt::AlignLeft,caption);
            if(active){p.setBrush(Kda::color("silver"));p.setPen(Qt::NoPen);for(const auto &point:handles(r))p.drawRoundedRect(QRectF(point-QPointF(3.5,3.5),QSizeF(7,7)),2,2);}
        }
        const qreal scale=f.width()/viewport.width();
        auto guideColor=Kda::color("ice");guideColor.setAlpha(190);
        p.setPen(QPen(guideColor,1,Qt::DashLine));
        if(verticalGuide){
            const qreal x=f.left()+(*verticalGuide-viewport.left())*scale;
            p.drawLine(QLineF(x,f.top(),x,f.bottom()));
        }
        if(horizontalGuide){
            const qreal y=f.top()+(*horizontalGuide-viewport.top())*scale;
            p.drawLine(QLineF(f.left(),y,f.right(),y));
        }
        p.restore();
    }
    void mousePressEvent(QMouseEvent *e)override{
        if(e->button()!=Qt::LeftButton)return;setFocus();
        const auto hit=edges(e->position());
        if(hit){resizeEdges=hit;}
        else{
            bool found=false;auto order=HudLayout::keys();order.removeOne(selected);order.append(selected);
            for(auto it=order.crbegin();it!=order.crend();++it)if(display(bounds(*it)).contains(e->position())){select(*it);found=true;break;}
            if(!found)return;
            resizeEdges=0;
        }
        dragging=true;origin=e->position();original=bounds(selected);grabMouse();e->accept();
    }
    void mouseMoveEvent(QMouseEvent *e)override{
        if(!dragging){const int edge=edges(e->position());setCursor(edge==3||edge==12?Qt::SizeHorCursor:edge==5||edge==10?Qt::SizeFDiagCursor:edge==6||edge==9?Qt::SizeBDiagCursor:edge&3?Qt::SizeHorCursor:edge?Qt::SizeVerCursor:Qt::ArrowCursor);return;}
        const qreal scale=viewport.width()/frame().width();const auto d=(e->position()-origin)*scale;
        verticalGuide.reset();horizontalGuide.reset();
        QRect r=original;
        if(!resizeEdges){
            r.translate(qRound(d.x()),qRound(d.y()));
            if(!e->modifiers().testFlag(Qt::AltModifier))alignMove(r,6*scale);
        }
        else{
            const auto min=HudLayout::minimumSize(selected).boundedTo(viewport.size());
            if(resizeEdges&1)r.setLeft(std::clamp(original.left()+qRound(d.x()),viewport.left(),original.right()-min.width()+1));
            if(resizeEdges&2)r.setRight(std::clamp(original.right()+qRound(d.x()),original.left()+min.width()-1,viewport.right()));
            if(resizeEdges&4)r.setTop(std::clamp(original.top()+qRound(d.y()),viewport.top(),original.bottom()-min.height()+1));
            if(resizeEdges&8)r.setBottom(std::clamp(original.bottom()+qRound(d.y()),original.top()+min.height()-1,viewport.bottom()));
        }
        draft.set(selected,r,viewport);update();if(changed)changed();
    }
    void mouseReleaseEvent(QMouseEvent *e)override{
        if(!dragging||e->button()!=Qt::LeftButton)return;
        dragging=false;
        verticalGuide.reset();horizontalGuide.reset();
        releaseMouse();update();e->accept();
    }
    void keyPressEvent(QKeyEvent *e)override{
        QPoint d;const int step=e->modifiers().testFlag(Qt::ShiftModifier)?10:1;
        if(e->key()==Qt::Key_Left)d.setX(-step);else if(e->key()==Qt::Key_Right)d.setX(step);else if(e->key()==Qt::Key_Up)d.setY(-step);else if(e->key()==Qt::Key_Down)d.setY(step);else{QWidget::keyPressEvent(e);return;}
        auto r=bounds(selected);r.translate(d);draft.set(selected,r,viewport);update();if(changed)changed();e->accept();
    }
private:
    bool dragging=false;int resizeEdges=0;QPointF origin;QRect original;
    std::optional<int> verticalGuide,horizontalGuide;
    void alignMove(QRect &r,qreal threshold){
        auto xTargets=anchors(viewport.left(),viewport.width());
        auto yTargets=anchors(viewport.top(),viewport.height());
        for(const auto &key:HudLayout::keys()){
            if(key==selected)continue;
            const auto other=bounds(key);
            xTargets.append(anchors(other.left(),other.width()));
            yTargets.append(anchors(other.top(),other.height()));
        }
        const auto x=nearestAlignment(r.left(),r.width(),viewport.left(),viewport.width(),xTargets,threshold);
        const auto y=nearestAlignment(r.top(),r.height(),viewport.top(),viewport.height(),yTargets,threshold);
        r.moveTo(x.position,y.position);
        verticalGuide=x.guide;horizontalGuide=y.guide;
    }
    QList<QPointF> handles(const QRectF &r)const{return {r.topLeft(),QPointF(r.center().x(),r.top()),r.topRight(),QPointF(r.right(),r.center().y()),r.bottomRight(),QPointF(r.center().x(),r.bottom()),r.bottomLeft(),QPointF(r.left(),r.center().y())};}
    int edges(const QPointF &point)const{
        const auto r=display(bounds(selected));if(!r.adjusted(-8,-8,8,8).contains(point))return 0;
        int result=0;if(qAbs(point.x()-r.left())<=8)result|=1;else if(qAbs(point.x()-r.right())<=8)result|=2;
        if(qAbs(point.y()-r.top())<=8)result|=4;else if(qAbs(point.y()-r.bottom())<=8)result|=8;return result;
    }
};

HudLayoutEditor::HudLayoutEditor(const QRect &viewport,const HudLayout &initial,const QMap<QString,QRect> &defaults,const QMap<QString,QImage> &previews,QWidget *parent):QDialog(parent){
    setObjectName("hudLayoutEditor");setWindowTitle(QStringLiteral("李金金 · HUD 管理"));setWindowFlags(Qt::Window|Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint);setModal(true);
    setAttribute(Qt::WA_TranslucentBackground);setAutoFillBackground(false);
    setFont(Kda::font(Kda::FontRole::Body,12));setStyleSheet(QStringLiteral("QLabel{color:%1;background:transparent;} QComboBox,QSpinBox{color:%2;background:rgba(16,18,24,195);border:1px solid %3;border-radius:4px;padding:4px 6px;} QComboBox QAbstractItemView{color:%2;background:%4;selection-background-color:%3;} QSlider::groove:horizontal{height:3px;background:%3;} QSlider::handle:horizontal{width:10px;margin:-4px 0;background:%2;border-radius:2px;}").arg(Kda::color("muted").name(),Kda::color("silver").name(),Kda::color("border").name(),Kda::color("surface").name()));
    canvas=new HudLayoutCanvas(viewport,initial,defaults,previews);canvas->setParent(this);
    topBar=new HudEditorBar(this);topBar->setObjectName("hudLayoutTopBar");
    auto top=new QHBoxLayout(topBar);top->setContentsMargins(12,8,12,8);top->setSpacing(10);
    auto heading=new QLabel(QStringLiteral("HUD 管理"));heading->setFont(Kda::font(Kda::FontRole::Body,14,QFont::DemiBold));top->addWidget(heading);
    top->addWidget(new QLabel(QStringLiteral("拖动吸附 · Alt 自由摆放 · 拉边缩放 · 方向键微调")));
    auto opacity=new QSlider(Qt::Horizontal);opacity->setObjectName("hudLayoutPreviewOpacity");opacity->setAccessibleName(QStringLiteral("参考浮层不透明度"));opacity->setRange(25,95);opacity->setValue(75);opacity->setFixedWidth(90);
    top->addSpacing(8);top->addWidget(new QLabel(QStringLiteral("参考透明度")));top->addWidget(opacity);
    auto opacityLabel=new QLabel(QStringLiteral("75%"));opacityLabel->setFixedWidth(32);top->addWidget(opacityLabel);
    connect(opacity,&QSlider::valueChanged,this,[this,opacityLabel](int value){canvas->previewOpacity=value/100.;opacityLabel->setText(QString::number(value)+"%");canvas->update();});
    auto action=[this](const QString &name,const QString &id){auto b=new Kda::Button(name,this);b->setObjectName(id);b->setMinimumHeight(36);b->setAutoDefault(false);return b;};
    auto cancel=action(QStringLiteral("取消"),"hudLayoutCancel");auto save=action(QStringLiteral("保存并退出"),"hudLayoutSave");top->addWidget(cancel);top->addWidget(save);
    bottomBar=new HudEditorBar(this);bottomBar->setObjectName("hudLayoutBottomBar");
    auto controls=new QHBoxLayout(bottomBar);controls->setContentsMargins(12,8,12,8);controls->setSpacing(8);selection=new QComboBox;selection->setObjectName("hudLayoutSelection");selection->setAccessibleName(QStringLiteral("选择 HUD"));for(const auto &key:HudLayout::keys())selection->addItem(HudLayout::title(key),key);controls->addWidget(selection);
    auto number=[this](const QString &name){auto n=new QSpinBox;n->setObjectName(name);n->setAccessibleName(name=="hudLayoutWidth"?QStringLiteral("HUD 宽度"):QStringLiteral("HUD 高度"));n->setSuffix(QStringLiteral(" px"));n->setKeyboardTracking(false);return n;};
    widthInput=number("hudLayoutWidth");heightInput=number("hudLayoutHeight");controls->addSpacing(12);controls->addWidget(new QLabel(QStringLiteral("宽")));controls->addWidget(widthInput);controls->addWidget(new QLabel(QStringLiteral("高")));controls->addWidget(heightInput);
    auto reset=action(QStringLiteral("重置当前"),"hudLayoutResetSelected");auto resetAll=action(QStringLiteral("恢复默认"),"hudLayoutResetAll");controls->addWidget(reset);controls->addWidget(resetAll);controls->addStretch();detail=new QLabel;controls->addWidget(detail);
    canvas->changed=[this]{refreshControls();};
    connect(selection,&QComboBox::currentIndexChanged,this,[this]{canvas->select(selection->currentData().toString());});
    connect(widthInput,&QSpinBox::valueChanged,this,[this](int w){canvas->changeSize({w,canvas->bounds(canvas->selected).height()});});
    connect(heightInput,&QSpinBox::valueChanged,this,[this](int h){canvas->changeSize({canvas->bounds(canvas->selected).width(),h});});
    connect(reset,&QPushButton::clicked,this,[this]{canvas->draft.reset(canvas->selected);canvas->update();refreshControls();});
    connect(resetAll,&QPushButton::clicked,this,[this]{canvas->draft=HudLayout{};canvas->update();refreshControls();});
    connect(cancel,&QPushButton::clicked,this,&QDialog::reject);connect(save,&QPushButton::clicked,this,&QDialog::accept);refreshControls();resize(viewport.size());positionControls();
}
void HudLayoutEditor::resizeEvent(QResizeEvent *event){QDialog::resizeEvent(event);positionControls();}
void HudLayoutEditor::positionControls(){
    canvas->setGeometry(rect());
    for(auto bar:{topBar,bottomBar}){bar->setFixedWidth(std::min(std::max(320,width()-24),bar->sizeHint().width()));bar->adjustSize();bar->raise();}
    topBar->move((width()-topBar->width())/2,12);bottomBar->move((width()-bottomBar->width())/2,height()-bottomBar->height()-12);
}
const HudLayout &HudLayoutEditor::editedLayout()const{return canvas->draft;}
void HudLayoutEditor::refreshControls(){
    const QSignalBlocker a(selection),b(widthInput),c(heightInput);selection->setCurrentIndex(selection->findData(canvas->selected));
    const auto min=HudLayout::minimumSize(canvas->selected).boundedTo(canvas->viewport.size());widthInput->setRange(min.width(),canvas->viewport.width());heightInput->setRange(min.height(),canvas->viewport.height());
    const auto r=canvas->bounds(canvas->selected);widthInput->setValue(r.width());heightInput->setValue(r.height());
    detail->setText(canvas->selected=="coach"?QStringLiteral("主面板按内容收起，高度为最大范围"):canvas->selected=="combat"?QStringLiteral("存活时自动收起详情"):QStringLiteral("仅在选牌时显示"));
}
