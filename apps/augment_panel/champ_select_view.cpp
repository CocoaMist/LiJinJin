#include "champ_select_view.h"
#include "analysis.h"
#include "catalog.h"
#include "hud_assets.h"
#include "hud_reading_plane.h"
#include "kda_sparkles.h"
#include "kda_surface.h"
#include "kda_theme.h"
#include <QFontMetrics>
#include <QJsonArray>
#include <QMap>
#include <QPainter>
#include <QResizeEvent>
#include <QSet>
#include <QStringList>
#include <algorithm>
#include <cmath>

namespace {
QString q(const char *value){return QString::fromUtf8(value);}
int positiveId(const QJsonValue &value){
    if(!value.isDouble()||!std::isfinite(value.toDouble())||value.toDouble()<=0||value.toDouble()>2147483647.)return 0;
    const int id=value.toInt();return value.toDouble()==id?id:0;
}
const QList<QPair<QString,QString>> &roleLabels(){
    static const QList<QPair<QString,QString>> labels{{q("tank"),q("坦克")},{q("fighter"),q("战士")},{q("mage"),q("法师")},
        {q("marksman"),q("射手")},{q("support"),q("辅助")},{q("assassin"),q("刺客")}};
    return labels;
}
QJsonObject champion(const Catalog &catalog,int id){
    const auto entry=catalog.champion(id);const auto name=entry.value(q("name_zh_cn")).toString().trimmed();
    const bool known=id>0&&!entry.isEmpty()&&!name.isEmpty();QJsonArray roles;QStringList names;
    const auto raw=entry.value(q("roles")).toArray();
    for(const auto &label:roleLabels())for(const auto &value:raw)if(value.toString().toLower()==label.first){roles.append(label.first);names.append(label.second);break;}
    return {{q("champion_id"),id>0?QJsonValue(id):QJsonValue(QJsonValue::Null)},{q("known"),known},
        {q("name"),known?name:q("英雄待确认")},{q("roles"),roles},{q("role_label"),names.isEmpty()?q("定位待确认"):names.join(q(" · "))}};
}
QFont type(int pixels,bool strong=false){return Kda::font(Kda::FontRole::Body,pixels,strong?QFont::DemiBold:QFont::Medium);}
QJsonArray rectJson(const QRect &rect){return {rect.x(),rect.y(),rect.width(),rect.height()};}
QString displayed(const QString &text,const QRect &rect,int pixels,bool strong=false){return QFontMetrics(type(pixels,strong)).elidedText(text,Qt::ElideRight,std::max(0,rect.width()));}
bool textFits(const QRect &bounds,const QRect &rect,const QString &text,int pixels,bool strong=false){
    const QFontMetrics metrics(type(pixels,strong));return bounds.contains(rect)&&metrics.height()<=rect.height()&&metrics.horizontalAdvance(displayed(text,rect,pixels,strong))<=rect.width();
}
void line(QPainter &p,const QRect &rect,const QString &value,int pixels,const char *token,bool strong=false){
    if(rect.isEmpty())return;
    p.setFont(type(pixels,strong));p.setPen(Kda::HudStyle::color(token));p.drawText(rect,Qt::AlignLeft|Qt::AlignVCenter,displayed(value,rect,pixels,strong));
}
struct Cell {QRect rect,portrait,name,role;int index=0;};
struct Layout {
    QRect heading,heroPortrait,heroName,heroRole,heroSurface,allyHeading,composition[2],benchHeading,empty,footer;
    QList<Cell> cells;QString compositionText[2],allyText,footerText;int visible=0,remaining=0;bool compact=false;
};
Layout contentLayout(const QJsonObject &model,const QSize &size){
    Layout out;const int width=std::max(0,size.width()-16);
    out.heading={8,4,width,22};out.heroPortrait={8,32,36,36};out.heroName={52,28,std::max(0,width-44),22};out.heroRole={52,50,std::max(0,width-44),20};
    out.allyHeading={8,78,width,22};out.composition[0]={8,100,width,20};out.composition[1]={8,120,width,20};
    out.benchHeading={8,144,width,22};out.empty={8,170,width,22};out.footer={8,std::max(196,size.height()-28),width,22};
    const QFontMetrics metrics(type(12));QStringList parts;for(const auto &value:model.value(q("composition")).toArray()){
        const auto entry=value.toObject();parts.append(entry.value(q("label")).toString()+q(" ")+QString::number(entry.value(q("count")).toInt()));
    }
    if(parts.isEmpty())out.compositionText[0]=q("等待盟友选定英雄");
    else {int row=0;for(const auto &part:parts){if(!out.compositionText[0].isEmpty()&&metrics.horizontalAdvance(out.compositionText[0]+q(" · ")+part)>width)row=1;
        out.compositionText[row]+=(out.compositionText[row].isEmpty()?QString():q(" · "))+part;}
    }
    if(model.value(q("unknown_role_count")).toInt()>0&&out.compositionText[1].isEmpty())out.compositionText[1]=q("部分英雄定位待确认");
    const int count=model.value(q("visible_ally_count")).toInt();out.allyText=count>0?q("可见队伍 · %1 人").arg(count):q("可见队伍 · 等待选人");
    out.compact=size.height()<286;int bodyTop=170;
    if(out.compact){
        out.heading={};out.heroPortrait={8,6,32,32};out.heroName={46,2,std::max(0,width-38),22};out.heroRole={46,24,std::max(0,width-38),20};
        out.allyHeading={8,46,width,22};out.allyText=parts.isEmpty()?q("队伍 · 等待选人"):q("队伍 · ")+parts.join(q(" · "));
        out.composition[0]={};out.composition[1]={};out.benchHeading={8,68,width,22};bodyTop=90;out.empty={8,bodyTop,width,22};
        out.footer=size.height()>=160?QRect(8,size.height()-26,width,22):QRect{};
    }
    out.heroSurface=out.heroPortrait.united(out.heroName).united(out.heroRole).adjusted(-2,-2,2,2);
    const int columns=std::max(2,(width+4)/105),gap=4,cellWidth=std::max(0,(width-gap*(columns-1))/columns);
    const int contentBottom=out.footer.isEmpty()?size.height()-8:out.footer.top()-4;
    const int rows=std::clamp((contentBottom-bodyTop+4)/44,0,2),capacity=rows*columns;
    const auto candidates=model.value(q("bench")).toArray();out.visible=std::min(capacity,int(candidates.size()));out.remaining=int(candidates.size())-out.visible;
    for(int i=0;i<out.visible;++i){const QRect rect(8+(i%columns)*(cellWidth+gap),bodyTop+(i/columns)*44,cellWidth,40);
        out.cells.append({rect,{rect.x()+1,rect.y()+5,28,28},{rect.x()+34,rect.y(),std::max(0,cellWidth-35),20},{rect.x()+34,rect.y()+20,std::max(0,cellWidth-35),20},i});}
    out.footerText=out.remaining>0?q("另 %1 位候选 · 按空间收起").arg(out.remaining):q("仅展示已公开的选人信息");
    return out;
}
QString benchHeading(const QJsonObject &model,int remaining){return model.value(q("bench_known")).toBool()?(remaining>0?q("候选 %1 位 · 收起 %2 位").arg(model.value(q("bench")).toArray().size()).arg(remaining):q("可换英雄 · %1 位").arg(model.value(q("bench")).toArray().size())):q("可换英雄 · 等待");}
QString emptyBench(const QJsonObject &model){return model.value(q("bench_known")).toBool()?q("暂无可换英雄"):q("等待可换英雄信息");}
QString candidateRole(const QJsonObject &row,const QRect &rect){
    const auto role=row.value(q("role_label")).toString(),need=row.value(q("need_label")).toString();if(need.isEmpty())return role;
    const auto combined=need+q(" · ")+role;return QFontMetrics(type(12)).horizontalAdvance(combined)<=rect.width()?combined:need;
}
QRect flowFrame(const QRect &portrait){return portrait.adjusted(-1,-1,1,1);}
QList<QRect> flowFrames(const QJsonObject &model,const Layout &geometry){
    QList<QRect> frames;
    if(model.value(q("self")).toObject().value(q("known")).toBool())frames.append(flowFrame(geometry.heroPortrait));
    const auto bench=model.value(q("bench")).toArray();
    for(const auto &cell:geometry.cells)if(bench[cell.index].toObject().value(q("known")).toBool())frames.append(flowFrame(cell.portrait));
    return frames;
}
void portrait(QPainter &p,const QRect &rect,HudAssets *assets,int id,bool self=false){
    const qreal radius=Kda::radius("artwork");const auto image=assets&&id>0?assets->champion(id):QPixmap{};
    p.save();p.setPen(Qt::NoPen);p.setBrush(Kda::HudStyle::color("raised"));p.drawRoundedRect(rect,radius,radius);
    if(!image.isNull())p.drawPixmap(rect,Kda::roundedPixmap(image,rect.size(),radius,p.device()->devicePixelRatioF()));
    else{p.setFont(type(12));p.setPen(Kda::HudStyle::color("muted"));p.drawText(rect,Qt::AlignCenter,q("?"));}
    p.restore();HudReadingPlane::portraitFrame(p,rect,radius,self);
}
QString accessibleText(const QJsonObject &model){
    QStringList text{q("选人信息"),model.value(q("self")).toObject().value(q("name")).toString(),model.value(q("self")).toObject().value(q("role_label")).toString()};
    for(const auto &value:model.value(q("allies")).toArray()){const auto row=value.toObject();text.append(row.value(q("name")).toString()+q(" · ")+row.value(q("role_label")).toString());}
    const auto bench=model.value(q("bench")).toArray();if(bench.isEmpty())text.append(emptyBench(model));
    for(const auto &value:bench){const auto row=value.toObject();text.append(q("可换 ")+row.value(q("name")).toString()+q(" · ")+row.value(q("role_label")).toString()+(row.value(q("need_label")).toString().isEmpty()?QString():q(" · ")+row.value(q("need_label")).toString()));}
    if(model.value(q("frontline_reference")).toObject().value(q("available")).toBool())text.append(q("补位仅参考可见英雄的本地定位，不代表强度排名或胜率"));
    text.append(q("对手信息不在此面板展示"));return text.join(q("；"));
}
}

QJsonObject buildChampSelectModel(const Catalog &catalog,const QJsonObject &input){
    const bool active=input.value(q("active")).isBool()&&input.value(q("active")).toBool()&&input.value(q("stage"))==q("champ_select");
    QJsonArray allies,bench,unknowns,composition;QSet<int> selected,seenBench;QMap<QString,int> counts;int selfId=0,selves=0,rosterSelf=0,hiddenEnemies=0,unknownRoles=0,knownRoleAllies=0;
    const auto roster=active?input.value(q("roster")).toArray():QJsonArray{};
    for(const auto &value:roster){const auto row=value.toObject();if(row.value(q("team"))==q("ENEMY"))++hiddenEnemies;
        if(row.value(q("self")).toBool()){++selves;if(row.value(q("team"))==q("ALLY")&&row.value(q("visible"))!=false)rosterSelf=positiveId(row.value(q("champion_id")));}}
    const int declared=positiveId(input.value(q("self_champion_id")));
    if(active&&selves==1&&rosterSelf>0&&declared==rosterSelf)selfId=declared;
    if(!active)unknowns.append(q("waiting_for_current_champion_select"));
    else if(selfId<=0)unknowns.append(q("self_identity_not_uniquely_confirmed"));
    for(const auto &value:roster){const auto raw=value.toObject();if(raw.value(q("team"))!=q("ALLY")||raw.value(q("visible"))==false||allies.size()>=5)continue;
        const int id=positiveId(raw.value(q("champion_id")));if(id<=0)continue;
        auto row=champion(catalog,id);row[q("self")]=selfId>0&&raw.value(q("self")).toBool();allies.append(row);selected.insert(id);
        const auto roles=row.value(q("roles")).toArray();if(roles.isEmpty())++unknownRoles;else if(row.value(q("known")).toBool())++knownRoleAllies;
        for(const auto &role:roles)++counts[role.toString()];
    }
    if(selfId>0)selected.insert(selfId);
    const bool frontlineReference=selfId>0&&champion(catalog,selfId).value(q("known")).toBool()&&knownRoleAllies>=3&&unknownRoles==0&&counts.value(q("tank"))+counts.value(q("fighter"))==0;
    bool benchKnown=active&&input.value(q("bench_champion_ids")).isArray()&&(!input.contains(q("bench_known"))||input.value(q("bench_known")).toBool());int invalidBench=0;
    if(benchKnown)for(const auto &value:input.value(q("bench_champion_ids")).toArray()){
        const int id=positiveId(value);if(id<=0){++invalidBench;continue;}if(selected.contains(id)||seenBench.contains(id))continue;seenBench.insert(id);
        auto candidate=champion(catalog,id);const auto roles=candidate.value(q("roles")).toArray();const bool fills=frontlineReference&&candidate.value(q("known")).toBool()&&(roles.contains(q("tank"))||roles.contains(q("fighter")));
        candidate[q("can_fill_frontline")]=fills;if(fills)candidate[q("need_label")]=q("可补前排");bench.append(candidate);if(bench.size()>=64){unknowns.append(q("bench_projection_limited_to_64"));break;}}
    if(invalidBench>0){unknowns.append(q("bench_entries_unresolved"));if(bench.isEmpty())benchKnown=false;}
    if(!benchKnown)unknowns.append(q("bench_not_observed"));
    if(allies.isEmpty())unknowns.append(q("visible_allies_not_observed"));
    for(const auto &role:roleLabels())if(counts.value(role.first)>0)composition.append(QJsonObject{{q("role"),role.first},{q("label"),role.second},{q("count"),counts.value(role.first)}});
    return {{q("schema"),1},{q("available"),active},{q("stage"),active?q("champ_select"):q("waiting")},
        {q("source"),q("sanitized_visible_champion_select")},{q("self"),champion(catalog,selfId)},{q("allies"),allies},{q("visible_ally_count"),allies.size()},
        {q("composition"),composition},{q("unknown_role_count"),unknownRoles},{q("role_source"),q("local_catalog_roles")},
        {q("frontline_reference"),QJsonObject{{q("available"),frontlineReference},{q("known_role_allies"),knownRoleAllies},{q("scope"),q("visible_known_roles_only")},{q("kind"),q("role_fill_reference_not_rank")}}},
        {q("bench_known"),benchKnown},{q("bench_complete"),benchKnown&&invalidBench==0},{q("bench"),bench},{q("bench_status"),!benchKnown?q("waiting"):bench.isEmpty()?q("none_available"):q("visible_candidates")},
        {q("hidden_enemy_count"),hiddenEnemies},{q("opponent_policy"),q("not_displayed")},{q("unknowns"),unknowns},
        {q("ranked"),false},{q("performance_scores_available"),false},{q("automatic_actions"),false},{q("identities_exported"),false}};
}
QJsonObject buildChampSelectModel(const Catalog &catalog,const GameState &state){
    QJsonArray bench;for(int id:state.bench)bench.append(id);
    const auto context=state.matchContext;
    return buildChampSelectModel(catalog,QJsonObject{{q("active"),context.value(q("active")).toBool()&&state.phase==q("ChampSelect")},
        {q("stage"),context.value(q("stage"))},{q("self_champion_id"),state.championId},{q("roster"),context.value(q("roster"))},
        {q("bench_champion_ids"),bench},{q("bench_known"),state.benchKnown}});
}

ChampSelectView::ChampSelectView(const Catalog &c,HudAssets *a,QWidget *parent):QWidget(parent),catalog(c),assets(a),flow(new Kda::SparkleField(this,Kda::SparkleField::Activity::VisibleOverlay)){
    setObjectName(q("champSelectView"));setMinimumWidth(188);setMinimumHeight(minimumSizeHint().height());
    setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Preferred);setFocusPolicy(Qt::NoFocus);setAttribute(Qt::WA_TransparentForMouseEvents);
    setAccessibleName(q("选人信息"));applyModel(buildChampSelectModel(c,QJsonObject{}));
}
void ChampSelectView::setState(const GameState &state){applyModel(buildChampSelectModel(catalog,state));}
void ChampSelectView::setState(const QJsonObject &state){applyModel(buildChampSelectModel(catalog,state));}
void ChampSelectView::applyModel(const QJsonObject &value){if(snapshot==value)return;snapshot=value;setAccessibleDescription(accessibleText(snapshot));syncFlowRegion();updateGeometry();update();}
void ChampSelectView::syncFlowRegion(){
    QRegion region;for(const auto &frame:flowFrames(snapshot,contentLayout(snapshot,size())))region|=frame;
    flow->setRepaintRegion(region);flow->setEnabled(!region.isEmpty());
}
void ChampSelectView::resizeEvent(QResizeEvent *event){QWidget::resizeEvent(event);syncFlowRegion();}
QJsonObject ChampSelectView::model()const{return snapshot;}
QSize ChampSelectView::sizeHint()const{return {232,286};}
QSize ChampSelectView::minimumSizeHint()const{return {188,128};}
void ChampSelectView::paintEvent(QPaintEvent *){
    Kda::OverlayCanvas canvas(this);auto &p=canvas.paint();Kda::configurePainter(p);Kda::HudStyle::paintSurface(p,QRectF(rect()).adjusted(.5,.5,-.5,-.5),true);
    const auto geometry=contentLayout(snapshot,size());const auto self=snapshot.value(q("self")).toObject();
    line(p,geometry.heading,snapshot.value(q("available")).toBool()?q("选人 · 公开信息"):q("选人 · 等待客户端"),12,"silver",true);
    HudReadingPlane::paint(p,geometry.heroSurface,self.value(q("known")).toBool());
    portrait(p,geometry.heroPortrait,assets,positiveId(self.value(q("champion_id"))),self.value(q("known")).toBool());
    if(self.value(q("known")).toBool())flow->paintFlow(p,flowFrame(geometry.heroPortrait),.8*Kda::HudStyle::strength(),Kda::radius("artwork"));
    line(p,geometry.heroName,self.value(q("known")).toBool()?self.value(q("name")).toString():q("等待确认当前英雄"),14,"text",true);
    line(p,geometry.heroRole,self.value(q("role_label")).toString(),12,"muted");
    line(p,geometry.allyHeading,geometry.allyText,12,"silver",true);
    for(int i=0;i<2;++i)line(p,geometry.composition[i],geometry.compositionText[i],12,"muted");
    line(p,geometry.benchHeading,benchHeading(snapshot,geometry.remaining),12,"silver",true);
    const auto bench=snapshot.value(q("bench")).toArray();
    for(const auto &cell:geometry.cells){const auto row=bench[cell.index].toObject();HudReadingPlane::paint(p,cell.rect);portrait(p,cell.portrait,assets,positiveId(row.value(q("champion_id"))));
        if(row.value(q("known")).toBool())flow->paintFlow(p,flowFrame(cell.portrait),.5*Kda::HudStyle::strength(),Kda::radius("artwork"));
        line(p,cell.name,row.value(q("name")).toString(),12,"text");line(p,cell.role,candidateRole(row,cell.role),12,row.value(q("can_fill_frontline")).toBool()?"ice":"muted");}
    if(bench.isEmpty())line(p,geometry.empty,emptyBench(snapshot),12,"muted");
    line(p,geometry.footer,geometry.footerText,12,"muted");
}
QJsonObject ChampSelectView::report()const{
    const auto geometry=contentLayout(snapshot,size());const auto self=snapshot.value(q("self")).toObject();const auto bench=snapshot.value(q("bench")).toArray();
    bool fits=rect().contains(geometry.heroPortrait)&&rect().contains(geometry.heroSurface);QJsonArray textRows,cells;
    const auto check=[&](const QRect &r,const QString &text,int pixels,bool strong=false){if(r.isEmpty())return;const bool fit=textFits(rect(),r,text,pixels,strong);fits&=fit;
        textRows.append(QJsonObject{{q("rect"),rectJson(r)},{q("full_text"),text},{q("displayed_text"),displayed(text,r,pixels,strong)},{q("fits"),fit}});};
    check(geometry.heading,snapshot.value(q("available")).toBool()?q("选人 · 公开信息"):q("选人 · 等待客户端"),12,true);
    check(geometry.heroName,self.value(q("known")).toBool()?self.value(q("name")).toString():q("等待确认当前英雄"),14,true);
    check(geometry.heroRole,self.value(q("role_label")).toString(),12);check(geometry.allyHeading,geometry.allyText,12,true);
    for(int i=0;i<2;++i)check(geometry.composition[i],geometry.compositionText[i],12);
    check(geometry.benchHeading,benchHeading(snapshot,geometry.remaining),12,true);
    for(const auto &cell:geometry.cells){const auto row=bench[cell.index].toObject();fits&=rect().contains(cell.rect)&&cell.rect.contains(cell.portrait);
        check(cell.name,row.value(q("name")).toString(),12);check(cell.role,candidateRole(row,cell.role),12);
        cells.append(QJsonObject{{q("champion_id"),row.value(q("champion_id"))},{q("rect"),rectJson(cell.rect)},{q("portrait_rect"),rectJson(cell.portrait)}});}
    if(bench.isEmpty())check(geometry.empty,emptyBench(snapshot),12);check(geometry.footer,geometry.footerText,12);
    QJsonArray frames;for(const auto &frame:flowFrames(snapshot,geometry))frames.append(rectJson(frame));
    const auto flowClock=flow->report();
    return {{q("model"),snapshot},{q("content_fits"),fits},{q("visible_bench_count"),geometry.visible},{q("remaining_bench_count"),geometry.remaining},
        {q("bench_cells"),cells},{q("hero_reading_rect"),rectJson(geometry.heroSurface)},{q("text_rows"),textRows},{q("passive"),testAttribute(Qt::WA_TransparentForMouseEvents)&&focusPolicy()==Qt::NoFocus},
        {q("skin"),Kda::skin()},{q("material"),Kda::HudStyle::identity()},{q("compact"),geometry.compact},{q("reduced_motion"),Kda::reducedMotion()},{q("motion_active"),flowClock.value(q("running"))},
        {q("flow"),QJsonObject{{q("clock"),flowClock},{q("surface_rects"),frames},{q("mode"),q("portrait_frames_only")},{q("palette"),q("shared_kda_skin_tokens")},{q("particles_painted"),false},{q("content_animated"),false},{q("captures_input"),false}}},
        {q("hidden_enemy_count"),snapshot.value(q("hidden_enemy_count"))},{q("unknowns"),snapshot.value(q("unknowns"))},{q("device_pixel_ratio"),devicePixelRatioF()}};
}
