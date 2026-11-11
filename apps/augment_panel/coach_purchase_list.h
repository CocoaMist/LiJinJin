#pragma once
#include "catalog.h"
#include "hud_assets.h"
#include "hud_reading_plane.h"
#include "hud_symbols.h"
#include "kda_surface.h"
#include <QJsonArray>
#include <QShowEvent>
#include <QHideEvent>
#include <QFontMetrics>
#include <QPainter>
#include <QVariantAnimation>
#include <QDynamicPropertyChangeEvent>
#include <cmath>

class CoachPurchaseList final : public QWidget {
public:
    CoachPurchaseList(const Catalog &catalog,HudAssets *assets,QWidget *parent=nullptr):QWidget(parent),catalog(catalog),assets(assets),response(this){
        setObjectName(QStringLiteral("purchaseChecklist"));setMinimumWidth(0);setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);setAttribute(Qt::WA_TransparentForMouseEvents);
        response.setDuration(240);response.setStartValue(0.);response.setEndValue(1.);connect(&response,&QVariantAnimation::valueChanged,this,[this]{update();});setFixedHeight(0);
    }
    void setChecklist(const QJsonObject &value){
        QJsonArray nextIdentity;for(const auto &v:value.value("rows").toArray()){const auto row=v.toObject();nextIdentity.append(QJsonArray{row.value("item_id"),row.value("estimated_cost"),row.value("step_kind"),row.value("affordable_now")});}
        const bool changed=identity!=nextIdentity;identity=nextIdentity;data=value;setFixedHeight(data.value("available").toBool()?data.value("rows").toArray().size()*rowHeight+4:0);
        setAccessibleName(QStringLiteral("购买清单"));if(changed&&!Kda::reducedMotion()&&isVisible()){response.stop();response.start();}update();
    }
    QJsonObject report()const{
        bool fit=true;QJsonArray visible;const auto rows=data.value("rows").toArray();
        for(int i=0;i<rows.size();++i){const QRect row(0,2+i*rowHeight,width(),rowHeight),icon(19,row.top()+iconInset,24,24);fit&=rect().contains(row)&&row.contains(icon);auto value=rows[i].toObject();value["row_rect"]=QJsonArray{row.x(),row.y(),row.width(),row.height()};value["icon_rect"]=QJsonArray{icon.x(),icon.y(),24,24};visible.append(value);}
        return {{"rows",visible},{"text_fits",fit},{"visible",isVisible()},{"explanatory_copy_visible",false},{"style_name_visible",false},{"price_kind","catalogue_estimate"},{"interactive",false},{"height",height()},{"motion_running",response.state()==QAbstractAnimation::Running}};
    }
protected:
    void hideEvent(QHideEvent *event)override{response.stop();QWidget::hideEvent(event);}
    void paintEvent(QPaintEvent *)override{
        Kda::OverlayCanvas canvas(this);auto &p=canvas.paint();const auto rows=data.value("rows").toArray();
        for(int i=0;i<rows.size();++i){const auto row=rows[i].toObject();const int y=2+i*rowHeight;
            if(i==0){
                p.fillRect(QRect(0,y,width(),rowHeight),Kda::HudStyle::color("surface"));
                if(response.state()==QAbstractAnimation::Running){
                    const qreal pulse=std::sin(3.141592653589793*response.currentValue().toReal());
                    p.fillRect(QRect(0,y,width(),rowHeight),Kda::HudStyle::accent(qRound(23*pulse)));
                }
            }
            p.setFont(Kda::hudFont(Kda::FontRole::Numeric,10,QFont::DemiBold));p.setPen(Kda::HudStyle::color("muted"));p.drawText(QRect(0,y,16,rowHeight),Qt::AlignVCenter|Qt::AlignLeft,QString::number(i+1).rightJustified(2,'0'));
            const QRect icon(19,y+iconInset,24,24);const auto art=assets?assets->item(row.value("item_id").toInt()):QPixmap();if(!art.isNull())p.drawPixmap(icon,Kda::roundedPixmap(art,icon.size(),2,devicePixelRatioF()));
            const auto gap=row.value("gold_gap");
            const bool ready=row.value("affordable_now")==QJsonValue(true);
            const auto cost=row.value("estimated_cost").toInt();
            QString price=QStringLiteral("≈%1G").arg(cost);
            if(i==0){
                if(!gap.isDouble())price=QStringLiteral("金币待同步");
                else if(ready)price=QStringLiteral("预算内 · %1").arg(price);
                else price=QStringLiteral("差 ≈%1G").arg(gap.toInt());
            }else if(row.value("dependent_on_previous").toBool())price=QStringLiteral("再付≈%1G").arg(cost);
            const auto priceFont=Kda::hudFont(Kda::FontRole::Numeric,12,QFont::DemiBold);
            const int priceWidth=QFontMetrics(priceFont).horizontalAdvance(price)+2;
            const auto nameFont=Kda::hudFont(Kda::FontRole::Body,12,QFont::DemiBold);p.setFont(nameFont);p.setPen(Kda::HudStyle::color("text"));const QRect name(50,y+1,std::max(0,width()-priceWidth-56),rowHeight-2);p.drawText(name,Qt::AlignVCenter|Qt::AlignLeft,QFontMetrics(nameFont).elidedText(catalog.itemName(row.value("item_id").toInt()),Qt::ElideRight,name.width()));
            p.setFont(priceFont);p.setPen(ready?Kda::HudStyle::accent():Kda::HudStyle::color("silver"));
            p.drawText(QRect(std::max(0,width()-priceWidth-2),y+1,priceWidth,rowHeight-2),Qt::AlignRight|Qt::AlignVCenter,price);
            p.setPen(QPen(QColor(85,68,106,85),.7));p.drawLine(19,y+rowHeight-1,width()-1,y+rowHeight-1);
        }
    }
private:
    static constexpr int rowHeight=27;
    static constexpr int iconInset=(rowHeight-24)/2;
    const Catalog &catalog;HudAssets *assets;QJsonObject data;QJsonArray identity;QVariantAnimation response;
};

class HudPhasePlate final : public QWidget {
public:
    explicit HudPhasePlate(QWidget *parent=nullptr):QWidget(parent),cycle(this){
        setObjectName(QStringLiteral("hudPhasePlate"));setFixedHeight(42);setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);setAttribute(Qt::WA_TransparentForMouseEvents);
        if(qApp)qApp->installEventFilter(this);cycle.setStartValue(0.);cycle.setEndValue(1.);connect(&cycle,&QVariantAnimation::valueChanged,this,[this]{update();});hide();
    }
    void setStage(const QString &value,bool pending=true,int rosterCount=-1){
        const bool nextBusy=value==QStringLiteral("loading")&&pending;
        if(stage==value&&busy==nextBusy&&confirmedRoster==rosterCount)return;
        if(stage!=value||busy!=nextBusy)cycle.stop();
        stage=value;busy=nextBusy;confirmedRoster=rosterCount;
        setAccessibleName(label());setVisible(!stage.isEmpty());sync();update();
    }
    QJsonObject report()const{return {{"stage",stage},{"label",label()},{"running",cycle.state()==QAbstractAnimation::Running},{"progress",cycle.currentValue().toDouble()},{"looping",busy},{"pending",busy},{"style_name_visible",false},{"explanatory_copy_visible",false}};}
protected:
    void showEvent(QShowEvent *event)override{QWidget::showEvent(event);sync();}
    void hideEvent(QHideEvent *event)override{cycle.stop();QWidget::hideEvent(event);}
    bool eventFilter(QObject *object,QEvent *event)override{if(object==qApp&&event->type()==QEvent::DynamicPropertyChange&&static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName()=="lixinReducedMotion"){cycle.stop();if(!Kda::reducedMotion())sync();update();}return QWidget::eventFilter(object,event);}
    void paintEvent(QPaintEvent *)override{
        Kda::OverlayCanvas canvas(this);auto &p=canvas.paint();
        const bool running=cycle.state()==QAbstractAnimation::Running;
        const qreal phase=cycle.currentValue().toReal();
        p.setFont(Kda::hudFont(Kda::FontRole::Body,16,QFont::DemiBold));p.setPen(Kda::HudStyle::neutral());
        p.drawText(QRect(24,0,width()-32,36),Qt::AlignVCenter|Qt::AlignLeft,label());
        p.setPen(Qt::NoPen);
        const qreal wave=running&&busy?.5-.5*std::cos(6.283185307179586*phase):0.;
        p.setBrush(busy?Kda::HudStyle::accent(qRound(130+110*wave)):Kda::HudStyle::neutral(155));
        p.drawEllipse(QPointF(13,18),3,3);
    }
private:
    QString label()const{
        if(stage=="ending")return QStringLiteral("对局结束");
        if(stage=="selecting")return QStringLiteral("选人");
        if(stage=="loading"){
            if(!busy)return QStringLiteral("阵容就绪");
            return confirmedRoster>=0?QStringLiteral("阵容 %1/10").arg(confirmedRoster):QStringLiteral("阵容载入");
        }
        return {};
    }
    void sync(){
        if(!isVisible()||Kda::reducedMotion()||cycle.state()==QAbstractAnimation::Running||!busy)return;
        cycle.setDuration(1500);cycle.setLoopCount(-1);cycle.start();
    }
    QString stage;QVariantAnimation cycle;bool busy=false;int confirmedRoster=-1;
};
