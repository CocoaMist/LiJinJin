#include "coach_live_hud.h"
#include "hud_assets.h"
#include "catalog.h"
#include "kda_theme.h"
#include "kda_surface.h"
#include "hud_reading_plane.h"
#include "hud_symbols.h"
#include <QPainter>
#include <QPaintEvent>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QToolTip>
#include <QJsonDocument>
#include <QSet>
#include <cmath>
#include <algorithm>

namespace {
QFont font(int px,bool bold=false,Kda::FontRole role=Kda::FontRole::Body){return Kda::hudFont(role,std::max(px,role==Kda::FontRole::Numeric?14:12),bold?QFont::DemiBold:QFont::Medium);}
void text(QPainter &p,const QRect &r,const QString &s,int px,const QColor &c,bool bold=false,Qt::Alignment a=Qt::AlignLeft|Qt::AlignVCenter,Kda::FontRole role=Kda::FontRole::Body){
    const auto f=font(px,bold,role);p.setFont(f);p.setPen(c);p.drawText(r,a,QFontMetrics(f).elidedText(s,Qt::ElideRight,std::max(0,r.width())));
}
QJsonArray rectJson(const QRect &r){return {r.x(),r.y(),r.width(),r.height()};}
QColor quietBorder(){auto color=Kda::HudStyle::color("border");color.setAlpha(110);return color;}
QColor teamInk(bool enemy,int alpha=255){auto color=Kda::skinColor(QStringLiteral("kda"),enemy?"lilac":"ice");color.setAlpha(alpha);return color;}
bool number(const QJsonValue &v){return v.isDouble()&&std::isfinite(v.toDouble())&&v.toDouble()>=0;}
QString count(const QJsonValue &v){if(!number(v))return QStringLiteral("—");const double n=v.toDouble();return n>=10000?QString::number(n/1000.,'f',1)+"k":QString::number(qRound(n));}
QString fullCount(const QJsonValue &v){return number(v)?QString::number(v.toDouble(),'f',0):QStringLiteral("—");}
QString kda(const QJsonObject &p){const auto s=p.value("scores").toObject();QStringList out;for(const auto &k:{"kills","deaths","assists"})out.append(count(s.value(k)));return out.join('/');}
bool pregameRoster(const QJsonObject &summary){return summary.value("source")==QStringLiteral("visible_champion_select");}
bool loadingRoster(const QJsonObject &summary){return pregameRoster(summary)&&summary.value("stage")==QStringLiteral("loading");}
QJsonObject selfPlayer(const QJsonObject &summary){
    for(const auto &value:summary.value("players").toArray()){
        const auto player=value.toObject();
        if(player.value("self").toBool())return player;
    }
    return {};
}
QString actualCounterMessage(const QJsonObject &summary,const QJsonObject &player){
    if(pregameRoster(summary))return loadingRoster(summary)?QStringLiteral("加载无统计"):QStringLiteral("选人无统计");
    if(number(player.value("damage_to_champions"))||number(player.value("damage_taken")))return {};
    if(!summary.value("fresh").toBool())return QStringLiteral("未连接对局");
    const auto source=summary.value("source").toString();
    if(source==QStringLiteral("live_read_only"))return QStringLiteral("接口未提供");
    if(source.contains(QStringLiteral("demo"))||source.contains(QStringLiteral("replay")))return QStringLiteral("无实测记录");
    return QStringLiteral("来源未确认");
}
QString actualCounterExplanation(const QJsonObject &summary,const QJsonObject &player){
    if(pregameRoster(summary))return loadingRoster(summary)?QStringLiteral("正在加载，尚无本局实测伤害与承伤。"):QStringLiteral("当前处于选人阶段，尚无本局实测伤害与承伤。");
    if(number(player.value("damage_to_champions"))||number(player.value("damage_taken")))
        return QStringLiteral("实测伤害 / 承伤：%1 / %2；未提供的一项显示 —。紧凑单位仅用于显示。").arg(fullCount(player.value("damage_to_champions")),fullCount(player.value("damage_taken")));
    if(!summary.value("fresh").toBool())return QStringLiteral("未连接到新鲜的对局数据，已清除旧局统计。");
    if(summary.value("source")==QStringLiteral("live_read_only"))return QStringLiteral("当前接口未提供累计伤害与承伤；这两项不会通过等待计算得出，也不以模型或历史值代替。");
    return QStringLiteral("此来源没有实测伤害与承伤记录；不以模型或历史值代替。");
}
void symbol(QPainter &p,QRect r,bool shield,const QColor &c){
    HudSymbols::draw(p,shield?HudSymbols::Symbol::Shield:HudSymbols::Symbol::Sword,r,c,1.3);
}
QString displayedEmptyText(const QString &source){
    if(source.contains(QStringLiteral("同版样本")))return QStringLiteral("同版样本");
    if(source.contains(QStringLiteral("金币未知")))return QStringLiteral("金币未知");
    if(source.contains(QStringLiteral("暂无组件")))return QStringLiteral("暂无组件");
    if(source.contains(QStringLiteral("待更新"))||source.contains(QStringLiteral("等待可购买")))return QStringLiteral("待同步");
    auto result=source.simplified();if(result.startsWith(QStringLiteral("可买 ")))result=result.mid(3).trimmed();
    return result.isEmpty()?QStringLiteral("待同步"):result;
}
HudSymbols::Symbol emptySymbol(const QString &source){
    return source.contains(QStringLiteral("同版样本"))?HudSymbols::Symbol::History:HudSymbols::Symbol::Coins;
}
void portrait(QPainter &p,const QRect &r,HudAssets *assets,int id,bool champion,bool focus=false){
    const auto image=assets?(champion?assets->champion(id):assets->item(id)):QPixmap{};
    const int side=std::min(r.width(),r.height());
    const qreal radius=side>=24?Kda::radius("artwork"):side>=18?Kda::radius("tag"):Kda::radius("small");
    p.save();p.setPen(Qt::NoPen);p.setBrush(Kda::HudStyle::color("raised"));p.drawRoundedRect(QRectF(r),radius,radius);
    if(!image.isNull())p.drawPixmap(r,Kda::roundedPixmap(image,r.size(),radius,p.device()->devicePixelRatioF()));
    else{p.setPen(Kda::HudStyle::color("muted"));p.drawRoundedRect(r.adjusted(4,4,-5,-5),Kda::radius("small"),Kda::radius("small"));}
    p.restore();
    if(champion){HudReadingPlane::portraitFrame(p,QRectF(r),radius,focus);return;}
    p.setPen(QPen(quietBorder(),.8));p.setBrush(Qt::NoBrush);p.drawRoundedRect(QRectF(r).adjusted(.5,.5,-.5,-.5),radius,radius);
}
struct StripSlot {int index;QRect icon,value,accent;};
QString stripSide(const QJsonObject &row){const auto side=row.value("team").toString();return side==QStringLiteral("ALLY")||side==QStringLiteral("ENEMY")?side:QStringLiteral("UNKNOWN");}
QString stripLifeState(const QJsonObject &row){const auto dead=row.value("is_dead");return dead.isBool()?(dead.toBool()?QStringLiteral("dead"):QStringLiteral("alive")):QStringLiteral("unknown");}
QColor stripTeamInk(const QJsonObject &row){const auto side=stripSide(row);return side==QStringLiteral("UNKNOWN")?Kda::skinColor(QStringLiteral("kda"),"silver"):teamInk(side==QStringLiteral("ENEMY"));}
int stripSelfCount(const QJsonArray &rows){int count=0;for(const auto &value:rows)count+=value.toObject().value("self")==QJsonValue(true);return count;}
QRect stripStateMarkerRect(const StripSlot &slot,const QString &state){
    const int inset=std::min(4,slot.icon.width()/3);
    return state==QStringLiteral("dead")?slot.icon.adjusted(inset,inset,-inset,-inset):state==QStringLiteral("unknown")?QRect(slot.icon.right()-6,slot.icon.bottom()-6,7,7):QRect();
}
QList<StripSlot> stripLayout(const QSize &size,const QJsonArray &rows,bool rosterMode){
    QList<StripSlot> result;
    if(rosterMode){
        QList<int> order;QStringList groups;
        for(const auto &team:{QStringLiteral("ALLY"),QStringLiteral("ENEMY"),QStringLiteral("UNKNOWN")})for(int i=0;i<rows.size();++i){
            if(stripSide(rows[i].toObject())!=team)continue;
            if(!groups.contains(team))groups.append(team);order.append(i);
        }
        const int columns=std::max(10,int(order.size())),teamBreaks=std::max(0,int(groups.size())-1);
        // Narrow scaled windows keep the portrait size by using the spare width for team gaps.
        const int gap=teamBreaks?std::clamp((size.width()-12*columns)/teamBreaks,0,6):0,gaps=teamBreaks*gap;
        const int cell=std::clamp((size.width()-gaps)/columns,12,31),side=std::min(20,cell-2);
        const int y=std::max(0,(size.height()-side-2)/2);int x=std::max(0,(size.width()-int(order.size())*cell-gaps)/2);QString previous;
        for(int index:order){
            const auto team=stripSide(rows[index].toObject());if(!previous.isEmpty()&&previous!=team)x+=gap;previous=team;
            const int iconX=x+(cell-side)/2;if(iconX+side>size.width())break;
            result.append({index,QRect(iconX,y,side,side),{},QRect(iconX,y+side+1,side,2)});x+=cell;
        }
    }else{
        const int capacity=std::min(3,std::max(0,(size.width()-24)/55));
        const int cell=capacity?std::min(65,(size.width()-24)/capacity):0;
        for(int i=0;i<std::min<int>(capacity,rows.size());++i){const int x=26+i*cell;
            result.append({i,QRect(x,2,20,20),QRect(x+23,0,cell-24,24),{}});
        }
    }
    return result;
}
bool paintedTextFits(const QRect &bounds,const QRect &line,const QString &value,int px,Kda::FontRole role=Kda::FontRole::Body){
    const QFontMetrics metrics(font(px,false,role));
    const auto displayed=metrics.elidedText(value,Qt::ElideRight,std::max(0,line.width()));
    return bounds.contains(line)&&line.height()>=metrics.height()&&metrics.horizontalAdvance(displayed)<=line.width();
}
struct SourceStatus {QString text;QColor color;bool live=false;const char *paletteToken="muted";};
SourceStatus sourceStatus(const QJsonObject &summary){
    const auto source=summary.value("source").toString();
    if(source==QStringLiteral("demo_or_replay"))return {QStringLiteral("演示/回放"),Kda::HudStyle::color("lilac"),false,"lilac"};
    if(source==QStringLiteral("demo")||source==QStringLiteral("synthetic_demo"))return {QStringLiteral("演示"),Kda::HudStyle::color("lilac"),false,"lilac"};
    if(source==QStringLiteral("replay")||source==QStringLiteral("synthetic_replay"))return {QStringLiteral("回放"),Kda::HudStyle::color("lilac"),false,"lilac"};
    if(pregameRoster(summary))return {loadingRoster(summary)?QStringLiteral("加载"):QStringLiteral("选人"),Kda::HudStyle::color("muted"),false};
    if(summary.value("fresh").toBool()&&source==QStringLiteral("live_read_only"))return {QStringLiteral("实况"),Kda::HudStyle::color("ready"),true,"ready"};
    return {summary.value("fresh").toBool()?QStringLiteral("来源未知"):QStringLiteral("离线"),Kda::HudStyle::color("muted"),false};
}
QRect sourceStatusRect(int width){return QRect(width-66,0,66,24);}
struct CounterText {QRect rect;QString rendered;int fontPx=14;bool fits=false;};
QFont counterFont(int px){return Kda::hudFont(Kda::FontRole::Numeric,std::max(12,px),QFont::DemiBold);}
qreal counterTextWidth(const QFontMetricsF &metrics,const QString &value){
    const auto ink=metrics.boundingRect(value);
    return std::max(metrics.horizontalAdvance(value),ink.right())-std::min(qreal(0),ink.left());
}
CounterText fittedCounter(const QJsonValue &value,const QRect &bounds){
    QStringList candidates{count(value)};
    if(number(value)&&value.toDouble()>=1000){
        const double n=value.toDouble();double scale=1000.;QString suffix=QStringLiteral("k");
        for(const auto &unit:{QStringLiteral("M"),QStringLiteral("B"),QStringLiteral("T")}){
            if(n<scale*1000.)break;scale*=1000.;suffix=unit;
        }
        candidates.append(QString::number(n/scale,'f',1)+suffix);
        candidates.append(QString::number(n/scale,'f',0)+suffix);
        auto scientific=QString::number(n,'e',0);scientific.replace(QStringLiteral("e+"),QStringLiteral("e"));
        scientific.replace(QStringLiteral("e0"),QStringLiteral("e"));candidates.append(scientific);
    }
    candidates.removeDuplicates();
    // Preserve precision before reducing the type size; never elide a known
    // counter. The complete observed integer also remains in the tooltip.
    for(const auto &candidate:candidates)for(int px=14;px>=12;--px){
        const QFontMetricsF metrics(counterFont(px));
        if(metrics.height()<=bounds.height()&&counterTextWidth(metrics,candidate)<=bounds.width())
            return {bounds,candidate,px,true};
    }
    return {bounds,candidates.last(),12,false};
}
struct CompactCounterLayout {QRect status,damageIcon,takenIcon;CounterText damage,taken;};
CompactCounterLayout compactCounterLayout(int width,const SourceStatus &status,const QJsonObject &player){
    CompactCounterLayout layout;
    const QFontMetricsF statusMetrics(font(10));
    const int statusWidth=int(std::ceil(std::max(statusMetrics.horizontalAdvance(status.text),statusMetrics.boundingRect(status.text).width())))+2;
    layout.status=QRect(std::max(0,width-statusWidth),0,statusWidth,24);
    const int contentWidth=std::max(0,layout.status.x()-5),cellWidth=std::max(0,(contentWidth-4)/2);
    layout.damageIcon=QRect(0,6,12,12);layout.takenIcon=QRect(cellWidth+4,6,12,12);
    layout.damage=fittedCounter(player.value("damage_to_champions"),QRect(15,0,std::max(0,cellWidth-15),24));
    layout.taken=fittedCounter(player.value("damage_taken"),QRect(cellWidth+19,0,std::max(0,contentWidth-cellWidth-19),24));
    return layout;
}
QJsonObject counterReport(const CounterText &counter,const QJsonValue &value){
    const QFontMetricsF metrics(counterFont(counter.fontPx));
    return {{"rendered",counter.rendered},{"full_value",fullCount(value)},{"known",number(value)},
        {"font_px",counter.fontPx},{"rect",rectJson(counter.rect)},{"text_width",counterTextWidth(metrics,counter.rendered)},
        {"text_height",metrics.height()},{"fits",counter.fits},{"elided",false}};
}
void drawCounter(QPainter &p,const CounterText &counter){
    p.setFont(counterFont(counter.fontPx));p.setPen(Kda::HudStyle::color("text"));
    p.drawText(counter.rect,Qt::AlignLeft|Qt::AlignVCenter,counter.rendered);
}
void drawSourceStatus(QPainter &p,const QRect &bounds,const SourceStatus &status){
    // This is provenance, not a button. Its explicit label carries the state
    // without repeating a frame around every compact information row.
    text(p,bounds,status.text,10,status.color,false,Qt::AlignRight|Qt::AlignVCenter);
}
QString identityKey(const QJsonObject &summary){
    QStringList identity{summary.value("source").toString(),summary.value("fresh").toBool()?QStringLiteral("fresh"):QStringLiteral("stale")};
    for(const auto &value:summary.value("players").toArray()){
        const auto row=value.toObject();identity.append(row.value("team").toString()+QLatin1Char(':')+QString::number(row.value("champion_id").toInt()));
    }
    return identity.join(QLatin1Char('|'));
}
QString rosterIdentityKey(const QJsonObject &summary){
    QStringList people;for(const auto &v:summary.value("players").toArray()){const auto p=v.toObject();people.append(p.value("team").toString()+QLatin1Char(':')+QString::number(p.value("champion_id").toInt())+QLatin1Char(':')+QString::number(p.value("row_index").toInt(-1))+QLatin1Char(':')+(p.value("self").toBool()?QStringLiteral("self"):QStringLiteral("other")));}
    people.sort();return summary.value("context_id").toString()+QLatin1Char('|')+people.join(QLatin1Char('|'));
}
QString rosterSide(const QJsonObject &player){const auto side=player.value("team").toString();return side==QStringLiteral("ALLY")||side==QStringLiteral("ENEMY")?side:QStringLiteral("UNKNOWN");}
int denseHeadingHeight(const QJsonObject &summary){
    QStringList sides;const auto players=summary.value("players").toArray();for(const auto &v:players){const auto side=rosterSide(v.toObject());if(!sides.contains(side))sides.append(side);}
    // Standard 5+5 teams get readable 17px headings. A third, unknown team
    // uses graphic separators so no participant is omitted to meet the bound.
    return sides.size()>2&&players.size()<=10?10:17;
}
QList<int> observedIds(const QJsonArray &values,int limit){QList<int> ids;for(const auto &v:values){const int id=v.toInt();if(v.isDouble()&&id>0&&v.toDouble()==id)ids.append(id);if(ids.size()>=limit)break;}return ids;}
struct DenseRowLayout{QRect portrait,role,state;QList<QRect> items,augments;QRect augmentMarker;};
DenseRowLayout denseRowLayout(const QRect &row,const QJsonObject &player){
    DenseRowLayout out;const int y=row.y(),side=row.width()>=206?14:12,stride=side+2;
    out.portrait=QRect(2,y,18,18);out.role=QRect(24,y+3,12,12);out.state=QRect(row.width()-14,y+3,12,12);
    const auto items=observedIds(player.value("items").toArray(),6);for(int i=0;i<items.size();++i)out.items.append(QRect(40+i*stride,y+(18-side)/2,side,side));
    const int augmentStart=40+6*stride+2,capacity=std::max(0,(out.state.x()-4-augmentStart+2)/14);
    const auto augments=observedIds(player.value("augment_ids").toArray(),100);const bool marker=augments.size()>capacity||!player.value("augments_complete").toBool();
    const int visible=std::min(int(augments.size()),std::max(0,capacity-(marker?1:0)));
    for(int i=0;i<visible;++i)out.augments.append(QRect(augmentStart+i*14,y+3,12,12));
    if(marker&&capacity>0)out.augmentMarker=QRect(augmentStart+visible*14,y+3,12,12);
    return out;
}
QString denseRole(const QJsonObject &player){
    const auto roles=player.value("tactical_role").toObject().value("roles").toArray();
    for(const auto &v:roles){const auto s=v.toString();if(s.contains(QStringLiteral("开团")))return QStringLiteral("engage");if(s.contains(QStringLiteral("保护"))||s.contains(QStringLiteral("辅助"))||s.contains(QStringLiteral("增益")))return QStringLiteral("support");if(s.contains(QStringLiteral("承伤")))return QStringLiteral("tank");if(s.contains(QStringLiteral("伤害"))||s.contains(QStringLiteral("输出")))return QStringLiteral("damage");}return QStringLiteral("unknown");
}
HudSymbols::Symbol denseRoleSymbol(const QString &role){if(role==QStringLiteral("engage"))return HudSymbols::Symbol::Link;if(role==QStringLiteral("support"))return HudSymbols::Symbol::Star;if(role==QStringLiteral("tank"))return HudSymbols::Symbol::Shield;if(role==QStringLiteral("damage"))return HudSymbols::Symbol::Sword;return HudSymbols::Symbol::Info;}
QString denseState(const QJsonObject &player){if(player.value("is_dead").isBool()&&player.value("is_dead").toBool())return QStringLiteral("dead");if(player.value("self").toBool())return QStringLiteral("self");if(!player.value("is_dead").isBool())return QStringLiteral("unknown");if(!player.value("items_complete").toBool()||!player.value("augments_complete").toBool())return QStringLiteral("incomplete");return QStringLiteral("alive");}
HudSymbols::Symbol denseStateSymbol(const QString &state){if(state==QStringLiteral("dead"))return HudSymbols::Symbol::Close;if(state==QStringLiteral("self"))return HudSymbols::Symbol::Check;if(state==QStringLiteral("incomplete"))return HudSymbols::Symbol::Warning;return HudSymbols::Symbol::Info;}
}

CoachItemStrip::CoachItemStrip(const Catalog &c,HudAssets *a,QWidget *parent):QWidget(parent),catalog(c),assets(a){setObjectName("components");setMinimumWidth(0);setFixedHeight(24);setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);setAttribute(Qt::WA_TransparentForMouseEvents);}
void CoachItemStrip::setComponents(const QJsonArray &v,const QString &empty){if(!rosterMode&&components==v&&emptyText==empty)return;rosterMode=false;components=v;emptyText=empty;roster={};setToolTip(text());update();}
void CoachItemStrip::setRoster(const QJsonArray &v){
    if(rosterMode&&roster==v)return;
    // Live rows also carry timestamps and model details that this strip does not draw.
    const bool samePortraits=rosterMode&&std::equal(roster.cbegin(),roster.cend(),v.cbegin(),v.cend(),[](const auto &a,const auto &b){
        const auto before=a.toObject(),after=b.toObject();
        for(const auto *key:{"champion_id","team","is_dead","self"})if(before.value(key)!=after.value(key))return false;
        return true;
    });
    roster=v;
    if(samePortraits)return;
    rosterMode=true;components={};emptyText={};setToolTip(text());update();
}
bool CoachItemStrip::hasVisualContent()const{return rosterMode?!roster.isEmpty():!components.isEmpty();}
QString CoachItemStrip::text()const{if(rosterMode)return QStringLiteral("阵容 %1 人").arg(roster.size());QStringList s;for(const auto &v:components){const auto p=v.toObject();s.append(catalog.itemName(p.value("item_id").toInt())+" "+count(p.value("remaining_cost"))+"G");}return s.isEmpty()?emptyText:s.join(QStringLiteral(" / "));}
void CoachItemStrip::paintEvent(QPaintEvent *){
    Kda::OverlayCanvas canvas(this);auto &p=canvas.paint();
    const auto renderedSlots=stripLayout(size(),rosterMode?roster:components,rosterMode);
    if(rosterMode){
        const bool uniqueSelf=stripSelfCount(roster)==1;
        for(const auto &slot:renderedSlots){
            const auto row=roster[slot.index].toObject();const auto state=stripLifeState(row);const auto marker=stripStateMarkerRect(slot,state);
            portrait(p,slot.icon,assets,row.value("champion_id").toInt(),true);
            if(state==QStringLiteral("dead")){
                HudReadingPlane::deadPortrait(p,slot.icon,Kda::radius("tag"),165);
                HudSymbols::draw(p,HudSymbols::Symbol::Close,marker,Kda::skinColor(QStringLiteral("kda"),"danger"),1.6);
            }else if(state==QStringLiteral("unknown")){
                p.setPen(Qt::NoPen);p.setBrush(Kda::HudStyle::color("bg"));p.drawEllipse(QRectF(marker));
                p.setPen(QPen(Kda::skinColor(QStringLiteral("kda"),"silver"),1.2,Qt::SolidLine,Qt::RoundCap));
                p.drawLine(QPointF(marker.left()+2,marker.center().y()+.5),QPointF(marker.right()-2,marker.center().y()+.5));
            }
            if(uniqueSelf&&row.value("self")==QJsonValue(true)){
                p.setBrush(Qt::NoBrush);p.setPen(QPen(Kda::skinColor(QStringLiteral("kda"),"silver"),1.4));
                p.drawRoundedRect(QRectF(slot.icon).adjusted(.65,.65,-.65,-.65),Kda::radius("tag"),Kda::radius("tag"));
            }
            p.setPen(Qt::NoPen);p.setBrush(stripTeamInk(row));p.drawRoundedRect(QRectF(slot.accent),1,1);
        }
        return;
    }
    HudSymbols::draw(p,components.isEmpty()?emptySymbol(emptyText):HudSymbols::Symbol::Coins,QRectF(0,4,16,16),Kda::HudStyle::color("muted"));
    if(components.isEmpty()){::text(p,QRect(24,0,std::max(0,width()-24),24),displayedEmptyText(emptyText),10,Kda::HudStyle::color("muted"));return;}
    for(const auto &slot:renderedSlots){const auto row=components[slot.index].toObject();portrait(p,slot.icon,assets,row.value("item_id").toInt(),false);::text(p,slot.value,count(row.value("remaining_cost")),11,Kda::HudStyle::color("silver"),false,Qt::AlignLeft|Qt::AlignVCenter,Kda::FontRole::Numeric);}
}
QJsonObject CoachItemStrip::report()const{
    const auto renderedSlots=stripLayout(size(),rosterMode?roster:components,rosterMode);QJsonArray geometry;
    bool fits=true;const bool uniqueSelf=rosterMode&&stripSelfCount(roster)==1;int selfMarkers=0,observedDead=0,unknownState=0;
    for(const auto &slot:renderedSlots){fits&=rect().contains(slot.icon);
        if(rosterMode)fits&=rect().contains(slot.accent);
        else fits&=paintedTextFits(rect(),slot.value,count(components[slot.index].toObject().value("remaining_cost")),11,Kda::FontRole::Numeric);
        QJsonObject entry{{"source_index",slot.index},{"icon_rect",rectJson(slot.icon)},{"value_rect",rectJson(slot.value)},{"accent_rect",rectJson(slot.accent)}};
        if(rosterMode){
            const auto row=roster[slot.index].toObject();const auto state=stripLifeState(row);const auto marker=stripStateMarkerRect(slot,state);const bool self=uniqueSelf&&row.value("self")==QJsonValue(true);
            selfMarkers+=self;observedDead+=state==QStringLiteral("dead");unknownState+=state==QStringLiteral("unknown");
            if(!marker.isEmpty())fits&=slot.icon.contains(marker);
            entry["champion_id"]=row.value("champion_id");entry["team"]=stripSide(row);entry["team_color"]=stripTeamInk(row).name();entry["self"]=row.value("self")==QJsonValue(true);entry["self_marker"]=self;
            entry["is_dead"]=row.value("is_dead").isBool()?row.value("is_dead"):QJsonValue(QJsonValue::Null);entry["life_state"]=state;
            entry["state_marker"]=state==QStringLiteral("dead")?QStringLiteral("cross"):state==QStringLiteral("unknown")?QStringLiteral("unknown_dash"):QString();entry["state_marker_rect"]=rectJson(marker);
        }
        geometry.append(entry);
    }
    if(!rosterMode)fits&=rect().contains(QRect(0,4,16,16))&&(!components.isEmpty()
        ||paintedTextFits(rect(),QRect(24,0,std::max(0,width()-24),24),displayedEmptyText(emptyText),10));
    const bool allVisible=renderedSlots.size()==(rosterMode?roster.size():components.size());
    QJsonObject out{{"theme",Kda::HudStyle::identity()},{"skin_strength",Kda::HudStyle::strength()},{"numeric_font_family",Kda::font(Kda::FontRole::Numeric,11).family()},{"rounded_icons",true},
        {"motion",QJsonObject{{"enabled",false},{"reason","static_components_and_roster_strip"}}},
        {"mode",rosterMode?"roster":"components"},{"has_visual_content",hasVisualContent()},{"uses_item_icons",!rosterMode},{"uses_champion_icons",rosterMode},
        {"roster_count",rosterMode?roster.size():0},{"visible_count",renderedSlots.size()},{"components",components},{"text",text()},
        {"leading_symbol",rosterMode?QString():components.isEmpty()&&emptySymbol(emptyText)==HudSymbols::Symbol::History?QStringLiteral("history"):QStringLiteral("coins")},
        {"displayed_empty_text",!rosterMode&&components.isEmpty()?displayedEmptyText(emptyText):QString()},
        {"slot_geometry",geometry},{"render_fits",fits},{"all_content_visible",allVisible},
        {"text_fits",fits&&(!rosterMode||allVisible)},{"runtime_network",false}};
    if(rosterMode){out["team_color_binding"]="fixed_team_semantics";out["team_colors"]=QJsonArray{teamInk(false).name(),teamInk(true).name()};out["self_marker_count"]=selfMarkers;out["observed_dead_count"]=observedDead;out["unknown_state_count"]=unknownState;out["state_counts_scope"]="rendered_slots";out["state_source"]="observed_is_dead_boolean_only";out["passive"]=true;out["self_identity_unambiguous"]=uniqueSelf;}
    return out;
}

CoachLiveHud::CoachLiveHud(const Catalog &c,HudAssets *a,bool small,QWidget *parent):QWidget(parent),catalog(c),assets(a),compact(small){
    setMinimumWidth(0);setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);setObjectName(small?"liveCounters":"liveRoster");
    if(compact)setFixedHeight(24);else setMinimumHeight(100);
    setMouseTracking(false);setToolTip(QString());
    motion=new Kda::Transition(this,220);
}
QList<CoachLiveHud::Row> CoachLiveHud::orderedRows()const{
    QList<Row> result;const auto players=summary.value("players").toArray();
    for(const auto &side:{QStringLiteral("ALLY"),QStringLiteral("ENEMY"),QStringLiteral("UNKNOWN")}){
        for(int i=0;i<players.size();++i){const auto row=players[i].toObject();if(rosterSide(row)!=side)continue;result.append({row,{},row.value("row_index").toInt(i)});}
    }return result;
}
int CoachLiveHud::rosterPlayerCount()const{return int(orderedRows().size());}
int CoachLiveHud::rosterPageCount()const{const int count=rosterPlayerCount();return count==0?0:pageSize>0&&!compact&&!denseRoster?(count+pageSize-1)/pageSize:1;}
int CoachLiveHud::rosterPage()const{return pageIndex;}
void CoachLiveHud::setDenseRoster(bool value){if(compact||denseRoster==value)return;denseRoster=value;pageIndex=0;refreshRosterGeometry();update();}
QList<CoachLiveHud::Row> CoachLiveHud::rows()const{
    if(denseRoster&&!compact){auto result=orderedRows();int y=20;QString side;for(auto &row:result){const auto team=rosterSide(row.player);if(side!=team){side=team;y+=denseHeadingHeight(summary);}row.rect=QRect(0,y,width(),18);y+=18;}return result;}
    const auto ordered=orderedRows();QList<Row> result;int y=28;QString side;
    const int begin=pageSize>0&&!compact?pageIndex*pageSize:0,end=pageSize>0&&!compact?std::min(int(ordered.size()),begin+pageSize):int(ordered.size());
    for(int i=begin;i<end;++i){auto row=ordered[i];const auto team=row.player.value("team").toString();if(team!=side){y+=22;side=team;}row.rect=QRect(0,y,width(),66);result.append(row);y+=66;}
    return result;
}
int CoachLiveHud::rosterContentHeight()const{if(compact)return 24;const auto rr=rows();if(denseRoster)return rr.isEmpty()?40:rr.last().rect.bottom()+3;return rr.isEmpty()?110:rr.last().rect.bottom()+5;}
QSize CoachLiveHud::sizeHint()const{return QSize(228,rosterContentHeight());}
void CoachLiveHud::refreshRosterGeometry(){
    if(compact)return;
    const int height=rosterContentHeight();if(pageSize>0||denseRoster)setFixedHeight(height);else{setMaximumHeight(QWIDGETSIZE_MAX);setMinimumHeight(height);}
    updateGeometry();
}
void CoachLiveHud::setRosterPageSize(int size){
    if(compact||denseRoster)return;const int next=std::clamp(size,1,5);if(pageSize==next)return;pageSize=next;
    pageIndex=std::clamp(pageIndex,0,std::max(0,rosterPageCount()-1));refreshRosterGeometry();update();
}
void CoachLiveHud::setRosterPage(int page){
    if(compact||denseRoster)return;const int next=std::clamp(page,0,std::max(0,rosterPageCount()-1));if(pageIndex==next)return;
    pageIndex=next;refreshRosterGeometry();update();
}
void CoachLiveHud::setSummary(const QJsonObject &v){
    auto next=v;if(!next.value("fresh").toBool()&&next.value("source")!=QStringLiteral("visible_champion_select"))next["players"]=QJsonArray{};
    if(summary==next)return;
    const auto self=selfPlayer(next);
    bool repaint=true,identityChanged=false;
    if(compact){
        // The compact surface displays only source state and the self counters.
        const auto previousSelf=selfPlayer(summary);
        repaint=summary.value("source")!=next.value("source")||summary.value("fresh")!=next.value("fresh")
            ||loadingRoster(summary)!=loadingRoster(next)
            ||previousSelf.value("damage_to_champions")!=self.value("damage_to_champions")
            ||previousSelf.value("damage_taken")!=self.value("damage_taken");
    }else{
        const auto key=identityKey(next);identityChanged=motionKey!=key;motionKey=key;
        const auto nextRosterIdentity=rosterIdentityKey(next);if(rosterIdentity!=nextRosterIdentity)pageIndex=0;
        rosterIdentity=nextRosterIdentity;
    }
    summary=next;
    const auto caption=actualCounterMessage(summary,self)+QStringLiteral("；")+actualCounterExplanation(summary,self);
    if(!toolTip().isEmpty())setToolTip(QString());
    if(accessibleName()!=caption)setAccessibleName(caption);
    if(!compact){
        pageIndex=std::clamp(pageIndex,0,std::max(0,rosterPageCount()-1));
        refreshRosterGeometry();if(identityChanged&&!denseRoster)motion->trigger();
    }
    if(repaint)update();
}
void CoachLiveHud::paintEvent(QPaintEvent *event){
    Kda::OverlayCanvas canvas(this);auto &p=canvas.paint();const bool fresh=summary.value("fresh").toBool();const auto players=summary.value("players").toArray();
    // One reading surface protects all counters and players. Team and self
    // emphasis are quiet bands within it, without nested opaque cards.
    HudReadingPlane::paint(p,QRectF(rect()).adjusted(0,0,-.5,-.5));
    if(denseRoster&&!compact){paintDenseRoster(p);return;}
    QJsonObject self;for(const auto &v:players)if(v.toObject().value("self").toBool()){self=v.toObject();break;}
    if(compact){
        const auto status=sourceStatus(summary);const auto counters=compactCounterLayout(width(),status,self);
        const auto unavailable=actualCounterMessage(summary,self);
        const auto statusRect=unavailable.isEmpty()?counters.status:sourceStatusRect(width());
        if(!unavailable.isEmpty())text(p,QRect(0,0,std::max(0,statusRect.x()-6),24),unavailable,12,Kda::HudStyle::color("muted"));
        else{
            symbol(p,counters.damageIcon,false,Kda::HudStyle::color("silver"));drawCounter(p,counters.damage);
            symbol(p,counters.takenIcon,true,Kda::HudStyle::color("silver"));drawCounter(p,counters.taken);
        }
        // A contained, named state remains recognizable without relying on a
        // tiny colored dot. Keep the full source label and counter geometry.
        drawSourceStatus(p,statusRect,status);return;
    }
    const auto status=sourceStatus(summary);const auto statusRect=sourceStatusRect(width());
    if(pregameRoster(summary)){
        HudSymbols::draw(p,HudSymbols::Symbol::Team,QRectF(2,5,14,14),Kda::HudStyle::color("muted"));
        text(p,QRect(22,0,std::max(0,statusRect.x()-26),24),QStringLiteral("阵容"),12,Kda::HudStyle::color("silver"));
    }else{
        HudSymbols::draw(p,HudSymbols::Symbol::Clock,QRectF(2,5,14,14),Kda::HudStyle::color("muted"));
        text(p,QRect(22,0,50,24),QStringLiteral("8s ≈"),14,Kda::HudStyle::color("silver"),true,Qt::AlignLeft|Qt::AlignVCenter,Kda::FontRole::Numeric);
        symbol(p,QRect(76,5,14,14),false,Kda::HudStyle::color("silver"));symbol(p,QRect(100,5,14,14),true,Kda::HudStyle::color("silver"));
    }
    drawSourceStatus(p,statusRect,status);
    p.setPen(QPen(Kda::HudStyle::color("border"),1));p.drawLine(0,27,width(),27);
    if(motion->running()){auto highlight=Kda::HudStyle::color("ice");highlight.setAlphaF(std::sin(motion->progress()*3.141592653589793)*.55);p.setPen(highlight);p.drawLine(2,27,width()-2,27);}
    const auto rr=rows();if(rr.isEmpty()){text(p,QRect(2,54,width()-4,26),pregameRoster(summary)?(loadingRoster(summary)?QStringLiteral("载入"):QStringLiteral("选人")):!fresh?QStringLiteral("断开"):QStringLiteral("同步中"),12,Kda::HudStyle::color("muted"));return;}
    double maxOutput=1,maxHealth=1;for(const auto &r:rr){if(number(r.player.value("output_8s_estimate")))maxOutput=std::max(maxOutput,r.player.value("output_8s_estimate").toDouble());if(number(r.player.value("effective_health_estimate")))maxHealth=std::max(maxHealth,r.player.value("effective_health_estimate").toDouble());}
    QString side;for(const auto &r:rr){const auto row=r.player;const int y=r.rect.y(),id=row.value("champion_id").toInt();
        if(!event->rect().intersects(r.rect.adjusted(0,-22,0,0))){side=row.value("team").toString();continue;}
        if(row.value("team").toString()!=side){side=row.value("team").toString();const auto label=side=="ALLY"?QStringLiteral("我方"):side=="ENEMY"?QStringLiteral("对手"):QStringLiteral("队伍待确认");
            const QRect heading(2,y-22,width()-4,22);text(p,heading,label,16,Kda::HudStyle::color("silver"),true,Qt::AlignLeft|Qt::AlignVCenter,Kda::FontRole::Title);
            HudReadingPlane::headingRule(p,heading,QFontMetrics(::font(16,true,Kda::FontRole::Title)).horizontalAdvance(label),side=="ENEMY"?"lilac":"silver");}
        const bool isSelf=row.value("self").toBool();
        if(isSelf){p.fillRect(r.rect,HudReadingPlane::ink("silver",12));p.setPen(QPen(Kda::HudStyle::color("silver"),1.2));p.drawLine(1,y+4,1,y+r.rect.height()-5);}
        auto rowRule=quietBorder();rowRule.setAlpha(65);p.setPen(QPen(rowRule,.7));p.drawLine(40,y+21,width()-2,y+21);
        portrait(p,QRect(3,y+3,30,30),assets,id,true,isSelf);
        if(row.value("is_dead").toBool()){HudReadingPlane::deadPortrait(p,QRectF(3,y+3,30,30),Kda::radius("artwork"),175);HudSymbols::draw(p,HudSymbols::Symbol::Close,QRectF(9,y+9,18,18),Kda::HudStyle::color("danger"),2);}
        text(p,QRect(3,y+36,30,22),number(row.value("level"))?QString::number(row.value("level").toInt()):QStringLiteral("—"),14,Kda::HudStyle::color(isSelf?"silver":"muted"),true,Qt::AlignCenter,Kda::FontRole::Numeric);
        text(p,QRect(40,y,72,22),kda(row),14,Kda::HudStyle::color("text"),true,Qt::AlignLeft|Qt::AlignVCenter,Kda::FontRole::Numeric);
        HudSymbols::draw(p,HudSymbols::Symbol::Coins,QRectF(116,y+4,14,14),Kda::HudStyle::color("muted"));
        text(p,QRect(134,y,std::max(0,width()-164),22),count(row.value("equipment_value")),14,Kda::HudStyle::color("silver"),true,Qt::AlignLeft|Qt::AlignVCenter,Kda::FontRole::Numeric);
        if(row.value("historical_reference").toObject().value("available").toBool())HudSymbols::draw(p,HudSymbols::Symbol::History,QRectF(width()-28,y+5,12,12),Kda::HudStyle::color("lilac"));
        if(row.value("model_partial").toBool())HudSymbols::draw(p,HudSymbols::Symbol::Warning,QRectF(width()-14,y+5,12,12),Kda::HudStyle::color("muted"));
        const auto items=row.value("items").toArray();for(int i=0;i<std::min<int>(6,items.size());++i)portrait(p,QRect(40+i*20,y+23,18,18),assets,items[i].toInt(),false);
        if(!row.value("items_complete").toBool())HudSymbols::draw(p,HudSymbols::Symbol::Warning,QRectF(std::min(width()-13,164),y+25,12,12),Kda::HudStyle::color("muted"));
        const auto augments=row.value("augment_ids").toArray();
        const int augmentSlots=std::min({8,int(augments.size()),std::max(0,(width()-40-(row.value("augments_complete").toBool()?0:14))/18)});
        for(int i=0;i<augmentSlots;++i){const QRect box(40+i*18,y+44,16,16);
            if(i==augmentSlots-1&&augments.size()>augmentSlots){text(p,box,QStringLiteral("+%1").arg(augments.size()-i),12,Kda::HudStyle::color("lilac"),true,Qt::AlignCenter,Kda::FontRole::Numeric);continue;}
            const auto art=assets?assets->augment(augments[i].toInt()):QPixmap{};
            if(!art.isNull())p.drawPixmap(box,Kda::roundedPixmap(art,box.size(),2,p.device()->devicePixelRatioF()));
            else HudSymbols::draw(p,HudSymbols::Symbol::Star,box.adjusted(3,3,-3,-3),Kda::HudStyle::color("silver"));
        }
        if(!row.value("augments_complete").toBool()){
            if(augments.isEmpty())text(p,QRect(40,y+41,std::max(0,width()-56),22),QStringLiteral("海克斯 —"),12,Kda::HudStyle::color("muted"));
            else HudSymbols::draw(p,HudSymbols::Symbol::Warning,QRectF(std::min(width()-13,40+augmentSlots*18),y+46,12,12),Kda::HudStyle::color("muted"));
        }
        const int barWidth=std::max(1,(width()-48)/2);for(int stat=0;stat<2;++stat){const int x=40+stat*(barWidth+4);const auto value=row.value(stat?"effective_health_estimate":"output_8s_estimate");
            p.setPen(Qt::NoPen);p.setBrush(Kda::HudStyle::color("raised"));p.drawRoundedRect(QRectF(x,y+63,barWidth,2),1,1);
            if(number(value)){const auto filled=barWidth*std::clamp(value.toDouble()/(stat?maxHealth:maxOutput),0.,1.);if(filled>0){
                QLinearGradient fill(QPointF(x,y+63),QPointF(x+barWidth,y+63));fill.setColorAt(0,Kda::HudStyle::color(stat?"muted":"silver"));fill.setColorAt(1,Kda::HudStyle::color(stat?"lilac":"ice"));
                p.setBrush(fill);p.drawRoundedRect(QRectF(x,y+63,filled,2),1,1);}}
        }
    }

}
void CoachLiveHud::paintDenseRoster(QPainter &p){
    const auto status=sourceStatus(summary);const QString label=pregameRoster(summary)?QStringLiteral("阵容"):QStringLiteral("双方阵容");
    HudSymbols::draw(p,HudSymbols::Symbol::Team,QRect(2,4,12,12),Kda::HudStyle::color("muted"));
    text(p,QRect(20,0,std::max(0,width()-88),20),label,12,Kda::HudStyle::color("silver"));drawSourceStatus(p,QRect(width()-66,0,66,20),status);
    QString side;const int heading=denseHeadingHeight(summary);const auto roster=rows();
    if(roster.isEmpty()){text(p,QRect(2,20,width()-4,20),pregameRoster(summary)?QStringLiteral("阵容未公开"):QStringLiteral("未连接对局"),12,Kda::HudStyle::color("muted"));return;}
    // Team bands share the one content surface. A row never acquires its own
    // card or padding, so the full ten-person roster keeps its compact height.
    for(int first=0;first<roster.size();){int last=first;
        while(last+1<roster.size()&&rosterSide(roster[last+1].player)==rosterSide(roster[first].player))++last;
        const QRect group=roster[first].rect.united(roster[last].rect);
        const auto team=rosterSide(roster[first].player);p.fillRect(group,team==QStringLiteral("UNKNOWN")?HudReadingPlane::ink("silver",5):teamInk(team==QStringLiteral("ENEMY"),6));first=last+1;
    }
    for(const auto &r:roster){const auto player=r.player;const auto team=rosterSide(player);const auto boxes=denseRowLayout(r.rect,player);
        if(side!=team){side=team;const QRect group(0,r.rect.top()-heading,width(),heading);const auto color=side==QStringLiteral("UNKNOWN")?Kda::HudStyle::color("muted"):teamInk(side==QStringLiteral("ENEMY"));
            if(heading>=16){const auto label=side==QStringLiteral("ALLY")?QStringLiteral("我方"):side==QStringLiteral("ENEMY")?QStringLiteral("对手"):QStringLiteral("队伍未知");
                HudSymbols::draw(p,HudSymbols::Symbol::Team,QRect(2,group.y()+2,12,12),color);const QRect title(19,group.y(),width()-21,heading);text(p,title,label,12,color);
                HudReadingPlane::headingRule(p,title,QFontMetrics(::font(12)).horizontalAdvance(label),side==QStringLiteral("ENEMY")?"lilac":"silver");}
            else{p.setPen(QPen(color,1));p.drawLine(2,group.center().y(),width()-3,group.center().y());}
        }
        if(player.value("self").toBool()){p.fillRect(r.rect,HudReadingPlane::ink("silver",14));p.setPen(QPen(Kda::HudStyle::color("silver"),1.2));p.drawLine(0,r.rect.top()+2,0,r.rect.bottom()-2);}
        portrait(p,boxes.portrait,assets,player.value("champion_id").toInt(),true,player.value("self").toBool());
        if(player.value("is_dead").isBool()&&player.value("is_dead").toBool()){HudReadingPlane::deadPortrait(p,boxes.portrait,Kda::radius("small"),160);HudSymbols::draw(p,HudSymbols::Symbol::Close,boxes.portrait.adjusted(3,3,-3,-3),Kda::HudStyle::color("danger"));}
        const auto role=denseRole(player);HudSymbols::draw(p,player.value("items_complete").toBool()?denseRoleSymbol(role):HudSymbols::Symbol::Warning,boxes.role,Kda::HudStyle::color(role==QStringLiteral("unknown")||!player.value("items_complete").toBool()?"muted":"silver"));
        const auto items=observedIds(player.value("items").toArray(),6);for(int i=0;i<boxes.items.size();++i)portrait(p,boxes.items[i],assets,items[i],false);
        const auto augments=observedIds(player.value("augment_ids").toArray(),100);for(int i=0;i<boxes.augments.size();++i){const auto art=assets?assets->augment(augments[i]):QPixmap{};if(!art.isNull())p.drawPixmap(boxes.augments[i],Kda::roundedPixmap(art,boxes.augments[i].size(),2,p.device()->devicePixelRatioF()));else HudSymbols::draw(p,HudSymbols::Symbol::Star,boxes.augments[i],Kda::HudStyle::color("muted"));}
        if(!boxes.augmentMarker.isEmpty())HudSymbols::draw(p,player.value("augments_complete").toBool()?HudSymbols::Symbol::More:HudSymbols::Symbol::Warning,boxes.augmentMarker,Kda::HudStyle::color("muted"));
        const auto state=denseState(player);if(state!=QStringLiteral("alive"))HudSymbols::draw(p,denseStateSymbol(state),boxes.state,Kda::HudStyle::color(state==QStringLiteral("dead")?"danger":state==QStringLiteral("self")?"ice":"muted"));
    }
}
QString CoachLiveHud::tooltip(const Row &r)const{
    const auto historyCoverage=r.player.value("history_coverage").toObject();
    const auto historyStatus=historyCoverage.isEmpty()?QString():historyCoverage.value("known").toBool()
        ?QStringLiteral("历史 %1 场 · 同版 %2 场 · %3 · %4").arg(count(historyCoverage.value("games")),count(historyCoverage.value("same_patch_games")),historyCoverage.value("analysis_consumed").toBool()?QStringLiteral("分析已接入"):QStringLiteral("分析未就绪"),historyCoverage.value("freshness")==QStringLiteral("fresh")?QStringLiteral("已刷新"):QStringLiteral("需刷新"))
        :QStringLiteral("历史记录尚缺，未按低水平或零表现计算。");
    QStringList equipment;for(int id:observedIds(r.player.value("items").toArray(),6))equipment.append(catalog.itemName(id));
    const QString loadout=QStringLiteral("装备：")+(equipment.isEmpty()?(r.player.value("items_complete").toBool()?QStringLiteral("空装备"):QStringLiteral("—，未完整读取")):equipment.join(QStringLiteral(" / ")));
    const QString denseStatus=QStringLiteral("等级 %1 · %2%3").arg(count(r.player.value("level")),r.player.value("self").toBool()?QStringLiteral("你 · "):QString(),r.player.value("is_dead").isBool()?(r.player.value("is_dead").toBool()?QStringLiteral("已阵亡"):QStringLiteral("存活")):QStringLiteral("存活状态 —"));
    if(pregameRoster(summary)){
        QStringList lines{catalog.championName(r.player.value("champion_id").toInt()),actualCounterExplanation(summary,r.player),QStringLiteral("来源：")+sourceStatus(summary).text};
        const auto history=r.player.value("historical_reference").toObject();if(history.value("available").toBool()){const auto m=history.value("metrics").toObject();lines.append(QStringLiteral("历史同版 %1 场 · 场均 %2 / %3 / %4；仅作历史参考。").arg(history.value("sampled_games").toInt()).arg(count(m.value("kills")),count(m.value("deaths")),count(m.value("assists"))));}
        if(!historyStatus.isEmpty())lines.append(historyStatus);if(denseRoster)lines.append(denseStatus);lines.append(loadout);if(!r.player.value("items_complete").toBool())lines.append(QStringLiteral("装备清单未完整读取，未知项不按空装备计算。"));if(!r.player.value("augments_complete").toBool())lines.append(QStringLiteral("海克斯未完整读取。"));return lines.join('\n');
    }
    const auto p=r.player;QStringList lines{catalog.championName(p.value("champion_id").toInt())+QStringLiteral(" · ")+kda(p),QStringLiteral("装备价值 ")+count(p.value("equipment_value"))+"G",
        actualCounterExplanation(summary,p),
        QStringLiteral("8秒模型输出≈ %1 · 有效生命≈ %2").arg(count(p.value("output_8s_estimate")),count(p.value("effective_health_estimate"))),QStringLiteral("未提供的实测值显示 —；输出为8秒模型，有效生命为抗性折算。"),QStringLiteral("来源：")+sourceStatus(summary).text};
    if(!historyStatus.isEmpty())lines.append(historyStatus);if(denseRoster)lines.append(denseStatus);lines.append(loadout);const auto role=p.value("tactical_role").toObject();QStringList responsibilities;for(const auto &v:role.value("roles").toArray())responsibilities.append(v.toString());
    if(!responsibilities.isEmpty())lines.append(QStringLiteral("职责估计：")+responsibilities.join(QStringLiteral(" / "))+QStringLiteral("；开团技能冷却未知"));
    if(role.value("respawn_in").isDouble())lines.append(QStringLiteral("观察到的复活倒计时：%1秒；不等于回到战场时间").arg(role.value("respawn_in").toDouble(),0,'f',1));
    QStringList augmentNames;for(const auto &id:p.value("augment_ids").toArray())augmentNames.append(catalog.augments.value(id.toInt()).value("name_zh_cn").toString(QString::number(id.toInt())));if(!augmentNames.isEmpty())lines.append(QStringLiteral("海克斯：")+augmentNames.join(QStringLiteral(" / ")));
    if(!p.value("augments_complete").toBool())lines.append(QStringLiteral("当前接口没有完整选牌记录；点击此行可补录海克斯，不从候选或终局装备猜选牌。"));
    if(!p.value("items_complete").toBool())lines.append(QStringLiteral("装备清单尚未完整读取，缺项未按空装备计算。"));
    if(!number(p.value("output_8s_estimate"))||!number(p.value("effective_health_estimate")))lines.append(QStringLiteral("当前英雄规则或输入不足，未生成完整的输出/有效生命模型。"));
    const auto history=p.value("historical_reference").toObject();if(history.value("available").toBool()){const auto m=history.value("metrics").toObject();lines.append(QStringLiteral("历史同版 %1 场 · 场均 %2 / %3 / %4").arg(history.value("sampled_games").toInt()).arg(count(m.value("kills")),count(m.value("deaths")),count(m.value("assists"))));}
    if(p.value("model_partial").toBool())lines.append(QStringLiteral("未知海克斯或触发未计入，模型不完整。"));for(const auto &v:p.value("model_unknowns").toArray())if(lines.size()<10)lines.append(v.toString());return lines.join('\n');
}
bool CoachLiveHud::event(QEvent *e){if(e->type()==QEvent::ToolTip){QToolTip::hideText();return true;}return QWidget::event(e);}
void CoachLiveHud::mousePressEvent(QMouseEvent *e){if(!compact&&e->button()==Qt::LeftButton&&onPlayer)for(const auto &r:rows())if(r.rect.contains(e->position().toPoint())){onPlayer(r.index);e->accept();return;}QWidget::mousePressEvent(e);}
QJsonObject CoachLiveHud::denseRosterReport()const{
    QJsonArray geometry,groups;QList<QRect> occupiedRows;int itemCount=0,artworkCount=0,augmentCount=0;bool fits=width()>=170,unique=true;QSet<int> identities;
    const auto status=sourceStatus(summary);const QRect sourceRect(width()-66,0,66,20),labelRect(20,0,std::max(0,width()-88),20);
    const QString label=pregameRoster(summary)?QStringLiteral("阵容"):QStringLiteral("双方阵容");
    const bool statusFits=paintedTextFits(rect(),sourceRect,status.text,12)&&QFontMetrics(::font(12)).horizontalAdvance(status.text)<=sourceRect.width();
    fits&=statusFits&&paintedTextFits(rect(),labelRect,label,12)&&QFontMetrics(::font(12)).horizontalAdvance(label)<=labelRect.width();
    QString side;const int heading=denseHeadingHeight(summary);QJsonObject self;
    for(const auto &r:rows()){
        const auto player=r.player;const auto team=rosterSide(player);if(player.value("self").toBool())self=player;
        if(side!=team){side=team;const QRect group(0,r.rect.y()-heading,width(),heading);groups.append(QJsonObject{{"team",team},{"rect",rectJson(group)},{"label_visible",heading>=16}});fits&=rect().contains(group);if(heading>=16)fits&=paintedTextFits(rect(),QRect(19,group.y(),width()-21,heading),side==QStringLiteral("ALLY")?QStringLiteral("我方"):side==QStringLiteral("ENEMY")?QStringLiteral("对手"):QStringLiteral("队伍未知"),12);}
        const auto boxes=denseRowLayout(r.rect,player);QJsonArray itemRects,itemIds,augmentRects,augmentIds;QList<QRect> icons{boxes.portrait,boxes.role};
        const auto items=observedIds(player.value("items").toArray(),6);for(int i=0;i<boxes.items.size();++i){itemRects.append(rectJson(boxes.items[i]));itemIds.append(items[i]);icons.append(boxes.items[i]);if(assets&&!assets->item(items[i]).isNull())++artworkCount;}
        const auto augments=observedIds(player.value("augment_ids").toArray(),100);for(int i=0;i<boxes.augments.size();++i){augmentRects.append(rectJson(boxes.augments[i]));augmentIds.append(augments[i]);icons.append(boxes.augments[i]);}
        if(!boxes.augmentMarker.isEmpty())icons.append(boxes.augmentMarker);const auto state=denseState(player);if(state!=QStringLiteral("alive"))icons.append(boxes.state);
        bool rowFits=rect().contains(r.rect);for(int i=0;i<icons.size();++i){rowFits&=r.rect.contains(icons[i]);for(int j=0;j<i;++j)rowFits&=!icons[i].intersects(icons[j]);}
        for(const auto &prior:occupiedRows)rowFits&=!prior.intersects(r.rect);occupiedRows.append(r.rect);fits&=rowFits;
        unique&=!identities.contains(r.index);identities.insert(r.index);itemCount+=items.size();augmentCount+=boxes.augments.size();
        geometry.append(QJsonObject{{"row_index",r.index},{"team",team},{"champion_id",player.value("champion_id")},{"rect",rectJson(r.rect)},
            {"champion_portrait_rect",rectJson(boxes.portrait)},{"champion_name_in_tooltip",true},{"self",player.value("self").toBool()},{"is_dead",player.value("is_dead")},
            {"self_marker",player.value("self").toBool()?QStringLiteral("portrait_outline"):QString()},{"state_icon",state},{"state_icon_rect",state==QStringLiteral("alive")?QJsonArray{}:rectJson(boxes.state)},
            {"role_icon",player.value("items_complete").toBool()?denseRole(player):QStringLiteral("equipment_warning")},{"inferred_role",denseRole(player)},{"role_icon_rect",rectJson(boxes.role)},{"equipment_missing_icon_rect",player.value("items_complete").toBool()?QJsonArray{}:rectJson(boxes.role)},{"item_icon_rects",itemRects},{"item_ids",itemIds},{"item_icon_count",items.size()},{"items_complete",player.value("items_complete").toBool()},
            {"augment_icon_rects",augmentRects},{"visible_augment_ids",augmentIds},{"augment_marker_rect",rectJson(boxes.augmentMarker)},{"augment_count",augments.size()},{"visible_augment_count",boxes.augments.size()},{"augments_complete",player.value("augments_complete").toBool()},
            {"equipment_value",count(player.value("equipment_value"))},{"kda",kda(player)},{"missing_augments_label",!player.value("augments_complete").toBool()?QStringLiteral("海克斯未完整读取"):QString()},
            {"data_explanation",tooltip(r)},{"content_fits",rowFits}});
    }
    const bool allTen=rosterPlayerCount()==10&&geometry.size()==10&&fits&&unique;
    return {{"theme",Kda::HudStyle::identity()},{"skin_strength",Kda::HudStyle::strength()},{"compact",false},{"dense_mode",true},{"all_ten_same_screen",allTen},{"no_pagination",true},{"pagination_enabled",false},{"page_size",0},{"page_count",rosterPageCount()},{"page_index",0},
        {"total_players",rosterPlayerCount()},{"visible_players",geometry.size()},{"item_icon_count",itemCount},{"item_artwork_count",artworkCount},{"augment_icon_count",augmentCount},{"row_geometry",geometry},{"rows",geometry},{"team_groups",groups},
        {"fresh",summary.value("fresh")},{"source",summary.value("source")},{"stage",summary.value("stage")},{"source_status",status.text},{"source_status_rect",rectJson(sourceRect)},{"source_status_full_text_fits",statusFits},{"live_indicator",status.live},{"status_color",status.color.name()},{"status_palette_token",QString::fromLatin1(status.paletteToken)},
        {"displayed_roster_label",label},{"displayed_model_label",pregameRoster(summary)?label:QString()},{"model_label",pregameRoster(summary)?label:QStringLiteral("8s 输出 / 有效生命 · 估算")},{"model_values_in_tooltip",!pregameRoster(summary)},
        {"damage_display",count(self.value("damage_to_champions"))},{"taken_display",count(self.value("damage_taken"))},{"actual_damage_source",summary.value("actual_damage_source")},{"actual_values_unknown_are_dash",!number(self.value("damage_to_champions"))&&!number(self.value("damage_taken"))},{"actual_values_not_model_or_history",true},{"actual_counter_explanation",actualCounterExplanation(summary,self)},
        {"row_height",18},{"champion_icon_size",18},{"item_icon_minimum_size",12},{"minimum_label_font_px",12},{"uses_champion_and_item_icons",true},{"uses_augment_icons",true},{"uses_shared_symbols",true},{"content_fits",fits&&unique},{"geometry",rectJson(rect())},{"takes_keyboard_focus",focusPolicy()!=Qt::NoFocus},
        {"layering","continuous_surface_with_team_bands"},{"palette_binding","theme_tokens_and_stable_team_semantics"},{"rounded_death_masks",true},{"data_surface",HudReadingPlane::surfaceIdentity()},{"portrait_finish","restrained_skin_rim"},{"decoration_placement","portrait_edges_and_heading_negative_space"},{"inset_frames",false},{"self_emphasis","subtle_row_wash_and_portrait_outline"},
        {"motion",QJsonObject{{"enabled",false},{"running",false},{"animates_geometry",false},{"animates_text_or_numbers",false},{"trigger_policy","static_dense_roster"}}},{"runtime_network",false}};
}
QJsonObject CoachLiveHud::report()const{
    if(denseRoster&&!compact)return denseRosterReport();
    QJsonArray geometry;bool fits=width()>=170;QJsonObject self;for(const auto &v:summary.value("players").toArray())if(v.toObject().value("self").toBool()){self=v.toObject();break;}
    const auto status=sourceStatus(summary);const auto counters=compactCounterLayout(width(),status,self);
    const auto unavailable=actualCounterMessage(summary,self);
    const auto statusRect=compact&&unavailable.isEmpty()?counters.status:sourceStatusRect(width());
    const QRect unavailableRect(0,0,std::max(0,statusRect.x()-6),24);
    const bool fullStatusFits=paintedTextFits(rect(),statusRect,status.text,10)&&QFontMetrics(::font(10)).horizontalAdvance(status.text)<=statusRect.width();
    const bool pregame=pregameRoster(summary);const QRect rosterLabelRect(22,0,std::max(0,statusRect.x()-26),24);
    if(!compact&&pregame)fits&=fullStatusFits&&paintedTextFits(rect(),rosterLabelRect,QStringLiteral("阵容"),12)
        &&QFontMetrics(::font(12)).horizontalAdvance(QStringLiteral("阵容"))<=rosterLabelRect.width();
    const bool counterValuesFit=counters.damage.fits&&counters.taken.fits
        &&rect().contains(counters.damage.rect)&&rect().contains(counters.taken.rect)
        &&rect().contains(counters.damageIcon)&&rect().contains(counters.takenIcon)
        &&counters.damage.rect.right()<counters.takenIcon.left()
        &&counters.taken.rect.right()<counters.status.left();
    if(compact)fits&=height()>=24&&fullStatusFits&&(unavailable.isEmpty()?counterValuesFit:paintedTextFits(rect(),unavailableRect,unavailable,12));else for(const auto &r:rows()){
        const int y=r.rect.y();const auto history=r.player.value("historical_reference").toObject();
        geometry.append(QJsonObject{{"row_index",r.index},{"team",r.player.value("team")},{"champion_id",r.player.value("champion_id")},{"rect",rectJson(r.rect)},
            {"champion_portrait_rect",rectJson(QRect(3,y+3,30,30))},{"champion_name_in_tooltip",true},
            {"kda_rect",rectJson(QRect(40,y,72,22))},{"equipment_value",count(r.player.value("equipment_value"))},
            {"missing_augments_label",r.player.value("augment_ids").toArray().isEmpty()&&!r.player.value("augments_complete").toBool()?QStringLiteral("海克斯未读取"):QString()},{"data_explanation",tooltip(r)},
            {"historical_reference_icon",history.value("available").toBool()},{"model_partial_icon",r.player.value("model_partial").toBool()},{"augment_count",r.player.value("augment_ids").toArray().size()},{"visible_augment_count",std::min({8,int(r.player.value("augment_ids").toArray().size()),std::max(0,(width()-40-(r.player.value("augments_complete").toBool()?0:14))/18)})}});
        fits&=rect().contains(r.rect);
    }
    auto motionReport=motion->report();motionReport["trigger_policy"]=compact?"static_actual_counters":"champion_identity_or_data_source_only";
    motionReport["animates_text_or_numbers"]=false;motionReport["animates_geometry"]=false;motionReport["loops"]=false;
    return {{"theme",Kda::HudStyle::identity()},{"skin_strength",Kda::HudStyle::strength()},{"motion",motionReport},{"rounded_icons",true},{"numeric_font_family",Kda::font(Kda::FontRole::Numeric,14).family()},
        {"pagination_enabled",!compact&&pageSize>0},{"page_size",pageSize},{"page_count",rosterPageCount()},{"page_index",pageIndex},{"total_players",rosterPlayerCount()},{"visible_players",compact?0:geometry.size()},
        {"compact",compact},{"dense_mode",false},{"fresh",summary.value("fresh")},{"source",summary.value("source")},{"stage",summary.value("stage")},{"source_status",status.text},{"source_status_rect",rectJson(statusRect)},{"source_status_full_text_fits",fullStatusFits},{"live_indicator",status.live},{"status_color",status.color.name()},{"status_palette_token",QString::fromLatin1(status.paletteToken)},
        {"actual_damage_source",summary.value("actual_damage_source")},{"damage_display",count(self.value("damage_to_champions"))},{"taken_display",count(self.value("damage_taken"))},{"actual_values_unknown_are_dash",!number(self.value("damage_to_champions"))&&!number(self.value("damage_taken"))},{"actual_values_not_model_or_history",true},{"model_label",pregame?QStringLiteral("阵容"):QStringLiteral("8s 输出 / 有效生命 · 估算")},
        {"actual_counter_mode",unavailable.isEmpty()?QStringLiteral("observed_values"):QStringLiteral("source_explanation")},{"actual_counter_explanation",actualCounterExplanation(summary,self)},{"displayed_actual_label",compact?unavailable:QString()},{"actual_counter_message_rect",rectJson(unavailableRect)},{"displayed_model_label",compact?QString():pregame?QStringLiteral("阵容"):QStringLiteral("8s ≈")},
        {"actual_counter_values_fit",counterValuesFit},{"actual_counter_damage",counterReport(counters.damage,self.value("damage_to_champions"))},{"actual_counter_taken",counterReport(counters.taken,self.value("damage_taken"))},
        {"actual_counter_damage_icon_rect",rectJson(counters.damageIcon)},{"actual_counter_taken_icon_rect",rectJson(counters.takenIcon)},
        {"uses_shared_symbols",true},{"design","platinum_ice_portrait_roster"},{"material_placement","champion_portrait_edges_and_heading_negative_space"},{"team_heading_font_px",16},{"data_surface",HudReadingPlane::surfaceIdentity()},
        {"layering",compact?"stable_counters":"continuous_surface_with_player_rows"},{"palette_binding","theme_tokens"},{"rounded_death_masks",true},{"inset_frames",false},
        {"self_emphasis","subtle_row_wash_and_portrait_outline"},{"self_full_row_fill",false},{"model_bar_track","raised"},{"model_values_in_tooltip",!pregame},{"row_height",66},
        {"uses_champion_and_item_icons",!compact},{"uses_augment_icons",!compact},{"minimum_numeric_font_px",compact?12:14},{"minimum_label_font_px",12},{"rasterization","2x_physical_grayscale_resolve"},{"rows",geometry},{"content_fits",fits},{"geometry",rectJson(rect())},{"runtime_network",false}};
}
