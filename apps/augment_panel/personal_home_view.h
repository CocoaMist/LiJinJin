#pragma once
#include "catalog.h"
#include "hud_assets.h"
#include "kda_controls.h"
#include "kda_theme.h"
#include "kda_reading_surface.h"
#include <QApplication>
#include <QDateTime>
#include <QDynamicPropertyChangeEvent>
#include <QFontMetrics>
#include <QJsonArray>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

// A local view of the current account only. Its fixed native actions sit above
// three painted readout depths; no screenshot, image-scaled text or timer.
class PersonalHomeView final : public QWidget {
    class RowAction final : public QPushButton {
    public:
        explicit RowAction(QWidget *parent):QPushButton(parent){setFlat(true);setFocusPolicy(Qt::StrongFocus);setCursor(Qt::PointingHandCursor);}
    protected:
        void paintEvent(QPaintEvent *)override{QPainter p(this);Kda::configurePainter(p);if(underMouse()||hasFocus()){auto ink=Kda::color("ice");auto wash=ink;wash.setAlpha(underMouse()?15:8);p.fillRect(rect().adjusted(1,1,-1,-1),wash);if(hasFocus()){p.setPen(QPen(ink,.8,Qt::DotLine));p.setBrush(Qt::NoBrush);p.drawRoundedRect(QRectF(rect()).adjusted(.5,.5,-.5,-.5),3,3);}p.setFont(Kda::font(Kda::FontRole::Body,18));p.setPen(Kda::color("silver"));p.drawText(QRect(width()-18,0,16,height()),Qt::AlignCenter,QStringLiteral("›"));}}
    };
public:
    explicit PersonalHomeView(const Catalog &catalog,QWidget *parent=nullptr):QWidget(parent),dataCatalog(catalog),assets(new HudAssets(catalog,this)){
        setObjectName(QStringLiteral("personalHomeView"));setProperty("kdaDepthMapped",true);setMinimumWidth(0);setMinimumHeight(230);setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
        Kda::markSharedGlassSurface(this);
        all=new Kda::Button(QStringLiteral("全部战绩"),this);all->setObjectName(QStringLiteral("personalHomeOpenAll"));all->setProperty("toolbarAction",true);all->setProperty("labelPixels",12);all->setAccessibleName(QStringLiteral("查看本人全部战绩"));
        connect(all,&QPushButton::clicked,this,[this]{if(onOpenAll)onOpenAll();});
        for(int i=0;i<int(rows.size());++i){auto action=new RowAction(this);action->setObjectName(QStringLiteral("personalHomeMatch_%1").arg(i));rows[i]=action;connect(action,&QPushButton::clicked,this,[this,i]{const auto ref=rows[i]->property("matchRef").toString();if(!ref.isEmpty()&&onMatchRequested)onMatchRequested(ref);});}
        assets->onChanged=[this]{update();};if(qApp)qApp->installEventFilter(this);syncActions();
    }
    ~PersonalHomeView()override{if(qApp)qApp->removeEventFilter(this);}
    QSize sizeHint()const override{return {740,340};}
    QSize minimumSizeHint()const override{return {0,230};}
    void setProfile(const QJsonObject &value){
        profile=value;matches=value.value("matches").toArray();if(!profile.isEmpty()){loading=false;reason.clear();}
        winSamples=kdaSamples=wins=0;kdaMean=0;windowCount=std::min(20,int(matches.size()));
        for(int i=0;i<windowCount;++i){const auto match=matches[i].toObject();const auto win=match.value("win");
            if(win.isBool()||(recorded(win)&&(win.toDouble()==0||win.toDouble()==1))){++winSamples;wins+=win.toBool(win.toDouble()!=0);}
            const auto score=matchScores(match);if(recorded(score.value("kills"))&&recorded(score.value("deaths"))&&recorded(score.value("assists"))){kdaMean+=(score.value("kills").toDouble()+score.value("assists").toDouble())/std::max(1.,score.value("deaths").toDouble());++kdaSamples;}}
        if(kdaSamples)kdaMean/=kdaSamples;
        syncActions();update();
    }
    void setState(bool isLoading,const QString &message){loading=isLoading;reason=message;all->setText(profile.isEmpty()?QStringLiteral("读取我的战绩"):QStringLiteral("全部战绩"));all->setEnabled(!loading||!profile.isEmpty());syncActions();update();}
    QJsonObject report()const{
        QJsonArray geometry;int visible=0;bool fits=width()>=560&&height()>=230;
        for(auto action:rows)if(!action->isHidden()){++visible;geometry.append(rectJson(action->geometry()));fits&=rect().contains(action->geometry());}
        fits&=rect().contains(all->geometry());QJsonObject offsets;
        for(int i=0;i<3;++i){const auto p=offset(i);offsets[i==0?"front":i==1?"middle":"rear"]=QJsonArray{p.x(),p.y()};}
        return {{"has_profile",!profile.isEmpty()},{"loading",loading},{"win_rate",winSamples?QJsonValue(100.*wins/winSamples):QJsonValue()},
            {"win_samples",winSamples},{"kda_mean",kdaSamples?QJsonValue(kdaMean):QJsonValue()},{"kda_samples",kdaSamples},
            {"effective_matches",profile.isEmpty()?QJsonValue():QJsonValue(windowCount)},{"window_limit",20},{"rows",visible},{"available_matches",matches.size()},
            {"radar_visible",false},{"statistics_destination","player_history"},
            {"content_fits",fits},{"native_input",true},{"fixed_hit_targets",true},{"clickable_layer","foreground"},
            {"depth_offsets",offsets},{"motion_direction","same_as_pointer"},{"idle_timer",false},
            {"recent_row_geometries",geometry},{"all_action_geometry",rectJson(all->geometry())},
            {"metric_layout","continuous_band"},{"primary_number_pixels",30},{"recent_row_height",rowRect(0).height()},{"recent_list_width",recentRect().width()},{"result_strip_count",0},{"result_summary_count",windowCount},
            {"result_strip_style","text_summary"},{"content_surface_alpha",Kda::glassEnabled()?204:255},{"adaptive_height",true},{"row_capacity",rowCapacity()},{"row_count",visible},
            {"no_synthetic_defaults",true},{"identities_exported",false},{"profile_exported",false},{"refs_exported",false}};
    }
    std::function<void(const QString &)> onMatchRequested;
    std::function<void()> onOpenAll;
protected:
    void resizeEvent(QResizeEvent *event)override{QWidget::resizeEvent(event);syncActions();}
    bool eventFilter(QObject *object,QEvent *event)override{if(object==qApp&&event->type()==QEvent::DynamicPropertyChange&&static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName()=="lixinSkin")update();return QWidget::eventFilter(object,event);}
    void paintEvent(QPaintEvent *)override{
        QPainter p(this);Kda::configurePainter(p);
        // Content uses a quiet translucent reading plane. The surrounding
        // campaign art can contribute color without competing with the data.
        Kda::paintReadingSurface(p,QRectF(rect()));
        const auto muted=Kda::color("muted"),silver=Kda::color("silver");
        const int middle=18+qRound((width()-36)*.29),summary=18+qRound((width()-36)*.57);
        draw(p,QRect(18,7,middle-36,18),QStringLiteral("近期胜率"),11,muted,0,Kda::FontRole::Label);
        draw(p,QRect(18,24,middle-36,38),winSamples?QString::number(100.*wins/winSamples,'f',1)+QStringLiteral("%"):QStringLiteral("—"),30,silver,1,Kda::FontRole::Numeric);
        draw(p,QRect(middle,7,summary-middle-16,18),QStringLiteral("平均 KDA"),11,muted,0,Kda::FontRole::Label);
        draw(p,QRect(middle,24,summary-middle-16,38),kdaSamples?QString::number(kdaMean,'f',2):QStringLiteral("—"),30,silver,1,Kda::FontRole::Numeric);
        paintResults(p);
        auto rule=Kda::color("border");rule.setAlpha(70);p.setPen(QPen(rule,.6));p.drawLine(18,64,width()-18,64);
        const auto left=recentRect();
        draw(p,QRect(left.left()+3,69,left.width()-112,25),QStringLiteral("最近对局"),12,Kda::color("text"),0,Kda::FontRole::Label,true);
        if(profile.isEmpty()||matches.isEmpty()){
            const auto message=loading?QStringLiteral("正在读取你的对局…"):reason.isEmpty()?QStringLiteral("还没有对局记录"):reason;
            draw(p,QRect(left.left()+10,left.center().y()-18,left.width()-20,36),message,12,muted,1);
        }else for(int i=0;i<rowCapacity()&&i<matches.size();++i)paintMatch(p,matches[i].toObject(),rowRect(i));
    }
private:
    static bool recorded(const QJsonValue &v){return v.isDouble()&&std::isfinite(v.toDouble())&&v.toDouble()>=0;}
    static QJsonArray rectJson(QRect r){return {r.x(),r.y(),r.width(),r.height()};}
    static QJsonObject matchScores(const QJsonObject &match){const auto score=match.value("scores").toObject();if(!score.isEmpty())return score;for(const auto &value:match.value("players").toArray())if(value.toObject().value("is_target").toBool())return value.toObject().value("scores").toObject();return {};}
    static QString stat(const QJsonValue &v){return recorded(v)?QString::number(v.toDouble(),'f',0):QStringLiteral("—");}
    static QString kdaText(const QJsonObject &match){const auto score=matchScores(match);return stat(score.value("kills"))+QStringLiteral(" / ")+stat(score.value("deaths"))+QStringLiteral(" / ")+stat(score.value("assists"));}
    static QString durationText(const QJsonObject &match){const auto v=match.value("duration_seconds");if(!recorded(v))return QStringLiteral("—");const int seconds=int(std::min(v.toDouble(),86400.));return QStringLiteral("%1:%2").arg(seconds/60).arg(seconds%60,2,10,QChar('0'));}
    static QString resultText(const QJsonObject &match){const auto win=match.value("win");if(!win.isBool()&&!(recorded(win)&&(win.toDouble()==0||win.toDouble()==1)))return QStringLiteral("未知");return win.toBool(win.toDouble()!=0)?QStringLiteral("胜利"):QStringLiteral("失败");}
    QPoint offset(int layer)const{if(!Kda::depthEnabled()||Kda::reducedMotion())return {};const QPointF phase=property("lixinDepthOffset").toPointF();const QPoint limit=layer==0?QPoint(1,1):layer==1?QPoint(3,2):QPoint(6,3);return {qRound(phase.x()*limit.x()),qRound(phase.y()*limit.y())};}
    QRect recentRect()const{return {14,97,std::max(0,width()-28),std::max(108,height()-109)};}
    int rowCapacity()const{return std::clamp(recentRect().height()/48,4,8);}
    QRect rowRect(int index)const{const auto r=recentRect();const int capacity=rowCapacity(),top=r.top()+index*r.height()/capacity,bottom=r.top()+(index+1)*r.height()/capacity;return {r.left(),top,r.width(),bottom-top};}
    void syncActions(){
        all->setGeometry(recentRect().right()-103,68,104,26);all->setText(profile.isEmpty()?QStringLiteral("读取我的战绩"):QStringLiteral("全部战绩"));all->setEnabled(!loading||!profile.isEmpty());
        for(int i=0;i<int(rows.size());++i){const auto match=i<matches.size()?matches[i].toObject():QJsonObject{};const auto ref=match.value("match_ref").toString();auto action=rows[i];action->setGeometry(rowRect(i));action->setProperty("matchRef",ref);action->setVisible(i<rowCapacity()&&i<matches.size());action->setEnabled(!ref.isEmpty());const auto info=resultText(match)+QStringLiteral(" · ")+dataCatalog.championName(match.value("champion_id").toInt())+QStringLiteral(" · ")+kdaText(match)+QStringLiteral(" · ")+durationText(match);action->setAccessibleName(info);action->setToolTip(info+QStringLiteral("\n打开本人这场对局"));}
        QStringList tips;
        tips.append(QStringLiteral("平均KDA按逐局 (击杀+助攻)/max(1,阵亡) 均值；只计三项完整记录。"));
        tips.append(QStringLiteral("胜率 %1 场有效胜负；KDA %2 场完整记录。").arg(winSamples).arg(kdaSamples));
        tips.append(QStringLiteral("五维图与近期胜负可在全部战绩中展开查看。"));setToolTip(tips.join('\n'));
    }
    void draw(QPainter &p,QRect slot,const QString &text,int pixels,QColor ink,int layer,Kda::FontRole role=Kda::FontRole::Body,bool bold=false,int alignment=Qt::AlignLeft|Qt::AlignVCenter)const{
        const auto face=Kda::font(role,pixels,bold?QFont::DemiBold:QFont::Normal);const QFontMetrics metrics(face);auto delta=offset(layer);
        const int horizontal=layer==0?1:layer==1?3:6,vertical=std::max(0,(slot.height()-metrics.height())/2);delta.setY(std::clamp(delta.y(),-vertical,vertical));
        const auto inkSlot=slot.adjusted(horizontal,0,-horizontal,0);p.save();p.setClipRect(slot,Qt::IntersectClip);p.setFont(face);p.setPen(ink);
        p.drawText(inkSlot.translated(delta),alignment,metrics.elidedText(text,Qt::ElideRight,std::max(0,inkSlot.width())));p.restore();
    }
    void paintResults(QPainter &p)const{
        const int left=18+qRound((width()-36)*.57),extent=std::max(1,width()-left-18);
        draw(p,QRect(left,7,extent,18),windowCount?QStringLiteral("近 %1 局").arg(windowCount):QStringLiteral("近期胜负"),11,Kda::color("muted"),0,Kda::FontRole::Label);
        draw(p,QRect(left,28,extent,30),winSamples?QStringLiteral("%1 胜  /  %2 负").arg(wins).arg(winSamples-wins):QStringLiteral("—"),20,Kda::color("silver"),1,Kda::FontRole::Numeric);
    }
    void paintMatch(QPainter &p,const QJsonObject &match,QRect area){
        const auto win=resultText(match);auto ink=win==QStringLiteral("胜利")?Kda::skinColor("kda","ice"):win==QStringLiteral("失败")?Kda::color("danger"):Kda::color("muted");
        p.fillRect(QRect(area.left()-5,area.center().y()-8,2,16),ink);
        auto rule=Kda::color("border");rule.setAlpha(44);p.setPen(QPen(rule,.5));p.drawLine(area.bottomLeft()+QPoint(38,0),area.bottomRight());
        const bool roomy=area.height()>=40;const int iconSize=std::clamp(area.height()-10,22,44);const auto icon=assets->champion(match.value("champion_id").toInt());const QRect iconArea(area.left()+4,area.center().y()-iconSize/2,iconSize,iconSize);if(!icon.isNull())p.drawPixmap(iconArea.translated(offset(0)),Kda::roundedPixmap(icon,iconArea.size(),5,devicePixelRatioF()));
        else draw(p,iconArea,QStringLiteral("◇"),12,Kda::color("muted"),0);
        if(roomy){
            const int nameLeft=iconArea.right()+13,kdaLeft=area.right()-300;
            const bool compact=area.height()<45;
            draw(p,QRect(nameLeft,area.center().y()-(compact?20:22),std::max(1,kdaLeft-nameLeft-12),compact?21:24),dataCatalog.championName(match.value("champion_id").toInt()),compact?13:14,Kda::color("text"),1,Kda::FontRole::Body,true);
            draw(p,QRect(nameLeft,area.center().y()+1,70,compact?18:21),win,compact?10:12,ink,0);
            draw(p,QRect(kdaLeft,area.center().y()-16,150,32),kdaText(match),16,Kda::color("silver"),1,Kda::FontRole::Numeric,false,Qt::AlignRight|Qt::AlignVCenter);
            draw(p,QRect(area.right()-90,area.center().y()-13,64,26),durationText(match),12,Kda::color("muted"),2,Kda::FontRole::Numeric,false,Qt::AlignRight|Qt::AlignVCenter);
            return;
        }
        const int textLeft=iconArea.right()+9;
        draw(p,QRect(textLeft,area.top(),36,area.height()),win,12,ink,0);
        const int durationWidth=area.width()>=355?50:0,kdaWidth=area.width()>=355?100:91;
        const int nameLeft=textLeft+40,nameWidth=std::max(0,area.right()-durationWidth-kdaWidth-18-nameLeft);
        draw(p,QRect(nameLeft,area.top(),nameWidth,area.height()),dataCatalog.championName(match.value("champion_id").toInt()),12,Kda::color("text"),1);
        draw(p,QRect(area.right()-durationWidth-kdaWidth-8,area.top(),kdaWidth,area.height()),kdaText(match),12,Kda::color("silver"),1,Kda::FontRole::Numeric);
        if(durationWidth)draw(p,QRect(area.right()-durationWidth-2,area.top(),durationWidth,area.height()),durationText(match),12,Kda::color("muted"),2,Kda::FontRole::Numeric,false,Qt::AlignRight|Qt::AlignVCenter);
    }
    const Catalog &dataCatalog;HudAssets *assets;QJsonObject profile;QJsonArray matches;Kda::Button *all;std::array<RowAction*,8> rows{};
    int winSamples=0,kdaSamples=0,wins=0,windowCount=0;double kdaMean=0;bool loading=false;QString reason;
};
