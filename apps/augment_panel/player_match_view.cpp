#include "player_match_view.h"
#include "catalog.h"
#include "hud_assets.h"
#include "hud_symbols.h"
#include "kda_controls.h"
#include "kda_identity_plate.h"
#include "server_names.h"
#include "kda_glass_surface.h"
#include "kda_depth_plane.h"
#include <QApplication>
#include <QDynamicPropertyChangeEvent>
#include <QDateTime>
#include <QJsonArray>
#include <QPainter>
#include <QPolygonF>
#include <QListWidget>
#include <QStyledItemDelegate>
#include <QStackedWidget>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QHelpEvent>
#include <QToolTip>
#include <QTimer>
#include <QPushButton>
#include <QButtonGroup>
#include <QResizeEvent>
#include <QScrollBar>
#include <QRegularExpression>
#include <QHash>
#include <QImage>
#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace {
class SkinStyleBinding final : public QObject {
public:
    SkinStyleBinding(QWidget *owner,std::function<void()> refresh):QObject(owner),refreshStyles(std::move(refresh)){
        refreshStyles();if(qApp)qApp->installEventFilter(this);
    }
protected:
    bool eventFilter(QObject *watched,QEvent *event)override{
        if(watched==qApp&&event->type()==QEvent::DynamicPropertyChange&&static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName()=="lixinSkin")refreshStyles();
        return QObject::eventFilter(watched,event);
    }
private:std::function<void()> refreshStyles;
};
QColor themeInk(const char *token,int alpha=255){auto color=Kda::color(token);color.setAlpha(alpha);return color;}
// Result, team and damage-type identity must stay distinguishable from the
// red skin's decorative accent. Other skins retain their existing semantics.
QColor semanticInk(const char *token){return Kda::skin()==QStringLiteral("kda_red")?Kda::skinColor(QStringLiteral("kda"),token):Kda::color(token);}
QString number(const QJsonValue &v,int precision=0){return v.isDouble()&&std::isfinite(v.toDouble())?QString::number(v.toDouble(),'f',precision):QStringLiteral("—");}
QString axisValue(const QJsonObject &axis){const auto value=axis.value("value");return value.isDouble()&&std::isfinite(value.toDouble())?number(value,axis.value("key")==QStringLiteral("deaths_per_minute")?2:1)+axis.value("unit").toString():QStringLiteral("—");}
QString fittedAxisValue(const QJsonObject &axis,int width){
    const auto full=axisValue(axis);const QFontMetrics metrics(Kda::font(Kda::FontRole::Numeric,12));
    if(metrics.horizontalAdvance(full)<=width)return full;
    const auto raw=axis.value("value");if(!raw.isDouble()||!std::isfinite(raw.toDouble()))return full;
    const double value=raw.toDouble(),magnitude=std::abs(value);const auto unit=axis.value("unit").toString();
    const double scale=magnitude>=1e9?1e9:magnitude>=1e6?1e6:magnitude>=1e3?1e3:1.;
    const auto suffix=scale==1e9?QStringLiteral("G"):scale==1e6?QStringLiteral("M"):scale==1e3?QStringLiteral("k"):QString();
    if(scale>1)for(int precision:{1,0}){const auto compact=QString::number(value/scale,'f',precision)+suffix+unit;if(metrics.horizontalAdvance(compact)<=width)return compact;}
    for(int precision:{3,2}){const auto compact=QString::number(value,'g',precision)+unit;if(metrics.horizontalAdvance(compact)<=width)return compact;}
    return full;
}
QString axisEvidence(const QJsonObject &axis){
    QStringList lines;const auto value=axis.value("value"),count=axis.value("n");
    if(value.isDouble()&&std::isfinite(value.toDouble()))lines.append(count.isDouble()&&count.toInt()>0?QStringLiteral("%1 · %2 场有效记录，逐局等权均值。").arg(axis.value("label").toString()).arg(count.toInt()):QStringLiteral("%1 · 有记录的历史均值，有效场次未记录。").arg(axis.value("label").toString()));
    else lines.append(QStringLiteral("%1 · 已保存对局没有这一项记录，无法计算；更新战绩后如来源提供该项才会补齐。").arg(axis.value("label").toString()));
    const auto percentile=axis.value("percentile");if(value.isDouble()&&std::isfinite(value.toDouble())&&value.toDouble()>=0&&percentile.isDouble()&&std::isfinite(percentile.toDouble())&&percentile.toDouble()>=0&&percentile.toDouble()<=100&&axis.value("percentile_n").toInt()>0)
        lines.append(QStringLiteral("同局相对位置 %1/100 · %2 场完整十人记录。50 为中位参考；只比较各自同局十人，非全服百分位。").arg(number(percentile,1)).arg(axis.value("percentile_n").toInt()));
    else lines.append(QStringLiteral("同局相对位置未记录，雷达此轴留空；不会用 0 补齐。"));
    if(axis.value("key")==QStringLiteral("deaths_per_minute")||axis.value("lower_is_better").toBool())lines.append(QStringLiteral("生存按每分钟阵亡次数反向：越少越靠外。"));
    if(axis.value("key")==QStringLiteral("damage_taken_per_minute"))lines.append(QStringLiteral("承伤仅表示每分钟承受伤害更多，不等于表现更好。"));
    lines.append(QStringLiteral("英雄职责未校正；五维描述记录，不直接评价实力。"));return lines.join('\n');
}
QString score(const QJsonObject &p){const auto s=p.value("scores").toObject();return number(s.value("kills"))+" / "+number(s.value("deaths"))+" / "+number(s.value("assists"));}
QString duration(const QJsonObject &m){const auto s=m.value("duration_seconds");return s.isDouble()?QStringLiteral("%1:%2").arg(s.toInt()/60).arg(s.toInt()%60,2,10,QChar('0')):QStringLiteral("—");}
QString date(const QJsonObject &m){const auto v=m.value("started_at_ms");return v.isDouble()?QDateTime::fromMSecsSinceEpoch(v.toInteger()).toString("MM/dd  HH:mm"):QStringLiteral("—");}
QJsonObject target(const QJsonObject &m){for(const auto &p:m.value("players").toArray())if(p.toObject().value("is_target").toBool())return p.toObject();return {};}
QString result(const QJsonObject &m){const auto v=m.value("win");if(!v.isBool()&&!v.isDouble())return QStringLiteral("未知");return v.toBool(v.toInt()!=0)?QStringLiteral("胜利"):QStringLiteral("失败");}
QColor resultColor(const QJsonObject &m){return result(m)==QStringLiteral("胜利")?semanticInk("ice"):result(m)==QStringLiteral("失败")?Kda::color("danger"):Kda::color("muted");}
void text(QPainter &p,const QRectF &r,const QString &s,int px,const QColor &ink,Kda::FontRole role=Kda::FontRole::Body,bool bold=false,int align=Qt::AlignLeft|Qt::AlignVCenter){const auto f=Kda::font(role,px,bold?QFont::DemiBold:QFont::Normal);p.setFont(f);p.setPen(ink);p.drawText(r,align,QFontMetrics(f).elidedText(s,Qt::ElideRight,qMax(0,int(r.width()))));}
void art(QPainter &p,const QRect &r,HudAssets *assets,const QString &kind,int id,qreal dpr){
    auto image=id>0?(kind=="champion"?assets->champion(id):kind=="augment"?assets->augment(id):assets->item(id)):QPixmap{};
    p.setPen(QPen(Kda::color("border"),.8));p.setBrush(Kda::color("raised"));
    const qreal radius=kind=="champion"?Kda::radius("artwork"):2.;
    p.drawRoundedRect(QRectF(r).adjusted(.5,.5,-.5,-.5),radius,radius);
    if(!image.isNull())p.drawPixmap(r,Kda::roundedPixmap(image,r.size(),radius,dpr));
    else if(id>0)HudSymbols::draw(p,kind=="augment"?HudSymbols::Symbol::Star:kind=="champion"?HudSymbols::Symbol::Team:HudSymbols::Symbol::Info,QRectF(r).adjusted(5,5,-5,-5),Kda::color("muted"),1.1);
}
QJsonArray inventory(const QJsonObject &p){QJsonArray a;for(const auto &id:p.value("items").toArray())if(id.toInt()>0&&id.toInt()!=2052&&id.toInt()!=220013)a.append(id);return a;}
QString tooltip(const Catalog &c,const QJsonObject &p){QStringList names;for(const auto &v:inventory(p))names<<c.itemName(v.toInt());for(const auto &v:p.value("augments").toArray())names<<c.augments.value(v.toInt()).value("name_zh_cn").toString(QStringLiteral("海克斯 %1").arg(v.toInt()));return c.championName(p.value("champion_id").toInt())+"\n"+names.join(QStringLiteral(" · "));}
QString augmentName(const Catalog &catalog,int id){const auto name=catalog.augments.value(id).value("name_zh_cn").toString().trimmed();return name.isEmpty()?QStringLiteral("海克斯 %1").arg(id):name;}
QString augmentTooltip(const Catalog &catalog,int id,const QString &matchPatch){
    const auto entry=catalog.augments.value(id);const auto description=plainText(entry.value("description").toString()).trimmed();
    QString detail=description.isEmpty()?QStringLiteral("本地资料未收录这一海克斯的说明。"):description;
    detail.replace(QStringLiteral("%i:goldCoins%"),QStringLiteral("金币 "));
    detail.remove(QRegularExpression(QStringLiteral("%i:[^%]+%")));
    const QRegularExpression unresolved(QStringLiteral("@[^@]+@"));if(detail.contains(unresolved)){detail.replace(unresolved,QStringLiteral("—"));detail+=QStringLiteral("\n部分数值未收录。");}
    if(!description.isEmpty()){
        auto descriptionPatch=entry.value("description_patch").toString().trimmed();if(descriptionPatch.isEmpty())descriptionPatch=catalog.patch;
        if(!descriptionPatch.isEmpty())detail+=QStringLiteral("\n资料版本 %1").arg(descriptionPatch);
        if(!matchPatch.isEmpty()&&matchPatch!=descriptionPatch)detail+=QStringLiteral(" · 本场版本 %1").arg(matchPatch);
    }
    return augmentName(catalog,id)+QStringLiteral("\n")+detail;
}
struct MatchMetric {QString label;QString key;bool percent=false;};
QVector<MatchMetric> matchMetrics(int mode){
    if(mode==1)return {{QStringLiteral("物理伤害"),"physical_damage_to_champions"},{QStringLiteral("魔法伤害"),"magic_damage_to_champions"},{QStringLiteral("真实伤害"),"true_damage_to_champions"},{QStringLiteral("伤害 / 分"),"damage_per_minute"},{QStringLiteral("减免伤害"),"damage_mitigated"},{QStringLiteral("伤害占比"),"team_damage_share",true}};
    if(mode==2)return {{QStringLiteral("获得金币"),"gold_earned"},{QStringLiteral("金币 / 分"),"gold_per_minute"},{QStringLiteral("补刀"),"cs"}};
    return {{QStringLiteral("总治疗"),"heal"},{QStringLiteral("队友治疗"),"heal_teammates"},{QStringLiteral("队友护盾"),"shield"}};
}
QString metricValue(const QJsonObject &scores,const MatchMetric &metric){const auto value=scores.value(metric.key);return metric.percent?(value.isDouble()&&std::isfinite(value.toDouble())?number(value.toDouble()*100,1)+"%":QStringLiteral("—")):number(value);}
bool recordedStat(const QJsonValue &value){return value.isDouble()&&std::isfinite(value.toDouble())&&value.toDouble()>=0;}
struct TeamSummary {
    QHash<QString,double> maximum;
    QHash<QString,int> recorded;
    int players=0;
    bool leads(const QJsonObject &scores,const QString &key)const{
        const auto value=scores.value(key);
        return players==5&&recorded.value(key)==players&&recordedStat(value)&&maximum.value(key)>0&&value.toDouble()==maximum.value(key);
    }
};
TeamSummary summarizeTeam(const QJsonArray &players,int team){
    TeamSummary summary;
    for(const auto &entry:players){const auto row=entry.toObject();if(row.value("team_id").toInt()!=team)continue;++summary.players;
        const auto scores=row.value("scores").toObject();for(const auto &key:{"kills","deaths","assists","damage_to_champions","damage_taken"}){
            const auto value=scores.value(key);if(!recordedStat(value))continue;++summary.recorded[key];summary.maximum[key]=std::max(summary.maximum.value(key),value.toDouble());
        }
    }return summary;
}
QString compactStat(const QJsonValue &value){
    if(!recordedStat(value))return QStringLiteral("—");
    if(value.toDouble()<1000)return number(value);
    auto digits=QString::number(value.toDouble()/1000.,'f',value.toDouble()<10000?2:1);while(digits.endsWith('0'))digits.chop(1);if(digits.endsWith('.'))digits.chop(1);return digits+"k";
}
int kdaTextWidth(const QJsonObject &scores,int pixels){
    const QFontMetrics metrics(Kda::font(Kda::FontRole::Numeric,pixels,QFont::Normal));
    int width=2*(metrics.horizontalAdvance(" / ")+2);for(const auto key:{"kills","deaths","assists"})width+=metrics.horizontalAdvance(number(scores.value(key)))+1;return width;
}
int kdaPixels(const QJsonObject &scores,qreal available,int maximum=16){
    int pixels=maximum;while(pixels>12&&kdaTextWidth(scores,pixels)>available)--pixels;return pixels;
}
void drawKda(QPainter &p,const QRectF &area,const QJsonObject &scores,const TeamSummary &summary,int maximum=16){
    const QStringList keys{"kills","deaths","assists"};const QColor colors[]{Kda::color("danger"),Kda::color("gold"),Kda::color("ready")};
    const int pixels=kdaPixels(scores,area.width(),maximum);const auto font=Kda::font(Kda::FontRole::Numeric,pixels,QFont::Normal);const QFontMetrics metrics(font);qreal x=area.x();
    p.save();p.setClipRect(area,Qt::IntersectClip);p.setFont(font);
    for(int i=0;i<3;++i){if(i){const int span=metrics.horizontalAdvance(" / ")+2;p.setPen(Kda::color("muted"));p.drawText(QRectF(x,area.y(),span,area.height()),Qt::AlignLeft|Qt::AlignVCenter,QStringLiteral(" / "));x+=span;}
        const auto digits=number(scores.value(keys[i]));const int span=metrics.horizontalAdvance(digits)+1;
        p.setPen(summary.leads(scores,keys[i])?colors[i]:Kda::color("text"));p.drawText(QRectF(x,area.y(),span,area.height()),Qt::AlignLeft|Qt::AlignVCenter,digits);x+=span;
    }p.restore();
}
bool kdaPaintClipVerified(){
    static const bool verified=[] {
        QImage image(220,48,QImage::Format_ARGB32_Premultiplied);image.fill(Qt::transparent);const QRect area(58,8,106,24);
        QPainter painter(&image);Kda::configurePainter(painter);drawKda(painter,area,QJsonObject{{"kills",123456789012.},{"deaths",123456789012.},{"assists",123456789012.}},TeamSummary{});painter.end();
        bool painted=false;for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)if(qAlpha(image.pixel(x,y))){if(!area.contains(x,y))return false;painted=true;}return painted;
    }();return verified;
}
void drawStatBar(QPainter &p,const QRectF &area,const QJsonObject &scores,const QString &key,const TeamSummary &summary,const QColor &color,HudSymbols::Symbol symbol){
    const auto value=scores.value(key);const auto label=compactStat(value);const bool leader=summary.leads(scores,key);
    text(p,QRectF(area.x(),area.y(),30,16),key=="damage_to_champions"?QStringLiteral("伤害"):QStringLiteral("承伤"),10,Kda::color("muted"),Kda::FontRole::Label);
    text(p,QRectF(area.x()+31,area.y(),area.width()-31-(leader?18:0),16),label,12,Kda::color("silver"),Kda::FontRole::Numeric,false,Qt::AlignRight|Qt::AlignVCenter);
    if(leader)HudSymbols::draw(p,symbol,QRectF(area.right()-12,area.y()+2,11,11),semanticInk(key=="damage_to_champions"?"gold":"ice"));
    const QRectF track(area.x(),area.y()+18,area.width(),3);auto trackInk=Kda::color("border");trackInk.setAlpha(150);p.fillPath(Kda::facet(track,1.5),trackInk);
    const double maximum=summary.maximum.value(key);if(recordedStat(value)&&maximum>0){auto bar=track;bar.setWidth(track.width()*std::clamp(value.toDouble()/maximum,0.,1.));
        QPainterPath fill;fill.addRoundedRect(bar,1.5,1.5);p.fillPath(fill,color);
    }
}

QString metricTooltip(const QJsonObject &row){
    const auto scores=row.value("scores").toObject();QStringList values;
    const QVector<MatchMetric> metrics{{QStringLiteral("英雄伤害"),"damage_to_champions"},{QStringLiteral("物理伤害"),"physical_damage_to_champions"},{QStringLiteral("魔法伤害"),"magic_damage_to_champions"},{QStringLiteral("真实伤害"),"true_damage_to_champions"},{QStringLiteral("承受伤害"),"damage_taken"},{QStringLiteral("减免伤害"),"damage_mitigated"},{QStringLiteral("总治疗"),"heal"},{QStringLiteral("队友治疗"),"heal_teammates"},{QStringLiteral("队友护盾"),"shield"},{QStringLiteral("控制时长"),"cc_time"},{QStringLiteral("伤害 / 分"),"damage_per_minute"},{QStringLiteral("金币 / 分"),"gold_per_minute"},{QStringLiteral("参团率"),"kill_participation",true},{QStringLiteral("团队伤害占比"),"team_damage_share",true},{QStringLiteral("团队金币占比"),"team_gold_share",true}};
    for(const auto &metric:metrics)values.append(metric.label+QStringLiteral("  ")+metricValue(scores,metric));
    return values.join('\n')+QStringLiteral("\n— 表示来源未记录或分母不完整。");
}
enum class ContentDepth { Front, Middle, Rear };
QPoint depthMaximum(ContentDepth layer){return layer==ContentDepth::Front?QPoint(1,1):layer==ContentDepth::Middle?QPoint(3,2):QPoint(6,3);}
QPoint contentOffset(const QWidget *widget,ContentDepth layer){
    QPointF normalized;for(auto current=widget;current;current=current->parentWidget()){const auto value=current->property("lixinDepthOffset");if(value.isValid()){normalized=value.toPointF();break;}}
    const auto maximum=depthMaximum(layer);return {qRound(std::clamp(normalized.x(),-1.,1.)*maximum.x()),qRound(std::clamp(normalized.y(),-1.,1.)*maximum.y())};
}
void markDepth(QWidget *widget){widget->setProperty("kdaDepthMapped",true);}
template<class Paint> void depthPaint(QPainter &p,const QWidget *widget,ContentDepth layer,const QRectF &slot,Paint paint){
    p.save();p.setClipRect(slot,Qt::IntersectClip);p.translate(contentOffset(widget,layer));paint();p.restore();
}
void depthText(QPainter &p,const QWidget *widget,ContentDepth layer,const QRectF &slot,const QString &value,int pixels,const QColor &ink,Kda::FontRole role=Kda::FontRole::Body,bool bold=false,int alignment=Qt::AlignLeft|Qt::AlignVCenter){
    const auto maximum=depthMaximum(layer);auto offset=contentOffset(widget,layer);const QFontMetrics metrics(Kda::font(role,pixels,bold?QFont::DemiBold:QFont::Normal));
    // Short native rows have less vertical slack than their horizontal slots.
    // Bound the ink using the actual font height, rather than clipping descenders.
    const int vertical=std::max(0,std::min(maximum.y(),int((slot.height()-metrics.height())/2)));
    offset.setY(std::clamp(offset.y(),-vertical,vertical));
    p.save();p.setClipRect(slot,Qt::IntersectClip);p.translate(offset);text(p,slot.adjusted(maximum.x(),0,-maximum.x(),0),value,pixels,ink,role,bold,alignment);p.restore();
}
QJsonArray rectJson(const QRectF &rect){return {rect.x(),rect.y(),rect.width(),rect.height()};}
QJsonObject depthLayers(const QWidget *widget){QJsonObject result;for(const auto pair:{std::pair{"front",ContentDepth::Front},std::pair{"middle",ContentDepth::Middle},std::pair{"rear",ContentDepth::Rear}}){const auto offset=contentOffset(widget,pair.second),maximum=depthMaximum(pair.second);result[pair.first]=QJsonObject{{"offset",QJsonArray{offset.x(),offset.y()}},{"maximum",QJsonArray{maximum.x(),maximum.y()}}};}return result;}
QJsonObject depthSlots(const QRectF &bounds,const QJsonArray &contentSlots){bool contained=true;for(const auto &value:contentSlots){const auto a=value.toObject().value("rect").toArray();contained&=a.size()==4&&bounds.contains(QRectF(a[0].toDouble(),a[1].toDouble(),a[2].toDouble(),a[3].toDouble()));}return {{"bounds",rectJson(bounds)},{"slots",contentSlots},{"slots_contained",contained},{"slot_geometry","fixed_clip_contains_painted_content"}};}
QJsonObject depthSlot(const char *layer,const QRectF &rect){return {{"layer",layer},{"rect",rectJson(rect)}};}
class DepthValueLabel final:public QLabel {
public:DepthValueLabel(QWidget *parent):QLabel(parent){markDepth(this);setContentsMargins(3,0,3,0);}
protected:void paintEvent(QPaintEvent *)override{QPainter p(this);Kda::configurePainter(p);depthText(p,this,ContentDepth::Middle,rect(),QLabel::text(),12,Kda::color("silver"),Kda::FontRole::Numeric);}
};
class PlayerNameButton final:public QPushButton {
public:QString primary,secondary;bool target=false;
    PlayerNameButton(QWidget *parent):QPushButton(parent){markDepth(this);setFlat(true);setFocusPolicy(Qt::StrongFocus);setCursor(Qt::PointingHandCursor);setProperty("elidedPlayerName",true);setAccessibleDescription(QStringLiteral("打开这位玩家的战绩列表"));}
protected:void paintEvent(QPaintEvent *)override{
    QPainter p(this);Kda::configurePainter(p);const int tagAt=primary.lastIndexOf('#');const auto name=tagAt>0?primary.left(tagAt):primary;
    depthText(p,this,ContentDepth::Rear,QRect(0,28,width()-4,18),secondary,11,Kda::color("muted"));
    depthText(p,this,ContentDepth::Front,QRect(0,3,width()-4,25),name,14,Kda::color(underMouse()||hasFocus()?"ice":target?"silver":"text"),Kda::FontRole::Body);
    if(underMouse()||hasFocus()){p.setPen(QPen(Kda::color("ice"),.8));p.drawLine(1,27,std::min(width()-4,QFontMetrics(Kda::font(Kda::FontRole::Body,14)).horizontalAdvance(name)),27);}
    if(hasFocus()){p.setPen(QPen(Kda::color("ice"),.8,Qt::DotLine));p.setBrush(Qt::NoBrush);p.drawRoundedRect(QRectF(rect()).adjusted(.5,.5,-.5,-.5),3,3);}
}
};
class ProfileHeader final:public QWidget {
public:
    static constexpr int CompactHeight=330,MetricTop=206,MetricPitch=20;
    QJsonObject profile;QVector<QLabel*> axisCells;
    ProfileHeader(){
        markDepth(this);setFixedHeight(CompactHeight);setMinimumWidth(0);setObjectName(QStringLiteral("compactPlayerProfile"));Kda::markSharedGlassSurface(this);
        for(int i=0;i<5;++i){auto value=new DepthValueLabel(this);value->setObjectName(QStringLiteral("playerProfileAxis_%1").arg(i));value->setTextFormat(Qt::PlainText);value->setFont(Kda::font(Kda::FontRole::Numeric,12));value->setAttribute(Qt::WA_TransparentForMouseEvents);axisCells.append(value);}
    }
    void setProfileData(const QJsonObject &value){
        profile=value;setAccessibleName(profile.value("player").toObject().value("display_name").toString()+QStringLiteral("的战绩档案"));
        const auto axes=profile.value("five_axes").toArray();const QStringList labels{QStringLiteral("输出"),QStringLiteral("经济"),QStringLiteral("参团"),QStringLiteral("生存"),QStringLiteral("承伤")};
        QStringList descriptions;for(int i=0;i<axisCells.size();++i){const auto axis=i<axes.size()?axes[i].toObject():QJsonObject{};axisCells[i]->setAccessibleName(axis.value("label").toString(labels[i]));axisCells[i]->setAccessibleDescription(axisValue(axis)+QStringLiteral("\n")+axisEvidence(axis));descriptions.append(axisEvidence(axis));}
        setAccessibleDescription(descriptions.join('\n'));positionCells();update();
    }
    void positionCells(){const auto axes=profile.value("five_axes").toArray();for(int i=0;i<axisCells.size();++i){auto label=axisCells[i];label->setGeometry(78,MetricTop+i*MetricPitch,std::max(0,width()-96),MetricPitch);const auto axis=i<axes.size()?axes[i].toObject():QJsonObject{};label->setText(fittedAxisValue(axis,label->contentsRect().width()));label->setToolTip(axisValue(axis)+QStringLiteral("\n")+axisEvidence(axis));}}
    void resizeEvent(QResizeEvent *event)override{QWidget::resizeEvent(event);positionCells();}
    bool labelsFit()const{const auto axes=profile.value("five_axes").toArray();const QFontMetrics values(Kda::font(Kda::FontRole::Numeric,12));if(values.height()>MetricPitch)return false;for(int i=0;i<axes.size()&&i<axisCells.size();++i)if(values.horizontalAdvance(axisCells[i]->text())>axisCells[i]->contentsRect().width()||!rect().contains(axisCells[i]->geometry())||!axisCells[i]->toolTip().startsWith(axisValue(axes[i].toObject())))return false;return true;}
    static bool rankKnown(const QJsonObject &axis){const auto rank=axis.value("percentile"),value=axis.value("value");return value.isDouble()&&std::isfinite(value.toDouble())&&value.toDouble()>=0&&rank.isDouble()&&std::isfinite(rank.toDouble())&&rank.toDouble()>=0&&rank.toDouble()<=100&&axis.value("percentile_n").toInt()>0;}
    QRectF radarBounds()const{return QRectF(6,82,width()-12,122);}
    QPointF point(int i,double fraction)const{constexpr double pi=3.14159265358979323846;const double angle=i*2*pi/5-pi/2;return QPointF(width()/2.+std::cos(angle)*36*fraction,149+std::sin(angle)*36*fraction);}
    QRectF trendRect(int i,int count)const{const qreal step=std::min(24.,std::max(8.,(width()-64.)/std::max(1,count)));return QRectF(46+i*step,310,step-3,16);}
    QJsonArray trendMatches()const{const auto matches=profile.value("matches").toArray();QJsonArray recent;for(int i=std::min(12,int(matches.size()))-1;i>=0;--i)recent.append(matches[i]);return recent;}
    QJsonObject visualReport()const{const auto axes=profile.value("five_axes").toArray();int known=0;QJsonArray ranks;for(int i=0;i<5;++i){const auto axis=i<axes.size()?axes[i].toObject():QJsonObject{};const bool recorded=rankKnown(axis);known+=recorded;ranks.append(recorded?axis.value("percentile"):QJsonValue());}return {{"visible",true},{"inline",true},{"requires_expansion",false},{"known_radar_axes",known},{"radar_values",ranks},{"radar_scope","mean_same_match_ten_player_percentile"},{"unknown_axes_unconnected",true},{"filled_polygon",known==5},{"global_percentile_claimed",false},{"roles_adjusted",false},{"trend_matches",trendMatches().size()},{"trend_order","oldest_to_newest"}};}
    void paintEvent(QPaintEvent *)override{
        QPainter p(this);Kda::configurePainter(p);Kda::paintIdentityPlate(p,QRectF(0,0,width(),74));const auto player=profile.value("player").toObject();
        const auto identity=player.value("display_name").toString(QStringLiteral("玩家档案"));const int tagAt=identity.lastIndexOf('#');const auto nickname=tagAt>0?identity.left(tagAt):identity;const auto secondary=GameRegion::name(player.value("server").toString())+(tagAt>0?QStringLiteral(" · ")+identity.mid(tagAt):QString());
        depthText(p,this,ContentDepth::Front,QRect(18,5,width()-36,38),nickname,25,Kda::identityInk(),Kda::FontRole::Title,true);
        depthText(p,this,ContentDepth::Rear,QRect(18,43,width()-48,23),secondary,11,Kda::identityInk(),Kda::FontRole::Numeric);
        const QStringList labels{QStringLiteral("输出"),QStringLiteral("经济"),QStringLiteral("参团"),QStringLiteral("阵亡"),QStringLiteral("承伤")};const auto axes=profile.value("five_axes").toArray();
        for(int i=0;i<5;++i)depthText(p,this,ContentDepth::Rear,QRect(18,MetricTop+i*MetricPitch,54,MetricPitch),labels[i],11,Kda::color("muted"),Kda::FontRole::Label);
        QPolygonF polygon;int known=0;p.save();p.setClipRect(radarBounds(),Qt::IntersectClip);p.translate(contentOffset(this,ContentDepth::Rear));
        for(double fraction:{.25,.5,.75,1.}){QPolygonF ring;for(int i=0;i<5;++i)ring<<point(i,fraction);p.setBrush(Qt::NoBrush);p.setPen(QPen(themeInk("border",fraction==.5?190:95),.7));p.drawPolygon(ring);}
        for(int i=0;i<5;++i){const auto axis=i<axes.size()?axes[i].toObject():QJsonObject{};p.setPen(QPen(Kda::color("border"),.7));p.drawLine(QPointF(width()/2.,149),point(i,1));if(rankKnown(axis)){++known;polygon<<point(i,axis.value("percentile").toDouble()/100.);}else polygon<<QPointF();const auto position=point(i,1.45);text(p,QRectF(position.x()-25,position.y()-9,50,18),(i==3?QStringLiteral("生存"):labels[i])+(rankKnown(axis)?QString():QStringLiteral(" —")),10,Kda::color("muted"),Kda::FontRole::Label,false,Qt::AlignCenter);}p.restore();
        p.save();p.setClipRect(radarBounds(),Qt::IntersectClip);p.translate(contentOffset(this,ContentDepth::Middle));auto fill=Kda::color("ice");fill.setAlpha(40);p.setPen(QPen(Kda::color("ice"),1.4));if(known==5){p.setBrush(fill);p.drawPolygon(polygon);}else{p.setBrush(Qt::NoBrush);for(int i=0;i<5;++i)if(rankKnown(i<axes.size()?axes[i].toObject():QJsonObject{})&&rankKnown((i+1)%5<axes.size()?axes[(i+1)%5].toObject():QJsonObject{}))p.drawLine(polygon[i],polygon[(i+1)%5]);}for(int i=0;i<5;++i)if(rankKnown(i<axes.size()?axes[i].toObject():QJsonObject{})){p.setPen(Qt::NoPen);p.setBrush(Kda::color("ice"));p.drawEllipse(polygon[i],2.4,2.4);}p.restore();
        depthText(p,this,ContentDepth::Rear,QRect(18,308,24,20),QStringLiteral("近"),10,Kda::color("muted"),Kda::FontRole::Label);
        const auto recent=trendMatches();p.save();p.setClipRect(QRectF(43,307,width()-55,22),Qt::IntersectClip);p.translate(contentOffset(this,ContentDepth::Middle));for(int i=0;i<recent.size();++i){const auto match=recent[i].toObject();const auto bar=trendRect(i,recent.size());auto ink=resultColor(match);ink.setAlpha(55);p.fillRect(bar,ink);if(bar.width()>=12)text(p,bar,result(match)==QStringLiteral("胜利")?QStringLiteral("胜"):result(match)==QStringLiteral("失败")?QStringLiteral("负"):QStringLiteral("?"),9,resultColor(match),Kda::FontRole::Label,false,Qt::AlignCenter);}p.restore();
    }
    bool event(QEvent *event)override{
        if(event->type()!=QEvent::ToolTip)return QWidget::event(event);const auto help=static_cast<QHelpEvent*>(event);const auto pos=help->pos();const auto axes=profile.value("five_axes").toArray();
        if(pos.y()<74){const auto player=profile.value("player").toObject();QToolTip::showText(help->globalPos(),player.value("display_name").toString()+QStringLiteral(" · ")+GameRegion::name(player.value("server").toString()),this);return true;}
        int index=pos.y()>=MetricTop&&pos.y()<MetricTop+5*MetricPitch?(pos.y()-MetricTop)/MetricPitch:-1;
        if(radarBounds().contains(pos)){double distance=1e9;for(int i=0;i<5;++i){const auto delta=point(i,1.45)-pos;const double d=delta.x()*delta.x()+delta.y()*delta.y();if(d<distance){index=i;distance=d;}}}
        if(index>=0&&index<axes.size()){const auto axis=axes[index].toObject();QToolTip::showText(help->globalPos(),axis.value("label").toString()+QStringLiteral("  ")+axisValue(axis)+QStringLiteral("\n")+axisEvidence(axis),this);return true;}
        if(pos.y()>=308){const auto recent=trendMatches();for(int i=0;i<recent.size();++i)if(trendRect(i,recent.size()).translated(contentOffset(this,ContentDepth::Middle)).contains(pos)){const auto match=recent[i].toObject();QToolTip::showText(help->globalPos(),result(match)+QStringLiteral(" · ")+date(match)+QStringLiteral(" · ")+duration(match),this);return true;}}
        QToolTip::hideText();return true;
    }
};
struct ArchiveRowLayout {
    int buildX;
    QRectF avatar,name,result,kda,build,gold,date,duration,chevron;
    explicit ArchiveRowLayout(int width){
        buildX=18;
        avatar={18,8,44,44};name={74,5,qreal(width-270),24};result={74,31,80,22};
        kda={qreal(width-178),5,150,30};build={15,58,qreal(width-110),27};gold={qreal(width-73),60,51,22};
        date={qreal(width-217),32,120,22};duration={qreal(width-90),32,64,22};chevron={qreal(width-19),13,14,14};
    }
    QJsonArray contentSlots()const{return {depthSlot("front",avatar.adjusted(-1,-1,1,1)),depthSlot("front",name),depthSlot("front",result),depthSlot("front",chevron),depthSlot("middle",kda),depthSlot("middle",build),depthSlot("middle",gold),depthSlot("rear",date),depthSlot("rear",duration)};}
};
class MatchDelegate final:public QStyledItemDelegate {
public:const Catalog &catalog;HudAssets *assets;MatchDelegate(const Catalog &c,HudAssets *a,QObject *parent):QStyledItemDelegate(parent),catalog(c),assets(a){}
static constexpr int RowHeight=90;
QSize sizeHint(const QStyleOptionViewItem &,const QModelIndex &)const override{return {420,RowHeight};}
void paint(QPainter *p,const QStyleOptionViewItem &opt,const QModelIndex &index)const override{
    p->save();Kda::configurePainter(*p);p->translate(opt.rect.topLeft());const int w=opt.rect.width();
    const auto m=index.data(Qt::UserRole).toJsonObject(),me=target(m);const auto ink=resultColor(m),muted=Kda::color("muted");const ArchiveRowLayout row(w);
    const bool selected=opt.state.testFlag(QStyle::State_Selected),hovered=opt.state.testFlag(QStyle::State_MouseOver),focused=opt.state.testFlag(QStyle::State_HasFocus);
    if(selected||hovered)p->fillRect(QRectF(1,1,w-2,RowHeight-2),themeInk("ice",hovered?15:8));
    p->fillRect(QRectF(6,22,2,16),ink);
    if(focused){p->setBrush(Qt::NoBrush);p->setPen(QPen(Kda::color("ice"),.8,Qt::DotLine));p->drawRoundedRect(QRectF(.5,.5,w-1,RowHeight-1),3,3);}
    p->setPen(QPen(themeInk("border",44),.5));p->drawLine(QPointF(18,RowHeight-.5),QPointF(w-18,RowHeight-.5));
    p->setClipRect(QRectF(0,0,w,RowHeight),Qt::IntersectClip);const QWidget *host=opt.widget;
    depthText(*p,host,ContentDepth::Rear,row.date,date(m),11,muted,Kda::FontRole::Numeric,false,Qt::AlignRight|Qt::AlignVCenter);
    depthText(*p,host,ContentDepth::Rear,row.duration,duration(m),12,muted,Kda::FontRole::Numeric,false,Qt::AlignRight|Qt::AlignVCenter);
    depthText(*p,host,ContentDepth::Middle,row.kda,score(m),18,Kda::color("silver"),Kda::FontRole::Numeric,false,Qt::AlignRight|Qt::AlignVCenter);
    depthPaint(*p,host,ContentDepth::Middle,row.build,[&]{
        const auto items=inventory(me);for(int i=0;i<7;++i){const int id=i<items.size()?items[i].toInt():0;if(id>0)art(*p,QRect(row.buildX+i*25,61,22,22),assets,"item",id,p->device()->devicePixelRatioF());}
        const auto augments=me.value("augments").toArray();HudSymbols::draw(*p,HudSymbols::Symbol::Star,QRectF(row.buildX+185,66,11,11),muted);
        for(int i=0;i<std::min(6,int(augments.size()));++i)if(augments[i].toInt()>0)art(*p,QRect(row.buildX+203+i*18,64,15,15),assets,"augment",augments[i].toInt(),p->device()->devicePixelRatioF());
        if(augments.isEmpty())text(*p,QRect(row.buildX+203,60,100,22),QStringLiteral("—"),12,muted);
        HudSymbols::draw(*p,HudSymbols::Symbol::Coins,QRectF(w-88,66,11,11),muted);
    });
    depthText(*p,host,ContentDepth::Middle,row.gold,number(m.value("scores").toObject().value("gold_earned")),11,muted,Kda::FontRole::Numeric);
    depthText(*p,host,ContentDepth::Front,row.name,catalog.championName(m.value("champion_id").toInt()),14,Kda::color("text"),Kda::FontRole::Body,true);
    depthText(*p,host,ContentDepth::Front,row.result,result(m),12,ink,Kda::FontRole::Label);
    depthPaint(*p,host,ContentDepth::Front,row.avatar.adjusted(-1,-1,1,1),[&]{art(*p,row.avatar.toRect(),assets,"champion",m.value("champion_id").toInt(),p->device()->devicePixelRatioF());});
    if(hovered||focused)depthPaint(*p,host,ContentDepth::Front,row.chevron,[&]{HudSymbols::draw(*p,HudSymbols::Symbol::Chevron,row.chevron.adjusted(1,1,-1,-1),Kda::color("silver"));});p->restore();
}
};
class HextechChip final:public QWidget {
public:
    const Catalog &catalog;HudAssets *assets;int id;
    HextechChip(const Catalog &c,HudAssets *a,int value,const QString &matchPatch,QWidget *parent):QWidget(parent),catalog(c),assets(a),id(value){
        markDepth(this);setFocusPolicy(Qt::StrongFocus);setAttribute(Qt::WA_Hover);setToolTip(augmentTooltip(catalog,id,matchPatch));setAccessibleName(augmentName(catalog,id));setAccessibleDescription(toolTip());
    }
protected:
    void paintEvent(QPaintEvent *)override{
        QPainter p(this);Kda::configurePainter(p);const QRectF area=QRectF(rect()).adjusted(.5,.5,-.5,-.5);
        if(underMouse()||hasFocus()){QPainterPath hover;hover.addRoundedRect(area,4,4);p.fillPath(hover,themeInk("ice",12));if(hasFocus()){p.setBrush(Qt::NoBrush);p.setPen(QPen(Kda::color("ice"),.8,Qt::DotLine));p.drawPath(hover);}}
        depthText(p,this,ContentDepth::Middle,QRect(27,0,std::max(0,width()-27),28),augmentName(catalog,id),11,Kda::color("silver"),Kda::FontRole::Label);
        depthPaint(p,this,ContentDepth::Front,QRectF(1,2,24,24),[&]{art(p,QRect(2,3,22,22),assets,"augment",id,devicePixelRatioF());});
    }
    bool event(QEvent *event)override{const auto type=event->type();if(type==QEvent::Enter||type==QEvent::Leave||type==QEvent::FocusIn||type==QEvent::FocusOut)update();return QWidget::event(event);}
};
class DetailHeader final:public QWidget {
public:DetailHeader(){setObjectName(QStringLiteral("matchDetailKdaHeader"));setFixedHeight(46);Kda::markSharedGlassSurface(this);}
protected:void paintEvent(QPaintEvent *)override{
    QPainter p(this);Kda::configurePainter(p);p.setPen(QPen(themeInk("border",44),.5));p.drawLine(QPointF(18,height()-.5),QPointF(width()-18,height()-.5));
}
};
class Scoreboard final:public QWidget {
public:
    static constexpr int HeaderHeight=24,TeamHeight=28,RowHeight=92,OverviewRowHeight=52,BattleRowHeight=170,HextechRowHeight=186,EquipmentRowHeight=94,NameX=64;
    const Catalog &catalog;HudAssets *assets;QJsonObject match;QHash<int,TeamSummary> teamSummaries;int mode=0,pageIndex=0,playersPerPage=5,viewportHeight=360;
    QVector<PlayerNameButton*> playerButtons;QVector<int> buttonRows;
    QVector<HextechChip*> augmentChips;QVector<int> augmentRows,augmentSlotIndices;
    std::function<void(const QJsonObject &)> onPlayerRequested;
    Scoreboard(const Catalog &c,HudAssets *a):catalog(c),assets(a){markDepth(this);setObjectName(QStringLiteral("matchDetailScoreboard"));setMinimumWidth(0);setMouseTracking(true);}
    int rowHeight()const{return mode==0?std::clamp((viewportHeight-HeaderHeight-TeamHeight-6)/5,48,OverviewRowHeight):mode==1?BattleRowHeight:mode==4?HextechRowHeight:mode==5?EquipmentRowHeight:RowHeight;}
    int teamSize()const{int left=0,right=0;for(const auto &v:match.value("players").toArray()){const auto team=v.toObject().value("team_id").toInt();left+=team==100;right+=team==200;}return std::max(left,right);}
    int pageCount()const{return std::max(1,(teamSize()+playersPerPage-1)/playersPerPage);}
    int playerOrder(int index)const{const auto players=match.value("players").toArray();const int team=players[index].toObject().value("team_id").toInt();int order=0;for(int i=0;i<index;++i)order+=players[i].toObject().value("team_id").toInt()==team;return order;}
    bool playerVisible(int index)const{const int order=playerOrder(index);return order>=pageIndex*playersPerPage&&order<(pageIndex+1)*playersPerPage;}
    void setViewportHeight(int value){const int anchor=pageIndex*playersPerPage;viewportHeight=std::max(1,value);const int capacity=std::clamp((viewportHeight-HeaderHeight-TeamHeight-6)/rowHeight(),1,5);if(capacity!=playersPerPage){playersPerPage=capacity;pageIndex=anchor/playersPerPage;}pageIndex=std::clamp(pageIndex,0,pageCount()-1);positionButtons();update();}
    int kdaColumnWidth()const{
        int width=106;for(const auto &entry:match.value("players").toArray())width=std::max(width,kdaTextWidth(entry.toObject().value("scores").toObject(),12));return width+6;
    }
    int minimumPanelWidth()const{
        // Reserve the same width across all six pages. Unusually long recorded
        // values grow the canvas instead of stealing space from adjacent fields.
        int cell=0;const QFontMetrics values(Kda::font(Kda::FontRole::Numeric,16,QFont::Normal)),labels(Kda::font(Kda::FontRole::Label,10));
        for(const auto &entry:match.value("players").toArray())for(int page:{1,2,3})for(const auto &metric:matchMetrics(page)){
            cell=std::max(cell,std::max(values.horizontalAdvance(metricValue(entry.toObject().value("scores").toObject(),metric)),labels.horizontalAdvance(metric.label))+8);
        }
        return std::max({280,NameX+70+kdaColumnWidth()+12,24+3*cell});
    }
    int panelWidth()const{return (width()-24)/2;}
    QRect teamPanel(int team)const{return {team==100?6:18+panelWidth(),HeaderHeight,panelWidth(),height()-HeaderHeight-4};}
    QRect playerRow(int index)const{
        const auto players=match.value("players").toArray();const int team=players[index].toObject().value("team_id").toInt();
        int order=0;for(int i=0;i<index;++i)if(players[i].toObject().value("team_id").toInt()==team)++order;
        return {teamPanel(team).left(),HeaderHeight+TeamHeight+(order-pageIndex*playersPerPage)*rowHeight(),panelWidth(),rowHeight()};
    }
    int nameAreaWidth()const{return panelWidth()-NameX-kdaColumnWidth()-12;}
    QRectF kdaArea(const QRect &row)const{return {qreal(row.right()-kdaColumnWidth()-6),qreal(row.y()+4),qreal(kdaColumnWidth()),26.};}
    int minimumKdaPixels()const{int pixels=18;for(const auto &entry:match.value("players").toArray())pixels=std::min(pixels,kdaPixels(entry.toObject().value("scores").toObject(),kdaColumnWidth()-6));return pixels;}
    int augmentSlotCount()const{int count=4;for(const auto &v:match.value("players").toArray())count=std::max(count,std::min(8,int(v.toObject().value("augments").toArray().size())));return count;}
    bool teamColumnsFit()const{return panelWidth()>=minimumPanelWidth()&&teamPanel(100).right()<teamPanel(200).left()&&teamPanel(200).right()<width();}
    bool overviewBarsFit()const{
        if(!teamColumnsFit()||nameAreaWidth()<70)return false;
        for(const auto &entry:match.value("players").toArray()){const auto scores=entry.toObject().value("scores").toObject();
            const int pixels=kdaPixels(scores,kdaColumnWidth()-6);if(pixels<12||kdaTextWidth(scores,pixels)>kdaColumnWidth()-6)return false;
        }return true;
    }
    bool columnsFit()const{
        if(!overviewBarsFit())return false;
        if(mode==4)return hextechCellsFit();
        if(mode==5)return equipmentCellsFit();
        if(mode==0)return true;
        const int cell=(panelWidth()-24)/3;const QFontMetrics values(Kda::font(Kda::FontRole::Numeric,16,QFont::Normal));
        for(const auto &entry:match.value("players").toArray())for(const auto &metric:matchMetrics(mode))
            if(values.horizontalAdvance(metricValue(entry.toObject().value("scores").toObject(),metric))>cell-8)return false;
        return true;
    }
    bool playerHeaderFieldsFit()const{
        const auto players=match.value("players").toArray();
        for(int i=0;i<players.size();++i){const auto row=playerRow(i);const QRectF name(row.x()+NameX,row.y(),nameAreaWidth()-5,48),kda=kdaArea(row);
            if(!QRectF(row).contains(name)||!QRectF(row).contains(kda)||name.intersects(kda))return false;
        }
        for(int i=0;i<playerButtons.size();++i)if(!playerButtons[i]->isHidden()&&QRectF(playerButtons[i]->geometry()).intersects(kdaArea(playerRow(buttonRows[i]))))return false;
        return overviewBarsFit();
    }
    QJsonArray leaderRows()const{
        QJsonArray rows;int index=0;for(const auto &entry:match.value("players").toArray()){const auto row=entry.toObject(),scores=row.value("scores").toObject();const auto summary=teamSummaries.value(row.value("team_id").toInt());QJsonObject leaders{{"row",index++}};
            for(const auto &key:{"kills","deaths","assists","damage_to_champions","damage_taken"})leaders[key]=summary.leads(scores,key);rows.append(leaders);
        }return rows;
    }
    QString statExplanation(const QJsonObject &row)const{
        const auto scores=row.value("scores").toObject();const auto summary=teamSummaries.value(row.value("team_id").toInt());QStringList leaders;
        const QStringList keys{"kills","deaths","assists","damage_to_champions","damage_taken"},labels{QStringLiteral("击杀最多"),QStringLiteral("死亡最多"),QStringLiteral("助攻最多"),QStringLiteral("伤害最高"),QStringLiteral("承伤最高")};
        for(int i=0;i<keys.size();++i)if(summary.leads(scores,keys[i]))leaders.append(labels[i]);
        return (leaders.isEmpty()?QString():QStringLiteral("本队：")+leaders.join(QStringLiteral(" · "))+QStringLiteral("\n"))+QStringLiteral("K 红色 / D 金色 / A 绿色：本队最高；并列一同标记。\n缺少队员数据时不标最高。进度条按本队已记录最高值缩放。");
    }
    QRect hextechCell(int index,int slot)const{
        const auto row=playerRow(index);const int gap=6,cell=(row.width()-24-gap)/2;
        return {row.x()+12+(slot%2)*(cell+gap),row.y()+54+(slot/2)*32,cell,28};
    }
    QRect itemCell(int index,int slot)const{
        const auto row=playerRow(index);return {row.x()+12+slot*38,row.y()+55,32,32};
    }
    bool equipmentCellsFit()const{
        if(mode!=5)return true;const auto players=match.value("players").toArray();
        for(int index=0;index<players.size();++index){const auto items=inventory(players[index].toObject());for(int slot=0;slot<std::min(7,int(items.size()));++slot)if(!playerRow(index).contains(itemCell(index,slot)))return false;}
        return true;
    }
    QJsonArray modeFields()const{
        if(mode==0)return {"kills","deaths","assists","kill_participation"};
        if(mode==4)return {"augments"};
        if(mode==5)return {"items","gold_earned"};
        QJsonArray keys;for(const auto &metric:matchMetrics(mode))keys.append(metric.key);if(mode==1){keys.append("damage_to_champions");keys.append("damage_taken");}return keys;
    }
    int recordedAugments()const{int count=0;for(const auto &v:match.value("players").toArray()){const auto augments=v.toObject().value("augments").toArray();for(int i=0;i<std::min(8,int(augments.size()));++i)if(augments[i].toInt()>0)++count;}return count;}
    int emptyAugmentRows()const{int count=0;for(const auto &v:match.value("players").toArray()){bool recorded=false;for(const auto &id:v.toObject().value("augments").toArray())recorded|=id.toInt()>0;if(!recorded)++count;}return count;}
    bool hextechCellsFit()const{
        if(mode!=4)return true;
        for(int i=0;i<augmentChips.size();++i){const auto chip=augmentChips[i];if(chip->isHidden())continue;if(chip->width()<76||chip->height()!=28||!rect().contains(chip->geometry())||!playerRow(augmentRows[i]).contains(chip->geometry()))return false;
            for(int j=0;j<i;++j)if(!augmentChips[j]->isHidden()&&chip->geometry().intersects(augmentChips[j]->geometry()))return false;
        }return true;
    }
    bool hextechTooltipsComplete()const{for(const auto chip:augmentChips)if(chip->accessibleName().isEmpty()||chip->accessibleDescription().isEmpty()||chip->toolTip().isEmpty()||!chip->toolTip().startsWith(augmentName(catalog,chip->id)))return false;return augmentChips.size()==recordedAugments();}
    void positionButtons(){
        const auto players=match.value("players").toArray();int left=0,right=0;
        for(const auto &entry:players){if(entry.toObject().value("team_id").toInt()==100)++left;else if(entry.toObject().value("team_id").toInt()==200)++right;}
        setMinimumWidth(24+minimumPanelWidth()*2);
        setMinimumHeight(HeaderHeight+TeamHeight+std::min(playersPerPage,std::max(left,right))*rowHeight()+6);
        for(int i=0;i<players.size();++i){const auto row=playerRow(i);const int button=buttonRows.indexOf(i);
            if(button>=0){playerButtons[button]->setGeometry(row.x()+NameX,row.y(),std::max(24,nameAreaWidth()-5),48);playerButtons[button]->setVisible(playerVisible(i));}
        }
        for(int j=0;j<augmentChips.size();++j){augmentChips[j]->setGeometry(hextechCell(augmentRows[j],augmentSlotIndices[j]));augmentChips[j]->setVisible(mode==4&&playerVisible(augmentRows[j]));}
    }
    void setMode(int value){mode=std::clamp(value,0,5);positionButtons();update();}
    void setMatch(const QJsonObject &m){
        qDeleteAll(playerButtons);playerButtons.clear();buttonRows.clear();qDeleteAll(augmentChips);augmentChips.clear();augmentRows.clear();augmentSlotIndices.clear();
        match=m;pageIndex=0;const auto players=match.value("players").toArray();teamSummaries={{100,summarizeTeam(players,100)},{200,summarizeTeam(players,200)}};
        for(int i=0;i<players.size();++i){const auto row=players[i].toObject(),navigation=row.value("navigation").toObject();const auto augments=row.value("augments").toArray();
            for(int slot=0;slot<std::min(8,int(augments.size()));++slot){const int id=augments[slot].toInt();if(id<=0)continue;auto chip=new HextechChip(catalog,assets,id,m.value("patch").toString(),this);chip->setObjectName(QStringLiteral("matchAugment_%1_%2").arg(i).arg(slot));augmentChips.append(chip);augmentRows.append(i);augmentSlotIndices.append(slot);}
            if(!navigation.value("available").toBool()||navigation.value("player_ref").toString().isEmpty()||navigation.value("server").toString().isEmpty())continue;
            auto button=new PlayerNameButton(this);button->setObjectName(QStringLiteral("matchPlayerHistory_%1").arg(i));button->primary=row.value("display_name").toString();button->secondary=catalog.championName(row.value("champion_id").toInt())+"  Lv."+number(row.value("scores").toObject().value("level"));button->target=row.value("is_target").toBool();button->setAccessibleName(button->primary+QStringLiteral("的战绩"));button->setToolTip(button->primary+QStringLiteral("\n")+button->secondary+QStringLiteral("\n点击查看玩家战绩\n")+metricTooltip(row)+"\n"+statExplanation(row));button->setAccessibleDescription(button->accessibleDescription()+"\n"+statExplanation(row));connect(button,&QPushButton::clicked,this,[this,navigation]{if(onPlayerRequested)onPlayerRequested(navigation);});playerButtons.append(button);buttonRows.append(i);button->show();
        }positionButtons();update();
    }
    void refreshArtwork(){update();for(const auto chip:augmentChips)chip->update();}
    void resizeEvent(QResizeEvent *event)override{QWidget::resizeEvent(event);positionButtons();}
    void paintEvent(QPaintEvent *)override{
        QPainter p(this);Kda::configurePainter(p);
        depthText(p,this,ContentDepth::Rear,QRect(12,0,width()-24,24),date(match)+QStringLiteral(" · ")+duration(match),11,Kda::color("muted"),Kda::FontRole::Numeric);
        p.setPen(QPen(themeInk("border",44),.5));p.drawLine(width()/2,HeaderHeight,width()/2,height()-6);
        const auto players=match.value("players").toArray();
        for(int team:{100,200}){
            const auto panel=teamPanel(team);
            QStringList totals;for(const auto &field:{"kills","deaths","assists","gold_earned"}){int count=0;double sum=0;for(const auto &v:players){const auto row=v.toObject();if(row.value("team_id").toInt()!=team)continue;const auto value=row.value("scores").toObject().value(field);if(recordedStat(value)){sum+=value.toDouble();++count;}}totals.append(count==5?(QString::fromLatin1(field)=="gold_earned"?compactStat(sum):number(sum)):QStringLiteral("—"));}
            depthText(p,this,ContentDepth::Middle,QRect(panel.x()+77,HeaderHeight,panel.width()-86,TeamHeight),totals.mid(0,3).join(" / ")+QStringLiteral(" · ")+totals[3],11,Kda::color("muted"),Kda::FontRole::Numeric,false,Qt::AlignRight|Qt::AlignVCenter);
            depthText(p,this,ContentDepth::Front,QRect(panel.x()+9,HeaderHeight,72,TeamHeight),team==100?QStringLiteral("队伍 1"):QStringLiteral("队伍 2"),11,Kda::color("muted"),Kda::FontRole::Label);
            depthPaint(p,this,ContentDepth::Front,QRectF(panel.x(),HeaderHeight+7,4,TeamHeight-14),[&]{p.fillRect(QRectF(panel.x()+1,HeaderHeight+8,2,TeamHeight-16),semanticInk(team==100?"ice":"lilac"));});
        }
        for(int index=0;index<players.size();++index){
            if(!playerVisible(index))continue;const auto row=players[index].toObject(),scores=row.value("scores").toObject();const auto area=playerRow(index);const bool own=row.value("is_target").toBool();
            const auto summary=teamSummaries.value(row.value("team_id").toInt());const int x=area.x(),y=area.y(),w=area.width();
            if(own){QPainterPath selected;selected.addRoundedRect(QRectF(area).adjusted(1,1,-1,-1),6,6);p.fillPath(selected,themeInk("ice",7));}
            p.setPen(QPen(themeInk("border",44),.5));p.drawLine(x+8,area.bottom(),area.right()-8,area.bottom());
            if(!buttonRows.contains(index))depthText(p,this,ContentDepth::Rear,QRect(x+NameX,y+28,nameAreaWidth()-5,18),catalog.championName(row.value("champion_id").toInt())+"  Lv."+number(scores.value("level")),11,Kda::color("muted"));
            const auto participation=scores.value("kill_participation");
            const auto second=mode==5?QStringLiteral("金币 ")+compactStat(scores.value("gold_earned")):QStringLiteral("参团 ")+(recordedStat(participation)?QString::number(participation.toDouble()*100,'f',0)+"%":QStringLiteral("—"));
            depthText(p,this,ContentDepth::Rear,QRect(area.right()-kdaColumnWidth()-6,y+31,kdaColumnWidth(),18),second,11,Kda::color("muted"));
            {const auto slot=kdaArea(area),content=slot.adjusted(3,0,-3,0);auto offset=contentOffset(this,ContentDepth::Middle);const int pixels=kdaPixels(scores,content.width());const int slack=std::max(0,int((slot.height()-QFontMetrics(Kda::font(Kda::FontRole::Numeric,pixels,QFont::Normal)).height())/2));offset.setY(std::clamp(offset.y(),-slack,slack));p.save();p.setClipRect(slot,Qt::IntersectClip);p.translate(offset);drawKda(p,content,scores,summary);p.restore();}
            if(mode==5){
                const auto items=inventory(row);depthPaint(p,this,ContentDepth::Middle,QRectF(area).adjusted(6,52,-6,-2),[&]{for(int slot=0;slot<std::min(7,int(items.size()));++slot)art(p,itemCell(index,slot),assets,"item",items[slot].toInt(),devicePixelRatioF());});
                if(items.isEmpty())depthText(p,this,ContentDepth::Rear,QRect(x+6,y+55,w-12,32),QStringLiteral("未记录装备"),11,Kda::color("muted"));
            }else if(mode==4){bool recorded=false;for(const auto &id:row.value("augments").toArray())recorded|=id.toInt()>0;if(!recorded)depthText(p,this,ContentDepth::Rear,QRect(x+6,y+58,w-12,32),QStringLiteral("未记录海克斯"),11,Kda::color("muted"));}
            else if(mode!=0){
                p.save();p.setClipRect(QRectF(area).adjusted(6,50,-6,-1),Qt::IntersectClip);p.translate(contentOffset(this,ContentDepth::Middle));
                const auto metrics=matchMetrics(mode);const int cell=(w-24)/3;
                for(int i=0;i<metrics.size();++i){const int line=i/3,dx=x+12+(i%3)*cell,dy=y+52+line*46;
                    const auto ink=mode==1&&i<3?semanticInk(i==0?"ice":i==1?"lilac":"gold"):Kda::color("muted");
                    text(p,QRect(dx,dy,cell-8,15),metrics[i].label,10,ink,Kda::FontRole::Label);
                    text(p,QRect(dx,dy+16,cell-8,20),metricValue(scores,metrics[i]),16,Kda::color("silver"),Kda::FontRole::Numeric);
                }
                if(mode==1){
                    const QStringList keys{"physical_damage_to_champions","magic_damage_to_champions","true_damage_to_champions"};bool complete=true;double total=0;
                    for(const auto &key:keys){complete&=recordedStat(scores.value(key));if(recordedStat(scores.value(key)))total+=scores.value(key).toDouble();}
                    const QRectF track(x+12,y+92,w-24,3);auto muted=Kda::color("border");muted.setAlpha(130);p.fillPath(Kda::facet(track,1.5),muted);
                    if(complete&&total>0){p.save();p.setClipPath(Kda::facet(track,1.5));qreal cursor=track.x();for(int i=0;i<3;++i){const qreal span=track.width()*scores.value(keys[i]).toDouble()/total;p.fillRect(QRectF(cursor,track.y(),span,track.height()),semanticInk(i==0?"ice":i==1?"lilac":"gold"));cursor+=span;}p.restore();}
                    const int totalCell=(w-36)/2;drawStatBar(p,QRectF(x+12,y+144,totalCell,21),scores,"damage_to_champions",summary,semanticInk("ice"),HudSymbols::Symbol::Flame);drawStatBar(p,QRectF(x+24+totalCell,y+144,totalCell,21),scores,"damage_taken",summary,semanticInk("lilac"),HudSymbols::Symbol::Shield);
                }
                p.restore();
            }
            if(!buttonRows.contains(index))depthText(p,this,ContentDepth::Front,QRect(x+NameX,y+3,nameAreaWidth()-5,25),row.value("display_name").toString().section('#',0,0),14,Kda::color(own?"silver":"text"),Kda::FontRole::Body);
            depthPaint(p,this,ContentDepth::Front,QRectF(x+11,y+4,42,42),[&]{art(p,QRect(x+12,y+5,40,40),assets,"champion",row.value("champion_id").toInt(),devicePixelRatioF());});
        }
        if(players.isEmpty())text(p,QRect(16,100,width()-32,24),QStringLiteral("本场双方资料未记录"),13,Kda::color("muted"));
    }
    bool event(QEvent *event)override{
        if(event->type()==QEvent::ToolTip){auto help=static_cast<QHelpEvent*>(event);const auto players=match.value("players").toArray();for(int i=0;i<players.size();++i)if(playerVisible(i)&&playerRow(i).contains(help->pos())){const auto row=players[i].toObject();
            if(mode==5){const auto items=inventory(row);for(int slot=0;slot<std::min(7,int(items.size()));++slot)if(itemCell(i,slot).translated(contentOffset(this,ContentDepth::Middle)).contains(help->pos())){const int id=items[slot].toInt();auto description=plainText(catalog.items.value(id).value("description").toString());QToolTip::showText(help->globalPos(),catalog.itemName(id)+(description.isEmpty()?QString():QStringLiteral("\n")+description),this);return true;}
                QStringList names;for(const auto &id:items)names.append(catalog.itemName(id.toInt()));QToolTip::showText(help->globalPos(),names.isEmpty()?QStringLiteral("未记录装备"):names.join(QStringLiteral("\n")),this);return true;}
            QString detail;
            if(mode==4)detail=QStringLiteral("悬停海克斯图标查看完整名称与说明；未记录的海克斯显示为空。");
            else if(mode==0)detail=metricTooltip(row)+"\n"+statExplanation(row);
            else{const auto scores=row.value("scores").toObject();QStringList lines;for(const auto &metric:matchMetrics(mode))lines.append(metric.label+QStringLiteral("：")+metricValue(scores,metric));detail=lines.join("\n");if(mode==1)detail+=QStringLiteral("\n构成条依次为物理、魔法、真实伤害；伤害占比以本队总英雄伤害计算。\n— 表示来源未记录。");}
            QToolTip::showText(help->globalPos(),row.value("display_name").toString()+QStringLiteral("\n")+detail,this);return true;}QToolTip::hideText();return true;}
        return QWidget::event(event);
    }
};
}

PlayerMatchView::PlayerMatchView(const Catalog &c,QWidget *parent):QWidget(parent),catalog(c){
    setObjectName("playerMatchArchive");setMinimumWidth(0);setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Expanding);assets=new HudAssets(catalog,this);
    auto layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
    pages=new QStackedWidget;pages->setObjectName(QStringLiteral("matchArchivePages"));layout->addWidget(pages,1);
    auto archivePage=new QWidget;archivePage->setObjectName(QStringLiteral("matchArchiveSplitPage"));auto columns=new QHBoxLayout(archivePage);columns->setContentsMargins(0,0,0,0);columns->setSpacing(8);pages->addWidget(archivePage);
    summary=new ProfileHeader;auto summaryPlane=new Kda::DepthPlane(summary,Kda::DepthLayer::Readout);summaryPlane->setObjectName(QStringLiteral("playerProfileReadoutPlane"));summaryPlane->setProperty("kdaDepthStationary",true);
    auto sidebar=new QScrollArea;archiveSidebar=sidebar;sidebar->setObjectName(QStringLiteral("playerArchiveSidebar"));sidebar->setFixedWidth(234);sidebar->setWidgetResizable(true);sidebar->setFrameShape(QFrame::NoFrame);sidebar->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);sidebar->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);sidebar->setStyleSheet(QStringLiteral("QScrollArea{background:transparent;border:0;}QScrollBar:vertical{width:5px;background:transparent;}QScrollBar::handle:vertical{background:rgba(200,185,190,60);min-height:24px;border-radius:2px;}QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}"));sidebar->viewport()->setAutoFillBackground(false);sidebar->setWidget(summaryPlane);columns->addWidget(sidebar);
    archiveListPanel=new QWidget;archiveListPanel->setObjectName(QStringLiteral("matchArchiveRightPanel"));auto rightLayout=new QVBoxLayout(archiveListPanel);rightLayout->setContentsMargins(0,0,0,0);rightLayout->setSpacing(4);columns->addWidget(archiveListPanel,1);
    emptyMessage=new QLabel;emptyMessage->setObjectName(QStringLiteral("playerArchiveEmptyReason"));emptyMessage->setAlignment(Qt::AlignCenter);emptyMessage->setWordWrap(true);emptyMessage->setFont(Kda::font(Kda::FontRole::Body,14));rightLayout->addWidget(emptyMessage,1);
    list=new QListWidget;list->setObjectName("matchArchiveList");list->viewport()->setObjectName(QStringLiteral("matchArchiveViewport"));markDepth(list->viewport());list->verticalScrollBar()->setObjectName(QStringLiteral("matchArchiveScrollBar"));list->setItemDelegate(new MatchDelegate(c,assets,list));list->setMouseTracking(true);list->setSelectionMode(QAbstractItemView::SingleSelection);list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);list->viewport()->installEventFilter(this);list->setStyleSheet("QListWidget{background:transparent;border:0;outline:0;}QListWidget::item{padding:0;background:transparent;}");auto listPlane=new Kda::DepthPlane(list,Kda::DepthLayer::Readout);listPlane->setObjectName(QStringLiteral("matchArchiveReadoutPlane"));listPlane->setProperty("kdaDepthStationary",true);rightLayout->addWidget(listPlane,1);
    archivePager=new QWidget;archivePager->setObjectName(QStringLiteral("matchArchivePager"));archivePager->setFixedHeight(28);auto pagerLayout=new QHBoxLayout(archivePager);pagerLayout->setContentsMargins(18,0,18,0);pagerLayout->setSpacing(8);pagerLayout->addStretch();
    previousPage=new Kda::Button(QStringLiteral("上一页"));previousPage->setObjectName(QStringLiteral("archivePreviousPage"));previousPage->setProperty("toolbarAction",true);previousPage->setProperty("labelPixels",11);previousPage->setFixedSize(58,26);previousPage->setAccessibleName(QStringLiteral("上一页对局"));pagerLayout->addWidget(previousPage);
    pageNumber=new QLabel;pageNumber->setObjectName(QStringLiteral("archivePageNumber"));pageNumber->setAlignment(Qt::AlignCenter);pageNumber->setFixedWidth(62);pageNumber->setFont(Kda::font(Kda::FontRole::Numeric,13,QFont::Medium));pagerLayout->addWidget(pageNumber);
    nextPage=new Kda::Button(QStringLiteral("下一页"));nextPage->setObjectName(QStringLiteral("archiveNextPage"));nextPage->setProperty("toolbarAction",true);nextPage->setProperty("labelPixels",11);nextPage->setFixedSize(58,26);nextPage->setAccessibleName(QStringLiteral("下一页对局"));pagerLayout->addWidget(nextPage);rightLayout->addWidget(archivePager);
    connect(previousPage,&QPushButton::clicked,this,[this]{setArchivePage(archivePageIndex-1);});connect(nextPage,&QPushButton::clicked,this,[this]{setArchivePage(archivePageIndex+1);});

    auto details=new QWidget;details->setObjectName(QStringLiteral("matchDetailKdaPage"));auto dl=new QVBoxLayout(details);dl->setContentsMargins(0,0,0,0);dl->setSpacing(4);
    auto headingSurface=new DetailHeader;auto heading=new QHBoxLayout(headingSurface);heading->setContentsMargins(18,4,18,4);heading->setSpacing(4);
    auto back=new Kda::Button(QStringLiteral("全部战绩"));back->setObjectName("backToMatchArchive");back->setProperty("toolbarAction",true);back->setSymbol(HudSymbols::Symbol::Back);back->setSymbolOnly();back->setFixedSize(28,26);back->setAccessibleName(QStringLiteral("返回全部战绩"));back->setToolTip(QStringLiteral("返回全部战绩"));heading->addWidget(back);
    detailTitle=new QLabel;detailTitle->setObjectName(QStringLiteral("matchArchiveDetailTitle"));detailTitle->setFont(Kda::font(Kda::FontRole::Title,16,QFont::DemiBold));detailTitle->setMinimumWidth(0);detailTitle->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);heading->addWidget(detailTitle,1);
    decisionReplay=new Kda::Button(QStringLiteral("决策回看"));
    decisionReplay->setObjectName(QStringLiteral("openArchivedDecisionReplay"));
    decisionReplay->setProperty("toolbarAction",true);
    decisionReplay->setProperty("labelPixels",11);
    decisionReplay->setFixedSize(74,26);
    decisionReplay->setToolTip(QStringLiteral("回看这场对局已在本机记录的牌面和建议"));
    decisionReplay->hide();
    heading->addWidget(decisionReplay);
    connect(decisionReplay,&QPushButton::clicked,this,[this]{
        const auto ref=currentMatchRef();
        if(!ref.isEmpty()&&decisionReplayAvailable&&decisionReplayAvailable(ref)&&onDecisionReplay)
            onDecisionReplay(ref);
    });
    auto modeGroup=new QButtonGroup(this);modeGroup->setObjectName(QStringLiteral("matchDetailModeGroup"));modeGroup->setExclusive(true);int modeIndex=0;
    const QStringList modeTips{QStringLiteral("概览：双方阵容、KDA 与参团率；伤害与承伤在战斗页查看"),QStringLiteral("战斗：伤害、承伤、物理/魔法/真实伤害构成，以及伤害效率、减免伤害和团队伤害占比"),QStringLiteral("经济：获得金币、金币 / 分、补刀"),QStringLiteral("支援：总治疗、队友治疗与队友护盾"),QStringLiteral("海克斯：查看双方已选海克斯的图标、名称与说明"),QStringLiteral("装备：终局装备与金币；悬停查看装备说明")};
    for(const auto &label:{QStringLiteral("概览"),QStringLiteral("战斗"),QStringLiteral("经济"),QStringLiteral("支援"),QStringLiteral("海克斯"),QStringLiteral("装备")}){auto button=new Kda::Button(label);button->setObjectName(QStringLiteral("matchMetricMode_%1").arg(modeIndex));button->setProperty("labelPixels",11);button->setProperty("labelRegular",true);button->setProperty("toolbarAction",true);button->setFixedSize(76,26);button->setToolTip(modeTips[modeIndex]);button->setAccessibleName(modeTips[modeIndex]);button->setCheckable(true);button->setChecked(modeIndex==0);modeGroup->addButton(button,modeIndex++);heading->addWidget(button);}
    auto hint=new Kda::Button(QStringLiteral("未记录说明"));hint->setObjectName(QStringLiteral("matchMissingDataInfo"));hint->setProperty("toolbarAction",true);hint->setSymbol(HudSymbols::Symbol::Info);hint->setSymbolOnly();hint->setFixedSize(26,26);hint->setToolTip(QStringLiteral("— 表示来源未记录或分母不完整；0 表示已记录的真实零值。"));hint->setAccessibleName(hint->toolTip());connect(hint,&QPushButton::clicked,hint,[hint]{QToolTip::showText(hint->mapToGlobal(QPoint(0,hint->height())),hint->toolTip(),hint);});heading->addWidget(hint);auto headingPlane=new Kda::DepthPlane(headingSurface,Kda::DepthLayer::Hud);headingPlane->setObjectName(QStringLiteral("matchDetailActionsPlane"));dl->addWidget(headingPlane);
    detailScroll=new QScrollArea;detailScroll->setObjectName(QStringLiteral("matchDetailScrollArea"));detailScroll->viewport()->setObjectName(QStringLiteral("matchDetailViewport"));detailScroll->setFrameShape(QFrame::NoFrame);detailScroll->setStyleSheet(QStringLiteral("QScrollArea{background:transparent;border:0;}"));detailScroll->viewport()->setAutoFillBackground(false);detailScroll->setWidgetResizable(true);detailScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);detailScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);detailScroll->viewport()->installEventFilter(this);
    auto gutter=detailScroll->verticalScrollBar();gutter->setObjectName(QStringLiteral("matchDetailScrollBar"));
    auto horizontalGutter=detailScroll->horizontalScrollBar();horizontalGutter->setObjectName(QStringLiteral("matchDetailHorizontalScrollBar"));
    connect(gutter,&QScrollBar::rangeChanged,gutter,[gutter](int low,int high){gutter->setEnabled(high>low);});gutter->setEnabled(false);
    scoreboard=new Scoreboard(c,assets);static_cast<Scoreboard*>(scoreboard)->onPlayerRequested=[this](const QJsonObject &navigation){if(onPlayerRequested)onPlayerRequested(navigation);};connect(modeGroup,&QButtonGroup::idClicked,this,[this](int id){static_cast<Scoreboard*>(scoreboard)->setMode(id);refreshDetailPage();});detailScroll->setWidget(scoreboard);scoreboard->setAutoFillBackground(false);auto detailPlane=new Kda::DepthPlane(detailScroll,Kda::DepthLayer::Readout);detailPlane->setObjectName(QStringLiteral("matchDetailReadoutPlane"));detailPlane->setProperty("kdaDepthStationary",true);dl->addWidget(detailPlane,1);
    auto detailPager=new QWidget;detailPager->setObjectName(QStringLiteral("detailPlayerPager"));detailPager->setFixedHeight(28);auto detailPaging=new QHBoxLayout(detailPager);detailPaging->setContentsMargins(18,0,18,0);detailPaging->setSpacing(8);detailPaging->addStretch();
    detailPrevious=new Kda::Button(QStringLiteral("上一组"));detailPrevious->setObjectName(QStringLiteral("detailPreviousPlayers"));detailPrevious->setProperty("toolbarAction",true);detailPrevious->setProperty("labelPixels",11);detailPrevious->setFixedSize(58,26);detailPrevious->setAccessibleName(QStringLiteral("上一组双方玩家"));detailPaging->addWidget(detailPrevious);
    detailPageNumber=new QLabel;detailPageNumber->setAlignment(Qt::AlignCenter);detailPageNumber->setFixedWidth(90);detailPageNumber->setFont(Kda::font(Kda::FontRole::Numeric,13));detailPaging->addWidget(detailPageNumber);
    detailNext=new Kda::Button(QStringLiteral("下一组"));detailNext->setObjectName(QStringLiteral("detailNextPlayers"));detailNext->setProperty("toolbarAction",true);detailNext->setProperty("labelPixels",11);detailNext->setFixedSize(58,26);detailNext->setAccessibleName(QStringLiteral("下一组双方玩家"));detailPaging->addWidget(detailNext);dl->addWidget(detailPager);pages->addWidget(details);
    connect(detailPrevious,&QPushButton::clicked,this,[this]{auto board=static_cast<Scoreboard*>(scoreboard);board->pageIndex=std::max(0,board->pageIndex-1);refreshDetailPage();});connect(detailNext,&QPushButton::clicked,this,[this]{auto board=static_cast<Scoreboard*>(scoreboard);board->pageIndex=std::min(board->pageCount()-1,board->pageIndex+1);refreshDetailPage();});
    connect(list,&QListWidget::itemClicked,this,[this](QListWidgetItem *item){openMatch(list->row(item));});connect(list,&QListWidget::itemActivated,this,[this](QListWidgetItem *item){openMatch(list->row(item));});connect(back,&QPushButton::clicked,this,[this]{closeMatch();});assets->onChanged=[this]{list->viewport()->update();static_cast<Scoreboard*>(scoreboard)->refreshArtwork();};
    new SkinStyleBinding(this,[this,gutter,horizontalGutter]{
        const auto valueStyle=QStringLiteral("color:%1;background:transparent;border:0;").arg(Kda::color("text").name());
        for(auto value:static_cast<ProfileHeader*>(summary)->axisCells)value->setStyleSheet(valueStyle);
        emptyMessage->setStyleSheet(QStringLiteral("color:%1;padding:12px;").arg(Kda::color("text").name()));
        detailTitle->setStyleSheet(QStringLiteral("color:%1;font-size:16px;font-weight:600;background:transparent;border:0;").arg(Kda::color("text").name()));
        gutter->setStyleSheet(QStringLiteral("QScrollBar:vertical{width:8px;background:transparent;margin:0;}QScrollBar::handle:vertical{background:%1;min-height:24px;border-radius:1px;}QScrollBar::handle:vertical:hover{background:%2;}QScrollBar::handle:vertical:disabled{background:transparent;}QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}").arg(Kda::color("border").name(),Kda::color("muted").name()));
        horizontalGutter->setStyleSheet(QStringLiteral("QScrollBar:horizontal{height:8px;background:transparent;margin:0;}QScrollBar::handle:horizontal{background:%1;min-width:24px;border-radius:1px;}QScrollBar::handle:horizontal:hover{background:%2;}QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal{width:0;}QScrollBar::add-page:horizontal,QScrollBar::sub-page:horizontal{background:transparent;}").arg(Kda::color("border").name(),Kda::color("muted").name()));
    });
    clear();
}
void PlayerMatchView::setArchiveHeading(QWidget *heading){static_cast<QVBoxLayout*>(archiveListPanel->layout())->insertWidget(0,heading);}
void PlayerMatchView::resizeEvent(QResizeEvent *event){QWidget::resizeEvent(event);QTimer::singleShot(0,this,[this]{refreshArchivePage();refreshDetailPage();});}
bool PlayerMatchView::eventFilter(QObject *watched,QEvent *event){if(list&&watched==list->viewport()){if(event->type()==QEvent::Resize)QTimer::singleShot(0,this,[this]{refreshArchivePage();});if(event->type()==QEvent::Wheel)return true;}if(watched->objectName()==QStringLiteral("matchDetailViewport")){if(event->type()==QEvent::Resize)QTimer::singleShot(0,this,[this]{refreshDetailPage();});if(event->type()==QEvent::Wheel)return true;}return QWidget::eventFilter(watched,event);}
void PlayerMatchView::refreshDetailPage(){
    auto board=static_cast<Scoreboard*>(scoreboard);auto pager=findChild<QWidget*>(QStringLiteral("detailPlayerPager"));pager->setVisible(board->mode!=0);pager->parentWidget()->layout()->activate();if(detailScroll->parentWidget()->layout())detailScroll->parentWidget()->layout()->activate();
    board->setViewportHeight(detailScroll->viewport()->height());detailScroll->verticalScrollBar()->setValue(0);const int first=board->teamSize()?board->pageIndex*board->playersPerPage+1:0,last=std::min(board->teamSize(),(board->pageIndex+1)*board->playersPerPage);detailPageNumber->setText(first==last?QStringLiteral("%1 / %2").arg(first,2,10,QChar('0')).arg(board->teamSize(),2,10,QChar('0')):QStringLiteral("%1–%2 / %3").arg(first).arg(last).arg(board->teamSize()));detailPageNumber->setAccessibleName(QStringLiteral("双方玩家，第 %1 至 %2 位，共 %3 位").arg(first).arg(last).arg(board->teamSize()));detailPrevious->setEnabled(board->pageIndex>0);detailNext->setEnabled(board->pageIndex+1<board->pageCount());
}
void PlayerMatchView::setArchivePage(int page){archivePageIndex=std::clamp(page,0,std::max(0,(list->count()-1)/std::max(1,archivePageSize)));refreshArchivePage();}
void PlayerMatchView::refreshArchivePage(){
    const int first=archivePageIndex*archivePageSize,newSize=std::clamp(list->viewport()->height()/MatchDelegate::RowHeight,1,8);if(newSize!=archivePageSize){archivePageSize=newSize;archivePageIndex=first/archivePageSize;}
    const int count=std::max(1,(list->count()+archivePageSize-1)/archivePageSize);archivePageIndex=std::clamp(archivePageIndex,0,count-1);
    for(int i=0;i<list->count();++i)list->item(i)->setHidden(i<archivePageIndex*archivePageSize||i>=(archivePageIndex+1)*archivePageSize);
    list->doItemsLayout();list->verticalScrollBar()->setValue(0);pageNumber->setText(QStringLiteral("%1 / %2").arg(archivePageIndex+1,2,10,QChar('0')).arg(count,2,10,QChar('0')));pageNumber->setAccessibleName(QStringLiteral("第 %1 页，共 %2 页").arg(archivePageIndex+1).arg(count));previousPage->setEnabled(archivePageIndex>0);nextPage->setEnabled(archivePageIndex+1<count);archivePager->setVisible(list->count()>0);
}
void PlayerMatchView::setProfile(const QJsonObject &p){const auto oldMatches=profile.value("matches").toArray();const auto oldRef=selected>=0&&selected<oldMatches.size()?oldMatches[selected].toObject().value("match_ref").toString():QString();const bool samePlayer=profile.value("player")==p.value("player");profile=p;if(!samePlayer)archivePageIndex=0;archiveSidebar->show();summary->show();static_cast<ProfileHeader*>(summary)->setProfileData(p);list->clear();for(const auto &v:p.value("matches").toArray()){const auto m=v.toObject();auto item=new QListWidgetItem(list);item->setData(Qt::UserRole,m);item->setText("");item->setToolTip(tooltip(catalog,target(m))+QStringLiteral("\n")+metricTooltip(target(m))+QStringLiteral("\n")+date(m)+QStringLiteral(" · ")+duration(m)+QStringLiteral(" · ")+m.value("patch").toString());item->setData(Qt::AccessibleTextRole,result(m)+" · "+catalog.championName(m.value("champion_id").toInt())+" · "+score(m));}selected=-1;pages->setCurrentIndex(0);pages->show();list->setVisible(list->count()>0);emptyMessage->setVisible(list->count()==0);emptyMessage->setText(QStringLiteral("暂无已完成的海克斯对局"));emptyMessage->setToolTip(QStringLiteral("点击刷新图标尝试补充。这里只显示已完成的海克斯对局；其他模式不会计入。"));if(!oldRef.isEmpty())for(int i=0;i<list->count();++i)if(list->item(i)->data(Qt::UserRole).toJsonObject().value("match_ref").toString()==oldRef){openMatch(i);break;}refreshArchivePage();QTimer::singleShot(0,this,[this]{refreshArchivePage();});if(onDetailChanged)onDetailChanged(selected>=0);}
void PlayerMatchView::clear(const QString &message){archiveSidebar->hide();summary->hide();profile={};list->clear();archivePageIndex=0;archivePager->hide();selected=-1;pages->setCurrentIndex(0);pages->show();list->hide();emptyMessage->setText(message.isEmpty()?QStringLiteral("输入名称#编号，搜索本机已保存的战绩\n登录客户端后可查看我的战绩"):message);emptyMessage->setToolTip(message.isEmpty()?QStringLiteral("本地搜索使用已保存的对局记录。— 表示来源未记录。"):message);emptyMessage->show();static_cast<ProfileHeader*>(summary)->setProfileData({});if(onDetailChanged)onDetailChanged(false);}
void PlayerMatchView::openMatch(int index){const auto matches=profile.value("matches").toArray();if(index<0||index>=matches.size())return;selected=index;archivePageIndex=index/std::max(1,archivePageSize);summary->hide();const auto m=matches[index].toObject();static_cast<Scoreboard*>(scoreboard)->setMatch(m);detailTitle->setText(result(m));detailTitle->setToolTip(result(m)+QStringLiteral(" · 海克斯大乱斗 · ")+date(m)+QStringLiteral(" · ")+duration(m)+QStringLiteral(" · v")+m.value("patch").toString(QStringLiteral("—")));refreshDecisionReplay();pages->setCurrentIndex(1);refreshDetailPage();QTimer::singleShot(0,this,[this]{refreshDetailPage();});if(onDetailChanged)onDetailChanged(true);}
void PlayerMatchView::closeMatch(){archiveSidebar->show();summary->show();selected=-1;pages->setCurrentIndex(0);list->setFocus(Qt::OtherFocusReason);QTimer::singleShot(0,this,[this]{refreshArchivePage();});if(onDetailChanged)onDetailChanged(false);}
QString PlayerMatchView::currentMatchRef()const{
    const auto matches=profile.value("matches").toArray();
    return selected>=0&&selected<matches.size()?matches[selected].toObject().value("match_ref").toString():QString();
}
void PlayerMatchView::refreshDecisionReplay(){
    const auto ref=currentMatchRef();
    decisionReplay->setVisible(!ref.isEmpty()&&decisionReplayAvailable&&decisionReplayAvailable(ref));
}
QJsonObject PlayerMatchView::navigationState()const{const auto matches=profile.value("matches").toArray();return {{"match_ref",selected>=0&&selected<matches.size()?matches[selected].toObject().value("match_ref"):QJsonValue()},{"archive_page",archivePageIndex},{"list_scroll",list->verticalScrollBar()->value()},{"detail_page",static_cast<Scoreboard*>(scoreboard)->pageIndex},{"detail_scroll",detailScroll->verticalScrollBar()->value()},{"detail_scroll_x",detailScroll->horizontalScrollBar()->value()},{"metric_mode",static_cast<Scoreboard*>(scoreboard)->mode}};}
void PlayerMatchView::restoreNavigationState(const QJsonObject &view){const auto ref=view.value("match_ref").toString();if(!ref.isEmpty()){const auto matches=profile.value("matches").toArray();for(int i=0;i<matches.size();++i)if(matches[i].toObject().value("match_ref").toString()==ref){openMatch(i);break;}}else closeMatch();const int mode=std::clamp(view.value("metric_mode").toInt(),0,5);static_cast<Scoreboard*>(scoreboard)->setMode(mode);if(auto button=findChild<QPushButton*>(QStringLiteral("matchMetricMode_%1").arg(mode)))button->setChecked(true);setArchivePage(view.value("archive_page").toInt());static_cast<Scoreboard*>(scoreboard)->pageIndex=view.value("detail_page").toInt();refreshDetailPage();detailScroll->horizontalScrollBar()->setValue(view.value("detail_scroll_x").toInt());}
QJsonObject PlayerMatchView::report()const{
    int recordedAxes=0,missingAxes=0;for(const auto &v:profile.value("five_axes").toArray()){const auto axis=v.toObject();const auto value=axis.value("value");if(value.isDouble()&&std::isfinite(value.toDouble()))++recordedAxes;else ++missingAxes;}
    const auto board=static_cast<Scoreboard*>(scoreboard);bool navigationFits=true;for(const auto button:board->playerButtons)if(!button->isHidden())navigationFits=navigationFits&&board->rect().contains(button->geometry())&&button->focusPolicy()==Qt::StrongFocus;
    int completeRows=0;if(pages->currentIndex()==0&&list->isVisible())for(int i=0;i<list->count();++i){const auto row=list->visualItemRect(list->item(i));if(!row.isEmpty()&&list->viewport()->rect().contains(row))++completeRows;}
    const bool nativeDepthChildren=summary->parentWidget()==findChild<QWidget*>(QStringLiteral("playerProfileReadoutPlane"))&&list->parentWidget()==findChild<QWidget*>(QStringLiteral("matchArchiveReadoutPlane"))&&detailScroll->parentWidget()==findChild<QWidget*>(QStringLiteral("matchDetailReadoutPlane"));
    bool axisSemantics=true;for(const auto cell:static_cast<ProfileHeader*>(summary)->axisCells)axisSemantics&=!cell->accessibleName().isEmpty()&&!cell->accessibleDescription().isEmpty();
    bool detailControlsFit=true,detailTabsEqual=true;QJsonArray tabWidths;const auto back=findChild<QPushButton*>(QStringLiteral("backToMatchArchive"));QVector<QRect> controlRects;for(int i=0;i<6;++i){const auto button=findChild<QPushButton*>(QStringLiteral("matchMetricMode_%1").arg(i));detailControlsFit&=button&&back&&button->parentWidget()->rect().contains(button->geometry())&&button->geometry().center().y()==back->geometry().center().y();if(button){detailTabsEqual&=button->width()==76;tabWidths.append(button->width());for(const auto &other:controlRects)detailControlsFit&=!button->geometry().intersects(other);controlRects.append(button->geometry());}}
    QJsonArray profileSlots{depthSlot("front",QRectF(18,5,summary->width()-36,38)),depthSlot("rear",QRectF(18,43,summary->width()-48,23))};
    for(const auto cell:static_cast<ProfileHeader*>(summary)->axisCells)profileSlots.append(depthSlot("middle",cell->geometry()));
    profileSlots.append(depthSlot("rear",static_cast<ProfileHeader*>(summary)->radarBounds()));profileSlots.append(depthSlot("middle",static_cast<ProfileHeader*>(summary)->radarBounds()));profileSlots.append(depthSlot("middle",QRectF(43,307,summary->width()-55,22)));
    const int rowWidth=list->viewport()->width();const QJsonArray listSlots=ArchiveRowLayout(rowWidth).contentSlots();
    QJsonArray detailSlots{depthSlot("rear",QRectF(12,0,board->width()-24,24))};
    for(int i=0;i<board->match.value("players").toArray().size();++i){if(!board->playerVisible(i))continue;const auto row=board->playerRow(i);detailSlots.append(depthSlot("front",QRectF(row.x()+11,row.y()+4,42,42)));detailSlots.append(depthSlot("middle",board->kdaArea(row)));detailSlots.append(depthSlot("rear",QRectF(row.right()-board->kdaColumnWidth()-6,row.y()+31,board->kdaColumnWidth(),18)));if(board->mode!=0&&board->mode!=4)detailSlots.append(depthSlot("middle",QRectF(row).adjusted(6,board->mode==0?36:board->mode==5?52:50,-6,-1)));}
    bool stationary=true;QJsonArray stationaryHosts;for(const auto name:{"playerProfileReadoutPlane","matchArchiveReadoutPlane","matchDetailReadoutPlane"}){const auto host=findChild<QWidget*>(QString::fromLatin1(name));stationary&=host&&host->property("kdaDepthStationary").toBool();stationaryHosts.append(name);}
    int visiblePlayers=0;for(int i=0;i<board->match.value("players").toArray().size();++i)visiblePlayers+=board->playerVisible(i);int completeDetailPlayers=0;if(selected>=0&&board->mode==0)for(int i=0;i<board->match.value("players").toArray().size();++i){if(!board->playerVisible(i))continue;const auto row=board->playerRow(i);if(detailScroll->viewport()->rect().contains(QRect(board->mapTo(detailScroll->viewport(),row.topLeft()),row.size())))++completeDetailPlayers;}
    int mapped=0;for(const auto widget:findChildren<QWidget*>())mapped+=widget->property("kdaDepthMapped").toBool();
    auto profileDepth=depthSlots(summary->rect(),profileSlots),listDepth=depthSlots(QRectF(0,0,rowWidth,MatchDelegate::RowHeight),listSlots),detailDepth=depthSlots(board->rect(),detailSlots);
    profileDepth["visible"]=summary->isVisible();listDepth["visible"]=list->isVisible();detailDepth["visible"]=detailScroll->isVisible();
    listDepth["viewport_bounds"]=rectJson(list->viewport()->rect());detailDepth["viewport_bounds"]=rectJson(detailScroll->viewport()->rect());
    const QJsonObject composition{{"sublayer_content",true},{"stable_frames",stationary},{"mapped_widget_count",mapped},{"stationary_hosts",stationaryHosts},{"layers",depthLayers(summary)},{"profiles",profileDepth},{"list",listDepth},{"details",detailDepth},{"reading_midground",QJsonArray{"compactPlayerProfile","matchArchiveList","matchDetailScrollArea"}},{"fixed_actions_foreground",QJsonArray{"matchDetailKdaHeader"}},{"input_follows_native_widget_geometry",nativeDepthChildren},{"profile_visibility_synced",summary->isHidden()==summary->parentWidget()->isHidden()},{"paint_order","fixed_plate_rear_middle_front"},{"text_vertical_offset","bounded_by_actual_font_height"},{"content_offset_direction","same_as_pointer_all_layers"},{"identities_exported",false}};
    return {{"detail_visible_players",visiblePlayers},{"detail_page",board->pageIndex},{"detail_pages",board->pageCount()},{"detail_scroll_max",detailScroll->verticalScrollBar()->maximum()},{"detail_page_capacity",board->playersPerPage},{"detail_gutter_free",detailScroll->verticalScrollBarPolicy()==Qt::ScrollBarAlwaysOff},{"archive_page",archivePageIndex},{"archive_page_size",archivePageSize},{"archive_pages",std::max(1,(list->count()+archivePageSize-1)/archivePageSize)},{"archive_scroll_max",list->verticalScrollBar()->maximum()},{"statistics_require_expansion",false},{"layout","player_left_matches_right"},{"sidebar_width",archiveSidebar->width()},{"list_panel_width",archiveListPanel->width()},{"design","kda_icon_match_archive"},{"detail_style",Kda::skin()==QStringLiteral("kda_red")?QStringLiteral("kda_firestarter_black_chrome"):QStringLiteral("kda_all_out_silver_cut")},{"skin",Kda::skin()},{"victory_color",semanticInk("ice").name()},{"defeat_color",Kda::color("danger").name()},{"team_colors",QJsonArray{semanticInk("ice").name(),semanticInk("lilac").name()}},{"depth_composition",composition},{"rows",list->count()},{"profile_header_height",summary->height()},{"five_dimension_visual",static_cast<ProfileHeader*>(summary)->visualReport()},{"match_row_height",MatchDelegate::RowHeight},{"visible_complete_rows",completeRows},{"detail_player_row_height",Scoreboard::RowHeight},{"detail_current_row_height",board->rowHeight()},{"detail_overview_complete_players",completeDetailPlayers},{"detail_surface_shared_with_home",true},{"detail_tabs_text_only",true},{"hextech_row_height",Scoreboard::HextechRowHeight},{"hextech_items_recorded",board->recordedAugments()},{"hextech_items_presented",board->mode==4?board->augmentChips.size():0},{"hextech_cells_fit",board->hextechCellsFit()},{"hextech_tooltips_complete",board->hextechTooltipsComplete()},{"hextech_empty_rows",board->emptyAugmentRows()},{"detail_controls_one_row",detailControlsFit},{"detail_tabs_equal_width",detailTabsEqual},{"detail_tab_widths",tabWidths},{"detail_viewport_width",detailScroll->viewport()->width()},{"detail_canvas_width",board->width()},{"detail_canvas_min_width",board->minimumWidth()},{"detail_horizontal_scroll_max",detailScroll->horizontalScrollBar()->maximum()},{"detail_horizontal_scroll_enabled",detailScroll->horizontalScrollBarPolicy()==Qt::ScrollBarAsNeeded},{"detail_header_fields_fit",board->playerHeaderFieldsFit()},{"kda_column_width",board->kdaColumnWidth()},{"kda_minimum_pixels",board->minimumKdaPixels()},{"kda_full_values_fit",board->overviewBarsFit()},{"kda_paint_clip_verified",kdaPaintClipVerified()},{"detail_scroll_gutter_reserved",detailScroll->verticalScrollBarPolicy()==Qt::ScrollBarAlwaysOn},{"displayed_fields",board->modeFields()},{"equipment_cells_fit",board->equipmentCellsFit()},{"axis_tooltip_accessibility",axisSemantics},{"list_item_slots",7},{"list_augment_slots",6},{"details_open",selected>=0},{"details_visible",selected>=0&&detailScroll->isVisible()},{"empty_state_visible",!emptyMessage->isHidden()},{"profile_header_visible",!summary->isHidden()},{"recorded_axes",recordedAxes},{"unrecorded_axes",missingAxes},{"missing_axes_label",QStringLiteral("未记录")},{"missing_axes_are_not_loading",true},{"detail_players",selected>=0?profile.value("matches").toArray().at(selected).toObject().value("players").toArray().size():0},{"detail_columns_fit",board->columnsFit()},{"overview_columns",QJsonArray{QStringLiteral("KDA"),QStringLiteral("参团")}},{"overview_fields_fit",board->overviewBarsFit()},{"side_by_side_teams",board->teamColumnsFit()},{"team_column_width",board->panelWidth()},{"leader_rows",board->leaderRows()},{"leader_scope","complete_team_positive_maxima"},{"detail_augment_slots",board->augmentSlotCount()},{"clickable_players",selected>=0?board->playerButtons.size():0},{"player_navigation_fits",navigationFits},{"keyboard_player_navigation",true},{"metric_groups",QJsonArray{QStringLiteral("概览"),QStringLiteral("战斗"),QStringLiteral("经济"),QStringLiteral("支援"),QStringLiteral("海克斯"),QStringLiteral("装备")}},{"metric_mode",board->mode},{"metric_unknown_is_dash",true},{"metric_zero_preserved",metricValue(QJsonObject{{"damage_to_champions",0}},MatchMetric{QStringLiteral("伤害"),"damage_to_champions"})==QStringLiteral("0")},{"identities_exported",false},{"profile_header_labels_fit",static_cast<ProfileHeader*>(summary)->labelsFit()},{"icons",true},{"native_text_table",false},{"horizontal_overflow",list->horizontalScrollBarPolicy()!=Qt::ScrollBarAlwaysOff},{"artwork",assets->report()}};
}
