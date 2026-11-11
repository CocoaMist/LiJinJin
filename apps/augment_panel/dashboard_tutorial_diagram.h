#pragma once

#include "kda_controls.h"
#include "kda_glass_surface.h"
#include "kda_theme.h"
#include <QLinearGradient>
#include <QWidget>
#include <algorithm>
#include <array>

class DashboardTutorialDiagram final : public QWidget {
public:
    explicit DashboardTutorialDiagram(QWidget *parent=nullptr):QWidget(parent),entrance(this,210){
        setObjectName(QStringLiteral("tutorialStageDiagram"));
        setAccessibleName(QStringLiteral("李金金界面示例"));
        setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
        setStage(0);
    }

    QSize sizeHint() const override {return {510,228};}
    QSize minimumSizeHint() const override {return {300,148};}

    void setStage(int value){
        value=std::clamp(value,0,int(scenes.size())-1);
        if(value==stage)return;
        direction=value>=stage?1:-1;
        stage=value;
        setAccessibleDescription(QString::fromUtf8(scenes[stage].description));
        const auto &images=loadScene(stage);
        const auto loaded=std::count_if(images.begin(),images.end(),[](const QPixmap &image){return !image.isNull();});
        setProperty("tutorialCaptureCount",int(loaded));
        entrance.finish();
        entrance.trigger();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        Kda::configurePainter(painter);
        const QRectF bounds=QRectF(rect()).adjusted(8,3,-8,-3);
        if(bounds.isEmpty())return;
        const auto &images=loadScene(stage);
        if(property("tutorialCaptureCount").toInt()==0){
            painter.setFont(Kda::font(Kda::FontRole::Label,12));
            painter.setPen(Kda::color("muted"));
            painter.drawText(bounds,Qt::AlignCenter,QStringLiteral("暂无界面预览"));
            return;
        }
        const qreal unsettled=1.-entrance.progress();
        painter.translate(direction*7.*unsettled,0);
        painter.setOpacity(1.-.16*unsettled);
        switch(stage){
        case 0: paintConnection(painter,bounds,images); break;
        case 1: paintQueue(painter,bounds,images); break;
        case 2: paintRecommendations(painter,bounds,images); break;
        case 3: paintRevival(painter,bounds,images); break;
        case 4: paintEditor(painter,bounds,images); break;
        }
    }

private:
    struct Scene {
        const char *description;
        std::array<const char *,4> assets;
    };
    using SceneImages=std::array<QPixmap,4>;
    static constexpr std::array<Scene,5> scenes{{
        {"主窗口底栏显示客户端已连接。", {"capture-client-window-v1.png","capture-client-status-v1.png"}},
        {"实际选人浮层和载入后的阵容提示。", {"capture-draft-v1.png","capture-loading-v1.png"}},
        {"三张海克斯提示和实际出装浮层。", {"capture-augment-1-v1.png","capture-augment-2-v1.png",
                                             "capture-augment-3-v1.png","capture-purchase-v1.png"}},
        {"同一场示例中，阵亡时展开本局战况，复活后收起五维对照。", {"capture-dead-v1.png","capture-respawn-v1.png"}},
        {"HUD 管理实图和选牌提示框的拖动把手。", {"capture-layout-v1.png","capture-layout-detail-v1.png"}},
    }};

    static const SceneImages &loadScene(int index){
        static std::array<SceneImages,scenes.size()> images;
        static std::array<bool,scenes.size()> loaded{};
        auto &result=images[index];
        if(loaded[index])return result;
        const auto &assets=scenes[index].assets;
        for(size_t frame=0;frame<assets.size();++frame){
            if(assets[frame])
                result[frame].load(QStringLiteral(":/lixin/kda/tutorial/")+QString::fromLatin1(assets[frame]));
        }
        loaded[index]=true;
        return result;
    }

    static QRectF portion(const QRectF &area,qreal x,qreal y,qreal width,qreal height){
        return {area.left()+x*area.width(),area.top()+y*area.height(),width*area.width(),height*area.height()};
    }

    static QRectF captureRect(const QPixmap &image,const QRectF &slot,
                             Qt::Alignment alignment=Qt::AlignCenter,qreal maxScale=1.5){
        if(image.isNull()||slot.isEmpty())return {};
        const qreal scale=std::min({slot.width()/image.width(),slot.height()/image.height(),maxScale});
        const QSizeF size=QSizeF(image.size())*scale;
        const qreal x=alignment.testFlag(Qt::AlignLeft)?slot.left():
            alignment.testFlag(Qt::AlignRight)?slot.right()-size.width():slot.center().x()-size.width()/2;
        const qreal y=alignment.testFlag(Qt::AlignTop)?slot.top():
            alignment.testFlag(Qt::AlignBottom)?slot.bottom()-size.height():slot.center().y()-size.height()/2;
        return {QPointF(x,y),size};
    }

    static void capture(QPainter &painter,const QPixmap &image,const QRectF &target){
        if(!target.isEmpty())painter.drawPixmap(target,image,QRectF(image.rect()));
    }

    static void plate(QPainter &painter,const QRectF &bounds){
        if(bounds.isEmpty())return;
        const qreal radius=Kda::radius("section");
        QPainterPath shape;
        shape.addRoundedRect(bounds,radius,radius);
        QLinearGradient glass(bounds.topLeft(),bounds.bottomRight());
        glass.setColorAt(0,Kda::color("raised"));
        glass.setColorAt(1,Kda::color("bg"));
        painter.fillPath(shape,glass);
        Kda::paintPolishedEdge(painter,shape,bounds,radius);
    }

    static void label(QPainter &painter,const QString &text,const QPointF &origin){
        painter.setFont(Kda::font(Kda::FontRole::Label,12,QFont::DemiBold));
        const QRectF badge(origin,QSizeF(QFontMetricsF(painter.font()).horizontalAdvance(text)+22.,22.));
        painter.setPen(Qt::NoPen);
        painter.setBrush(Kda::color("raised"));
        painter.drawPath(Kda::facet(badge,5));
        painter.setBrush(Kda::color("ice"));
        painter.drawEllipse(QPointF(badge.left()+7,badge.center().y()),1.5,1.5);
        painter.setPen(Kda::color("text"));
        painter.drawText(badge.adjusted(13,0,-5,0),Qt::AlignLeft|Qt::AlignVCenter,text);
    }

    static void leader(QPainter &painter,const QPointF &anchor,const QPointF &end){
        const auto ink=Kda::color("ice");
        painter.setPen(QPen(ink,.9,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
        QPainterPath path(anchor);
        path.lineTo(anchor.x(),end.y());
        path.lineTo(end);
        painter.drawPath(path);
        painter.setBrush(ink);
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(anchor,2.5,2.5);
    }

    static void arrow(QPainter &painter,const QPointF &center,qreal halfWidth){
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(Kda::color("ice"),1.25,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
        const QPointF tip=center+QPointF(halfWidth,0);
        painter.drawLine(center-QPointF(halfWidth,0),tip);
        QPainterPath head(tip+QPointF(-5,-4));
        head.lineTo(tip);
        head.lineTo(tip+QPointF(-5,4));
        painter.drawPath(head);
    }

    static void paintConnection(QPainter &painter,const QRectF &area,const SceneImages &images){
        const auto window=captureRect(images[0],portion(area,0,.01,.92,.90),Qt::AlignLeft|Qt::AlignVCenter);
        const auto status=captureRect(images[1],portion(area,.53,.74,.45,.24),
                                      Qt::AlignRight|Qt::AlignBottom,1.2);
        capture(painter,images[0],window);
        if(status.isEmpty())return;
        const auto inset=status.adjusted(-8,-6,8,6);
        if(!window.isEmpty())
            leader(painter,QPointF(window.left()+window.width()*.05,window.top()+window.height()*.978),
                   QPointF(inset.left(),inset.center().y()));
        plate(painter,inset);
        capture(painter,images[1],status);
    }

    static void paintQueue(QPainter &painter,const QRectF &area,const SceneImages &images){
        const auto draft=captureRect(images[0],portion(area,0,.16,.43,.83),Qt::AlignRight|Qt::AlignTop);
        const auto loading=captureRect(images[1],portion(area,.55,.41,.44,.54),Qt::AlignLeft|Qt::AlignTop);
        capture(painter,images[0],draft);
        capture(painter,images[1],loading);
        if(!draft.isEmpty())label(painter,QStringLiteral("选人"),draft.topLeft()-QPointF(0,27));
        if(!loading.isEmpty())label(painter,QStringLiteral("载入"),loading.topLeft()-QPointF(0,27));
        arrow(painter,portion(area,.43,.55,.12,0).center(),std::min(17.,area.width()*.043));
    }

    static void paintRecommendations(QPainter &painter,const QRectF &area,const SceneImages &images){
        const auto rack=portion(area,0,.18,.61,.76);
        constexpr qreal stagger=5.,gap=5.;
        const qreal rowHeight=(rack.height()-2*gap)/3;
        const QRectF firstSlot(rack.topLeft(),QSizeF(rack.width()-2*stagger,rowHeight));
        for(int row=0;row<3;++row){
            const auto card=captureRect(images[row],firstSlot.translated(row*stagger,row*(rowHeight+gap)),
                                        Qt::AlignLeft|Qt::AlignTop);
            capture(painter,images[row],card);
        }
        label(painter,QStringLiteral("海克斯提示"),rack.topLeft()-QPointF(0,27));
        const auto item=captureRect(images[3],portion(area,.65,.30,.35,.67),Qt::AlignLeft|Qt::AlignBottom);
        capture(painter,images[3],item);
        if(!item.isEmpty())label(painter,QStringLiteral("装备推荐"),item.topLeft()-QPointF(0,27));
    }

    static void paintRevival(QPainter &painter,const QRectF &area,const SceneImages &images){
        const auto left=portion(area,0,.18,.43,.81);
        const auto right=portion(area,.57,.18,.43,.81);
        const auto expanded=captureRect(images[0],left,Qt::AlignRight|Qt::AlignTop);
        const qreal commonScale=expanded.isEmpty()?1.5:expanded.width()/images[0].width();
        const auto compact=captureRect(images[1],right,Qt::AlignLeft|Qt::AlignTop,commonScale);
        capture(painter,images[0],expanded);
        capture(painter,images[1],compact);
        if(!expanded.isEmpty())label(painter,QStringLiteral("阵亡"),expanded.topLeft()-QPointF(0,27));
        if(!compact.isEmpty())label(painter,QStringLiteral("复活"),compact.topLeft()-QPointF(0,27));
        arrow(painter,portion(area,.43,.43,.14,0).center(),std::min(18.,area.width()*.05));
    }

    static void paintEditor(QPainter &painter,const QRectF &area,const SceneImages &images){
        const auto editor=captureRect(images[0],portion(area,0,.12,.68,.86),Qt::AlignRight|Qt::AlignTop);
        const auto detail=captureRect(images[1],portion(area,.58,.45,.40,.40),
                                      Qt::AlignRight|Qt::AlignTop,1.1);
        if(!editor.isEmpty()){
            plate(painter,editor.adjusted(-2,-2,2,2));
            capture(painter,images[0],editor);
        }
        if(detail.isEmpty())return;
        const auto inset=detail.adjusted(-8,-29,8,7);
        if(!editor.isEmpty())
            leader(painter,QPointF(editor.left()+editor.width()*.54,editor.top()+editor.height()*.73),
                   QPointF(inset.left(),inset.center().y()));
        plate(painter,inset);
        label(painter,QStringLiteral("拖边框缩放"),inset.topLeft()+QPointF(7,4));
        capture(painter,images[1],detail);
    }

    Kda::Transition entrance;
    int stage=-1,direction=1;
};
